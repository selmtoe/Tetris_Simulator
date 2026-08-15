#ifdef __EMSCRIPTEN__

#include "recovery.hpp"
#include "onnx_model.hpp"
#include "tetris_engine.hpp"
#include "vision.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <iomanip>
#include <string>
#include <utility>
#include <vector>

namespace {

using tr::BoardObservation;
using tr::Cell;
using tr::Frame;
using tr::QueueRecognitionSample;
using tr::QueueObservation;
using tr::RecoveryOutput;
using tr::Settings;
using tr::VisionAnalyzer;

struct Runtime {
    int width = 0;
    int height = 0;
    Frame currentFrame;
    bool frameReady = false;
    Settings settings;
    double duration = 0;
    std::unique_ptr<tr::OnnxBoardModel> geometryModel;
    std::unique_ptr<VisionAnalyzer> p1Vision;
    std::unique_ptr<VisionAnalyzer> p2Vision;
    std::vector<QueueRecognitionSample> p1Queue;
    std::vector<QueueRecognitionSample> p2Queue;
    std::vector<double> p1BoardTimes;
    std::vector<double> p2BoardTimes;
    bool requestsPrepared = false;
    std::vector<BoardObservation> p1Boards;
    std::vector<BoardObservation> p2Boards;
    RecoveryOutput output;
    std::string error;
    std::string outputPath;
    std::string reviewJson;
    std::string candidateJson;
};

Runtime runtime;

void setError(const std::string& value) {
    runtime.error = value;
}

int fail(const std::string& value) {
    setError(value);
    return 0;
}

void copyString(const std::string& value, char* destination, int capacity) {
    if (!destination || capacity <= 0) return;
    const std::size_t count = std::min<std::size_t>(value.size(), static_cast<std::size_t>(capacity - 1));
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                result += escaped.str();
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    return result;
}

std::string cellString(Cell cell) {
    return std::string(1, tr::cellChar(cell));
}

std::string piecesString(const std::vector<Cell>& pieces) {
    return tr::pieceString(pieces);
}

std::string boardString(const tr::Board& board) {
    std::string result;
    result.reserve(board.size());
    for (const Cell cell : board) result.push_back(tr::cellChar(cell));
    return result;
}

void writeGarbage(std::ostringstream& out, const tr::GarbageRise& garbage) {
    out << "{\"lines\":" << garbage.lines << ",\"holeMasks\":[";
    for (std::size_t i = 0; i < garbage.holeMasks.size(); ++i) {
        if (i) out << ',';
        out << garbage.holeMasks[i];
    }
    out << "],\"matchRatio\":" << std::fixed << std::setprecision(4) << garbage.matchRatio
        << ",\"manual\":" << (garbage.manuallySpecified ? "true" : "false") << '}';
}

void writeTimeline(std::ostringstream& out, const std::vector<tr::TimelineStep>& timeline) {
    out << '[';
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        if (i) out << ',';
        const auto& step = timeline[i];
        out << "{\"start\":" << std::fixed << std::setprecision(3) << step.startSeconds
            << ",\"time\":" << step.timeSeconds
            << ",\"piece\":\"" << tr::cellChar(step.piece)
            << "\",\"action\":\"" << jsonEscape(step.action)
            << "\",\"hold\":\"" << tr::cellChar(step.hold)
            << "\",\"next\":\"" << jsonEscape(piecesString(step.next))
            << "\",\"observed\":\"" << boardString(step.observed)
            << "\",\"board\":\"" << boardString(step.board)
            << "\",\"fullBoard\":\"" << boardString(step.fullBoard)
            << "\",\"garbage\":";
        writeGarbage(out, step.garbage);
        out << ",\"placedPiece\":\"" << tr::cellChar(step.placedPiece)
            << "\",\"x\":" << step.placementX
            << ",\"y\":" << step.placementY
            << ",\"rotation\":" << step.placementRotation
            << ",\"clearedLines\":" << step.clearedLines
            << ",\"score\":" << step.score
            << ",\"uncertain\":" << (step.uncertain ? "true" : "false")
            << ",\"manual\":" << (step.manuallyFixed ? "true" : "false")
            << ",\"holdUsed\":" << (step.holdUsed ? "true" : "false")
            << ",\"queueManual\":" << (step.queueManuallyFixed ? "true" : "false") << '}';
    }
    out << ']';
}

void writeQueue(std::ostringstream& out, const std::vector<tr::QueueRecognitionSample>& samples) {
    out << '[';
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i) out << ',';
        const auto& sample = samples[i];
        out << "{\"time\":" << std::fixed << std::setprecision(3) << sample.timeSeconds
            << ",\"active\":\"" << tr::cellChar(sample.active)
            << "\",\"hold\":\"" << tr::cellChar(sample.observation.hold)
            << "\",\"next\":\"" << jsonEscape(piecesString(sample.observation.next))
            << "\",\"decodedHold\":\"" << tr::cellChar(sample.decoded.hold)
            << "\",\"decodedNext\":\"" << jsonEscape(piecesString(sample.decoded.next))
            << "\",\"stable\":" << (sample.stable ? "true" : "false")
            << ",\"manual\":" << (sample.manuallyEdited ? "true" : "false")
            << ",\"sequenceCorrected\":" << (sample.sequenceCorrected ? "true" : "false")
            << ",\"holdCorrected\":" << (sample.holdCorrected ? "true" : "false")
            << ",\"rejected\":" << (sample.rejected ? "true" : "false") << '}';
    }
    out << ']';
}

std::string makeReviewSnapshot() {
    std::ostringstream out;
    out << "{\"duration\":" << runtime.duration << ",\"p1\":{";
    out << "\"raw\":"; writeTimeline(out, runtime.output.rawP1);
    out << ",\"solved\":"; writeTimeline(out, runtime.output.p1);
    out << ",\"queue\":"; writeQueue(out, runtime.p1Queue);
    out << ",\"originalQueue\":"; writeQueue(out, runtime.output.originalQueueObservationsP1);
    out << "},\"p2\":{";
    out << "\"raw\":"; writeTimeline(out, runtime.output.rawP2);
    out << ",\"solved\":"; writeTimeline(out, runtime.output.p2);
    out << ",\"queue\":"; writeQueue(out, runtime.p2Queue);
    out << ",\"originalQueue\":"; writeQueue(out, runtime.output.originalQueueObservationsP2);
    out << "}}";
    return out.str();
}

std::string makeCandidateJson(int player, std::size_t phase, bool useOverride,
                              int lines, const std::uint16_t* masks, int maskCount) {
    const auto& raw = player == 1 ? runtime.output.rawP1 : runtime.output.rawP2;
    const auto& solved = player == 1 ? runtime.output.p1 : runtime.output.p2;
    std::optional<tr::GarbageRise> overrideGarbage;
    if (useOverride) {
        tr::GarbageRise rise;
        rise.lines = std::clamp(lines, 0, tr::VisibleRows);
        rise.manuallySpecified = true;
        for (int i = 0; i < maskCount && i < rise.lines; ++i) rise.holeMasks.push_back(masks[i]);
        while (rise.holeMasks.size() < static_cast<std::size_t>(rise.lines)) rise.holeMasks.push_back(1u << 4);
        overrideGarbage = rise;
    }
    const auto candidates = tr::TetrisEngine::correctionCandidates(raw, solved, phase, runtime.settings, overrideGarbage);
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (i) out << ',';
        const auto& candidate = candidates[i];
        out << "{\"index\":" << i
            << ",\"piece\":\"" << tr::cellChar(candidate.move.piece)
            << "\",\"x\":" << candidate.move.x
            << ",\"y\":" << candidate.move.y
            << ",\"rotation\":" << candidate.move.rotation
            << ",\"clearedLines\":" << candidate.move.clearedLines
            << ",\"cells\":[";
        for (int cell = 0; cell < candidate.move.cellCount; ++cell) {
            if (cell) out << ',';
            out << '[' << candidate.move.cells[static_cast<std::size_t>(cell)][0] << ','
                << candidate.move.cells[static_cast<std::size_t>(cell)][1] << ']';
        }
        out << "],\"board\":\"" << boardString(candidate.move.board)
            << "\",\"fullBoard\":\"" << boardString(candidate.move.fullBoard)
            << "\",\"score\":" << candidate.observationScore << ",\"garbage\":";
        writeGarbage(out, candidate.garbage);
        out << '}';
    }
    out << ']';
    return out.str();
}

bool validFrameInput(const std::uint8_t* rgba, int byteCount) {
    if (!rgba || runtime.width <= 0 || runtime.height <= 0) {
        setError("WASM vision is not initialized");
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(runtime.width) * runtime.height * 4;
    if (byteCount < 0 || static_cast<std::size_t>(byteCount) < expected) {
        setError("Canvas frame has fewer pixels than the initialized video size");
        return false;
    }
    return true;
}

Frame makeFrame(const std::uint8_t* rgba) {
    Frame frame;
    frame.width = runtime.width;
    frame.height = runtime.height;
    const std::size_t pixels = static_cast<std::size_t>(runtime.width) * runtime.height;
    frame.bgra.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        // Canvas ImageData is RGBA; VisionAnalyzer consumes the native
        // VideoReader BGRA layout. Alpha is retained for byte-for-byte frame
        // shape compatibility even though the classifier ignores it.
        frame.bgra[i * 4 + 0] = rgba[i * 4 + 2];
        frame.bgra[i * 4 + 1] = rgba[i * 4 + 1];
        frame.bgra[i * 4 + 2] = rgba[i * 4 + 0];
        frame.bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    return frame;
}

bool uploadFrame(const std::uint8_t* rgba, int byteCount) {
    if (!validFrameInput(rgba, byteCount)) return false;
    runtime.currentFrame = makeFrame(rgba);
    runtime.frameReady = true;
    return true;
}

VisionAnalyzer* visionForPlayer(int player) {
    if (player == 1) return runtime.p1Vision.get();
    if (player == 2) return runtime.p2Vision.get();
    setError("Player must be 1 or 2");
    return nullptr;
}

std::vector<QueueRecognitionSample>& queueForPlayer(int player) {
    return player == 1 ? runtime.p1Queue : runtime.p2Queue;
}

std::vector<BoardObservation>& boardsForPlayer(int player) {
    return player == 1 ? runtime.p1Boards : runtime.p2Boards;
}

std::vector<double>& boardTimesForPlayer(int player) {
    return player == 1 ? runtime.p1BoardTimes : runtime.p2BoardTimes;
}

Cell labelToCell(int value) {
    // Same class order as native OnnxBoardModel::labelToCell:
    // null, G, S, Z, L, J, O, I, T.
    switch (value) {
    case 1: return Cell::Garbage;
    case 2: return Cell::S;
    case 3: return Cell::Z;
    case 4: return Cell::L;
    case 5: return Cell::J;
    case 6: return Cell::O;
    case 7: return Cell::I;
    case 8: return Cell::T;
    default: return Cell::Empty;
    }
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int tr_runtime_init(int width, int height, const char* settingsPath) {
    runtime = Runtime{};
    runtime.width = width;
    runtime.height = height;
    if (width <= 0 || height <= 0) return fail("Video dimensions must be positive");

    if (settingsPath && settingsPath[0] != '\0') {
        if (!tr::loadSettings(std::filesystem::path(settingsPath), runtime.settings, runtime.error)) return 0;
    }
    std::string geometryError;
    runtime.geometryModel = std::make_unique<tr::OnnxBoardModel>(std::filesystem::path{}, geometryError);
    runtime.p1Vision = std::make_unique<VisionAnalyzer>(width, height, runtime.settings.p1, runtime.geometryModel.get());
    runtime.p2Vision = std::make_unique<VisionAnalyzer>(width, height, runtime.settings.p2, runtime.geometryModel.get());
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const char* tr_last_error() {
    return runtime.error.c_str();
}

EMSCRIPTEN_KEEPALIVE
void tr_runtime_reset(double durationSeconds) {
    runtime.duration = durationSeconds;
    runtime.p1Queue.clear();
    runtime.p2Queue.clear();
    runtime.p1BoardTimes.clear();
    runtime.p2BoardTimes.clear();
    runtime.requestsPrepared = false;
    runtime.p1Boards.clear();
    runtime.p2Boards.clear();
    runtime.output = RecoveryOutput{};
    runtime.currentFrame = Frame{};
    runtime.frameReady = false;
    runtime.error.clear();
}

EMSCRIPTEN_KEEPALIVE
int tr_frame_upload(const std::uint8_t* rgba, int byteCount) {
    return uploadFrame(rgba, byteCount) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int tr_queue_observe_and_add(int player, const std::uint8_t* rgba, int byteCount, double timeSeconds,
                             int* hold, int* next, int nextCapacity,
                             std::uint8_t* colors, int colorsCapacity) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount)) return 0;
    if (!runtime.frameReady && !uploadFrame(rgba, byteCount)) return 0;
    const QueueObservation observation = vision->observeQueue(runtime.currentFrame);
    if (hold) *hold = static_cast<int>(observation.hold);
    if (next && nextCapacity > 0) {
        const int count = std::min<int>(nextCapacity, observation.next.size());
        for (int i = 0; i < count; ++i) next[i] = static_cast<int>(observation.next[static_cast<std::size_t>(i)]);
    }
    if (colors && colorsCapacity > 0) {
        const int count = std::min<int>(colorsCapacity / 3, observation.colors.size());
        for (int i = 0; i < count; ++i) {
            colors[i * 3 + 0] = observation.colors[static_cast<std::size_t>(i)][0];
            colors[i * 3 + 1] = observation.colors[static_cast<std::size_t>(i)][1];
            colors[i * 3 + 2] = observation.colors[static_cast<std::size_t>(i)][2];
        }
    }
    QueueRecognitionSample sample;
    sample.timeSeconds = timeSeconds;
    sample.observation = observation;
    queueForPlayer(player).push_back(std::move(sample));
    return static_cast<int>(observation.next.size()) + 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_prepare_board_requests(int player, double* times, int capacity) {
    if (runtime.duration <= 0) return fail("Video duration must be set before queue preparation");
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    if (!runtime.requestsPrepared) {
        if (!tr::prepareObservationRequests(runtime.duration, runtime.settings,
                                            runtime.p1Queue, runtime.p2Queue,
                                            runtime.p1BoardTimes, runtime.p2BoardTimes,
                                            runtime.error)) return 0;
        runtime.requestsPrepared = true;
    }
    const auto& source = boardTimesForPlayer(player);
    if (times && capacity > 0) {
        const int count = std::min<int>(capacity, source.size());
        std::copy_n(source.begin(), count, times);
    }
    return static_cast<int>(source.size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_features(int player, const std::uint8_t* rgba, int byteCount, float* output, int outputCapacity) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !output || outputCapacity < 200 * 63) return 0;
    if (!runtime.frameReady && !uploadFrame(rgba, byteCount)) return 0;
    const auto features = vision->boardFeatures(runtime.currentFrame);
    std::copy(features.begin(), features.end(), output);
    return static_cast<int>(features.size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_finish(int player, const std::uint8_t* rgba, int byteCount, double timeSeconds,
                    const std::uint8_t* classLabels, int labelCount) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !classLabels || labelCount < 200) return 0;
    if (!runtime.frameReady && !uploadFrame(rgba, byteCount)) return 0;
    std::vector<Cell> labels(200, Cell::Empty);
    for (int i = 0; i < 200; ++i) labels[static_cast<std::size_t>(i)] = labelToCell(classLabels[i]);
    BoardObservation observation = vision->analyzeBoardWithLabels(runtime.currentFrame, labels);
    observation.timeSeconds = timeSeconds;
    boardsForPlayer(player).push_back(std::move(observation));
    return static_cast<int>(boardsForPlayer(player).size());
}

EMSCRIPTEN_KEEPALIVE
int tr_recover() {
    if (runtime.duration <= 0) return fail("Video duration must be set before recovery");
    if (!tr::recoverObservations(runtime.duration, runtime.settings,
                                 runtime.p1Queue, runtime.p2Queue,
                                 runtime.p1Boards, runtime.p2Boards,
                                 runtime.output, runtime.error)) return 0;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const char* tr_review_snapshot() {
    runtime.reviewJson = makeReviewSnapshot();
    return runtime.reviewJson.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* tr_review_candidates(int player, int phase, int useOverride, int lines,
                                 const std::uint16_t* masks, int maskCount) {
    runtime.candidateJson = makeCandidateJson(player, static_cast<std::size_t>(std::max(0, phase)),
                                               useOverride != 0, lines, masks, std::max(0, maskCount));
    return runtime.candidateJson.c_str();
}

EMSCRIPTEN_KEEPALIVE
int tr_review_queue_edit(int player, int sampleIndex, int active, int hold,
                         const int* next, int nextCount) {
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    auto& samples = player == 1 ? runtime.p1Queue : runtime.p2Queue;
    if (sampleIndex < 0 || static_cast<std::size_t>(sampleIndex) >= samples.size()) return fail("Queue sample is out of range");
    if (nextCount < 3 || nextCount > 5) return fail("NEXT must contain 3 to 5 pieces");
    auto& sample = samples[static_cast<std::size_t>(sampleIndex)];
    sample.active = static_cast<Cell>(std::clamp(active, 0, 8));
    sample.observation.hold = static_cast<Cell>(std::clamp(hold, 0, 8));
    sample.observation.next.clear();
    for (int i = 0; i < nextCount; ++i) sample.observation.next.push_back(static_cast<Cell>(std::clamp(next[i], 0, 8)));
    sample.stable = true;
    sample.manuallyEdited = true;
    sample.rejected = false;
    sample.decoded = sample.observation;
    sample.sequenceCorrected = false;
    sample.holdCorrected = false;
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_queue_restore(int player, int sampleIndex) {
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    auto& samples = player == 1 ? runtime.p1Queue : runtime.p2Queue;
    const auto& original = player == 1 ? runtime.output.originalQueueObservationsP1 : runtime.output.originalQueueObservationsP2;
    if (sampleIndex < 0 || static_cast<std::size_t>(sampleIndex) >= samples.size() ||
        static_cast<std::size_t>(sampleIndex) >= original.size()) return fail("Queue sample is out of range");
    samples[static_cast<std::size_t>(sampleIndex)] = original[static_cast<std::size_t>(sampleIndex)];
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_reanalyze() {
    if (!tr::reanalyzeQueueObservations(runtime.settings, runtime.output, runtime.error)) return 0;
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_apply_candidate(int player, int phase, int candidateIndex, int useOverride,
                              int lines, const std::uint16_t* masks, int maskCount) {
    std::optional<tr::GarbageRise> overrideGarbage;
    if (useOverride) {
        tr::GarbageRise rise;
        rise.lines = std::clamp(lines, 0, tr::VisibleRows);
        rise.manuallySpecified = true;
        for (int i = 0; i < maskCount && i < rise.lines; ++i) rise.holeMasks.push_back(masks[i]);
        while (rise.holeMasks.size() < static_cast<std::size_t>(rise.lines)) rise.holeMasks.push_back(1u << 4);
        overrideGarbage = rise;
    }
    if (!tr::applyCorrectionCandidate(runtime.output, player, static_cast<std::size_t>(std::max(0, phase)),
                                      static_cast<std::size_t>(std::max(0, candidateIndex)),
                                      runtime.settings, overrideGarbage, runtime.error)) return 0;
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_restore_automatic(int player, int phase) {
    if (!tr::restoreAutomaticFrom(runtime.output, player, static_cast<std::size_t>(std::max(0, phase)),
                                  runtime.settings, runtime.error)) return 0;
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
double tr_sample_interval() {
    return runtime.settings.sampleIntervalSeconds;
}

EMSCRIPTEN_KEEPALIVE
int tr_onnx_samples() {
    return runtime.settings.onnxSamples;
}

EMSCRIPTEN_KEEPALIVE
int tr_write_output(const char* inputPath, const char* outputDirectory) {
    if (!inputPath || !outputDirectory) {
        return fail("No recovered timeline is available for output");
    }
    runtime.output.humanApproved = true;
    runtime.output.includeSourceVideoInTraining = true;
    if (!tr::writeRecoveredOutput(std::filesystem::path(inputPath),
                                  std::filesystem::path(outputDirectory),
                                  runtime.settings, runtime.output, runtime.error)) return 0;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const char* tr_output_path(int kind) {
    const std::filesystem::path* path = nullptr;
    switch (kind) {
    case 0: path = &runtime.output.jsonPath; break;
    case 1: path = &runtime.output.p1UrlPath; break;
    case 2: path = &runtime.output.p2UrlPath; break;
    case 3: path = &runtime.output.combinedUrlPath; break;
    case 4: path = &runtime.output.linksPath; break;
    case 5: path = &runtime.output.reportPath; break;
    case 6: path = &runtime.output.trainingAnnotationPath; break;
    case 7: path = &runtime.output.trainingManifestPath; break;
    case 8: path = &runtime.output.trainingVideoPath; break;
    default: runtime.outputPath.clear(); return runtime.outputPath.c_str();
    }
    runtime.outputPath = path->u8string();
    return runtime.outputPath.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* tr_output_url(int player) {
    if (player == 1) return runtime.output.p1Url.c_str();
    if (player == 2) return runtime.output.p2Url.c_str();
    if (player == 3) return runtime.output.combinedUrl.c_str();
    return "";
}

EMSCRIPTEN_KEEPALIVE
void tr_copy_error(char* destination, int capacity) {
    copyString(runtime.error, destination, capacity);
}

}

#endif
