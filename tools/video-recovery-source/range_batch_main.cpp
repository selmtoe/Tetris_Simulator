#include <condition_variable>
#include <mutex>
#include "recovery.hpp"
#include "batch_runtime.hpp"
#include "recovery_quality_json.hpp"
#include "nvdec_video_backend.hpp"
#include "scene_detector.hpp"
#include "training_import.hpp"

#include <windows.h>
#include <mfapi.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Streaming support.  When the source is still being written by a downloader,
// a match may only be analysed once the whole range (plus a margin for the
// refinement/opening-guard re-reads) exists on disk.  TR_STREAM_READY_FILE
// holds the seconds of media that are already complete; TR_STREAM_DONE_FILE
// is created by the downloader when it finished ("done") or failed ("failed").
// Without TR_STREAM_READY_FILE every helper below is a no-op, so the ordinary
// file path is byte-for-byte unchanged.
std::string readSmallFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return {};
    std::string value;
    std::getline(file, value);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
    return value;
}

bool streamRangeReady(double endSeconds, std::string& error) {
    const char* readyPath = std::getenv("TR_STREAM_READY_FILE");
    if (!readyPath || !*readyPath) return true;
    double margin = 8.0;
    if (const char* configured = std::getenv("TR_STREAM_MARGIN_SECONDS")) {
        margin = std::max(0.0, std::atof(configured));
    }
    double timeout = 3600.0;
    if (const char* configured = std::getenv("TR_STREAM_WAIT_TIMEOUT_SECONDS")) {
        timeout = std::max(1.0, std::atof(configured));
    }
    const auto started = Clock::now();
    for (;;) {
        if (const char* donePath = std::getenv("TR_STREAM_DONE_FILE");
            donePath && *donePath) {
            const std::string done = readSmallFile(donePath);
            if (done == "failed") {
                error = "Streaming download failed before this range was complete";
                return false;
            }
            if (done == "done") return true;
        }
        const std::string text = readSmallFile(readyPath);
        if (!text.empty()) {
            try {
                if (std::stod(text) >= endSeconds + margin) return true;
            } catch (...) {
                // A partially written number is expected while the downloader
                // updates the file; retry on the next poll.
            }
        }
        if (std::chrono::duration<double>(Clock::now() - started).count() > timeout) {
            error = "Timed out waiting for the streaming download to reach this range";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::string utf8FromWide(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

class MediaRuntime {
public:
    bool initialize(std::string& error) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            error = "CoInitializeEx failed";
            return false;
        }
        comInitialized_ = true;
        const HRESULT media = MFStartup(MF_VERSION);
        if (FAILED(media)) {
            error = "MFStartup failed";
            return false;
        }
        mediaInitialized_ = true;
        return true;
    }

    ~MediaRuntime() {
        if (mediaInitialized_) MFShutdown();
        if (comInitialized_) CoUninitialize();
    }

private:
    bool comInitialized_ = false;
    bool mediaInitialized_ = false;
};

struct DetectedRange {
    int index = 0;
    double fps = 0;
    std::int64_t startFrame = -1;
    std::int64_t endFrame = -1;
    double startSeconds = 0;
    double endSeconds = 0;
    std::int64_t legacyStartFrame = -1;
    double legacyStartSeconds = 0;
    std::int64_t goFirstFrame = -1;
    std::int64_t goLastFrame = -1;
    std::int64_t goExitFrame = -1;
    double goExitSeconds = -1;
    double goDurationSeconds = 0;
};

// 終盤だけで解けなかった試合は「終盤のみ」として通す（要修正の表示を出さない）。
// 最後の数手は結果画面との境目で盤面が読めないことが多く、修正しても意味が薄いため。
// 判定は「未解決になった手が末尾から何手か」だけで行う。中盤の一致率低下
// （suspicious）は品質記録には残すが、要修正にはしない。
constexpr std::size_t kLateMatchEndToleranceSteps = 8;

inline bool isLateMatchEndTolerance(const tr::RecoveryOutput& recovery,
                                    int player, std::size_t phase,
                                    const tr::RecoveryQualityReport& quality) {
    (void)quality;
    const auto& timeline = player == 2 ? recovery.p2 : recovery.p1;
    if (timeline.empty() || phase >= timeline.size()) return false;
    return timeline.size() - phase <= kLateMatchEndToleranceSteps;
}

// 終盤のみ許容した試合は、未解決の手以降を落としてからリンクを書く
// （未解決のままではシミュレータ用リンクが生成できないため）。
inline void trimLateMatchEndTail(tr::RecoveryOutput& recovery, int player,
                                 std::size_t phase) {
    auto& timeline = player == 2 ? recovery.p2 : recovery.p1;
    if (phase < timeline.size()) timeline.resize(phase);
}

struct OpeningState {
    bool checked = false;
    bool valid = false;
    int p1Occupied = -1;
    int p2Occupied = -1;
    tr::Cell p1Hold = tr::Cell::Empty;
    tr::Cell p2Hold = tr::Cell::Empty;
};

struct StartDecision {
    std::string mode = "adaptive";
    std::string initialRule = "legacy-analysis-start";
    std::string finalRule = "legacy-analysis-start";
    double initialStartSeconds = 0;
    double finalStartSeconds = 0;
    bool guardSearched = false;
    bool guardReanalyzed = false;
    bool guardFallback = false;
    double guardTargetSeconds = -1;
    double legacyDistanceSeconds = -1;
    double goExitDistanceSeconds = -1;
    tr::OpeningStartSearchResult search;
    OpeningState initialState;
    OpeningState finalState;
};

struct CompletedRange {
    DetectedRange range;
    StartDecision startDecision;
    tr::RecoveryQualityReport quality;
    tr::TrajectoryRepairReport trajectoryRepair;
    bool success = false;
    bool needsManualCorrection = false;
    std::string error;
    std::filesystem::path autosave;
    std::filesystem::path p1Shortcut;
    std::filesystem::path p2Shortcut;
    std::filesystem::path combinedShortcut;
    std::filesystem::path linksPage;
    std::string p1Url;
    std::string p2Url;
    std::string combinedUrl;
    tr::PlayerNameRecognition p1PlayerName;
    tr::PlayerNameRecognition p2PlayerName;
    std::size_t p1Phases = 0;
    std::size_t p2Phases = 0;
    double elapsedSeconds = 0;
};

struct BatchModelStats {
    std::uint64_t created = 0, reused = 0;
    double initialization = 0;
    void add(const tr::VideoRangeAnalyzer& analyzer) {
        created += analyzer.createdModelSessions();
        reused += analyzer.reusedModelSessions();
        initialization += analyzer.modelInitializationSeconds();
    }
    std::uint64_t createdModelSessions() const { return created; }
    std::uint64_t reusedModelSessions() const { return reused; }
    double modelInitializationSeconds() const { return initialization; }
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::optional<double> numberField(const std::string& object,
                                  const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t position = object.find(marker);
    if (position == std::string::npos) return std::nullopt;
    position = object.find(':', position + marker.size());
    if (position == std::string::npos) return std::nullopt;
    ++position;
    while (position < object.size() &&
           std::isspace(static_cast<unsigned char>(object[position]))) ++position;
    const char* begin = object.c_str() + position;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<std::string> bracedObjectAfter(const std::string& json,
                                              std::size_t marker) {
    const std::size_t begin = json.find('{', marker);
    if (begin == std::string::npos) return std::nullopt;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t position = begin; position < json.size(); ++position) {
        const char value = json[position];
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') {
            quoted = true;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}' && --depth == 0) {
            return json.substr(begin, position - begin + 1);
        }
    }
    return std::nullopt;
}

bool parseRanges(const std::filesystem::path& path,
                 std::vector<DetectedRange>& ranges,
                 std::string& error) {
    const std::string json = readFile(path);
    if (json.empty()) {
        error = "Could not read match detection JSON";
        return false;
    }
    double fps = 0;
    const auto videoMarker = json.find("\"video\"");
    if (videoMarker != std::string::npos) {
        if (const auto video = bracedObjectAfter(
                json, videoMarker + std::string("\"video\"").size())) {
            if (const auto parsed = numberField(*video, "fps")) fps = *parsed;
        }
    }
    const std::string marker = "\"analysis_range\"";
    std::size_t position = 0;
    while ((position = json.find(marker, position)) != std::string::npos) {
        const auto object = bracedObjectAfter(json, position + marker.size());
        if (!object) {
            error = "Malformed analysis_range object";
            return false;
        }
        const auto startFrame = numberField(*object, "start_frame_inclusive");
        const auto endFrame = numberField(*object, "end_frame_exclusive");
        const auto startSeconds = numberField(*object, "start_seconds");
        const auto endSeconds = numberField(*object, "end_seconds");
        if (!startFrame || !endFrame || !startSeconds || !endSeconds) {
            error = "analysis_range is missing a required boundary";
            return false;
        }
        DetectedRange range;
        range.index = static_cast<int>(ranges.size() + 1);
        range.fps = fps;
        range.startFrame = static_cast<std::int64_t>(std::llround(*startFrame));
        range.endFrame = static_cast<std::int64_t>(std::llround(*endFrame));
        range.startSeconds = *startSeconds;
        range.endSeconds = *endSeconds;
        range.legacyStartFrame = range.startFrame;
        range.legacyStartSeconds = range.startSeconds;

        // Native v2-v5 detections already contain the exact GO run. Derive
        // the first post-GO frame here so old cached detections immediately
        // gain the new start candidate without forcing another full-video
        // scene scan.
        const auto goMarker = json.rfind("\"go\"", position);
        if (goMarker != std::string::npos && fps > .000001) {
            if (const auto go = bracedObjectAfter(
                    json, goMarker + std::string("\"go\"").size())) {
                const auto first = numberField(*go, "first_frame");
                const auto last = numberField(*go, "last_frame");
                if (first && last) {
                    range.goFirstFrame = static_cast<std::int64_t>(
                        std::llround(*first));
                    range.goLastFrame = static_cast<std::int64_t>(
                        std::llround(*last));
                    if (range.goFirstFrame >= 0 &&
                        range.goLastFrame >= range.goFirstFrame &&
                        range.goLastFrame < range.endFrame) {
                        range.goExitFrame = range.goLastFrame + 1;
                        range.goExitSeconds =
                            static_cast<double>(range.goExitFrame) / fps;
                        range.goDurationSeconds = static_cast<double>(
                            range.goLastFrame - range.goFirstFrame + 1) / fps;
                    }
                }
            }
        }
        if (range.startFrame < 0 || range.endFrame <= range.startFrame ||
            range.startSeconds < 0 ||
            range.endSeconds <= range.startSeconds + .000001) {
            error = "analysis_range has invalid boundaries";
            return false;
        }
        if (!ranges.empty() &&
            range.startFrame < ranges.back().endFrame) {
            error = "analysis ranges overlap or are not ordered";
            return false;
        }
        ranges.push_back(range);
        position += marker.size();
    }
    if (ranges.empty()) {
        error = "The detection JSON contains no confirmed matches";
        return false;
    }
    return true;
}

enum class MatchStartMode { Adaptive, Current, GoExit };

MatchStartMode matchStartMode(std::string& name) {
    name = "adaptive";
    const char* configured = std::getenv("TR_MATCH_START_MODE");
    if (!configured || !*configured) return MatchStartMode::Adaptive;
    std::string value(configured);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "current" || value == "legacy" || value == "old") {
        name = "current";
        return MatchStartMode::Current;
    }
    if (value == "go-exit" || value == "go_exit" || value == "go") {
        name = "go-exit";
        return MatchStartMode::GoExit;
    }
    return MatchStartMode::Adaptive;
}

bool openingGuardEnabled() {
    const char* configured = std::getenv("TR_OPENING_GUARD");
    return !configured || std::string(configured) != "0";
}

bool usableGoExit(const DetectedRange& range) {
    // 0.30 s was tested, but it dropped two already confirmed ceremonies in
    // the labelled videos (0.25 s and 0.283 s). READY evidence plus 0.25 s of
    // contiguous GO remains the lossless lower bound.
    constexpr double MinimumGoSeconds = .25;
    return range.goExitFrame >= 0 && range.goExitSeconds >= 0 &&
           range.goDurationSeconds + .000001 >= MinimumGoSeconds &&
           range.goExitSeconds <= range.legacyStartSeconds + .000001;
}

OpeningState inspectOpeningState(const tr::RecoveryOutput& recovery,
                                 const tr::Settings& settings) {
    OpeningState state;
    state.checked = true;
    const auto occupied = [](const std::vector<tr::BoardObservation>& values) {
        if (values.empty()) return -1;
        return static_cast<int>(std::count_if(
            values.front().board.begin(), values.front().board.end(),
            [](tr::Cell cell) { return cell != tr::Cell::Empty; }));
    };
    const auto hold = [](const std::vector<tr::QueueRecognitionSample>& values) {
        return values.empty() ? tr::Cell::Empty : values.front().observation.hold;
    };
    state.p1Occupied = settings.player1Enabled
        ? occupied(recovery.boardObservationsP1) : 0;
    state.p2Occupied = settings.player2Enabled
        ? occupied(recovery.boardObservationsP2) : 0;
    state.p1Hold = settings.player1Enabled
        ? hold(recovery.queueObservationsP1) : tr::Cell::Empty;
    state.p2Hold = settings.player2Enabled
        ? hold(recovery.queueObservationsP2) : tr::Cell::Empty;
    state.valid = state.p1Occupied == 0 && state.p2Occupied == 0 &&
                  state.p1Hold == tr::Cell::Empty &&
                  state.p2Hold == tr::Cell::Empty;
    return state;
}

const char* holdText(tr::Cell cell) {
    return cell == tr::Cell::Empty ? "none" : tr::cellName(cell);
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(byte)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(byte);
            }
        }
    }
    return output.str();
}

void writeQualityWarningJson(std::ostream& output,
                             const CompletedRange& item) {
    tr::writeRecoveryQualityJson(output, item.quality, item.range.startSeconds);
}

std::string htmlEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '&': output << "&amp;"; break;
        case '<': output << "&lt;"; break;
        case '>': output << "&gt;"; break;
        case '"': output << "&quot;"; break;
        case '\'': output << "&#39;"; break;
        default: output << static_cast<char>(byte); break;
        }
    }
    return output.str();
}

std::string clockText(double seconds) {
    const auto milliseconds = static_cast<long long>(std::llround(
        std::max(0.0, seconds) * 1000.0));
    const auto hours = milliseconds / 3600000;
    const auto minutes = (milliseconds / 60000) % 60;
    const auto wholeSeconds = (milliseconds / 1000) % 60;
    const auto remainder = milliseconds % 1000;
    std::ostringstream text;
    if (hours > 0) text << hours << ':' << std::setw(2) << std::setfill('0');
    text << minutes << ':' << std::setw(2) << std::setfill('0')
         << wholeSeconds << '.' << std::setw(3) << remainder;
    return text.str();
}

bool writeBatchLinksPage(const std::filesystem::path& destination,
                         const std::filesystem::path& source,
                         const std::vector<CompletedRange>& completed,
                         std::string& error) {
    std::filesystem::path temporary = destination;
    temporary += L".writing";
    std::ofstream page(temporary, std::ios::binary | std::ios::trunc);
    if (!page) {
        error = "Could not create simulator links index";
        return false;
    }
    page << "<!doctype html><html lang='ja'><meta charset='utf-8'>"
         << "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         << "<title>ぷよテト・試合別シミュレータ</title>"
         << "<style>"
            ":root{color-scheme:dark}body{margin:0;background:#10131a;color:#edf2ff;"
            "font-family:system-ui,'Yu Gothic UI',sans-serif}.wrap{max-width:980px;margin:auto;"
            "padding:28px 18px 56px}h1{font-size:1.65rem;margin:0 0 8px}.source{color:#aab4ca;"
            "margin:0 0 24px;word-break:break-all}.match{display:grid;grid-template-columns:90px 1fr "
            "repeat(3,100px);gap:10px;align-items:center;background:#181e2a;border:1px solid #2a3346;"
            "border-radius:12px;padding:12px 14px;margin:10px 0}.number{font-weight:750;font-size:1.08rem}"
            ".time{color:#b9c4db}.button{display:block;text-align:center;text-decoration:none;color:white;"
            "background:#2867d6;border-radius:8px;padding:10px 8px;font-weight:700}.button:hover{background:#3c7aea}"
            ".two{background:#b548c6}.two:hover{background:#ca5ddb}.failed{color:#ff9292}"
            ".players{color:#d7e2fa;margin-top:4px}.quality-warning{color:#ffd166;font-weight:750;margin-top:4px}.warning-match{border-color:#a36d18}"
            "@media(max-width:660px){.match{grid-template-columns:70px 1fr;}.button{grid-row:2}.time{font-size:.9rem}}"
            "</style><body><main class='wrap'><h1>試合別シミュレータ</h1><p class='source'>"
         << htmlEscape(source.filename().u8string()) << "</p>";
    for (const auto& item : completed) {
        page << "<section class='match"
             << (item.quality.suspicious() ? " warning-match" : "")
             << "'><div class='number'>試合 "
             << std::setw(3) << std::setfill('0') << item.range.index
             << "</div><div class='time'>" << clockText(item.range.startSeconds)
             << " ～ " << clockText(item.range.endSeconds)
             << "<div class='players'>P1 プレイヤー："
             << htmlEscape(item.p1PlayerName.text.empty()
                    ? std::string("未認識") : item.p1PlayerName.text)
             << " / P2 プレイヤー："
             << htmlEscape(item.p2PlayerName.text.empty()
                    ? std::string("未認識") : item.p2PlayerName.text)
             << "</div>";
        if (item.quality.suspicious()) {
            const auto& issue = item.quality.issues.front();
            page << "<div class='quality-warning'>⚠ 解析崩壊の可能性 P"
                 << issue.player << "・最低一致率 "
                 << std::fixed << std::setprecision(1)
                 << issue.minimumMatchRatio * 100.0 << "%</div>";
        }
        if (item.success && !item.needsManualCorrection && item.quality.terminalAnomaly())
            page << "<div class='players'>正常（終盤のみ異常）</div>";
        page << "</div>";
        if (item.success && !item.p1Url.empty() && !item.p2Url.empty() &&
            !item.combinedUrl.empty()) {
            page << "<a class='button' target='_blank' rel='noopener' href='"
                 << htmlEscape(item.p1Url) << "'>P1</a>"
                 << "<a class='button' target='_blank' rel='noopener' href='"
                 << htmlEscape(item.p2Url) << "'>P2</a>"
                 << "<a class='button two' target='_blank' rel='noopener' href='"
                 << htmlEscape(item.combinedUrl) << "'>2P</a>";
        } else if (item.needsManualCorrection) {
            page << "<div class='failed'>要修正（修正画面用データ保存済み）</div><div></div><div></div>";
        } else {
            page << "<div class='failed'>生成失敗</div><div></div><div></div>";
        }
        page << "</section>";
    }
    page << "</main></body></html>\n";
    page.close();
    if (!page) {
        error = "Could not finish simulator links index";
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::remove(destination, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "Could not finalize simulator links index: " + filesystemError.message();
        return false;
    }
    return true;
}

bool writeEndDetectionTrainingFile(const std::filesystem::path& directory,
                                   const std::filesystem::path& source,
                                   const std::vector<CompletedRange>& completed,
                                   std::string& error) {
    const auto destination = directory / L"end-detection-training.json";
    auto temporary = destination; temporary += L".writing";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) { error = "Could not create end-detection training file"; return false; }
    file << "{\"schema\":\"tetris-match-end-training/v1\",\"source\":\""
         << jsonEscape(source.generic_u8string())
         << "\",\"batch_manifest\":\"batch-results.json\""
         << ",\"annotation_policy\":\"Candidate windows are not ground-truth end frames; annotate the correct boundary before training.\""
         << ",\"samples\":[" << std::fixed << std::setprecision(6);
    bool first = true;
    for (const auto& item : completed) {
        if (!item.success || item.needsManualCorrection || !item.quality.terminalAnomaly()) continue;
        if (!first) file << ',';
        first = false;
        const auto& issue = item.quality.terminalIssues.front();
        const double evidenceStart = item.range.startSeconds + issue.startSeconds;
        file << "{\"match_index\":" << item.range.index
             << ",\"classification\":\"late-match-end\""
             << ",\"annotation_status\":\"boundary-required\""
             << ",\"corrected_end_seconds\":null,\"autosave\":\""
             << jsonEscape(item.autosave.generic_u8string())
             << "\",\"detected_range\":{\"start_seconds\":" << item.range.startSeconds
             << ",\"end_seconds\":" << item.range.endSeconds
             << "},\"review_window\":{\"start_seconds\":"
             << std::max(item.range.startSeconds, evidenceStart - 3.0)
             << ",\"end_seconds\":" << item.range.endSeconds
             << "},\"quality\":";
        writeQualityWarningJson(file, item);
        file << '}';
    }
    file << "]}\n";
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not finalize end-detection training file";
        return false;
    }
    return true;
}

bool writeQualityWarningsFile(const std::filesystem::path& destination,
                              const std::filesystem::path& source,
                              const std::vector<CompletedRange>& completed,
                              std::string& error) {
    std::filesystem::path temporary = destination;
    temporary += L".writing";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not create quality warnings file";
        return false;
    }
    file << "{\"schema\":\"tetris-recovery-quality-warnings/v1\""
         << ",\"source\":\"" << jsonEscape(source.generic_u8string()) << '"'
         << ",\"clip_scope\":\"whole-match-no-padding\""
         << ",\"matches\":[";
    for (std::size_t index = 0; index < completed.size(); ++index) {
        if (index) file << ',';
        const auto& item = completed[index];
        const char* status = item.success ? "complete"
            : (item.needsManualCorrection ? "needs_correction" : "failed");
        file << "{\"index\":" << item.range.index
             << ",\"status\":\"" << status << '"'
             << ",\"source_range\":{\"start_seconds\":"
             << std::fixed << std::setprecision(6)
             << item.range.startSeconds << ",\"end_seconds\":"
             << item.range.endSeconds << ",\"duration_seconds\":"
             << std::max(0.0, item.range.endSeconds - item.range.startSeconds)
             << ",\"no_padding\":true},\"quality_warning\":";
        writeQualityWarningJson(file, item);
        file << '}';
    }
    file << "]}\n";
    file.close();
    if (!file) {
        error = "Could not finish quality warnings file";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not finalize quality warnings file";
        return false;
    }
    return writeEndDetectionTrainingFile(destination.parent_path(), source, completed, error);
}

void writeTrajectoryRepairJson(std::ostream& file,
                               const tr::TrajectoryRepairReport& repair) {
    file << "{\"attempted\":" << (repair.attempted ? "true" : "false")
         << ",\"adopted\":" << (repair.adopted ? "true" : "false")
         << ",\"garbage_hypotheses_searched\":"
         << (repair.garbageHypothesesSearched ? "true" : "false")
         << ",\"neural_ranker_used\":"
         << (repair.neuralRankerUsed ? "true" : "false")
         << ",\"player\":" << repair.player
         << ",\"first_phase\":" << repair.firstSuspiciousPhase
         << ",\"first_seconds\":" << std::fixed << std::setprecision(6)
         << repair.firstSuspiciousSeconds
         << ",\"cause\":\"" << jsonEscape(repair.cause) << "\""
         << ",\"beam_before\":" << repair.initialBeamWidth
         << ",\"beam_after\":" << repair.finalBeamWidth
         << ",\"severe_phases_before\":" << repair.initialSeverePhases
         << ",\"severe_phases_after\":" << repair.finalSeverePhases
         << ",\"minimum_match_before\":" << repair.initialMinimumMatchRatio
         << ",\"minimum_match_after\":" << repair.finalMinimumMatchRatio << '}';
}

int matchIndex(const std::filesystem::path& directory) {
    const std::wstring name = directory.filename().wstring();
    if (name.rfind(L"match_", 0) != 0 || name.size() <= 6) return 0;
    wchar_t* end = nullptr;
    const long value = std::wcstol(name.c_str() + 6, &end, 10);
    return end && *end == L'\0' && value > 0
        ? static_cast<int>(value) : 0;
}

bool rebuildBatchLinks(const std::filesystem::path& manifest,
                       const std::filesystem::path& settingsPath,
                       std::string& error) {
    if (!std::filesystem::is_regular_file(manifest)) {
        error = "Batch manifest does not exist";
        return false;
    }
    tr::Settings settings;
    if (!tr::loadSettings(settingsPath, settings, error)) return false;
    const auto batchDirectory = manifest.parent_path();
    std::vector<std::pair<int, std::filesystem::path>> autosaves;
    std::error_code iterationError;
    for (const auto& entry : std::filesystem::directory_iterator(
             batchDirectory, iterationError)) {
        if (iterationError) break;
        if (!entry.is_directory()) continue;
        const int index = matchIndex(entry.path());
        if (index <= 0) continue;
        const auto autosave = entry.path() / L"autosave" /
            L"latest_editing_autosave.v2.json";
        if (std::filesystem::is_regular_file(autosave)) {
            autosaves.emplace_back(index, autosave);
        }
    }
    if (iterationError) {
        error = "Could not enumerate batch matches: " + iterationError.message();
        return false;
    }
    if (autosaves.empty()) {
        error = "No completed match autosaves were found beside the manifest";
        return false;
    }
    std::sort(autosaves.begin(), autosaves.end());
    std::vector<CompletedRange> completed;
    completed.reserve(autosaves.size());
    std::filesystem::path source;
    for (const auto& [index, autosave] : autosaves) {
        CompletedRange item;
        item.range.index = index;
        item.autosave = autosave;
        tr::RecoveryOutput recovery;
        tr::TrainingImportInfo importInfo;
        std::string itemError;
        if (tr::loadTrainingAnnotation(autosave, recovery, importInfo, itemError)) {
            if (source.empty()) source = importInfo.suggestedVideoPath;
            item.range.startSeconds = recovery.sourceStartSeconds;
            item.range.endSeconds = recovery.sourceEndSeconds;
            item.p1Phases = recovery.p1.size();
            item.p2Phases = recovery.p2.size();
            int unresolvedPlayer = 0;
            std::size_t unresolvedPhase = 0;
            item.needsManualCorrection = tr::firstRecoveryUnresolved(
                recovery, unresolvedPlayer, unresolvedPhase);
            item.quality = tr::evaluateRecoveryQuality(recovery.p1, recovery.p2);
            if (item.needsManualCorrection &&
                isLateMatchEndTolerance(recovery, unresolvedPlayer, unresolvedPhase,
                                        item.quality)) {
                trimLateMatchEndTail(recovery, unresolvedPlayer, unresolvedPhase);
                item.needsManualCorrection = false;
            }
            item.quality = tr::evaluateRecoveryQuality(recovery.p1, recovery.p2);
            item.trajectoryRepair = recovery.trajectoryRepair;
            if (item.trajectoryRepair.timeBudgetExceeded) {
                item.needsManualCorrection = true;
            }
            item.p1PlayerName = recovery.p1PlayerName;
            item.p2PlayerName = recovery.p2PlayerName;
            const auto outputDirectory = autosave.parent_path().parent_path() /
                L"simulator";
            const auto input = importInfo.suggestedVideoPath.empty()
                ? std::filesystem::path(L"stream.mp4")
                : importInfo.suggestedVideoPath;
            if (item.needsManualCorrection) {
                itemError = "Manual correction required for P" +
                    std::to_string(unresolvedPlayer) + " at phase " +
                    std::to_string(unresolvedPhase);
            } else if (tr::writeRecoveredSimulatorLinks(
                    input, outputDirectory, settings, recovery, itemError)) {
                item.success = true;
                item.p1Shortcut = recovery.p1UrlPath;
                item.p2Shortcut = recovery.p2UrlPath;
                item.combinedShortcut = recovery.combinedUrlPath;
                item.linksPage = recovery.linksPath;
                item.p1Url = recovery.p1Url;
                item.p2Url = recovery.p2Url;
                item.combinedUrl = recovery.combinedUrl;
            }
        }
        item.error = itemError;
        completed.push_back(std::move(item));
    }
    const auto linksPage = batchDirectory / L"simulator-links.html";
    if (!writeBatchLinksPage(linksPage, source, completed, error)) return false;
    const auto warningsFile = batchDirectory / L"quality-warnings.json";
    if (!writeQualityWarningsFile(
            warningsFile, source, completed, error)) return false;
    const auto failed = std::count_if(completed.begin(), completed.end(),
        [](const CompletedRange& value) {
            return !value.success && !value.needsManualCorrection;
        });
    const auto needsCorrection = std::count_if(
        completed.begin(), completed.end(),
        [](const CompletedRange& value) { return value.needsManualCorrection; });
    std::cout << "links_done=1 matches=" << completed.size()
              << " failures=" << failed
              << " needs_correction=" << needsCorrection
              << " page=" << linksPage.u8string()
              << " quality=" << warningsFile.u8string() << '\n';
    return failed == 0;
}

void setFastProfile() {
    // Keep the installed fast path unchanged. Offline audits can opt into a
    // CPU fallback when CUDA/ONNX initialization fails on the host.
    if (const char* mode = std::getenv("TR_BATCH_CPU_MODE")) {
        if (std::string(mode) == "1") {
            const std::pair<const char*, const char*> cpuValues[] = {
                {"TR_GPU_ONNX", "0"},
                {"TR_GPU_QUEUE_ONNX", "0"},
                {"TR_GPU_VIDEO", "0"},
                {"TR_BOARD_PREFETCH", "0"},
                {"TR_ULTRA_GPU_BOARD_STREAM", "0"},
                {"TR_ULTRA_GPU_QUEUE_PREPROCESS", "0"},
                {"TR_NVDEC_FRONT", "0"},
                {"TR_NVDEC_DIRECT", "0"},
            };
            for (const auto& [name, value] : cpuValues) _putenv_s(name, value);
            return;
        }
    }
    const std::pair<const char*, const char*> values[] = {
        {"TR_GPU_ONNX", "1"},
        {"TR_GPU_QUEUE_ONNX", "1"},
        {"TR_GPU_VIDEO", "2"},
        {"TR_QUEUE_SPARSE_ROI", "1"},
        {"TR_GPU_HYBRID_QUEUE", "0"},
        {"TR_BOARD_PREFETCH", "1"},
        {"TR_BOARD_PREFETCH_MB", "512"},
        {"TR_ULTRA_BOARD_STREAM", "0"},
        {"TR_ULTRA_GPU_BOARD_STREAM", "1"},
        {"TR_ULTRA_GPU_QUEUE_PREPROCESS", "1"},
        {"TR_INCREMENTAL_PREFETCH_PHASES", "1"},
        {"TR_LEGACY_OPENING_BOARD", "0"},
        {"TR_QUEUE_FRAME_BATCH", "16"},
        {"TR_QUEUE_SAFE_PIPELINE", "0"},
        {"TR_QUEUE_ADAPTIVE", "0"},
        {"TR_GPU_FUSED_PREPROCESS", "1"},
        {"TR_GPU_FUSED_DISPATCH", "0"},
        {"TR_NVDEC_FRONT", "1"},
        {"TR_NVDEC_DIRECT", "1"},
        {"TR_NVDEC_ASYNC_FRAMES", "1"},
        {"TR_NVDEC_ASYNC_BATCH", "32"},
        {"TR_NVDEC_MAPPED_ZERO_COPY", "0"},
        {"TR_NVDEC_RAW_BOARD_RING", "1"},
        {"TR_NVDEC_BOARD_FRAME_STRIDE", "1"},
        {"TR_NVDEC_QUEUE_FRAME_STRIDE", "2"},
        {"TR_NVDEC_FAST_FP32", "1"},
        {"TR_ORT_PERSISTENT_BINDING", "1"},
        {"TR_FAST_TRAJECTORY_GUARD", "0"},
        {"TR_NVDEC_BOARD_BATCH", "0"},
        {"TR_NVDEC_BOARD_BATCH_SCALAR_CNN", "1"},
        {"TR_NVDEC_BOARD_BATCH_SIZE", "3"},
    };
    for (const auto& [name, value] : values) {
        if (!std::getenv(name)) _putenv_s(name, value);
    }
}

bool writeManifest(const std::filesystem::path& destination,
                   const std::filesystem::path& source,
                   const std::filesystem::path& detections,
                   const std::vector<CompletedRange>& completed,
                   const BatchModelStats& analyzer,
                   double elapsedSeconds,
                   double sceneDetectionElapsedSeconds,
                   double pipelineElapsedSeconds,
                   const std::filesystem::path& batchLinksPage,
                   std::string& error) {
    std::filesystem::path temporary = destination;
    temporary += L".writing";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not create batch manifest";
        return false;
    }
    const std::size_t succeeded = static_cast<std::size_t>(std::count_if(
        completed.begin(), completed.end(),
        [](const CompletedRange& value) { return value.success; }));
    const std::size_t needsCorrection = static_cast<std::size_t>(std::count_if(
        completed.begin(), completed.end(),
        [](const CompletedRange& value) { return value.needsManualCorrection; }));
    const std::size_t failed = completed.size() - succeeded - needsCorrection;
    file << "{\"schema\":\"tetris-video-recovery.batch/v1\""
         << ",\"source\":\"" << jsonEscape(source.generic_u8string()) << '"'
         << ",\"detections\":\"" << jsonEscape(detections.generic_u8string()) << '"'
         << ",\"elapsed_seconds\":" << std::fixed << std::setprecision(6)
         << elapsedSeconds
         << ",\"scene_detection_elapsed_seconds\":"
         << sceneDetectionElapsedSeconds
         << ",\"pipeline_elapsed_seconds\":" << pipelineElapsedSeconds
         << ",\"succeeded\":" << succeeded
         << ",\"needs_correction\":" << needsCorrection
         << ",\"failed\":" << failed
         << ",\"simulator_links_page\":\""
         << jsonEscape(batchLinksPage.generic_u8string()) << '"'
         << ",\"model_sessions\":{\"created\":"
         << analyzer.createdModelSessions() << ",\"reused\":"
         << analyzer.reusedModelSessions() << ",\"initialization_seconds\":"
         << analyzer.modelInitializationSeconds() << "},\"matches\":[";
    for (std::size_t index = 0; index < completed.size(); ++index) {
        if (index) file << ',';
        const auto& item = completed[index];
        const char* status = item.success ? "complete"
            : (item.needsManualCorrection ? "needs_correction" : "failed");
        file << "{\"index\":" << item.range.index
             << ",\"status\":\"" << status << '"'
             << ",\"source_range\":{\"start_frame_inclusive\":"
             << item.range.startFrame << ",\"end_frame_exclusive\":"
             << item.range.endFrame << ",\"start_seconds\":"
             << item.range.startSeconds << ",\"end_seconds\":"
             << item.range.endSeconds << "},\"start_decision\":{\"mode\":\""
             << jsonEscape(item.startDecision.mode)
             << "\",\"legacy_start_seconds\":"
             << item.range.legacyStartSeconds
             << ",\"go_exit_seconds\":" << item.range.goExitSeconds
             << ",\"go_duration_seconds\":" << item.range.goDurationSeconds
             << ",\"initial_start_seconds\":"
             << item.startDecision.initialStartSeconds
             << ",\"final_start_seconds\":"
             << item.startDecision.finalStartSeconds
             << ",\"initial_rule\":\""
             << jsonEscape(item.startDecision.initialRule)
             << "\",\"final_rule\":\""
             << jsonEscape(item.startDecision.finalRule)
             << "\",\"guard_searched\":"
             << (item.startDecision.guardSearched ? "true" : "false")
             << ",\"guard_reanalyzed\":"
             << (item.startDecision.guardReanalyzed ? "true" : "false")
             << ",\"guard_fallback\":"
             << (item.startDecision.guardFallback ? "true" : "false")
             << ",\"guard_target_seconds\":"
             << item.startDecision.guardTargetSeconds
             << ",\"legacy_distance_seconds\":"
             << item.startDecision.legacyDistanceSeconds
             << ",\"go_exit_distance_seconds\":"
             << item.startDecision.goExitDistanceSeconds
             << ",\"search\":{\"found\":"
             << (item.startDecision.search.found ? "true" : "false")
             << ",\"decoded_frames\":"
             << item.startDecision.search.decodedFrames
             << ",\"tested_frames\":"
             << item.startDecision.search.testedFrames
             << ",\"consecutive_frames\":"
             << item.startDecision.search.consecutiveFrames
             << "},\"initial_state\":{\"valid\":"
             << (item.startDecision.initialState.valid ? "true" : "false")
             << ",\"p1_occupied\":"
             << item.startDecision.initialState.p1Occupied
             << ",\"p2_occupied\":"
             << item.startDecision.initialState.p2Occupied
             << ",\"p1_hold\":\""
             << holdText(item.startDecision.initialState.p1Hold)
             << "\",\"p2_hold\":\""
             << holdText(item.startDecision.initialState.p2Hold)
             << "\"},\"final_state\":{\"valid\":"
             << (item.startDecision.finalState.valid ? "true" : "false")
             << ",\"p1_occupied\":"
             << item.startDecision.finalState.p1Occupied
             << ",\"p2_occupied\":"
             << item.startDecision.finalState.p2Occupied
             << ",\"p1_hold\":\""
             << holdText(item.startDecision.finalState.p1Hold)
             << "\",\"p2_hold\":\""
             << holdText(item.startDecision.finalState.p2Hold)
             << "\"}},\"quality_warning\":";
        writeQualityWarningJson(file, item);
        file << ",\"trajectory_repair\":";
        writeTrajectoryRepairJson(file, item.trajectoryRepair);
        file << ",\"player_names\":{\"p1\":{\"text\":\""
             << jsonEscape(item.p1PlayerName.text)
             << "\",\"confidence\":" << item.p1PlayerName.confidence
             << ",\"attempted\":"
             << (item.p1PlayerName.attempted ? "true" : "false")
             << "},\"p2\":{\"text\":\""
             << jsonEscape(item.p2PlayerName.text)
             << "\",\"confidence\":" << item.p2PlayerName.confidence
             << ",\"attempted\":"
             << (item.p2PlayerName.attempted ? "true" : "false")
             << "}}";
        file << ",\"elapsed_seconds\":"
             << item.elapsedSeconds << ",\"p1_phases\":" << item.p1Phases
             << ",\"p2_phases\":" << item.p2Phases
             << ",\"autosave\":\""
             << jsonEscape(item.autosave.generic_u8string()) << '"'
             << ",\"simulator\":{\"p1\":\""
             << jsonEscape(item.p1Shortcut.generic_u8string())
             << "\",\"p2\":\"" << jsonEscape(item.p2Shortcut.generic_u8string())
             << "\",\"combined\":\""
             << jsonEscape(item.combinedShortcut.generic_u8string())
             << "\",\"links_page\":\""
             << jsonEscape(item.linksPage.generic_u8string()) << "\"}"
             << ",\"error\":\"" << jsonEscape(item.error) << "\"}";
    }
    file << "]}\n";
    file.close();
    if (!file) {
        error = "Could not finish batch manifest";
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::remove(destination, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "Could not finalize batch manifest: " + filesystemError.message();
        return false;
    }
    return true;
}

double elapsed(Clock::time_point started) {
    return std::chrono::duration<double>(Clock::now() - started).count();
}

#ifdef TR_CONSUMER_OUTPUT
#include "consumer_output.hpp"
#endif
CompletedRange analyzeRange(DetectedRange range, std::size_t totalHint,
    tr::VideoRangeAnalyzer& analyzer, const tr::Settings& settings,
    const std::filesystem::path& source, const std::filesystem::path& outputDirectory,
    MatchStartMode startMode, const std::string& configuredStartMode, bool useOpeningGuard,
    const std::function<void(int, int, const std::string&)>& progressChanged) {

    StartDecision decision;
    decision.mode = configuredStartMode;
    if (startMode != MatchStartMode::Current && usableGoExit(range)) {
        range.startFrame = range.goExitFrame;
        range.startSeconds = range.goExitSeconds;
        decision.initialRule = startMode == MatchStartMode::GoExit
            ? "forced-go-exit" : "adaptive-go-exit";
    }
    {
        // v76: start at the first frame where the uncovered HOLD/NEXT slots move after GO
        // (the first spawn). GO-exit is 3-8 frames earlier; starting there let the
        // engine miss a fast first piece or HOLD (bTL5 49, FMhM 47). TR_QUEUE_MOVE_START=0
        // restores the GO-exit start.
        // v77: search GO-exit -1 s .. +1 s (never before GO-first). v76 searched from GO-first
        // with a 3 s cap, which ended before the first move when READY was counted as GO
        // (bTL5 49: found=0). The start may also move earlier than GO-exit, because the
        // queue can move before the detected GO end (uRZn 52/69: 0.37/0.60 s earlier).
        const char* flag = std::getenv("TR_QUEUE_MOVE_START");
        const bool enabled = !(flag && std::string(flag) == "0");
        if (enabled && usableGoExit(range) && range.goFirstFrame >= 0 && range.fps > 0) {
            const double goFirstSeconds = static_cast<double>(range.goFirstFrame) / range.fps;
            const double searchFrom = std::max(goFirstSeconds, range.goExitSeconds - 1.0);
            const double limit = std::min(range.endSeconds, range.goExitSeconds + 1.0);
            tr::OpeningStartSearchResult moved;
            std::string moveError;
            if (limit > searchFrom &&
                analyzer.findQueueMoveStart(searchFrom, limit, moved, moveError) && moved.found) {
                const std::int64_t frame = static_cast<std::int64_t>(std::llround(moved.startSeconds * range.fps));
                if (frame != range.startFrame && frame > range.goFirstFrame && frame < range.endFrame) {
                    range.startFrame = frame;
                    range.startSeconds = static_cast<double>(frame) / range.fps;
                    decision.initialRule = "queue-move";
                }
                tr::BatchLog() << "queue_move_start=" << range.index << " seconds=" << moved.startSeconds
                               << " slots=" << moved.consecutiveFrames << " tested=" << moved.testedFrames
                               << " applied=" << (decision.initialRule == "queue-move" ? 1 : 0) << '\n' << std::flush;
            } else {
                tr::BatchLog() << "queue_move_start=" << range.index << " found=0"
                               << (moveError.empty() ? "" : " error=" + moveError) << '\n' << std::flush;
            }
        }
    }
    {
        // Opt-in experiment: TR_START_OVERRIDE_FILE is a TSV of "goFirstFrame<TAB>startFrame" lines
        // (e.g. the frame where the uncovered NEXT/HOLD slots first change after GO).
        static const std::map<std::int64_t, std::int64_t> startOverrides = [] {
            std::map<std::int64_t, std::int64_t> table;
            const char* path = std::getenv("TR_START_OVERRIDE_FILE");
            if (!path || !*path) return table;
            std::ifstream in(path);
            std::int64_t go = 0, start = 0;
            while (in >> go >> start) table[go] = start;
            tr::BatchLog() << "start_override_entries=" << table.size() << '\n' << std::flush;
            return table;
        }();
        const auto found = startOverrides.find(range.goFirstFrame);
        if (found != startOverrides.end() && range.fps > 0 && found->second > range.goFirstFrame &&
            found->second < range.endFrame) {
            range.startFrame = found->second;
            range.startSeconds = static_cast<double>(found->second) / range.fps;
            decision.initialRule = "override-file";
        }
    }
    decision.finalRule = decision.initialRule;
    decision.initialStartSeconds = range.startSeconds;
    decision.finalStartSeconds = range.startSeconds;
    tr::BatchLog() << "worker_match_start=" << range.index << '/' << totalHint
              << " source=" << std::fixed << std::setprecision(6)
              << range.startSeconds << '-' << range.endSeconds
              << " legacy=" << range.legacyStartSeconds
              << " go_exit=" << range.goExitSeconds
              << " rule=" << decision.initialRule << '\n'
              << std::flush;
    tr::Status status;
    tr::RecoveryOutput recovery;
    std::string rangeError;
    std::atomic<bool> monitoring{true};
    std::mutex monitorMutex;
    std::condition_variable monitorWake;

    std::atomic<int> progressGeneration{0};
    std::thread monitor([&] {
        int previousProgress = -1;
        int previousGeneration = -1;
        while (monitoring.load(std::memory_order_relaxed)) {
            const int generation = progressGeneration.load(
                std::memory_order_relaxed);
            if (generation != previousGeneration) {
                previousProgress = -1;
                previousGeneration = generation;
            }
            const int progress = status.progress.load(std::memory_order_relaxed);
            if (progress > previousProgress &&
                (previousProgress < 0 || progress == 100 ||
                 progress / 5 != previousProgress / 5)) {
                tr::BatchLog() << "worker_match_progress=" << range.index
                          << " percent=" << progress
                          << " message=\"" << status.getMessage() << "\"\n"
                          << std::flush;
                progressChanged(range.index, progress, status.getMessage());
                previousProgress = progress;
            }
            // Preserve the normal progress cadence, but wake immediately
            // when this match finishes instead of waiting out the interval.
            std::unique_lock<std::mutex> lock(monitorMutex);
            monitorWake.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return !monitoring.load(std::memory_order_relaxed);
            });
        }
    });
    struct MonitorGuard {
        std::atomic<bool>& running;
        std::mutex& mutex;
        std::condition_variable& wake;
        std::thread& thread;
        void stop() {
            { std::lock_guard<std::mutex> lock(mutex); running.store(false); }
            wake.notify_one();
            if (thread.joinable()) thread.join();
        }
        ~MonitorGuard() { stop(); }
    } monitorGuard{monitoring, monitorMutex, monitorWake, monitor};
    const auto rangeStarted = Clock::now();
    bool success = false;
    if (!streamRangeReady(range.endSeconds, rangeError)) {
        success = false;
    } else {
        success = analyzer.analyze(
            range.startSeconds, range.endSeconds, status, recovery, rangeError);
    }
    if (success) {
        decision.initialState = inspectOpeningState(recovery, settings);
        decision.finalState = decision.initialState;
        tr::BatchLog() << "opening_state=" << range.index
                  << " valid=" << (decision.initialState.valid ? 1 : 0)
                  << " p1_occupied=" << decision.initialState.p1Occupied
                  << " p2_occupied=" << decision.initialState.p2Occupied
                  << " p1_hold=" << holdText(decision.initialState.p1Hold)
                  << " p2_hold=" << holdText(decision.initialState.p2Hold)
                  << '\n' << std::flush;
        if (std::getenv("TR_OPENING_STATE_DIAG")) {
            // Opt-in: where the first board/queue samples come from in time.
            const auto dump = [&](const char* name, const std::vector<tr::BoardObservation>& boards,
                                  const std::vector<tr::QueueRecognitionSample>& queue) {
                auto cells = [](const tr::BoardObservation& b) {
                    return static_cast<int>(std::count_if(b.board.begin(), b.board.end(),
                        [](tr::Cell c) { return c != tr::Cell::Empty; }));
                };
                tr::BatchLog log;
                log << "opening_state_diag=" << range.index << ' ' << name
                    << " range_start=" << range.startSeconds << " boards=" << boards.size();
                for (std::size_t i = 0; i < boards.size() && i < 6; ++i)
                    log << " b" << i << '@' << boards[i].timeSeconds << ':' << cells(boards[i]);
                double minT = 1e18; std::size_t minI = 0;
                for (std::size_t i = 0; i < boards.size(); ++i)
                    if (boards[i].timeSeconds < minT) { minT = boards[i].timeSeconds; minI = i; }
                if (!boards.empty()) log << " earliest=" << minI << '@' << minT << ':' << cells(boards[minI]);
                if (!boards.empty()) {
                    // Later samples whose board is identical to the first one (where the first frame really comes from).
                    int shown = 0;
                    for (std::size_t i = 1; i < boards.size() && shown < 6; ++i) {
                        if (boards[i].timeSeconds > boards.front().timeSeconds + .1 &&
                            boards[i].board == boards.front().board) {
                            log << " same_as_b0@" << boards[i].timeSeconds;
                            ++shown;
                        }
                    }
                    if (!shown) log << " same_as_b0=none";
                }
                for (std::size_t i = 0; i < queue.size() && i < 4; ++i)
                    log << " q" << i << '@' << queue[i].timeSeconds << ":hold=" << holdText(queue[i].observation.hold);
                log << '\n' << std::flush;
            };
            dump("p1", recovery.boardObservationsP1, recovery.queueObservationsP1);
            dump("p2", recovery.boardObservationsP2, recovery.queueObservationsP2);
        }
    }

    const bool normalizedOpening = success &&
        recovery.forcedEmptyOpeningApplied;
    if (success && useOpeningGuard &&
        (!decision.initialState.valid || normalizedOpening)) {
        decision.guardSearched = true;
        const double frameSeconds = range.fps > .000001
            ? 1.0 / range.fps : 1.0 / 60.0;
        const double candidate = usableGoExit(range)
            ? range.goExitSeconds : range.legacyStartSeconds;
        const double searchStart = std::max(
            0.0, candidate - 3.0 * frameSeconds);
        const double searchEnd = std::min(
            range.endSeconds,
            std::max(range.legacyStartSeconds,
                     candidate + .75));
        std::string searchError;
        tr::BatchLog() << "opening_guard_search=" << range.index
                  << " range=" << searchStart << '-' << searchEnd
                  << '\n' << std::flush;
        if (searchEnd > searchStart + .000001 &&
            analyzer.findEmptyOpeningStart(
                searchStart, searchEnd, 2, decision.search,
                searchError) && decision.search.found) {
            decision.guardTargetSeconds = decision.search.startSeconds;
            decision.legacyDistanceSeconds = std::abs(
                range.legacyStartSeconds - decision.guardTargetSeconds);
            decision.goExitDistanceSeconds = usableGoExit(range)
                ? std::abs(range.goExitSeconds -
                           decision.guardTargetSeconds) : -1;
            const bool goExitCloser = usableGoExit(range) &&
                decision.goExitDistanceSeconds + .000001 <
                    decision.legacyDistanceSeconds;
            tr::BatchLog() << "opening_guard_target=" << range.index
                      << " seconds=" << decision.guardTargetSeconds
                      << " run_frames="
                      << decision.search.consecutiveFrames
                      << " legacy_distance="
                      << decision.legacyDistanceSeconds
                      << " go_exit_distance="
                      << decision.goExitDistanceSeconds
                      << " go_exit_closer=" << (goExitCloser ? 1 : 0)
                      << '\n' << std::flush;

            // The empty-board probe is the authority for an invalid
            // opening. If the new-match invariant already normalized the
            // opening, only move backwards: a later clean frame would
            // discard valid match footage and the normalized GO-exit path
            // is already known to repair those cases.
            tr::RecoveryQualityReport baselineQuality;
            int baselinePlayer = 0;
            std::size_t baselinePhase = 0;
            bool baselineUnresolved = false;
            if (normalizedOpening) {
                baselineQuality = tr::evaluateRecoveryQuality(
                    recovery.p1, recovery.p2);
                baselineUnresolved = tr::firstRecoveryUnresolved(
                    recovery, baselinePlayer, baselinePhase);
            }
            const bool targetEarlier =
                decision.guardTargetSeconds + .000001 <
                    range.startSeconds;
            const bool baselineNeedsAlternative =
                !normalizedOpening || baselineUnresolved ||
                baselineQuality.totalIssueCount() != 0;
            const bool shouldReanalyze = !decision.initialState.valid ||
                (targetEarlier && baselineNeedsAlternative);
            if (!shouldReanalyze) {
                tr::BatchLog() << "opening_guard_keep_normalized="
                          << range.index
                          << " target_not_earlier="
                          << (targetEarlier ? 0 : 1)
                          << " baseline_clean="
                          << (baselineNeedsAlternative ? 0 : 1)
                          << '\n' << std::flush;
            } else {
                // The empty-board probe is the authority. GO-exit and the
                // legacy scene label are only search anchors: neither may
                // override a frame that was actually verified as empty.
                // In particular, the fading GO graphic can remain inside
                // the board crop after the scene classifier has left the
                // GO class and be misread as locked cells.
                const double correctedStartSeconds =
                    decision.guardTargetSeconds;

                tr::RecoveryOutput guardedRecovery;
                std::string guardedError;
                decision.guardReanalyzed = true;
                tr::BatchLog() << "opening_guard_reanalyze=" << range.index
                          << " old_start=" << range.startSeconds
                          << " new_start="
                          << correctedStartSeconds << '\n'
                          << std::flush;
                status.progress.store(0, std::memory_order_relaxed);
                status.setMessage(
                    "Opening guard: strict empty-board reanalysis");
                progressGeneration.fetch_add(1,
                    std::memory_order_relaxed);
                const bool guardedSuccess = analyzer.analyze(
                    correctedStartSeconds, range.endSeconds,
                    status, guardedRecovery, guardedError);
                const OpeningState guardedState = guardedSuccess
                    ? inspectOpeningState(guardedRecovery, settings)
                    : OpeningState{};

                bool guardImproves = true;
                if (normalizedOpening && guardedSuccess &&
                    guardedState.valid) {
                    const auto guardedQuality =
                        tr::evaluateRecoveryQuality(
                            guardedRecovery.p1, guardedRecovery.p2);
                    int guardedPlayer = 0;
                    std::size_t guardedPhase = 0;
                    const bool guardedUnresolved =
                        tr::firstRecoveryUnresolved(
                            guardedRecovery, guardedPlayer, guardedPhase);
                    guardImproves =
                        (baselineUnresolved && !guardedUnresolved) ||
                        (baselineUnresolved == guardedUnresolved &&
                         guardedQuality.totalIssueCount() <
                             baselineQuality.totalIssueCount());
                    tr::BatchLog() << "opening_guard_compare=" << range.index
                              << " baseline_unresolved="
                              << (baselineUnresolved ? 1 : 0)
                              << " guarded_unresolved="
                              << (guardedUnresolved ? 1 : 0)
                              << " baseline_issues="
                              << baselineQuality.totalIssueCount()
                              << " guarded_issues="
                              << guardedQuality.totalIssueCount()
                              << " adopt=" << (guardImproves ? 1 : 0)
                              << '\n' << std::flush;
                }
                decision.finalState = guardedState;
                if (guardedSuccess && guardedState.valid &&
                    guardImproves) {
                    recovery = std::move(guardedRecovery);
                    range.startSeconds = correctedStartSeconds;
                    if (range.fps > .000001) {
                        range.startFrame = static_cast<std::int64_t>(
                            std::llround(range.startSeconds * range.fps));
                    }
                    decision.finalStartSeconds = range.startSeconds;
                    decision.finalRule = "empty-opening-guard-target";
                } else if (guardedSuccess && guardedState.valid &&
                           !guardImproves) {
                    // Sampling phase changes can alter a late ambiguous
                    // beam decision even when both starts look empty.
                    // Keep the established result unless the alternative
                    // strictly reduces unresolved/low-agreement output.
                    decision.finalState = decision.initialState;
                    tr::BatchLog() << "opening_guard_keep_normalized="
                              << range.index
                              << " no_quality_improvement=1\n"
                              << std::flush;
                } else {
                    // Keep the already reconstructed match. Its persisted
                    // time-zero state is normalized below, so a failed
                    // visual probe cannot turn an otherwise healthy match
                    // into a failure.
                    decision.guardFallback = true;
                    decision.finalRule = "forced-empty-opening-export";
                    const std::string fallbackReason = guardedError.empty()
                        ? "Opening guard rejected a non-empty corrected start"
                        : "Opening guard reanalysis failed: " + guardedError;
                    std::cerr << "opening_guard_fallback=" << range.index
                              << " reason=\"" << fallbackReason << "\"\n";
                }
            }
        } else {
            if (!normalizedOpening) {
                decision.guardFallback = true;
                decision.finalRule = "forced-empty-opening-export";
                const std::string fallbackReason = searchError.empty()
                    ? "Opening guard found no two-frame empty opening run"
                    : "Opening guard search failed: " + searchError;
                std::cerr << "opening_guard_fallback=" << range.index
                          << " reason=\"" << fallbackReason << "\"\n";
            }
        }
    }
    if (success && settings.forceEmptyOpening) {
        tr::forceRecoveryOpeningEmpty(recovery, &settings);
        decision.finalState = inspectOpeningState(recovery, settings);
        if (!decision.finalState.valid) {
            success = false;
            decision.finalRule = "forced-empty-opening-invariant-failed";
            rangeError = "Failed to enforce the empty opening invariant";
        }
    }
    {
        std::lock_guard<std::mutex> lock(monitorMutex);
        monitoring.store(false, std::memory_order_relaxed);
    }
    monitorWake.notify_one();
    monitor.join();

    CompletedRange result;
    result.range = range;
    result.startDecision = decision;
    result.elapsedSeconds = elapsed(rangeStarted);
    if (success) {
        int unresolvedPlayer = 0;
        std::size_t unresolvedPhase = 0;
        result.needsManualCorrection = tr::firstRecoveryUnresolved(
            recovery, unresolvedPlayer, unresolvedPhase);
        result.quality = tr::evaluateRecoveryQuality(recovery.p1, recovery.p2);
        if (result.needsManualCorrection &&
            isLateMatchEndTolerance(recovery, unresolvedPlayer, unresolvedPhase,
                                    result.quality)) {
            // 終盤のみ: 未解決の手以降を落として、要修正の表示を出さずに通す。
            trimLateMatchEndTail(recovery, unresolvedPlayer, unresolvedPhase);
            result.needsManualCorrection = false;
        }
        result.success = !result.needsManualCorrection;
        result.trajectoryRepair = recovery.trajectoryRepair;
        // A ladder that hit its wall-clock ceiling kept the fast-pass timeline,
        // so nothing verified the suspicious phases. Surface it exactly like any
        // other unresolved match: the reviewer opens it instead of trusting an
        // unconfirmed trajectory, and the batch summary counts it.
        if (result.trajectoryRepair.timeBudgetExceeded) {
            result.needsManualCorrection = true;
            result.success = false;
        }
        result.p1PlayerName = recovery.p1PlayerName;
        result.p2PlayerName = recovery.p2PlayerName;
        if (result.trajectoryRepair.attempted) {
            const auto& repair = result.trajectoryRepair;
            tr::BatchLog() << "trajectory_repair=" << range.index
                      << " player=" << repair.player
                      << " phase=" << repair.firstSuspiciousPhase
                      << " cause=" << repair.cause
                      << " garbage_search="
                      << (repair.garbageHypothesesSearched ? 1 : 0)
                      << " adopted=" << (repair.adopted ? 1 : 0)
                      << " severe=" << repair.initialSeverePhases
                      << "->" << repair.finalSeverePhases
                      << " minimum=" << repair.initialMinimumMatchRatio
                      << "->" << repair.finalMinimumMatchRatio << '\n'
                      << std::flush;
        }
        if (result.quality.suspicious()) {
            const auto& firstIssue = result.quality.issues.front();
            tr::BatchLog() << "quality_warning=" << range.index
                      << " issues=" << result.quality.issues.size()
                      << " player=" << firstIssue.player
                      << " relative=" << firstIssue.startSeconds
                      << '-' << firstIssue.endSeconds
                      << " minimum_match="
                      << firstIssue.minimumMatchRatio
                      << " consecutive="
                      << firstIssue.consecutivePhases << '\n'
                      << std::flush;
        }
#ifdef TR_CONSUMER_OUTPUT
        result.p1Phases = recovery.p1.size();
        result.p2Phases = recovery.p2.size();
        const bool nameFailed =
            (recovery.p1PlayerName.attempted && !recovery.p1PlayerName.error.empty()) ||
            (recovery.p2PlayerName.attempted && !recovery.p2PlayerName.error.empty());
        result.success = !result.needsManualCorrection && !result.quality.suspicious() && !nameFailed;
        if (result.success) {
            result.success = tr::prepareConsumerSimulatorLinks(settings, recovery, rangeError);
            if (result.success) {
                result.p1Url = recovery.p1Url;
                result.p2Url = recovery.p2Url;
                result.combinedUrl = recovery.combinedUrl;
            }
        }
        success = result.success;
        if (!success) rangeError = "Final result did not pass verification";
#else
        std::wostringstream directoryName;
        directoryName << L"match_" << std::setw(3) << std::setfill(L'0')
                      << range.index;
        const auto matchDirectory = outputDirectory / directoryName.str();
        result.autosave = matchDirectory / L"autosave" /
            L"latest_editing_autosave.v2.json";
        recovery.includeSourceVideoInTraining = false;
        if (!tr::writeRecoveryAutosave(
                source, result.autosave, recovery, rangeError)) {
            success = false;
            result.success = false;
            result.needsManualCorrection = false;
        } else {
            result.p1Phases = recovery.p1.size();
            result.p2Phases = recovery.p2.size();
            if (result.needsManualCorrection) {
                if (result.trajectoryRepair.timeBudgetExceeded) {
                    // No unresolved phase index describes this case: the ladder
                    // stopped early, so the suspicious phases were never
                    // verified. Say so instead of reporting "P0 at phase 0".
                    rangeError =
                        "Trajectory repair time budget exceeded; review autosave preserved";
                } else {
                    rangeError = "Manual correction required for P" +
                        std::to_string(unresolvedPlayer) + " at phase " +
                        std::to_string(unresolvedPhase) +
                        "; review autosave preserved";
                }
                success = false;
            } else if (!tr::writeRecoveredSimulatorLinks(
                    source, matchDirectory / L"simulator", settings,
                    recovery, rangeError)) {
                success = false;
                result.success = false;
            } else {
                result.p1Shortcut = recovery.p1UrlPath;
                result.p2Shortcut = recovery.p2UrlPath;
                result.combinedShortcut = recovery.combinedUrlPath;
                result.linksPage = recovery.linksPath;
                result.p1Url = recovery.p1Url;
                result.p2Url = recovery.p2Url;
                result.combinedUrl = recovery.combinedUrl;
            }
        }
#endif
    }
    result.error = rangeError;
    tr::BatchLog doneLog;
    doneLog << "worker_match_done=" << range.index
              << " success=" << (success ? 1 : 0)
              << " repairable=" << (result.needsManualCorrection ? 1 : 0)
              << " elapsed=" << std::fixed << std::setprecision(3)
              << result.elapsedSeconds << " p1=" << result.p1Phases
              << " p2=" << result.p2Phases;
    if (!success) doneLog << " error=\"" << result.error << '"';
    doneLog << '\n';
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Metadata-only edit used by the study manager. Keeping this operation
    // native avoids a fragile textual rewrite of the large autosave JSON.
    const bool setPlayerNamesOnly = argc >= 2 &&
        std::wcscmp(argv[1], L"--set-player-names") == 0;
    if (setPlayerNamesOnly) {
        if (argc != 5) {
            std::cerr << "usage: TetrisVideoRecovery_batch --set-player-names "
                         "<autosave> <p1-name> <p2-name>\n";
            return 2;
        }
        const auto autosave = std::filesystem::absolute(argv[2]);
        tr::RecoveryOutput recovery;
        tr::TrainingImportInfo importInfo;
        std::string editError;
        if (!tr::loadTrainingAnnotation(autosave, recovery,
                                        importInfo, editError)) {
            std::cerr << "player_name_edit_error=" << editError << '\n';
            return 1;
        }
        const auto applyName = [](const wchar_t* value,
                                  tr::PlayerNameRecognition& name) {
            name.text = utf8FromWide(
                value ? std::wstring(value) : std::wstring());
            name.attempted = true;
            name.confidence = 1.0;
            name.engine = "manual";
            name.error.clear();
        };
        applyName(argv[3], recovery.p1PlayerName);
        applyName(argv[4], recovery.p2PlayerName);
        if (!tr::writeRecoveryAutosave(importInfo.suggestedVideoPath,
                                       autosave, recovery, editError)) {
            std::cerr << "player_name_edit_error=" << editError << '\n';
            return 1;
        }
        std::cout << "player_name_edit_done=1 autosave="
                  << autosave.u8string() << '\n';
        return 0;
    }
    // Internal one-frame probe used when calibrating the two fixed name
    // rectangles.  Normal users never need this mode; batch analysis reads
    // the same permanent rectangles from the shipped settings automatically.
    const bool playerNameOcrOnly = argc >= 2 &&
        std::wcscmp(argv[1], L"--player-name-ocr") == 0;
    if (playerNameOcrOnly) {
        if (argc != 5) {
            std::cerr << "usage: TetrisVideoRecovery_batch --player-name-ocr "
                         "<video> <seconds> <settings>\n";
            return 2;
        }
        std::string ocrError;
        MediaRuntime ocrRuntime;
        if (!ocrRuntime.initialize(ocrError)) {
            std::cerr << "media_runtime_error=" << ocrError << '\n';
            return 1;
        }
        tr::Settings ocrSettings;
        if (!tr::loadSettings(std::filesystem::absolute(argv[4]),
                              ocrSettings, ocrError)) {
            std::cerr << "settings_error=" << ocrError << '\n';
            return 1;
        }
        wchar_t* secondsEnd = nullptr;
        const double seconds = std::wcstod(argv[3], &secondsEnd);
        if (!secondsEnd || *secondsEnd != L'\0' || seconds < 0) {
            std::cerr << "seconds_error=invalid timestamp\n";
            return 2;
        }
        tr::RecoveryOutput ocrOutput;
        const auto ocrStarted = Clock::now();
        if (!tr::recognizePlayerNames(std::filesystem::absolute(argv[2]),
                                      seconds, ocrSettings, ocrOutput,
                                      ocrError)) {
            std::cerr << "player_name_ocr_error=" << ocrError << '\n';
            return 1;
        }
        std::cout << "player_name_ocr_done=1 elapsed=" << std::fixed
                  << std::setprecision(3) << elapsed(ocrStarted)
                  << " p1=\"" << ocrOutput.p1PlayerName.text
                  << "\" p1_confidence="
                  << ocrOutput.p1PlayerName.confidence
                  << " p2=\"" << ocrOutput.p2PlayerName.text
                  << "\" p2_confidence="
                  << ocrOutput.p2PlayerName.confidence;
        if (!ocrError.empty()) std::cout << " warning=\"" << ocrError << '"';
        std::cout << '\n';
        return 0;
    }
    const bool linksOnly = argc >= 2 && std::wcscmp(argv[1], L"--links") == 0;
    if (linksOnly) {
        if (argc != 4) {
            std::cerr << "usage: TetrisVideoRecovery_batch --links "
                         "<batch-results.json> <settings>\n";
            return 2;
        }
        std::string linkError;
        if (!rebuildBatchLinks(std::filesystem::absolute(argv[2]),
                               std::filesystem::absolute(argv[3]), linkError)) {
            std::cerr << "links_error=" << linkError << '\n';
            return 1;
        }
        return 0;
    }
    const bool classifyOnly = argc >= 2 &&
        std::wcscmp(argv[1], L"--classify") == 0;
    if (classifyOnly) {
        if (argc != 5) {
            std::cerr << "usage: TetrisVideoRecovery_batch --classify "
                         "<video> <segments.matches.json> <scene-model>\n";
            return 2;
        }
        const auto classifyStarted = Clock::now();
        const auto classifySource = std::filesystem::absolute(argv[2]);
        const auto classifyDestination = std::filesystem::absolute(argv[3]);
        const auto classifyModel = std::filesystem::absolute(argv[4]);
        std::error_code directoryError;
        std::filesystem::create_directories(
            classifyDestination.parent_path(), directoryError);
        if (directoryError) {
            std::cerr << "output_error=" << directoryError.message() << '\n';
            return 1;
        }
        if (tr::sceneDetectionCacheValid(
                classifyDestination, classifySource, classifyModel)) {
            std::cout << "scene_cache=hit path="
                      << classifyDestination.u8string() << '\n';
            return 0;
        }
        std::string classifyError;
        MediaRuntime classifyRuntime;
        if (!classifyRuntime.initialize(classifyError)) {
            std::cerr << "media_runtime_error=" << classifyError << '\n';
            return 1;
        }
        tr::SceneDetectionResult detection;
        setFastProfile();
        if (!tr::detectSceneMatches(
                classifySource, classifyModel, 0.25,
                detection, classifyError)) {
            std::cerr << "scene_detection_error=" << classifyError << '\n';
            return 1;
        }
        if (!tr::writeSceneDetection(
                classifyDestination, classifySource, classifyModel,
                detection, classifyError)) {
            std::cerr << "scene_detection_write_error=" << classifyError << '\n';
            return 1;
        }
        std::cout << "classify_done=1 matches=" << detection.matches.size()
                  << " frames=" << detection.classifiedFrames
                  << " elapsed=" << std::fixed << std::setprecision(3)
                   << elapsed(classifyStarted)
                   << " detection=" << classifyDestination.u8string()
                   << '\n';
        return 0;
    }
    // Report whether this file can be decoded directly, before anyone considers
    // converting it.  The answer comes from the same decoder the analysis uses.
    if (argc >= 3 && std::wcscmp(argv[2], L"--probe") == 0) {
        const std::filesystem::path probePath = std::filesystem::absolute(argv[1]);
        std::error_code probeError;
        const auto probeStarted = Clock::now();
        if (!std::filesystem::is_regular_file(probePath, probeError)) {
            std::cout << "probe=ng reason=missing_file path=" << probePath.u8string() << '\n';
            return 3;
        }
#if defined(TR_NVDEC_DEFAULT)
        tr::NvdecVideoBackend probe;
        std::string openError;
        if (!probe.open(probePath, openError)) {
            std::cout << "probe=ng reason=open_failed error=\"" << openError << "\"\n";
            return 3;
        }
        const double duration = probe.durationSeconds();
        bool decoded = false;
        std::string decodeError;
        // A growing fragmented download reports no duration.  Probing the
        // middle would also seek past the written prefix, so decode from the
        // start instead of reporting a false negative.
        if (duration <= 0.5) {
            const auto sink = [](const void*, std::size_t, int, int, bool, std::int64_t,
                                 std::string&) { return true; };
            for (int attempt = 0; attempt < 3 && !decoded; ++attempt) {
                std::int64_t timestamp = 0;
                bool endOfStream = false;
                if (!probe.readCudaSample(timestamp, endOfStream, decodeError, sink)) break;
                decoded = !endOfStream;
            }
            std::cout << "probe=" << (decoded ? "ok" : "ng")
                      << " width=" << probe.width()
                      << " height=" << probe.height()
                      << " fps=" << std::fixed << std::setprecision(3)
                      << probe.frameRate()
                      << " duration=" << probe.durationSeconds()
                      << " decode=" << (decoded ? "ok" : "failed")
                      << " mode=head"
                      << " elapsed=" << elapsed(probeStarted);
            if (!decodeError.empty()) std::cout << " error=\"" << decodeError << "\"";
            std::cout << '\n';
            probe.close();
            return decoded ? 0 : 3;
        }
        if (duration > 0.5 && probe.seek(duration * 0.5, decodeError)) {
            const auto sink = [](const void*, std::size_t, int, int, bool, std::int64_t,
                                 std::string&) { return true; };
            for (int attempt = 0; attempt < 3 && !decoded; ++attempt) {
                std::int64_t timestamp = 0;
                bool endOfStream = false;
                if (!probe.readCudaSample(timestamp, endOfStream, decodeError, sink)) break;
                decoded = !endOfStream;
            }
        }
        std::cout << "probe=" << (decoded ? "ok" : "ng")
                  << " width=" << probe.width()
                  << " height=" << probe.height()
                  << " fps=" << std::fixed << std::setprecision(3) << probe.frameRate()
                  << " duration=" << probe.durationSeconds()
                  << " decode=" << (decoded ? "ok" : "failed")
                  << " elapsed=" << elapsed(probeStarted);
        if (!decodeError.empty()) std::cout << " error=\"" << decodeError << "\"";
        std::cout << '\n';
        probe.close();
        return decoded ? 0 : 3;
#else
        std::cout << "probe=ng reason=no_nvdec_build\n";
        return 3;
#endif
    }
    const bool nativeDetection = argc >= 3 &&
        std::wcscmp(argv[2], L"--detect") == 0;
    if ((!nativeDetection && (argc < 6 || argc > 7)) ||
        (nativeDetection && (argc < 7 || argc > 8))) {
        std::cerr << "usage: TetrisVideoRecovery_batch <video> <matches.json> "
                     "<output-dir> <board-model> <settings> [limit]\n"
                     "   or: TetrisVideoRecovery_batch <video> --detect "
                     "<output-dir> <scene-model> <board-model> <settings> [limit]\n"
                     "   or: TetrisVideoRecovery_batch <video> --probe\n";
        return 2;
    }

    const auto pipelineStarted = Clock::now();
    const std::filesystem::path source = std::filesystem::absolute(argv[1]);
    const std::filesystem::path outputDirectory =
        std::filesystem::absolute(argv[3]);
    const std::filesystem::path detections = nativeDetection
        ? outputDirectory / L"segments.matches.json"
        : std::filesystem::absolute(argv[2]);
    const std::filesystem::path sceneModel = nativeDetection
        ? std::filesystem::absolute(argv[4]) : std::filesystem::path{};
    const std::filesystem::path model = std::filesystem::absolute(
        nativeDetection ? argv[5] : argv[4]);
    const std::filesystem::path settingsPath = std::filesystem::absolute(
        nativeDetection ? argv[6] : argv[5]);
    const int limitArgument = nativeDetection ? 7 : 6;
    const int limit = argc > limitArgument
        ? static_cast<int>(std::max<long>(
              0, std::wcstol(argv[limitArgument], nullptr, 10)))
        : 0;

    std::string error;
    MediaRuntime mediaRuntime;
    if (!mediaRuntime.initialize(error)) {
        std::cerr << "media_runtime_error=" << error << '\n';
        return 1;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if (filesystemError) {
        std::cerr << "output_error=" << filesystemError.message() << '\n';
        return 1;
    }

    tr::Settings settings;
    if (!tr::loadSettings(settingsPath, settings, error)) {
        std::cerr << "settings_error=" << error << '\n';
        return 1;
    }
    settings.forceEmptyOpening = true;
    // Environment is immutable after this point: no worker can change another
    // worker's profile, and the detector starts with the same NVDEC defaults.
    setFastProfile();
    std::string configuredStartMode;
    const MatchStartMode startMode = matchStartMode(configuredStartMode);
    const bool useOpeningGuard = openingGuardEnabled();
    // TR_SEGMENT_WINDOW="start,end" (source seconds): analyse only the detected
    // matches that start inside this window. The scene scan still covers the
    // whole file, so every exported time stays in the original video's clock
    // (cutting the file first shifted the times). Matches are renumbered 1..N.
    double windowStart = -1, windowEnd = -1;
    if (const char* v = std::getenv("TR_SEGMENT_WINDOW")) {
        double a = 0, b = 0;
        if (std::sscanf(v, "%lf,%lf", &a, &b) == 2 && b > a && a >= 0) {
            windowStart = a;
            windowEnd = b;
        }
    }
    const bool segmentWindow = windowStart >= 0;
    // Overlapped submission starts workers before the full list is known, so
    // a window turns it off (the scan itself is unchanged).
    const bool overlap = !segmentWindow &&
        tr::batchSetting("TR_BATCH_OVERLAP", 1, 0, 1) != 0;
    tr::BatchQueue<DetectedRange> queue;
    std::vector<CompletedRange> completed;
    BatchModelStats analyzer;
    std::mutex resultsMutex;
    std::string workerError;
    bool protocolReady = false;
    std::size_t protocolTotal = 0, protocolNext = 1, protocolAnnounced = 0;
    // The existing PowerShell reader treats index/total as sequential progress.
    // Keep its protocol ordered and delay it until the detector knows the total.
    // Live work remains visible as worker_match_* diagnostic lines.
    auto publishCompleted = [&] {
        if (!protocolReady) return;
        while (protocolNext <= protocolTotal) {
            if (protocolAnnounced != protocolNext) {
                tr::BatchLog() << "match_start=" << protocolNext << '/' << protocolTotal << '\n';
                protocolAnnounced = protocolNext;
            }
            const auto found = std::find_if(completed.begin(), completed.end(), [&](const auto& item) {
                return item.range.index == static_cast<int>(protocolNext);
            });
            if (found == completed.end()) break;
            tr::BatchLog() << "match_done=" << found->range.index
                << " success=" << (found->success ? 1 : 0)
                << " repairable=" << (found->needsManualCorrection ? 1 : 0)
                << " elapsed=" << std::fixed << std::setprecision(3) << found->elapsedSeconds
                << " p1=" << found->p1Phases << " p2=" << found->p2Phases
                << (found->error.empty() ? "" : " error=\"" + found->error + "\"") << '\n';
            ++protocolNext;
        }
    };
    auto publishProgress = [&](int index, int progress, const std::string& message) {
        std::lock_guard<std::mutex> lock(resultsMutex);
        if (protocolReady && static_cast<std::size_t>(index) == protocolNext)
            tr::BatchLog() << "match_progress=" << index << " percent=" << progress
                          << " message=\"" << message << "\"\n";
    };
    std::vector<std::thread> workers;
    auto batchStarted = Clock::now();
    auto batchFinished = batchStarted;
    bool batchHasStarted = false;
    std::size_t totalHint = limit > 0 ? static_cast<std::size_t>(limit) : 0;
    struct JoinWorkers {
        tr::BatchQueue<DetectedRange>& queue;
        std::vector<std::thread>& workers;
        ~JoinWorkers() {
            queue.cancel();
            for (auto& worker : workers) if (worker.joinable()) worker.join();
        }
    } joinWorkers{queue, workers};
    auto submit = [&](const DetectedRange& range) {
        if (limit > 0 && range.index > limit) return;
        if (!batchHasStarted) { batchStarted = Clock::now(); batchHasStarted = true; }
        queue.push(range);
    };
    // Only the producer writes totalHint before workers start. For a live scan,
    // the exact total is unknown until EOF; each start line has a valid lower bound.
    auto startWorkers = [&] {
        if (!workers.empty()) return;
        tr::BatchLog() << "start_policy mode=" << configuredStartMode
                      << " opening_guard=" << (useOpeningGuard ? 1 : 0)
                      << " go_minimum_seconds=0.25 rollback=TR_MATCH_START_MODE=current\n";
        tr::BatchLog() << "scheduler_start=1 workers=" << queue.workers
                      << " overlap=" << (overlap ? 1 : 0) << '\n';
        for (int i = 0; i < queue.workers; ++i) workers.emplace_back([&] {
            try {
                MediaRuntime workerRuntime;
                std::string runtimeError;
                if (!workerRuntime.initialize(runtimeError)) throw std::runtime_error(runtimeError);
                std::unique_ptr<tr::VideoRangeAnalyzer> localAnalyzer;
                auto releaseIdle = [&] {
                    if (!localAnalyzer) return;
                    {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        analyzer.add(*localAnalyzer);
                    }
                    localAnalyzer.reset();
                    tr::BatchLog() << "scheduler_release=idle_sessions\n";
                };
                DetectedRange range;
                while (queue.take(range, releaseIdle)) {
                    CompletedRange result;
                    try {
                        if (!localAnalyzer) localAnalyzer = std::make_unique<tr::VideoRangeAnalyzer>(source, model, settings);
                        result = analyzeRange(range, std::max(totalHint, static_cast<std::size_t>(range.index)),
                            *localAnalyzer, settings, source, outputDirectory,
                            startMode, configuredStartMode, useOpeningGuard, publishProgress);
                    } catch (const std::exception& exception) {
                        result.range = range;
                        result.error = exception.what();
                        tr::BatchLog() << "worker_match_done=" << range.index << " success=0 error=\""
                                      << result.error << "\"\n";
                    }
                    {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        completed.push_back(std::move(result));
                        batchFinished = Clock::now();
                        publishCompleted();
#ifdef TR_CONSUMER_OUTPUT
                        std::sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) { return a.range.index < b.range.index; });
                        std::string consumerError;
                        if (!writeConsumerResults(outputDirectory, source, completed, false, consumerError))
                            workerError = consumerError;
#endif
                    }
                    queue.finished();
                }
                releaseIdle();
            } catch (const std::exception& exception) {
                { std::lock_guard<std::mutex> lock(resultsMutex); workerError = exception.what(); }
                queue.cancel();
            }
        });
    };

    std::vector<DetectedRange> ranges;
    double sceneDetectionElapsedSeconds = 0;
    std::size_t submitted = 0;
    if (nativeDetection) {
        if (tr::sceneDetectionCacheValid(detections, source, sceneModel)) {
            tr::BatchLog() << "scene_cache=hit path=" << detections.u8string() << '\n';
        } else {
            tr::BatchLog() << "scene_cache=miss engine=cpp-nvdec-cuda-onnx-v5\n";
            tr::SceneDetectionResult detection;
            tr::SceneMatchCallback onMatch;
            if (overlap) {
                startWorkers();
                onMatch = [&](const tr::SceneMatchRange& match, double fps) {
                    DetectedRange range;
                    range.index = match.index;
                    range.fps = fps;
                    range.startFrame = range.legacyStartFrame = match.startFrame;
                    range.endFrame = match.endFrame;
                    // Match the on-disk detector's nine-decimal boundary contract.
                    range.startSeconds = range.legacyStartSeconds = std::round(match.startSeconds * 1e9) / 1e9;
                    range.endSeconds = std::round(match.endSeconds * 1e9) / 1e9;
                    range.goFirstFrame = match.goFirstFrame;
                    range.goLastFrame = match.goLastFrame;
                    if (match.goFirstFrame >= 0 && match.goLastFrame >= match.goFirstFrame && match.goLastFrame < match.endFrame) {
                        range.goExitFrame = match.goLastFrame + 1;
                        range.goExitSeconds = static_cast<double>(range.goExitFrame) / fps;
                        range.goDurationSeconds = static_cast<double>(match.goLastFrame - match.goFirstFrame + 1) / fps;
                    }
                    submit(range);
                    ++submitted;
                };
            }
            if (!tr::detectSceneMatches(source, sceneModel, 0.25, detection, error, onMatch)) {
                std::cerr << "scene_detection_error=" << error << '\n';
                return 1;
            }
            sceneDetectionElapsedSeconds = detection.elapsedSeconds;
            if (!tr::writeSceneDetection(detections, source, sceneModel, detection, error)) {
                std::cerr << "scene_detection_write_error=" << error << '\n';
                return 1;
            }
            tr::BatchLog() << "scene_done=1 matches=" << detection.matches.size()
                          << " frames=" << detection.classifiedFrames
                          << " elapsed=" << std::fixed << std::setprecision(3)
                          << detection.elapsedSeconds << '\n';
        }
    }
    if (!parseRanges(detections, ranges, error)) {
        std::cerr << "range_parse_error=" << error << '\n';
        return 1;
    }
#ifdef TR_CONSUMER_OUTPUT
    if (nativeDetection) std::filesystem::remove(detections, filesystemError);
#endif
    if (segmentWindow) {
        const std::size_t before = ranges.size();
        ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [&](const DetectedRange& range) {
            return range.startSeconds < windowStart || range.startSeconds >= windowEnd;
        }), ranges.end());
        int renumbered = 0;
        for (DetectedRange& range : ranges) {
            tr::BatchLog() << "segment_window_keep=" << range.index << "->" << (renumbered + 1)
                          << " start=" << std::fixed << std::setprecision(3) << range.startSeconds << "\n";
            range.index = ++renumbered;
        }
        tr::BatchLog() << "segment_window=" << windowStart << ',' << windowEnd
                      << " kept=" << ranges.size() << '/' << before << "\n";
        if (ranges.empty()) {
            std::cerr << "range_parse_error=no confirmed matches inside TR_SEGMENT_WINDOW\n";
            return 1;
        }
    }
    if (limit > 0 && ranges.size() > static_cast<std::size_t>(limit)) ranges.resize(static_cast<std::size_t>(limit));
    {
        std::lock_guard<std::mutex> lock(resultsMutex);
        protocolTotal = ranges.size();
        protocolReady = true;
        publishCompleted();
    }
    if (workers.empty()) totalHint = ranges.size();
    startWorkers();
    for (std::size_t i = submitted; i < ranges.size(); ++i) submit(ranges[i]);
    queue.close();
    for (auto& worker : workers) worker.join();
    if (!workerError.empty()) {
        std::cerr << "scheduler_error=" << workerError << '\n';
        return 1;
    }
    std::sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) { return a.range.index < b.range.index; });
    if (completed.size() != ranges.size()) {
        std::cerr << "scheduler_error=incomplete_match_count\n";
        return 1;
    }

#ifdef TR_CONSUMER_OUTPUT
    if (!writeConsumerResults(outputDirectory, source, completed, true, error)) {
        std::cerr << error << '\n'; return 1;
    }
    std::cout << "batch_done=1 matches=" << completed.size()
              << " pipeline_elapsed=" << elapsed(pipelineStarted) << '\n';
    return 0; // Individual failures are final results; an engine failure returns 1 above.
#else
    const double batchElapsed = std::chrono::duration<double>(batchFinished - batchStarted).count();
    const double pipelineElapsed = elapsed(pipelineStarted);
    const auto batchLinksPage = outputDirectory / L"simulator-links.html";
    if (!writeBatchLinksPage(batchLinksPage, source, completed, error)) {
        std::cerr << "links_page_error=" << error << '\n';
        return 1;
    }
    const auto warningsFile = outputDirectory / L"quality-warnings.json";
    if (!writeQualityWarningsFile(
            warningsFile, source, completed, error)) {
        std::cerr << "quality_file_error=" << error << '\n';
        return 1;
    }
    const auto manifest = outputDirectory / L"batch-results.json";
    if (!writeManifest(manifest, source, detections, completed,
                       analyzer, batchElapsed, sceneDetectionElapsedSeconds,
                       pipelineElapsed, batchLinksPage, error)) {
        std::cerr << "manifest_error=" << error << '\n';
        return 1;
    }
    const std::size_t failures = static_cast<std::size_t>(std::count_if(
        completed.begin(), completed.end(),
        [](const CompletedRange& value) {
            return !value.success && !value.needsManualCorrection;
        }));
    const std::size_t needsCorrection = static_cast<std::size_t>(std::count_if(
        completed.begin(), completed.end(),
        [](const CompletedRange& value) { return value.needsManualCorrection; }));
    std::cout << "batch_done=1 matches=" << completed.size()
              << " failures=" << failures
              << " needs_correction=" << needsCorrection
              << " elapsed=" << std::fixed << std::setprecision(3)
              << batchElapsed << " pipeline_elapsed=" << pipelineElapsed
              << " model_sessions_created=" << analyzer.createdModelSessions()
              << " model_sessions_reused=" << analyzer.reusedModelSessions()
              << " manifest=" << manifest.u8string()
              << " simulator_links=" << batchLinksPage.u8string() << '\n';
    if (completed.empty()) {
        // Silence here is what makes an empty result look like a success.
        std::cerr << "batch_error=no_matches_detected source="
                  << source.u8string()
                  << " detections=" << detections.u8string() << '\n';
        return 4;
    }
    return failures == 0 ? 0 : 1;
#endif
}
