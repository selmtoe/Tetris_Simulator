#include "recovery.hpp"
#include "tetris_engine.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfplay.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr int IDC_STATUS = 1001;
constexpr int IDC_PROGRESS = 1002;
constexpr int IDC_DETAILS = 1003;

constexpr int IDC_PLAYER_P1 = 2001;
constexpr int IDC_PLAYER_P2 = 2002;
constexpr int IDC_PHASE_LIST = 2003;
constexpr int IDC_CANDIDATE_GALLERY = 2004;
constexpr int IDC_CLEAR_FILTER = 2005;
constexpr int IDC_APPLY_CANDIDATE = 2006;
constexpr int IDC_RESTORE_AUTOMATIC = 2007;
constexpr int IDC_EXPORT = 2008;
constexpr int IDC_EXIT = 2009;

constexpr int IDC_PHASE_LABEL = 2010;
constexpr int IDC_HEADER = 2011;
constexpr int IDC_PREVIOUS_LABEL = 2012;
constexpr int IDC_OBSERVED_LABEL = 2013;
constexpr int IDC_CANDIDATE_LABEL = 2014;
constexpr int IDC_FILTER_INFO = 2015;
constexpr int IDC_CANDIDATE_INFO = 2016;
constexpr int IDC_GARBAGE_LABEL = 2017;
constexpr int IDC_GARBAGE_LINES = 2018;
constexpr int IDC_GARBAGE_SPIN = 2019;
constexpr int IDC_GARBAGE_AUTO = 2020;
constexpr int IDC_GARBAGE_INFO = 2021;
constexpr int IDC_VIDEO_LABEL = 2022;
constexpr int IDC_VIDEO_INFO = 2023;
constexpr int IDC_VIDEO_PLAY_PAUSE = 2024;
constexpr int IDC_VIDEO_BACK = 2025;
constexpr int IDC_VIDEO_FORWARD = 2026;
constexpr int IDC_VIDEO_PHASE = 2027;
constexpr int IDC_TRAINING_VIDEO = 2028;
constexpr int IDC_FULLSCREEN = 2039;
constexpr int IDC_QUEUE_LOG = 2040;

constexpr int IDC_QUEUE_LOG_LIST = 2201;
constexpr int IDC_QUEUE_LOG_TIME = 2202;
constexpr int IDC_QUEUE_LOG_HOLD = 2203;
constexpr int IDC_QUEUE_LOG_NEXT_1 = 2204;
constexpr int IDC_QUEUE_LOG_NEXT_2 = 2205;
constexpr int IDC_QUEUE_LOG_NEXT_3 = 2206;
constexpr int IDC_QUEUE_LOG_NEXT_4 = 2207;
constexpr int IDC_QUEUE_LOG_NEXT_5 = 2208;
constexpr int IDC_QUEUE_LOG_APPLY_ROW = 2209;
constexpr int IDC_QUEUE_LOG_RESTORE_ROW = 2210;
constexpr int IDC_QUEUE_LOG_REANALYZE = 2211;
constexpr int IDC_QUEUE_LOG_CLOSE = 2212;
constexpr int IDC_QUEUE_LOG_INFO = 2213;
constexpr int IDC_QUEUE_LOG_HOLD_LABEL = 2220;
constexpr int IDC_QUEUE_LOG_NEXT_LABEL_1 = 2221;
constexpr int IDC_QUEUE_LOG_NEXT_LABEL_2 = 2222;
constexpr int IDC_QUEUE_LOG_NEXT_LABEL_3 = 2223;
constexpr int IDC_QUEUE_LOG_NEXT_LABEL_4 = 2224;
constexpr int IDC_QUEUE_LOG_NEXT_LABEL_5 = 2225;
constexpr int IDC_QUEUE_LOG_ACTIVE = 2226;
constexpr int IDC_QUEUE_LOG_ACTIVE_LABEL = 2227;
constexpr int IDC_QUEUE_LOG_VIDEO_INFO = 2228;
constexpr int IDC_QUEUE_LOG_COPY_SELECTED = 2229;
constexpr int IDC_QUEUE_LOG_COPY_ALL = 2230;

constexpr int IDC_PREVIOUS_BOARD = 2101;
constexpr int IDC_OBSERVED_BOARD = 2102;
constexpr int IDC_CANDIDATE_BOARD = 2103;
constexpr int IDC_GARBAGE_BOARD = 2104;
constexpr int IDC_VIDEO_SURFACE = 2105;

constexpr wchar_t ApplicationClass[] = L"TetrisVideoRecoveryWindow";
constexpr wchar_t BoardClass[] = L"TetrisVideoRecoveryBoard";
constexpr wchar_t CandidateGalleryClass[] = L"TetrisVideoRecoveryCandidateGallery";
constexpr wchar_t QueueLogClass[] = L"TetrisVideoRecoveryQueueLog";

tr::Status g_status;
tr::Settings g_settings;
tr::RecoveryOutput g_output;
std::filesystem::path g_input;
std::filesystem::path g_outputDir;
std::thread g_worker;
bool g_analysisHandled = false;
bool g_reviewActive = false;
bool g_fullscreen = false;
LONG g_windowedStyle = 0;
LONG g_windowedExStyle = 0;
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};

HWND g_analysisStatus = nullptr;
HWND g_analysisProgress = nullptr;
HWND g_analysisDetails = nullptr;
HWND g_mainWindow = nullptr;

struct BoardViewState {
    HWND window = nullptr;
    tr::Board board{};
    // A temporary, colored placement sketch drawn over `board`.  It is never
    // committed until the user chooses a filtered legal candidate.
    std::array<tr::Cell, tr::BoardWidth * tr::BoardHeight> overlay{};
    std::array<bool, tr::BoardWidth * tr::BoardHeight> marked{};
    bool filtersOnClick = false;
    bool placementPaintOnClick = false;
    bool garbageHolesOnClick = false;
};

BoardViewState g_previousBoard;
BoardViewState g_observedBoard;
BoardViewState g_candidateBoard;
BoardViewState g_garbageBoard;

struct CandidateGalleryState {
    HWND window = nullptr;
    int scrollY = 0;
};

CandidateGalleryState g_candidateGallery;

constexpr UINT WM_MEDIA_READY = WM_APP + 2;

class VideoCallback final : public IMFPMediaPlayerCallback {
public:
    explicit VideoCallback(HWND owner) : owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IMFPMediaPlayerCallback)) {
            *value = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *value = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override {
        if (eventHeader && eventHeader->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET && SUCCEEDED(eventHeader->hrEvent)) {
            PostMessageW(owner_, WM_MEDIA_READY, 0, 0);
        }
    }

private:
    std::atomic<ULONG> references_{1};
    HWND owner_ = nullptr;
};

struct VideoPreviewState {
    HWND surface = nullptr;
    IMFPMediaPlayer* player = nullptr;
    VideoCallback* callback = nullptr;
    bool ready = false;
    bool playing = false;
    double requestedSeconds = 0;
};

VideoPreviewState g_video;

struct ReviewUi {
    HWND header = nullptr;
    HWND playerP1 = nullptr;
    HWND playerP2 = nullptr;
    HWND phaseLabel = nullptr;
    HWND phaseList = nullptr;
    HWND previousLabel = nullptr;
    HWND observedLabel = nullptr;
    HWND candidateLabel = nullptr;
    HWND filterInfo = nullptr;
    HWND candidateInfo = nullptr;
    HWND candidateGallery = nullptr;
    HWND clearFilter = nullptr;
    HWND garbageLabel = nullptr;
    HWND garbageLines = nullptr;
    HWND garbageSpin = nullptr;
    HWND garbageAuto = nullptr;
    HWND garbageInfo = nullptr;
    HWND videoLabel = nullptr;
    HWND videoInfo = nullptr;
    HWND videoPlayPause = nullptr;
    HWND videoBack = nullptr;
    HWND videoForward = nullptr;
    HWND videoPhase = nullptr;
    HWND applyCandidate = nullptr;
    HWND restoreAutomatic = nullptr;
    HWND trainingVideo = nullptr;
    HWND queueLog = nullptr;
    HWND fullscreen = nullptr;
    HWND exportButton = nullptr;
    HWND exitButton = nullptr;
    int player = 0;
    std::size_t phase = 0;
    std::vector<tr::CorrectionCandidate> candidates;
    std::vector<std::size_t> visibleCandidates;
    int selectedCandidate = -1;
    std::vector<std::array<int, 2>> requiredCells;
    std::vector<std::array<int, 2>> placementCells;
    tr::GarbageRise garbageEditor;
    bool garbageOverrideEnabled = false;
    bool updatingGarbageControls = false;
    bool updatingQueueControls = false;
};

ReviewUi g_review;

struct QueueLogUi {
    HWND window = nullptr;
    HWND list = nullptr;
    HWND time = nullptr;
    HWND info = nullptr;
    HWND videoInfo = nullptr;
    HWND active = nullptr;
    HWND hold = nullptr;
    std::array<HWND, 5> next{};
    HWND applyRow = nullptr;
    HWND restoreRow = nullptr;
    HWND copySelected = nullptr;
    HWND copyAll = nullptr;
    HWND reanalyze = nullptr;
    HWND close = nullptr;
    int player = 0;
    std::size_t selected = 0;
    bool updating = false;
};

QueueLogUi g_queueLog;

void setText(HWND window, const std::wstring& value) {
    if (window) SetWindowTextW(window, value.c_str());
}

void useDefaultFont(HWND window) {
    if (window) SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"(message conversion failed)";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring cellText(tr::Cell cell) {
    if (!tr::isPiece(cell)) return L"-";
    return std::wstring(1, static_cast<wchar_t>(tr::cellChar(cell)));
}

std::wstring piecesText(const std::vector<tr::Cell>& pieces) {
    std::wstring result;
    for (const tr::Cell piece : pieces) {
        if (tr::isPiece(piece)) result.push_back(static_cast<wchar_t>(tr::cellChar(piece)));
    }
    return result.empty() ? L"-" : result;
}

std::wstring queueCellText(tr::Cell cell) {
    return tr::isPiece(cell) ? cellText(cell) : L"-";
}

tr::Cell queueComboCell(HWND combo) {
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected <= 0 || selected > static_cast<LRESULT>(tr::Pieces.size())) return tr::Cell::Empty;
    return tr::Pieces[static_cast<std::size_t>(selected - 1)];
}

void setQueueComboCell(HWND combo, tr::Cell cell) {
    const int selected = tr::isPiece(cell) ? 1 + static_cast<int>(std::find(tr::Pieces.begin(), tr::Pieces.end(), cell) - tr::Pieces.begin()) : 0;
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

void populateQueueCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"-"));
    for (tr::Cell piece : tr::Pieces) {
        const std::wstring text = cellText(piece);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

COLORREF cellColor(tr::Cell cell) {
    switch (cell) {
    case tr::Cell::I: return RGB(0, 220, 230);
    case tr::Cell::O: return RGB(238, 205, 0);
    case tr::Cell::T: return RGB(170, 70, 220);
    case tr::Cell::L: return RGB(238, 135, 20);
    case tr::Cell::J: return RGB(45, 90, 225);
    case tr::Cell::S: return RGB(70, 195, 70);
    case tr::Cell::Z: return RGB(220, 60, 55);
    case tr::Cell::Garbage: return RGB(128, 134, 142);
    default: return RGB(23, 28, 38);
    }
}

constexpr std::uint16_t BoardHoleMask = static_cast<std::uint16_t>((1u << tr::BoardWidth) - 1u);

std::uint16_t oneHoleMask(std::uint16_t mask) {
    const std::uint16_t candidates = static_cast<std::uint16_t>(mask & BoardHoleMask);
    int selected = 4;
    int bestDistance = tr::BoardWidth + 1;
    for (int x = 0; x < tr::BoardWidth; ++x) {
        if ((candidates & (1u << x)) == 0) continue;
        const int distance = std::abs(x - 4);
        if (distance < bestDistance) {
            selected = x;
            bestDistance = distance;
        }
    }
    return static_cast<std::uint16_t>(1u << selected);
}

void paintVisibleBoard(HDC dc, const RECT& bounds, const tr::Board& board,
                       const std::array<bool, tr::BoardWidth * tr::BoardHeight>* marked = nullptr,
                       const std::array<tr::Cell, tr::BoardWidth * tr::BoardHeight>* overlay = nullptr) {
    constexpr int padding = 5;
    const int innerWidth = std::max(1, static_cast<int>(bounds.right - bounds.left) - padding * 2);
    const int innerHeight = std::max(1, static_cast<int>(bounds.bottom - bounds.top) - padding * 2);
    for (int y = 0; y < tr::VisibleRows; ++y) {
        for (int x = 0; x < tr::BoardWidth; ++x) {
            RECT cell{
                bounds.left + padding + x * innerWidth / tr::BoardWidth,
                bounds.top + padding + y * innerHeight / tr::VisibleRows,
                bounds.left + padding + (x + 1) * innerWidth / tr::BoardWidth,
                bounds.top + padding + (y + 1) * innerHeight / tr::VisibleRows,
            };
            const int boardY = y + tr::VisibleRows;
            const tr::Cell sketch = overlay ? (*overlay)[tr::index(x, boardY)] : tr::Cell::Empty;
            const tr::Cell value = tr::isPiece(sketch) ? sketch : board[tr::index(x, boardY)];
            HBRUSH fill = CreateSolidBrush(cellColor(value));
            FillRect(dc, &cell, fill);
            DeleteObject(fill);
            HBRUSH grid = CreateSolidBrush(RGB(52, 61, 78));
            FrameRect(dc, &cell, grid);
            DeleteObject(grid);
            if (marked && (*marked)[tr::index(x, boardY)]) {
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 224, 68));
                const HGDIOBJ oldPen = SelectObject(dc, pen);
                const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(dc, cell.left + 1, cell.top + 1, cell.right - 1, cell.bottom - 1);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            }
        }
    }
}

void updateBoardWindow(BoardViewState& view) {
    if (view.window) InvalidateRect(view.window, nullptr, FALSE);
}

std::vector<tr::TimelineStep>& reviewRaw() {
    return g_review.player == 0 ? g_output.rawP1 : g_output.rawP2;
}

std::vector<tr::TimelineStep>& reviewSolved() {
    return g_review.player == 0 ? g_output.p1 : g_output.p2;
}

const wchar_t* playerName() {
    return g_review.player == 0 ? L"P1" : L"P2";
}

bool candidateMatchesFilter(const tr::CorrectionCandidate& candidate) {
    for (const auto& cell : g_review.requiredCells) {
        const tr::Cell value = candidate.move.board[tr::index(cell[0], cell[1])];
        // Match 動画解析.html's filter: the selected square must be a colored
        // mino, not merely a garbage block.
        if (!tr::isPiece(value)) return false;
    }
    // The placement sketch is stricter than the ONNX-board filter: each
    // painted square must be one of the four cells placed by this exact legal
    // move.  Four painted cells therefore identify a landing unambiguously
    // whenever the legacy generator contains a single matching candidate.
    for (const auto& requested : g_review.placementCells) {
        bool contains = false;
        for (int i = 0; i < candidate.move.cellCount; ++i) {
            if (candidate.move.cells[static_cast<std::size_t>(i)][0] == requested[0] &&
                candidate.move.cells[static_cast<std::size_t>(i)][1] == requested[1]) {
                contains = true;
                break;
            }
        }
        if (!contains) return false;
    }
    return true;
}

std::wstring phaseListText(std::size_t index, const std::vector<tr::TimelineStep>& raw) {
    const auto& step = raw[index];
    std::wostringstream out;
    out << L'#' << std::setw(2) << std::setfill(L'0') << index << std::setfill(L' ')
        << L"  " << std::fixed << std::setprecision(3) << step.timeSeconds << L" s  ";
    if (index == 0) {
        out << L"初期盤面";
    } else {
        out << L"設置候補: " << cellText(raw[index - 1].piece)
            << L"  / " << widen(step.action.empty() ? "phase" : step.action);
    }
    if (step.manuallyFixed) out << L"  [盤面修正]";
    if (step.queueManuallyFixed) out << L"  [キュー修正]";
    return out.str();
}

std::wstring candidateListText(std::size_t sourceIndex, const tr::CorrectionCandidate& candidate) {
    const auto& move = candidate.move;
    std::wostringstream out;
    out << L'#' << std::setw(3) << std::setfill(L'0') << sourceIndex + 1 << std::setfill(L' ')
        << L"  " << cellText(move.piece)
        << L"  x=" << move.x << L"  y=" << move.y << L"  回転=" << move.rotation
        << L"  消去=" << move.clearedLines
        << L"  一致=" << std::fixed << std::setprecision(0) << candidate.observationScore;
    return out.str();
}

void refreshBoardViews();
void refreshCandidates(bool preferFirst);
void selectPhase(std::size_t index);
void refreshGarbagePatternView();
void selectCandidateIndex(int candidateIndex);
void toggleGarbageHoleCell(int x, int y);
void togglePlacementCell(int x, int y);
void syncVideoToSelectedPhase(bool play);
void createQueueLogWindow(HWND owner);
void rebuildPhaseList();
void seekVideo(double seconds, bool play);
void updateQueueLogVideoInfo();
void layoutReview(HWND window);
void toggleFullscreen(HWND window);

void toggleFilterCell(int x, int y) {
    if (!g_reviewActive || x < 0 || x >= tr::BoardWidth || y < tr::VisibleRows || y >= tr::BoardHeight) return;
    const auto found = std::find_if(g_review.requiredCells.begin(), g_review.requiredCells.end(), [=](const auto& cell) {
        return cell[0] == x && cell[1] == y;
    });
    if (found == g_review.requiredCells.end()) g_review.requiredCells.push_back({x, y});
    else g_review.requiredCells.erase(found);
    g_review.selectedCandidate = -1;
    refreshCandidates(true);
}

tr::Cell placementPieceForCurrentPhase() {
    const auto& raw = reviewRaw();
    if (g_review.phase == 0 || g_review.phase >= raw.size()) return tr::Cell::Empty;
    const auto& previous = raw[g_review.phase - 1];
    if (tr::isPiece(previous.piece)) return previous.piece;
    if (!previous.next.empty() && tr::isPiece(previous.next.front())) return previous.next.front();
    return tr::Cell::Empty;
}

void togglePlacementCell(int x, int y) {
    if (!g_reviewActive || x < 0 || x >= tr::BoardWidth || y < tr::VisibleRows || y >= tr::BoardHeight) return;
    if (!tr::isPiece(placementPieceForCurrentPhase())) return;
    const auto found = std::find_if(g_review.placementCells.begin(), g_review.placementCells.end(), [=](const auto& cell) {
        return cell[0] == x && cell[1] == y;
    });
    if (found != g_review.placementCells.end()) {
        g_review.placementCells.erase(found);
    } else {
        // A tetromino has four cells.  Keeping this cap means an accidental
        // fifth click cannot make the interaction look like a broken filter.
        if (g_review.placementCells.size() >= 4) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        g_review.placementCells.push_back({x, y});
    }
    g_review.selectedCandidate = -1;
    refreshCandidates(true);
}

tr::GarbageRise automaticGarbageForCurrentPhase() {
    tr::GarbageRise rise;
    const auto& raw = reviewRaw();
    const auto& solved = reviewSolved();
    if (g_review.phase == 0 || g_review.phase >= raw.size() || g_review.phase - 1 >= solved.size()) return rise;
    const auto& current = raw[g_review.phase];
    if (current.manuallyFixed && current.garbage.manuallySpecified) return current.garbage;
    const tr::BoardShift shift = tr::TetrisEngine::detectBoardShift(solved[g_review.phase - 1].board,
                                                                      current.observed, g_settings.shiftThreshold);
    rise.lines = shift.lines;
    rise.holeMasks = tr::TetrisEngine::garbageHolesFromObserved(current.observed, rise.lines);
    rise.matchRatio = shift.ratio;
    return rise;
}

void normalizeGarbageEditor(tr::GarbageRise& rise) {
    rise.lines = std::clamp(rise.lines, 0, tr::VisibleRows);
    if (rise.holeMasks.size() > static_cast<std::size_t>(rise.lines)) rise.holeMasks.resize(rise.lines);
    while (rise.holeMasks.size() < static_cast<std::size_t>(rise.lines)) rise.holeMasks.push_back(static_cast<std::uint16_t>(1u << 4));
    for (auto& mask : rise.holeMasks) {
        // Garbage mode is deliberately one-hole-per-row.  If ONNX exposed
        // several apparent gaps, retain one deterministic hole until the
        // reviewer selects the exact column.
        mask = oneHoleMask(mask);
    }
}

void setGarbageLinesText() {
    if (!g_review.garbageLines) return;
    g_review.updatingGarbageControls = true;
    setText(g_review.garbageLines, std::to_wstring(g_review.garbageEditor.lines));
    g_review.updatingGarbageControls = false;
}

void refreshGarbagePatternView() {
    if (!g_reviewActive) return;
    normalizeGarbageEditor(g_review.garbageEditor);
    g_garbageBoard.board = {};
    g_garbageBoard.marked.fill(false);
    for (int row = 0; row < g_review.garbageEditor.lines; ++row) {
        const int y = tr::BoardHeight - g_review.garbageEditor.lines + row;
        const std::uint16_t holes = g_review.garbageEditor.holeMasks[static_cast<std::size_t>(row)];
        for (int x = 0; x < tr::BoardWidth; ++x) {
            const bool hole = (holes & (1u << x)) != 0;
            g_garbageBoard.board[tr::index(x, y)] = hole ? tr::Cell::Empty : tr::Cell::Garbage;
            g_garbageBoard.marked[tr::index(x, y)] = hole;
        }
    }
    updateBoardWindow(g_garbageBoard);
    if (g_review.garbageInfo) {
        std::wostringstream info;
        info << (g_review.garbageOverrideEnabled ? L"手動設定" : L"自動検出の参考値")
             << L": " << g_review.garbageEditor.lines << L" 段";
        if (g_review.garbageEditor.lines > 0) {
            info << L" / 黄色が穴（クリックした列だけを穴にする）";
            if (!g_review.garbageOverrideEnabled) info << L" / 編集すると手動設定へ";
        } else {
            info << L" / 0なら下穴上昇なし";
        }
        setText(g_review.garbageInfo, info.str());
    }
    setGarbageLinesText();
}

void initializeGarbageEditorForPhase() {
    const auto& raw = reviewRaw();
    if (g_review.phase < raw.size() && raw[g_review.phase].manuallyFixed && raw[g_review.phase].garbage.manuallySpecified) {
        g_review.garbageEditor = raw[g_review.phase].garbage;
        g_review.garbageOverrideEnabled = true;
    } else {
        g_review.garbageEditor = automaticGarbageForCurrentPhase();
        g_review.garbageOverrideEnabled = false;
    }
    normalizeGarbageEditor(g_review.garbageEditor);
    refreshGarbagePatternView();
}

void toggleGarbageHoleCell(int x, int y) {
    const int first = tr::BoardHeight - g_review.garbageEditor.lines;
    if (!g_reviewActive || g_review.phase == 0 || y < first || y >= tr::BoardHeight || x < 0 || x >= tr::BoardWidth) return;
    const std::size_t row = static_cast<std::size_t>(y - first);
    if (row >= g_review.garbageEditor.holeMasks.size()) return;
    g_review.garbageEditor.manuallySpecified = true;
    g_review.garbageOverrideEnabled = true;
    // One selected hole closes every other gap in the same garbage row.
    g_review.garbageEditor.holeMasks[row] = static_cast<std::uint16_t>(1u << x);
    g_review.placementCells.clear();
    refreshGarbagePatternView();
    refreshCandidates(true);
}

LRESULT CALLBACK boardWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<BoardViewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<BoardViewState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state) state->window = window;
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(14, 17, 24));
        FillRect(dc, &client, background);
        DeleteObject(background);
        if (state) paintVisibleBoard(dc, client, state->board, &state->marked, &state->overlay);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state && (state->filtersOnClick || state->placementPaintOnClick || state->garbageHolesOnClick) && g_reviewActive) {
            RECT client{};
            GetClientRect(window, &client);
            constexpr int padding = 5;
            const int innerWidth = std::max(1, static_cast<int>(client.right - client.left) - padding * 2);
            const int innerHeight = std::max(1, static_cast<int>(client.bottom - client.top) - padding * 2);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int column = (x - padding) * tr::BoardWidth / innerWidth;
            const int row = (y - padding) * tr::VisibleRows / innerHeight;
            if (column >= 0 && column < tr::BoardWidth && row >= 0 && row < tr::VisibleRows) {
                if (state->placementPaintOnClick) togglePlacementCell(column, row + tr::VisibleRows);
                else if (state->filtersOnClick) toggleFilterCell(column, row + tr::VisibleRows);
                else if (state->garbageHolesOnClick) toggleGarbageHoleCell(column, row + tr::VisibleRows);
            }
        }
        return 0;
    case WM_NCDESTROY:
        if (state) state->window = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct GalleryMetrics {
    int padding = 8;
    int gap = 8;
    int columns = 1;
    int cardWidth = 110;
    int cardHeight = 250;
    int contentHeight = 0;
};

GalleryMetrics galleryMetrics(const RECT& client) {
    GalleryMetrics metrics;
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    metrics.columns = std::max(2, (width - metrics.padding) / 128);
    metrics.cardWidth = std::max(94, (width - metrics.padding * 2 - metrics.gap * (metrics.columns - 1)) / metrics.columns);
    const int boardWidth = std::max(70, metrics.cardWidth - 12);
    metrics.cardHeight = boardWidth * 2 + 40;
    const int rows = static_cast<int>((g_review.visibleCandidates.size() + static_cast<std::size_t>(metrics.columns) - 1) /
                                      static_cast<std::size_t>(metrics.columns));
    metrics.contentHeight = metrics.padding * 2 + std::max(0, rows * (metrics.cardHeight + metrics.gap) - metrics.gap);
    return metrics;
}

void updateGalleryScroll(HWND window, CandidateGalleryState& state, int contentHeight, int pageHeight) {
    const int maxScroll = std::max(0, contentHeight - pageHeight);
    state.scrollY = std::clamp(state.scrollY, 0, maxScroll);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = std::max(0, contentHeight - 1);
    info.nPage = static_cast<UINT>(std::max(1, pageHeight));
    info.nPos = state.scrollY;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}

void scrollGallery(HWND window, CandidateGalleryState& state, int requested) {
    RECT client{};
    GetClientRect(window, &client);
    const GalleryMetrics metrics = galleryMetrics(client);
    const int page = std::max(1, static_cast<int>(client.bottom - client.top));
    const int maxScroll = std::max(0, metrics.contentHeight - page);
    const int next = std::clamp(requested, 0, maxScroll);
    if (next == state.scrollY) return;
    state.scrollY = next;
    SetScrollPos(window, SB_VERT, state.scrollY, TRUE);
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK candidateGalleryWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<CandidateGalleryState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<CandidateGalleryState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (state) state->window = window;
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (state) {
            RECT client{};
            GetClientRect(window, &client);
            updateGalleryScroll(window, *state, galleryMetrics(client).contentHeight, static_cast<int>(client.bottom - client.top));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(14, 17, 24));
        FillRect(dc, &client, background);
        DeleteObject(background);
        if (state) {
            const GalleryMetrics metrics = galleryMetrics(client);
            updateGalleryScroll(window, *state, metrics.contentHeight, static_cast<int>(client.bottom - client.top));
            const HGDIOBJ oldFont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, TRANSPARENT);
            for (std::size_t viewIndex = 0; viewIndex < g_review.visibleCandidates.size(); ++viewIndex) {
                const int column = static_cast<int>(viewIndex % static_cast<std::size_t>(metrics.columns));
                const int row = static_cast<int>(viewIndex / static_cast<std::size_t>(metrics.columns));
                const int left = metrics.padding + column * (metrics.cardWidth + metrics.gap);
                const int top = metrics.padding + row * (metrics.cardHeight + metrics.gap) - state->scrollY;
                const RECT card{left, top, left + metrics.cardWidth, top + metrics.cardHeight};
                if (card.bottom < 0 || card.top > client.bottom) continue;
                const int candidateIndex = static_cast<int>(g_review.visibleCandidates[viewIndex]);
                const bool selected = candidateIndex == g_review.selectedCandidate;
                HBRUSH cardBrush = CreateSolidBrush(selected ? RGB(48, 68, 110) : RGB(28, 34, 47));
                FillRect(dc, &card, cardBrush);
                DeleteObject(cardBrush);
                HPEN border = CreatePen(PS_SOLID, selected ? 3 : 1, selected ? RGB(255, 224, 68) : RGB(82, 95, 121));
                const HGDIOBJ oldPen = SelectObject(dc, border);
                const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                Rectangle(dc, card.left, card.top, card.right, card.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(border);

                const auto& candidate = g_review.candidates[static_cast<std::size_t>(candidateIndex)];
                std::wostringstream label;
                label << L'#' << (candidateIndex + 1) << L' ' << cellText(candidate.move.piece)
                      << L" r" << candidate.move.rotation << L" x" << candidate.move.x;
                SetTextColor(dc, RGB(238, 242, 248));
                RECT labelRect{card.left + 4, card.top + 3, card.right - 4, card.top + 20};
                DrawTextW(dc, label.str().c_str(), -1, &labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                const RECT boardRect{card.left + 6, card.top + 22, card.right - 6, card.bottom - 15};
                paintVisibleBoard(dc, boardRect, candidate.move.fullBoard);
                std::wostringstream footer;
                footer << L"clear " << candidate.move.clearedLines;
                RECT footerRect{card.left + 4, card.bottom - 15, card.right - 4, card.bottom - 2};
                SetTextColor(dc, RGB(187, 201, 220));
                DrawTextW(dc, footer.str().c_str(), -1, &footerRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
            if (g_review.visibleCandidates.empty()) {
                SetTextColor(dc, RGB(220, 225, 235));
                RECT label{12, 12, client.right - 12, 52};
                DrawTextW(dc, L"この条件に一致する合法手はありません", -1, &label, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            }
            SelectObject(dc, oldFont);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state && g_reviewActive) {
            RECT client{};
            GetClientRect(window, &client);
            const GalleryMetrics metrics = galleryMetrics(client);
            const int x = GET_X_LPARAM(lParam) - metrics.padding;
            const int y = GET_Y_LPARAM(lParam) + state->scrollY - metrics.padding;
            if (x >= 0 && y >= 0) {
                const int column = x / (metrics.cardWidth + metrics.gap);
                const int row = y / (metrics.cardHeight + metrics.gap);
                if (column >= 0 && column < metrics.columns && x % (metrics.cardWidth + metrics.gap) < metrics.cardWidth &&
                    y % (metrics.cardHeight + metrics.gap) < metrics.cardHeight) {
                    const std::size_t viewIndex = static_cast<std::size_t>(row * metrics.columns + column);
                    if (viewIndex < g_review.visibleCandidates.size()) selectCandidateIndex(static_cast<int>(g_review.visibleCandidates[viewIndex]));
                }
            }
        }
        return 0;
    case WM_VSCROLL:
        if (state) {
            RECT client{};
            GetClientRect(window, &client);
            const int page = std::max(1, static_cast<int>(client.bottom - client.top));
            int requested = state->scrollY;
            switch (LOWORD(wParam)) {
            case SB_LINEUP: requested -= 40; break;
            case SB_LINEDOWN: requested += 40; break;
            case SB_PAGEUP: requested -= page - 30; break;
            case SB_PAGEDOWN: requested += page - 30; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: requested = HIWORD(wParam); break;
            case SB_TOP: requested = 0; break;
            case SB_BOTTOM: requested = galleryMetrics(client).contentHeight; break;
            default: break;
            }
            scrollGallery(window, *state, requested);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state) scrollGallery(window, *state, state->scrollY - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 80);
        return 0;
    case WM_NCDESTROY:
        if (state) state->window = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void refreshBoardViews() {
    if (!g_reviewActive) return;
    const auto& raw = reviewRaw();
    const auto& solved = reviewSolved();
    if (raw.empty() || g_review.phase >= raw.size()) return;

    g_previousBoard.board = g_review.phase > 0 && g_review.phase - 1 < solved.size()
        ? tr::TetrisEngine::applyGarbageRise(solved[g_review.phase - 1].board, g_review.garbageEditor)
        : tr::Board{};
    g_previousBoard.overlay.fill(tr::Cell::Empty);
    g_previousBoard.marked.fill(false);
    const tr::Cell placementPiece = placementPieceForCurrentPhase();
    for (const auto& cell : g_review.placementCells) {
        if (cell[0] >= 0 && cell[0] < tr::BoardWidth && cell[1] >= tr::VisibleRows && cell[1] < tr::BoardHeight) {
            const int boardIndex = tr::index(cell[0], cell[1]);
            g_previousBoard.overlay[boardIndex] = placementPiece;
            g_previousBoard.marked[boardIndex] = true;
        }
    }
    g_observedBoard.board = raw[g_review.phase].observed;
    g_observedBoard.marked.fill(false);
    for (const auto& cell : g_review.requiredCells) {
        if (cell[0] >= 0 && cell[0] < tr::BoardWidth && cell[1] >= tr::VisibleRows && cell[1] < tr::BoardHeight) {
            g_observedBoard.marked[tr::index(cell[0], cell[1])] = true;
        }
    }

    if (g_review.selectedCandidate >= 0 &&
        static_cast<std::size_t>(g_review.selectedCandidate) < g_review.candidates.size()) {
        g_candidateBoard.board = g_review.candidates[static_cast<std::size_t>(g_review.selectedCandidate)].move.fullBoard;
    } else if (g_review.phase < solved.size()) {
        g_candidateBoard.board = solved[g_review.phase].fullBoard;
    } else {
        g_candidateBoard.board = {};
    }
    g_candidateBoard.marked.fill(false);
    g_candidateBoard.overlay.fill(tr::Cell::Empty);
    updateBoardWindow(g_previousBoard);
    updateBoardWindow(g_observedBoard);
    updateBoardWindow(g_candidateBoard);

    std::wostringstream header;
    header << playerName() << L"  / 局面 #" << g_review.phase;
    if (g_review.phase == 0) {
        header << L"  初期盤面（ここは修正対象外）";
    } else {
        const auto& previous = raw[g_review.phase - 1];
        const auto& current = raw[g_review.phase];
        header << L"  " << std::fixed << std::setprecision(3) << current.timeSeconds << L" 秒"
               << L"  前局面の active=" << cellText(previous.piece)
               << L"  next=" << piecesText(previous.next);
        if (current.manuallyFixed) header << L"  [この局面は手動修正済]";
    }
    setText(g_review.header, header.str());
    std::wstring previousLabel = L"前局面（確定）";
    if (tr::isPiece(placementPiece)) {
        previousLabel += L" + " + cellText(placementPiece) + L"を描いて絞込";
    }
    setText(g_review.previousLabel, previousLabel);
    setText(g_review.observedLabel, L"ONNX観測（クリックで絞込）");

    if (g_review.selectedCandidate >= 0 &&
        static_cast<std::size_t>(g_review.selectedCandidate) < g_review.candidates.size()) {
        const auto& selected = g_review.candidates[static_cast<std::size_t>(g_review.selectedCandidate)];
        std::wostringstream info;
        info << L"選択候補: " << cellText(selected.move.piece)
             << L"  x=" << selected.move.x << L" / y=" << selected.move.y
             << L" / 回転=" << selected.move.rotation
             << L" / ライン消去=" << selected.move.clearedLines
             << L" / 観測一致=" << std::fixed << std::setprecision(0) << selected.observationScore
             << L" / 下穴上昇=" << selected.garbage.lines;
        if (selected.garbage.manuallySpecified) info << L"（手動）";
        setText(g_review.candidateLabel, L"選択候補（消去前）");
        setText(g_review.candidateInfo, info.str());
    } else {
        setText(g_review.candidateLabel, L"自動候補（消去前）");
        setText(g_review.candidateInfo, L"候補を選択すると、ここにその着地盤面を表示します。");
    }

    std::wostringstream filter;
    filter << L"合法候補 " << g_review.visibleCandidates.size() << L" / " << g_review.candidates.size() << L" 手（盤面カードをクリックして選択）";
    if (g_review.requiredCells.empty() && g_review.placementCells.empty()) {
        filter << L"  （絞り込みなし）";
    }
    if (!g_review.requiredCells.empty()) {
        filter << L"  必須セル:";
        for (const auto& cell : g_review.requiredCells) filter << L" (" << cell[0] << L',' << (cell[1] - tr::VisibleRows) << L')';
    }
    if (!g_review.placementCells.empty()) {
        filter << L"  " << cellText(placementPiece) << L"配置:";
        for (const auto& cell : g_review.placementCells) filter << L" (" << cell[0] << L',' << (cell[1] - tr::VisibleRows) << L')';
        filter << L" " << g_review.placementCells.size() << L"/4";
    }
    setText(g_review.filterInfo, filter.str());
    const bool canApply = g_review.phase > 0 && g_review.selectedCandidate >= 0;
    EnableWindow(g_review.applyCandidate, canApply ? TRUE : FALSE);
    EnableWindow(g_review.restoreAutomatic, g_review.phase > 0 ? TRUE : FALSE);
    const bool hasRawQueueLog = g_review.player == 0
        ? !g_output.queueObservationsP1.empty()
        : !g_output.queueObservationsP2.empty();
    EnableWindow(g_review.queueLog, hasRawQueueLog ? TRUE : FALSE);
}

void refreshCandidates(bool preferFirst) {
    if (!g_reviewActive) return;
    const auto& raw = reviewRaw();
    const auto& solved = reviewSolved();
    g_review.candidates.clear();
    g_review.visibleCandidates.clear();
    const int previousSelection = g_review.selectedCandidate;
    g_review.selectedCandidate = -1;
    if (g_review.phase > 0 && g_review.phase < raw.size() && solved.size() == raw.size()) {
        const std::optional<tr::GarbageRise> overrideGarbage = g_review.garbageOverrideEnabled
            ? std::optional<tr::GarbageRise>(g_review.garbageEditor) : std::nullopt;
        g_review.candidates = tr::TetrisEngine::correctionCandidates(raw, solved, g_review.phase, g_settings, overrideGarbage);
    }

    for (std::size_t i = 0; i < g_review.candidates.size(); ++i) {
        if (!candidateMatchesFilter(g_review.candidates[i])) continue;
        g_review.visibleCandidates.push_back(i);
    }
    if (previousSelection >= 0 && static_cast<std::size_t>(previousSelection) < g_review.candidates.size() &&
        std::find(g_review.visibleCandidates.begin(), g_review.visibleCandidates.end(), static_cast<std::size_t>(previousSelection)) != g_review.visibleCandidates.end()) {
        g_review.selectedCandidate = previousSelection;
    } else if (preferFirst && !g_review.visibleCandidates.empty()) {
        g_review.selectedCandidate = static_cast<int>(g_review.visibleCandidates.front());
    }
    g_candidateGallery.scrollY = 0;
    if (g_candidateGallery.window) InvalidateRect(g_candidateGallery.window, nullptr, FALSE);
    refreshBoardViews();
}

void selectCandidateIndex(int candidateIndex) {
    if (candidateIndex < 0 || static_cast<std::size_t>(candidateIndex) >= g_review.candidates.size()) return;
    g_review.selectedCandidate = candidateIndex;
    if (g_candidateGallery.window) InvalidateRect(g_candidateGallery.window, nullptr, FALSE);
    refreshBoardViews();
}

void rebuildPhaseList() {
    const auto& raw = reviewRaw();
    SendMessageW(g_review.phaseList, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const int row = static_cast<int>(SendMessageW(g_review.phaseList, LB_ADDSTRING, 0,
                                                       reinterpret_cast<LPARAM>(phaseListText(i, raw).c_str())));
        SendMessageW(g_review.phaseList, LB_SETITEMDATA, row, static_cast<LPARAM>(i));
    }
    if (!raw.empty()) {
        g_review.phase = std::min(g_review.phase, raw.size() - 1);
        SendMessageW(g_review.phaseList, LB_SETCURSEL, static_cast<WPARAM>(g_review.phase), 0);
    }
}

void selectPhase(std::size_t index) {
    const auto& raw = reviewRaw();
    if (raw.empty()) return;
    g_review.phase = std::min(index, raw.size() - 1);
    g_review.requiredCells.clear();
    g_review.placementCells.clear();
    g_review.selectedCandidate = -1;
    SendMessageW(g_review.phaseList, LB_SETCURSEL, static_cast<WPARAM>(g_review.phase), 0);
    initializeGarbageEditorForPhase();
    refreshCandidates(true);
    syncVideoToSelectedPhase(false);
}

void resetManualFrom(std::vector<tr::TimelineStep>& raw, std::size_t first) {
    for (std::size_t index = first; index < raw.size(); ++index) {
        raw[index].manuallyFixed = false;
        raw[index].board = raw[index].observed;
        raw[index].fullBoard = raw[index].observed;
        raw[index].garbage = {};
        raw[index].placedPiece = tr::Cell::Empty;
        raw[index].placementX = 0;
        raw[index].placementY = 0;
        raw[index].placementRotation = 0;
        raw[index].clearedLines = 0;
        raw[index].score = 0;
    }
}

bool laterManualCorrectionExists(const std::vector<tr::TimelineStep>& raw, std::size_t index) {
    return std::any_of(raw.begin() + static_cast<std::ptrdiff_t>(std::min(index + 1, raw.size())), raw.end(),
                       [](const tr::TimelineStep& step) { return step.manuallyFixed; });
}

void applySelectedCandidate(HWND window) {
    if (g_review.phase == 0 || g_review.selectedCandidate < 0 ||
        static_cast<std::size_t>(g_review.selectedCandidate) >= g_review.candidates.size()) return;
    auto& raw = reviewRaw();
    auto& solved = reviewSolved();
    const std::size_t phase = g_review.phase;
    if (laterManualCorrectionExists(raw, phase)) {
        const int answer = MessageBoxW(window,
            L"この局面を修正すると、後続局面の手動修正は前盤面との整合性を失うため解除されます。続行しますか？",
            L"後続の手動修正を解除", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        if (answer != IDYES) return;
    }

    const tr::CorrectionCandidate selected = g_review.candidates[static_cast<std::size_t>(g_review.selectedCandidate)];
    resetManualFrom(raw, phase + 1);
    raw[phase].manuallyFixed = true;
    raw[phase].board = selected.move.board;
    raw[phase].fullBoard = selected.move.fullBoard;
    raw[phase].garbage = selected.garbage;
    raw[phase].placedPiece = selected.move.piece;
    raw[phase].placementX = selected.move.x;
    raw[phase].placementY = selected.move.y;
    raw[phase].placementRotation = selected.move.rotation;
    raw[phase].clearedLines = selected.move.clearedLines;
    raw[phase].score = selected.observationScore;

    // Keep the approved prefix intact; only the selected phase and following
    // phases are searched again.
    solved = tr::TetrisEngine::recomputeFrom(raw, solved, phase, g_settings);
    rebuildPhaseList();
    std::wostringstream message;
    message << playerName() << L" の局面 #" << phase << L" を " << cellText(selected.move.piece)
            << L" の合法候補として固定し、以降 " << (raw.size() - phase - 1) << L" 局面を再探索しました。";
    setText(g_review.header, message.str());
    selectPhase(phase);
}

void restoreAutomaticFromHere(HWND window) {
    if (g_review.phase == 0) return;
    const int answer = MessageBoxW(window,
        L"この局面以降の手動修正を解除し、自動の合法手ビーム探索へ戻します。続行しますか？",
        L"自動復元へ戻す", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    if (answer != IDYES) return;
    auto& raw = reviewRaw();
    auto& solved = reviewSolved();
    const std::size_t phase = g_review.phase;
    resetManualFrom(raw, phase);
    solved = tr::TetrisEngine::beamSearch(raw, g_settings);
    rebuildPhaseList();
    setText(g_review.header, std::wstring(playerName()) + L" の局面 #" + std::to_wstring(phase) +
                             L" 以降を自動の合法手探索へ戻しました。");
    selectPhase(phase);
}

void exportApprovedResult(HWND window) {
    std::string error;
    g_output.humanApproved = true;
    g_output.includeSourceVideoInTraining = !g_review.trainingVideo ||
        SendMessageW(g_review.trainingVideo, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!tr::writeRecoveredOutput(g_input, g_outputDir, g_settings, g_output, error)) {
        MessageBoxW(window, widen(error).c_str(), L"出力に失敗しました", MB_OK | MB_ICONERROR);
        return;
    }
    // The simulator URL can be several kilobytes long. Passing it straight
    // to ShellExecute depends on the browser command-line handler and fails
    // silently on some Windows installations. Open the generated Internet
    // Shortcut instead; Explorer resolves it through the default browser.
    bool browserOpened = false;
    if (!g_output.combinedUrlPath.empty() && std::filesystem::exists(g_output.combinedUrlPath)) {
        const auto launchResult = reinterpret_cast<INT_PTR>(ShellExecuteW(
            window, L"open", g_output.combinedUrlPath.c_str(), nullptr,
            g_outputDir.c_str(), SW_SHOWNORMAL));
        browserOpened = launchResult > 32;
    }
    // If Explorer cannot resolve the .url shortcut, open the small HTML link
    // page instead. It contains ordinary clickable browser links and is also
    // a reliable manual fallback shown in the completion dialog.
    if (!browserOpened && !g_output.linksPath.empty() && std::filesystem::exists(g_output.linksPath)) {
        const auto launchResult = reinterpret_cast<INT_PTR>(ShellExecuteW(
            window, L"open", g_output.linksPath.c_str(), nullptr,
            g_outputDir.c_str(), SW_SHOWNORMAL));
        browserOpened = launchResult > 32;
    }

    const std::wstring message = L"確認済みの復元結果を書き出しました。\n\n"
        L"出力先:\n" + g_outputDir.wstring() +
        (browserOpened
            ? L"\n\n2Pシミュレータを既定ブラウザで開きました。"
            : L"\n\nブラウザを起動できませんでした。出力先の *_links.html をダブルクリックしてください。")
        + L"\n\nClickable links: " + g_output.linksPath.wstring();
    std::wstring messageWithTraining = message;
    if (!g_output.trainingAnnotationPath.empty()) {
        messageWithTraining += L"\n\n学習用の確定ラベル:\n" + g_output.trainingAnnotationPath.wstring();
    }
    if (!g_output.trainingVideoPath.empty()) {
        messageWithTraining += L"\n\n学習用に保存した動画:\n" + g_output.trainingVideoPath.wstring();
    }
    MessageBoxW(window, messageWithTraining.c_str(), L"テト譜を出力しました", MB_OK | MB_ICONINFORMATION);
}

void layoutReview(HWND window) {
    if (!g_reviewActive) return;
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    constexpr int margin = 12;
    constexpr int gap = 10;
    const int phaseWidth = std::clamp(width / 7, 210, 260);
    const int galleryWidth = std::clamp(width / 4, 410, 560);
    const int videoWidth = std::clamp(width / 4, 350, 470);
    const int contentY = 110;
    const int bottom = height - margin;
    const int leftCenter = margin + phaseWidth + gap;
    const int galleryX = width - margin - galleryWidth;
    const int videoX = galleryX - gap - videoWidth;
    const int centerWidth = std::max(300, videoX - gap - leftCenter);
    const int boardWidth = std::max(88, (centerWidth - gap * 2) / 3);
    const int boardHeight = std::min(boardWidth * 2, std::max(180, bottom - contentY - 62));
    const int actionY = contentY + boardHeight + 10;

    MoveWindow(g_review.header, margin, 8, std::max(100, width - margin * 2 - 132), 23, TRUE);
    MoveWindow(g_review.fullscreen, width - margin - 120, 8, 120, 23, TRUE);
    MoveWindow(g_review.playerP1, margin, 34, 78, 23, TRUE);
    MoveWindow(g_review.playerP2, margin + 82, 34, 78, 23, TRUE);
    MoveWindow(g_review.phaseLabel, margin, 61, phaseWidth, 18, TRUE);
    MoveWindow(g_review.phaseList, margin, contentY, phaseWidth, std::max(100, bottom - contentY), TRUE);

    const int boardX1 = leftCenter;
    const int boardX2 = boardX1 + boardWidth + gap;
    const int boardX3 = boardX2 + boardWidth + gap;
    MoveWindow(g_review.previousLabel, boardX1, 80, boardWidth, 18, TRUE);
    MoveWindow(g_review.observedLabel, boardX2, 80, boardWidth, 18, TRUE);
    MoveWindow(g_review.candidateLabel, boardX3, 80, boardWidth, 18, TRUE);
    MoveWindow(g_previousBoard.window, boardX1, contentY, boardWidth, boardHeight, TRUE);
    MoveWindow(g_observedBoard.window, boardX2, contentY, boardWidth, boardHeight, TRUE);
    MoveWindow(g_candidateBoard.window, boardX3, contentY, boardWidth, boardHeight, TRUE);
    MoveWindow(g_review.restoreAutomatic, boardX1, actionY, boardWidth + gap, 30, TRUE);
    MoveWindow(g_review.applyCandidate, boardX2, actionY, boardWidth * 2 + gap, 30, TRUE);

    MoveWindow(g_review.videoLabel, videoX, 34, videoWidth, 18, TRUE);
    MoveWindow(g_review.videoInfo, videoX, 53, videoWidth, 19, TRUE);
    const int videoButtonWidth = std::max(70, (videoWidth - gap * 3) / 4);
    MoveWindow(g_review.videoBack, videoX, 76, videoButtonWidth, 25, TRUE);
    MoveWindow(g_review.videoPhase, videoX + (videoButtonWidth + gap), 76, videoButtonWidth, 25, TRUE);
    MoveWindow(g_review.videoPlayPause, videoX + (videoButtonWidth + gap) * 2, 76, videoButtonWidth, 25, TRUE);
    MoveWindow(g_review.videoForward, videoX + (videoButtonWidth + gap) * 3, 76, videoButtonWidth, 25, TRUE);
    const int videoHeight = std::clamp(videoWidth * 9 / 16, 190, 290);
    if (!g_queueLog.window) {
        MoveWindow(g_video.surface, videoX, contentY, videoWidth, videoHeight, TRUE);
    }

    const int garbageY = contentY + videoHeight + 10;
    MoveWindow(g_review.garbageLabel, videoX, garbageY, 128, 20, TRUE);
    MoveWindow(g_review.garbageLines, videoX + 130, garbageY, 58, 22, TRUE);
    MoveWindow(g_review.garbageSpin, videoX + 188, garbageY, 22, 22, TRUE);
    MoveWindow(g_review.garbageAuto, videoX + 216, garbageY, std::max(100, videoWidth - 216), 24, TRUE);
    MoveWindow(g_review.garbageInfo, videoX, garbageY + 27, videoWidth, 31, TRUE);
    const int garbageBoardWidth = std::min(142, std::max(105, videoWidth / 3));
    const int garbageBoardHeight = std::min(garbageBoardWidth * 2, std::max(160, bottom - (garbageY + 60)));
    MoveWindow(g_garbageBoard.window, videoX, garbageY + 60, garbageBoardWidth, garbageBoardHeight, TRUE);

    MoveWindow(g_review.queueLog, galleryX, 34, galleryWidth, 27, TRUE);
    // Export is the final primary action. Keep it high in the right column so
    // it does not disappear below the gallery on a short monitor.
    MoveWindow(g_review.exportButton, galleryX, 65, galleryWidth, 34, TRUE);
    MoveWindow(g_review.filterInfo, galleryX, 103, galleryWidth, 18, TRUE);
    MoveWindow(g_review.clearFilter, galleryX, 123, galleryWidth, 25, TRUE);
    const int galleryY = 156;
    MoveWindow(g_review.candidateGallery, galleryX, galleryY, galleryWidth, std::max(100, bottom - galleryY - 96), TRUE);
    MoveWindow(g_review.candidateInfo, galleryX, bottom - 91, galleryWidth, 34, TRUE);
    MoveWindow(g_review.trainingVideo, galleryX, bottom - 53, galleryWidth, 22, TRUE);
    MoveWindow(g_review.exitButton, galleryX, bottom - 27, galleryWidth, 27, TRUE);
    if (g_video.player) g_video.player->UpdateVideo();
}

void toggleFullscreen(HWND window) {
    if (!window) return;
    if (!g_fullscreen) {
        g_windowedStyle = GetWindowLongW(window, GWL_STYLE);
        g_windowedExStyle = GetWindowLongW(window, GWL_EXSTYLE);
        g_windowedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(window, &g_windowedPlacement);

        MONITORINFO monitor{sizeof(monitor)};
        const HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(handle, &monitor);
        const LONG fullscreenStyle = g_windowedStyle &
            ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongW(window, GWL_STYLE, fullscreenStyle | WS_POPUP);
        SetWindowLongW(window, GWL_EXSTYLE, g_windowedExStyle &
            ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = true;
        setText(g_review.fullscreen, L"ウィンドウに戻す (F11)");
    } else {
        SetWindowLongW(window, GWL_STYLE, g_windowedStyle);
        SetWindowLongW(window, GWL_EXSTYLE, g_windowedExStyle);
        SetWindowPlacement(window, &g_windowedPlacement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = false;
        setText(g_review.fullscreen, L"全画面 (F11)");
    }
    layoutReview(window);
}

HWND createControl(HWND parent, const wchar_t* className, const wchar_t* title, DWORD style, int id) {
    HWND control = CreateWindowExW(0, className, title, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    useDefaultFont(control);
    return control;
}

std::vector<tr::QueueRecognitionSample>& queueLogSamples() {
    return g_queueLog.player == 0 ? g_output.queueObservationsP1 : g_output.queueObservationsP2;
}

const std::vector<tr::QueueRecognitionSample>& queueLogOriginalSamples() {
    return g_queueLog.player == 0 ? g_output.originalQueueObservationsP1 : g_output.originalQueueObservationsP2;
}

std::wstring queueLogRowText(const tr::QueueRecognitionSample& sample) {
    std::wostringstream text;
    text << std::fixed << std::setprecision(3) << sample.timeSeconds << L" s  "
         << L"現在=" << queueCellText(sample.active) << L"  "
         << L"Hold=" << queueCellText(sample.observation.hold) << L"  "
         << L"  Next=" << piecesText(sample.observation.next);
    if (!sample.decoded.next.empty() && sample.observation.next != sample.decoded.next) {
        text << L"  →復元=" << piecesText(sample.decoded.next);
    }
    text << (sample.rejected ? L"  [棄却:認識不能]"
                              : (sample.sequenceCorrected ? L"  [補正:全履歴/7-bag]"
                                                          : (sample.stable ? L"  [確定]" : L"  [保留]")))
         << (sample.manuallyEdited ? L"  [手動]" : L"");
    return text.str();
}

void queueLogFillEditor() {
    const auto& samples = queueLogSamples();
    if (!g_queueLog.list || samples.empty() || g_queueLog.selected >= samples.size()) return;
    const auto& sample = samples[g_queueLog.selected];
    g_queueLog.updating = true;
    std::wostringstream time;
    time << std::fixed << std::setprecision(3) << sample.timeSeconds << L" 秒の認識";
    setText(g_queueLog.time, time.str());
    setQueueComboCell(g_queueLog.active, sample.active);
    setQueueComboCell(g_queueLog.hold, sample.observation.hold);
    for (std::size_t i = 0; i < g_queueLog.next.size(); ++i) {
        setQueueComboCell(g_queueLog.next[i], i < sample.observation.next.size()
            ? sample.observation.next[i] : tr::Cell::Empty);
    }
    g_queueLog.updating = false;
}

void refreshQueueLogList() {
    if (!g_queueLog.window || !g_queueLog.list) return;
    const auto& samples = queueLogSamples();
    SendMessageW(g_queueLog.list, LB_RESETCONTENT, 0, 0);
    if (samples.empty()) {
        setText(g_queueLog.info, L"このプレイヤーには生のキュー認識ログがありません。");
        return;
    }
    g_queueLog.selected = std::min(g_queueLog.selected, samples.size() - 1);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const int row = static_cast<int>(SendMessageW(g_queueLog.list, LB_ADDSTRING, 0,
                                                       reinterpret_cast<LPARAM>(queueLogRowText(samples[i]).c_str())));
        SendMessageW(g_queueLog.list, LB_SETITEMDATA, row, static_cast<LPARAM>(i));
    }
    SendMessageW(g_queueLog.list, LB_SETSEL, TRUE, static_cast<LPARAM>(g_queueLog.selected));
    SendMessageW(g_queueLog.list, LB_SETCARETINDEX, static_cast<WPARAM>(g_queueLog.selected), TRUE);
    queueLogFillEditor();
    std::wostringstream info;
    info << L"" << samples.size() << L"件の生ログ。行を選ぶと、その時刻から動画を再生します。\n"
         << L"現在ミノ / Hold / Next1〜5を編集できます。Ctrl/Shiftで複数行を選び、同じ値をまとめて適用できます。\n"
         << L"編集後は「反映して局面を再解析」で、キュー安定化・局面分割・合法手探索を最初から再計算します。";
    setText(g_queueLog.info, info.str());
}

std::vector<std::size_t> queueLogSelectedIndices() {
    std::vector<std::size_t> result;
    if (!g_queueLog.list) return result;
    const LRESULT count = SendMessageW(g_queueLog.list, LB_GETSELCOUNT, 0, 0);
    if (count > 0 && count < 1000000) {
        std::vector<int> rows(static_cast<std::size_t>(count));
        const LRESULT copied = SendMessageW(g_queueLog.list, LB_GETSELITEMS,
                                            static_cast<WPARAM>(count), reinterpret_cast<LPARAM>(rows.data()));
        for (LRESULT i = 0; i < copied; ++i) {
            const LRESULT data = SendMessageW(g_queueLog.list, LB_GETITEMDATA,
                                              static_cast<WPARAM>(rows[static_cast<std::size_t>(i)]), 0);
            if (data != LB_ERR) result.push_back(static_cast<std::size_t>(data));
        }
    }
    if (result.empty() && !queueLogSamples().empty()) result.push_back(g_queueLog.selected);
    return result;
}

bool copyUnicodeText(HWND owner, const std::wstring& value) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const std::size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, value.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

std::wstring queueHistoryText(const std::vector<std::size_t>& indices) {
    const auto& samples = queueLogSamples();
    std::wostringstream text;
    for (const std::size_t index : indices) {
        if (index >= samples.size()) continue;
        const auto& sample = samples[index];
        text << L"* " << std::fixed << std::setprecision(3) << sample.timeSeconds << L" s"
             << L"  現在=" << queueCellText(sample.active)
             << L"  Hold=" << queueCellText(sample.observation.hold)
             << L"  認識Next=" << piecesText(sample.observation.next);
        if (!sample.decoded.next.empty()) text << L"  復元Next=" << piecesText(sample.decoded.next);
        if (sample.sequenceCorrected) text << L"  [補正:全履歴/7-bag]";
        if (sample.rejected) text << L"  [棄却:認識不能]";
        if (sample.manuallyEdited) text << L"  [手動]";
        text << L"\r\n";
    }
    return text.str();
}

void copyQueueHistory(HWND owner, bool allRows) {
    std::vector<std::size_t> indices;
    const auto& samples = queueLogSamples();
    if (allRows) {
        indices.reserve(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) indices.push_back(i);
    } else {
        indices = queueLogSelectedIndices();
    }
    if (indices.empty() || !copyUnicodeText(owner, queueHistoryText(indices))) {
        MessageBoxW(owner, L"NEXT履歴をクリップボードへコピーできませんでした。", L"時刻別NEXT認識ログ", MB_OK | MB_ICONWARNING);
    }
}

bool readQueueLogEditor(tr::Cell& active, tr::Cell& hold, std::vector<tr::Cell>& next, std::wstring& error) {
    active = queueComboCell(g_queueLog.active);
    hold = queueComboCell(g_queueLog.hold);
    next.clear();
    bool blankSeen = false;
    for (HWND combo : g_queueLog.next) {
        const tr::Cell piece = queueComboCell(combo);
        if (piece == tr::Cell::Empty) {
            blankSeen = true;
            continue;
        }
        if (blankSeen) {
            error = L"Nextは途中を空欄にできません。末尾だけ空欄にしてください。";
            return false;
        }
        next.push_back(piece);
    }
    if (next.size() < 3) {
        error = L"Nextは少なくとも3個、連続して入力してください。";
        return false;
    }
    return true;
}

void applyQueueLogRow(HWND owner) {
    tr::Cell active = tr::Cell::Empty;
    tr::Cell hold = tr::Cell::Empty;
    std::vector<tr::Cell> next;
    std::wstring error;
    if (!readQueueLogEditor(active, hold, next, error)) {
        MessageBoxW(owner, error.c_str(), L"時刻別Next認識ログ", MB_OK | MB_ICONWARNING);
        return;
    }
    auto& samples = queueLogSamples();
    for (const std::size_t index : queueLogSelectedIndices()) {
        if (index >= samples.size()) continue;
        samples[index].active = active;
        samples[index].observation.hold = hold;
        samples[index].observation.next = next;
        samples[index].stable = true;
        samples[index].manuallyEdited = true;
        samples[index].rejected = false;
        samples[index].decoded = samples[index].observation;
        samples[index].sequenceCorrected = false;
    }
    refreshQueueLogList();
}

void restoreQueueLogRow(HWND owner) {
    const auto& original = queueLogOriginalSamples();
    auto& samples = queueLogSamples();
    const auto indices = queueLogSelectedIndices();
    if (original.empty()) {
        MessageBoxW(owner, L"元の認識ログが保存されていません。", L"時刻別Next認識ログ", MB_OK | MB_ICONWARNING);
        return;
    }
    for (const std::size_t index : indices) {
        if (index >= samples.size() || index >= original.size()) continue;
        samples[index] = original[index];
    }
    refreshQueueLogList();
}

void reanalyzeFromQueueLog(HWND owner) {
    applyQueueLogRow(owner);
    if (!g_queueLog.window) return;
    const int answer = MessageBoxW(owner,
        L"修正した生ログから、キューの安定化・局面の時系列・合法手候補を再計算します。\n"
        L"現在の局面手動修正は、再計算結果に置き換わります。続行しますか？",
        L"生ログを反映して再解析", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (answer != IDYES) return;
    std::string error;
    if (!tr::reanalyzeQueueObservations(g_settings, g_output, error)) {
        MessageBoxW(owner, widen(error).c_str(), L"生ログの再解析に失敗", MB_OK | MB_ICONERROR);
        return;
    }
    const std::size_t previousPhase = g_review.phase;
    rebuildPhaseList();
    if (!reviewRaw().empty()) selectPhase(std::min(previousPhase, reviewRaw().size() - 1));
    refreshQueueLogList();
}

LRESULT CALLBACK queueLogWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE:
        // Set this before WM_CREATE runs. CreateWindowEx invokes the window
        // procedure before returning its HWND, so assigning only from the
        // caller would leave the dialog state unable to identify itself
        // during early creation/destruction notifications.
        g_queueLog.window = window;
        return TRUE;
    case WM_CREATE: {
        g_queueLog.info = createControl(window, L"STATIC",
            L"生ログは解析時刻ごとのONNXキュー認識です。局面単位の推測ではありません。",
            SS_LEFT, IDC_QUEUE_LOG_INFO);
        g_queueLog.time = createControl(window, L"STATIC", L"", SS_LEFT, IDC_QUEUE_LOG_TIME);
        g_queueLog.videoInfo = createControl(window, L"STATIC", L"動画プレビュー（停止）",
                                             SS_LEFT, IDC_QUEUE_LOG_VIDEO_INFO);
        g_queueLog.list = createControl(window, L"LISTBOX", L"",
            WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
            IDC_QUEUE_LOG_LIST);
        createControl(window, L"STATIC", L"現在ミノ", SS_LEFT, IDC_QUEUE_LOG_ACTIVE_LABEL);
        g_queueLog.active = createControl(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_QUEUE_LOG_ACTIVE);
        createControl(window, L"STATIC", L"Hold", SS_LEFT, IDC_QUEUE_LOG_HOLD_LABEL);
        g_queueLog.hold = createControl(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_QUEUE_LOG_HOLD);
        const std::array<const wchar_t*, 5> labels{L"Next1", L"Next2", L"Next3", L"Next4", L"Next5"};
        for (std::size_t i = 0; i < g_queueLog.next.size(); ++i) {
            createControl(window, L"STATIC", labels[i], SS_LEFT, IDC_QUEUE_LOG_NEXT_LABEL_1 + static_cast<int>(i));
            g_queueLog.next[i] = createControl(window, L"COMBOBOX", L"",
                                                CBS_DROPDOWNLIST | WS_TABSTOP,
                                                IDC_QUEUE_LOG_NEXT_1 + static_cast<int>(i));
        }
        populateQueueCombo(g_queueLog.active);
        populateQueueCombo(g_queueLog.hold);
        for (HWND combo : g_queueLog.next) populateQueueCombo(combo);
        g_queueLog.applyRow = createControl(window, L"BUTTON", L"選択行を更新（複数可）",
                                             BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_APPLY_ROW);
        g_queueLog.restoreRow = createControl(window, L"BUTTON", L"選択行を元の認識へ戻す",
                                               BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_RESTORE_ROW);
        g_queueLog.copySelected = createControl(window, L"BUTTON", L"選択行のNEXT履歴をコピー",
                                                 BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_COPY_SELECTED);
        g_queueLog.copyAll = createControl(window, L"BUTTON", L"全NEXT履歴をコピー",
                                            BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_COPY_ALL);
        g_queueLog.reanalyze = createControl(window, L"BUTTON", L"修正を反映して局面を再解析",
                                             BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_REANALYZE);
        g_queueLog.close = createControl(window, L"BUTTON", L"閉じる",
                                         BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG_CLOSE);
        refreshQueueLogList();
        return 0;
    }
    case WM_SIZE: {
        constexpr int margin = 16;
        constexpr int gap = 16;
        const int width = std::max(900, static_cast<int>(LOWORD(lParam)));
        const int height = std::max(680, static_cast<int>(HIWORD(lParam)));
        const int leftWidth = std::clamp(width * 48 / 100, 470, width - 430);
        const int rightX = margin + leftWidth + gap;
        const int rightWidth = std::max(390, width - rightX - margin);
        const int labelWidth = 68;
        const int editorX = rightX + labelWidth + 8;
        const int editorWidth = std::max(220, rightWidth - labelWidth - 8);
        MoveWindow(g_queueLog.info, rightX, margin, rightWidth, 42, TRUE);
        MoveWindow(g_queueLog.time, rightX, margin + 46, rightWidth, 22, TRUE);
        MoveWindow(g_queueLog.list, margin, margin, leftWidth, std::max(180, height - margin * 2), TRUE);
        MoveWindow(GetDlgItem(window, IDC_QUEUE_LOG_ACTIVE_LABEL), rightX, 96, labelWidth, 22, TRUE);
        MoveWindow(g_queueLog.active, editorX, 92, editorWidth, 26, TRUE);
        MoveWindow(GetDlgItem(window, IDC_QUEUE_LOG_HOLD_LABEL), rightX, 132, labelWidth, 22, TRUE);
        MoveWindow(g_queueLog.hold, editorX, 128, editorWidth, 26, TRUE);
        for (std::size_t i = 0; i < g_queueLog.next.size(); ++i) {
            const int y = 164 + static_cast<int>(i) * 36;
            MoveWindow(GetDlgItem(window, IDC_QUEUE_LOG_NEXT_LABEL_1 + static_cast<int>(i)), rightX, y + 4, labelWidth, 22, TRUE);
            MoveWindow(g_queueLog.next[i], editorX, y, editorWidth, 26, TRUE);
        }
        const int buttonY = 354;
        const int halfButton = (rightWidth - gap) / 2;
        MoveWindow(g_queueLog.applyRow, rightX, buttonY, halfButton, 30, TRUE);
        MoveWindow(g_queueLog.restoreRow, rightX + halfButton + gap, buttonY, rightWidth - halfButton - gap, 30, TRUE);
        MoveWindow(g_queueLog.copySelected, rightX, buttonY + 38, halfButton, 30, TRUE);
        MoveWindow(g_queueLog.copyAll, rightX + halfButton + gap, buttonY + 38, rightWidth - halfButton - gap, 30, TRUE);
        MoveWindow(g_queueLog.reanalyze, rightX, buttonY + 76, rightWidth, 32, TRUE);
        MoveWindow(g_queueLog.close, rightX, buttonY + 116, rightWidth, 30, TRUE);
        const int videoY = buttonY + 158;
        MoveWindow(g_queueLog.videoInfo, rightX, videoY, rightWidth, 24, TRUE);
        if (g_video.surface && GetParent(g_video.surface) == window) {
            MoveWindow(g_video.surface, rightX, videoY + 26, rightWidth,
                       std::max(160, height - videoY - 42), TRUE);
        }
        if (g_video.player) g_video.player->UpdateVideo();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 1100;
        info->ptMinTrackSize.y = 720;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_QUEUE_LOG_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                const LRESULT row = SendMessageW(g_queueLog.list, LB_GETCARETINDEX, 0, 0);
                if (row != LB_ERR) {
                    const LRESULT data = SendMessageW(g_queueLog.list, LB_GETITEMDATA,
                                                      static_cast<WPARAM>(row), 0);
                    if (data != LB_ERR) {
                        g_queueLog.selected = static_cast<std::size_t>(data);
                        queueLogFillEditor();
                        const auto& samples = queueLogSamples();
                        if (g_queueLog.selected < samples.size()) {
                            // Selecting a scan row is an inspection action:
                            // seek exactly there and keep the new preview
                            // frame paused until the user explicitly presses
                            // the main play control after closing this dialog.
                            seekVideo(samples[g_queueLog.selected].timeSeconds, false);
                        }
                    }
                }
            }
            return 0;
        case IDC_QUEUE_LOG_APPLY_ROW:
            if (HIWORD(wParam) == BN_CLICKED) applyQueueLogRow(window);
            return 0;
        case IDC_QUEUE_LOG_RESTORE_ROW:
            if (HIWORD(wParam) == BN_CLICKED) restoreQueueLogRow(window);
            return 0;
        case IDC_QUEUE_LOG_COPY_SELECTED:
            if (HIWORD(wParam) == BN_CLICKED) copyQueueHistory(window, false);
            return 0;
        case IDC_QUEUE_LOG_COPY_ALL:
            if (HIWORD(wParam) == BN_CLICKED) copyQueueHistory(window, true);
            return 0;
        case IDC_QUEUE_LOG_REANALYZE:
            if (HIWORD(wParam) == BN_CLICKED) reanalyzeFromQueueLog(window);
            return 0;
        case IDC_QUEUE_LOG_CLOSE:
            if (HIWORD(wParam) == BN_CLICKED) DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        // Detach before the dialog's child windows are destroyed. Doing this
        // only in WM_NCDESTROY is too late: the reparented video surface would
        // otherwise be destroyed together with this log window.
        if (g_queueLog.window == window) g_queueLog.window = nullptr;
        if (g_video.surface && g_mainWindow && IsWindow(g_mainWindow)) {
            SetParent(g_video.surface, g_mainWindow);
            ShowWindow(g_video.surface, SW_SHOW);
            layoutReview(g_mainWindow);
        }
        return 0;
    case WM_NCDESTROY: {
        const HWND owner = GetWindow(window, GW_OWNER);
        if (owner) {
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void createQueueLogWindow(HWND owner) {
    if (g_queueLog.window) {
        ShowWindow(g_queueLog.window, SW_SHOWNORMAL);
        SetForegroundWindow(g_queueLog.window);
        return;
    }
    g_queueLog = QueueLogUi{};
    g_queueLog.player = g_review.player;
    g_queueLog.window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                        QueueLogClass, L"時刻別キュー認識ログ（現在ミノ・Hold・Next修正）",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 1320, 920,
                                        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_queueLog.window) {
        g_queueLog = QueueLogUi{};
        return;
    }
    EnableWindow(owner, FALSE);
    if (g_video.surface) {
        SetParent(g_video.surface, g_queueLog.window);
        SetWindowLongPtrW(g_video.surface, GWL_STYLE,
                          GetWindowLongPtrW(g_video.surface, GWL_STYLE) | WS_CHILD | WS_VISIBLE);
        ShowWindow(g_video.surface, SW_SHOW);
    }
    ShowWindow(g_queueLog.window, SW_SHOWNORMAL);
    UpdateWindow(g_queueLog.window);
    RECT queueClient{};
    GetClientRect(g_queueLog.window, &queueClient);
    SendMessageW(g_queueLog.window, WM_SIZE, 0,
                 MAKELPARAM(queueClient.right, queueClient.bottom));
    const auto& samples = queueLogSamples();
    if (!samples.empty()) {
        g_queueLog.selected = std::min(g_queueLog.selected, samples.size() - 1);
        seekVideo(samples[g_queueLog.selected].timeSeconds, false);
    } else {
        seekVideo(g_video.requestedSeconds, false);
    }
    refreshQueueLogList();
}

void updateVideoInfo() {
    if (!g_review.videoInfo) return;
    const auto& raw = reviewRaw();
    std::wostringstream info;
    if (!raw.empty() && g_review.phase < raw.size()) {
        info << L"局面 " << std::fixed << std::setprecision(3) << raw[g_review.phase].timeSeconds
             << L" 秒 / プレビュー " << g_video.requestedSeconds << L" 秒";
    } else {
        info << L"動画プレビューを準備中";
    }
    if (!g_video.player) info << L" （再生器を初期化できませんでした）";
    else if (!g_video.ready) info << L" （読み込み中）";
    else if (g_video.playing) info << L" （再生中）";
    else info << L" （一時停止）";
    setText(g_review.videoInfo, info.str());
    if (g_review.videoPlayPause) setText(g_review.videoPlayPause, g_video.playing ? L"一時停止" : L"再生");
    updateQueueLogVideoInfo();
}

void updateQueueLogVideoInfo() {
    if (!g_queueLog.videoInfo) return;
    std::wostringstream info;
    info << L"動画プレビュー（停止） / " << std::fixed << std::setprecision(3)
         << g_video.requestedSeconds << L" 秒";
    if (!g_video.player) info << L" （読み込み中）";
    else if (g_video.ready) info << L" （停止中）";
    else info << L" （動画を読み込み中）";
    setText(g_queueLog.videoInfo, info.str());
}

void seekVideo(double seconds, bool play) {
    g_video.requestedSeconds = std::max(0.0, seconds);
    g_video.playing = play;
    if (!g_video.player || !g_video.ready) {
        updateVideoInfo();
        return;
    }
    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::llround(g_video.requestedSeconds * 10000000.0));
    const HRESULT seekResult = g_video.player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    if (FAILED(seekResult)) {
        g_video.playing = false;
        updateVideoInfo();
        return;
    }
    if (play) g_video.player->Play();
    else g_video.player->Pause();
    updateVideoInfo();
}

void syncVideoToSelectedPhase(bool play) {
    const auto& raw = reviewRaw();
    if (raw.empty() || g_review.phase >= raw.size()) return;
    // The list displays `timeSeconds`, so selecting a row must land on the
    // same timestamp rather than an implicit earlier preview point.
    seekVideo(raw[g_review.phase].timeSeconds, play);
}

void releaseVideoPreview() {
    if (g_video.player) {
        g_video.player->Shutdown();
        g_video.player->Release();
        g_video.player = nullptr;
    }
    if (g_video.callback) {
        g_video.callback->Release();
        g_video.callback = nullptr;
    }
    g_video.ready = false;
    g_video.playing = false;
}

void createVideoPreview(HWND owner) {
    releaseVideoPreview();
    if (!g_video.surface || g_input.empty()) return;
    g_video.callback = new VideoCallback(owner);
    // MFPlay does not reliably paint the first frame until it has entered
    // playback once. Start the media item, then WM_MEDIA_READY immediately
    // seeks to the selected row and pauses it, so the preview is initialized
    // without the reviewer having to press Play first.
    const HRESULT result = MFPCreateMediaPlayer(g_input.c_str(), TRUE, MFP_OPTION_NONE,
                                                 g_video.callback, g_video.surface, &g_video.player);
    if (FAILED(result)) {
        g_video.callback->Release();
        g_video.callback = nullptr;
        g_video.player = nullptr;
        updateVideoInfo();
        return;
    }
    g_video.player->SetAspectRatioMode(MFVideoARMode_PreservePicture);
    updateVideoInfo();
}

void destroyAnalysisControls() {
    if (g_analysisStatus) DestroyWindow(g_analysisStatus);
    if (g_analysisProgress) DestroyWindow(g_analysisProgress);
    if (g_analysisDetails) DestroyWindow(g_analysisDetails);
    g_analysisStatus = nullptr;
    g_analysisProgress = nullptr;
    g_analysisDetails = nullptr;
}

void createReviewControls(HWND window) {
    destroyAnalysisControls();
    g_review = ReviewUi{};
    g_reviewActive = true;
    g_review.player = g_output.rawP1.empty() && !g_output.rawP2.empty() ? 1 : 0;

    g_review.header = createControl(window, L"STATIC", L"解析結果を確認してください。出力は確定するまで行いません。", SS_LEFT, IDC_HEADER);
    g_review.playerP1 = createControl(window, L"BUTTON", L"P1", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, IDC_PLAYER_P1);
    g_review.playerP2 = createControl(window, L"BUTTON", L"P2", BS_AUTORADIOBUTTON | WS_TABSTOP, IDC_PLAYER_P2);
    g_review.phaseLabel = createControl(window, L"STATIC", L"局面（前局面の active を置いた後の盤面）", SS_LEFT, IDC_PHASE_LABEL);
    g_review.phaseList = createControl(window, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, IDC_PHASE_LIST);
    g_review.previousLabel = createControl(window, L"STATIC", L"前盤面にミノを描いて絞り込み", SS_CENTER, IDC_PREVIOUS_LABEL);
    g_review.observedLabel = createControl(window, L"STATIC", L"ONNX観測盤面", SS_CENTER, IDC_OBSERVED_LABEL);
    g_review.candidateLabel = createControl(window, L"STATIC", L"合法候補", SS_CENTER, IDC_CANDIDATE_LABEL);
    g_review.filterInfo = createControl(window, L"STATIC", L"", SS_LEFT, IDC_FILTER_INFO);
    g_review.clearFilter = createControl(window, L"BUTTON", L"観測・配置フィルターを解除", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR_FILTER);
    g_review.candidateInfo = createControl(window, L"STATIC", L"", SS_LEFT, IDC_CANDIDATE_INFO);
    g_review.queueLog = createControl(window, L"BUTTON", L"時刻別キュー認識ログを開く（現在ミノ / Hold / Next）",
                                      BS_PUSHBUTTON | WS_TABSTOP, IDC_QUEUE_LOG);
    g_candidateGallery = CandidateGalleryState{};
    g_review.candidateGallery = CreateWindowExW(WS_EX_CLIENTEDGE, CandidateGalleryClass, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 1, 1, window,
        reinterpret_cast<HMENU>(IDC_CANDIDATE_GALLERY), nullptr, &g_candidateGallery);
    g_review.garbageLabel = createControl(window, L"STATIC", L"下穴せり上がり（段数）", SS_LEFT, IDC_GARBAGE_LABEL);
    g_review.garbageLines = createControl(window, L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP, IDC_GARBAGE_LINES);
    g_review.garbageSpin = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS,
        0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_GARBAGE_SPIN), nullptr, nullptr);
    SendMessageW(g_review.garbageSpin, UDM_SETRANGE32, 0, tr::VisibleRows);
    SendMessageW(g_review.garbageSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(g_review.garbageLines), 0);
    useDefaultFont(g_review.garbageSpin);
    g_review.garbageAuto = createControl(window, L"BUTTON", L"自動検出に戻す", BS_PUSHBUTTON | WS_TABSTOP, IDC_GARBAGE_AUTO);
    g_review.garbageInfo = createControl(window, L"STATIC", L"", SS_LEFT, IDC_GARBAGE_INFO);
    g_review.videoLabel = createControl(window, L"STATIC", L"選択局面の動画プレビュー", SS_LEFT, IDC_VIDEO_LABEL);
    g_review.videoInfo = createControl(window, L"STATIC", L"動画を読み込み中", SS_LEFT, IDC_VIDEO_INFO);
    g_review.videoBack = createControl(window, L"BUTTON", L"-0.5 秒", BS_PUSHBUTTON | WS_TABSTOP, IDC_VIDEO_BACK);
    g_review.videoPhase = createControl(window, L"BUTTON", L"局面の前から", BS_PUSHBUTTON | WS_TABSTOP, IDC_VIDEO_PHASE);
    g_review.videoPlayPause = createControl(window, L"BUTTON", L"再生", BS_PUSHBUTTON | WS_TABSTOP, IDC_VIDEO_PLAY_PAUSE);
    g_review.videoForward = createControl(window, L"BUTTON", L"+0.5 秒", BS_PUSHBUTTON | WS_TABSTOP, IDC_VIDEO_FORWARD);
    g_video.surface = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
        0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_VIDEO_SURFACE), nullptr, nullptr);
    g_review.restoreAutomatic = createControl(window, L"BUTTON", L"この局面以降を自動探索へ戻す", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESTORE_AUTOMATIC);
    g_review.applyCandidate = createControl(window, L"BUTTON", L"選択候補で固定して以降を再探索", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_APPLY_CANDIDATE);
    g_review.trainingVideo = createControl(window, L"BUTTON", L"学習データに動画も保存（自己完結）", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_TRAINING_VIDEO);
    SendMessageW(g_review.trainingVideo, BM_SETCHECK, BST_CHECKED, 0);
    g_review.exportButton = createControl(window, L"BUTTON", L"テト譜を出力して2Pシミュレータを開く", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_EXPORT);
    g_review.exitButton = createControl(window, L"BUTTON", L"閉じる", BS_PUSHBUTTON | WS_TABSTOP, IDC_EXIT);
    g_review.fullscreen = createControl(window, L"BUTTON", L"全画面 (F11)", BS_PUSHBUTTON | WS_TABSTOP, IDC_FULLSCREEN);

    g_previousBoard = BoardViewState{};
    g_observedBoard = BoardViewState{};
    g_candidateBoard = BoardViewState{};
    g_garbageBoard = BoardViewState{};
    g_previousBoard.placementPaintOnClick = true;
    g_observedBoard.filtersOnClick = true;
    g_garbageBoard.garbageHolesOnClick = true;
    g_previousBoard.window = CreateWindowExW(WS_EX_CLIENTEDGE, BoardClass, L"", WS_CHILD | WS_VISIBLE,
                                              0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_PREVIOUS_BOARD), nullptr, &g_previousBoard);
    g_observedBoard.window = CreateWindowExW(WS_EX_CLIENTEDGE, BoardClass, L"", WS_CHILD | WS_VISIBLE,
                                              0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_OBSERVED_BOARD), nullptr, &g_observedBoard);
    g_candidateBoard.window = CreateWindowExW(WS_EX_CLIENTEDGE, BoardClass, L"", WS_CHILD | WS_VISIBLE,
                                               0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_CANDIDATE_BOARD), nullptr, &g_candidateBoard);
    g_garbageBoard.window = CreateWindowExW(WS_EX_CLIENTEDGE, BoardClass, L"", WS_CHILD | WS_VISIBLE,
                                             0, 0, 1, 1, window, reinterpret_cast<HMENU>(IDC_GARBAGE_BOARD), nullptr, &g_garbageBoard);

    EnableWindow(g_review.playerP1, g_output.rawP1.empty() ? FALSE : TRUE);
    EnableWindow(g_review.playerP2, g_output.rawP2.empty() ? FALSE : TRUE);
    SendMessageW(g_review.playerP1, BM_SETCHECK, g_review.player == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_review.playerP2, BM_SETCHECK, g_review.player == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    rebuildPhaseList();
    // Phase 1 is normally the initial spawn/no-placement transition.  Open
    // the first actual placement by default so the user immediately sees the
    // generated legal moves rather than a single "no placement" entry.
    if (!reviewRaw().empty()) selectPhase(std::min<std::size_t>(2, reviewRaw().size() - 1));
    layoutReview(window);
    createVideoPreview(window);
    syncVideoToSelectedPhase(false);
    SetWindowTextW(window, L"Tetris Video Recovery - 動画確認・合法手・下穴修正");
}

std::filesystem::path executableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

std::filesystem::path packageRoot() {
    return executableDirectory().parent_path();
}

std::filesystem::path chooseVideo() {
    wchar_t path[MAX_PATH * 4]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"Video files\0*.mp4;*.webm;*.mov;*.mkv;*.avi\0All files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(path) : std::filesystem::path();
}

std::filesystem::path commandLineVideo() {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::filesystem::path result;
    if (args && count >= 2) result = std::filesystem::path(args[1]);
    if (args) LocalFree(args);
    return result;
}

void startWorker(HWND window) {
    g_worker = std::thread([window]() {
        std::string error;
        const auto modelPath = packageRoot() / L"models" / L"tetris.onnx";
        const bool success = tr::analyzeVideo(g_input, modelPath, g_settings, g_status, g_output, error);
        if (success) {
            g_status.setMessage("Analysis complete. Choose legal candidates to review before export.");
        } else {
            g_status.setMessage("Recovery failed: " + error);
        }
        g_status.success.store(success);
        g_status.done.store(true);
        PostMessageW(window, WM_APP + 1, 0, 0);
    });
}

void finishAnalysis(HWND window) {
    if (g_analysisHandled || !g_status.done.load()) return;
    g_analysisHandled = true;
    if (g_worker.joinable()) g_worker.join();
    if (!g_status.success.load()) {
        MessageBoxW(window, widen(g_status.getMessage()).c_str(), L"Tetris Video Recovery", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return;
    }
    // Use the actual monitor work area instead of a fixed 1780x980 review
    // window. The review layout is responsive and can be switched to a
    // borderless monitor-sized view with F11.
    ShowWindow(window, SW_SHOWMAXIMIZED);
    createReviewControls(window);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_analysisStatus = CreateWindowW(L"STATIC", L"Analyzing video with ONNX Runtime...", WS_CHILD | WS_VISIBLE,
                                         20, 20, 720, 28, window, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);
        g_analysisProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                           20, 60, 720, 24, window, reinterpret_cast<HMENU>(IDC_PROGRESS), nullptr, nullptr);
        g_analysisDetails = CreateWindowW(L"STATIC",
            L"ONNX recognizes all 200 visible cells. The original compatible queue/vote/legal-move beam\n"
            L"pipeline will finish first; then you can inspect and correct only legal candidates before export.",
            WS_CHILD | WS_VISIBLE, 20, 100, 720, 70, window, reinterpret_cast<HMENU>(IDC_DETAILS), nullptr, nullptr);
        useDefaultFont(g_analysisStatus);
        useDefaultFont(g_analysisDetails);
        SendMessageW(g_analysisProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SetTimer(window, 1, 200, nullptr);
        startWorker(window);
        return 0;
    case WM_TIMER:
        if (!g_reviewActive) {
            SendMessageW(g_analysisProgress, PBM_SETPOS, g_status.progress.load(), 0);
            setText(g_analysisStatus, widen(g_status.getMessage()));
            finishAnalysis(window);
        }
        return 0;
    case WM_APP + 1:
        finishAnalysis(window);
        return 0;
    case WM_HOTKEY:
        if (wParam == 0xF11) toggleFullscreen(window);
        return 0;
    case WM_MEDIA_READY:
        g_video.ready = true;
        if (g_queueLog.window) {
                const auto& samples = queueLogSamples();
                if (!samples.empty() && g_queueLog.selected < samples.size()) {
                seekVideo(samples[g_queueLog.selected].timeSeconds, false);
            } else {
                syncVideoToSelectedPhase(false);
            }
        } else {
            syncVideoToSelectedPhase(false);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (g_reviewActive) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 1200;
            info->ptMinTrackSize.y = 680;
            return 0;
        }
        break;
    case WM_SIZE:
        if (g_reviewActive) layoutReview(window);
        return 0;
    case WM_COMMAND:
        if (!g_reviewActive) break;
        switch (LOWORD(wParam)) {
        case IDC_PLAYER_P1:
            if (HIWORD(wParam) == BN_CLICKED && !g_output.rawP1.empty()) {
                g_review.player = 0;
                SendMessageW(g_review.playerP1, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(g_review.playerP2, BM_SETCHECK, BST_UNCHECKED, 0);
                g_review.phase = 0;
                rebuildPhaseList();
                selectPhase(std::min<std::size_t>(2, reviewRaw().size() - 1));
            }
            return 0;
        case IDC_PLAYER_P2:
            if (HIWORD(wParam) == BN_CLICKED && !g_output.rawP2.empty()) {
                g_review.player = 1;
                SendMessageW(g_review.playerP1, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(g_review.playerP2, BM_SETCHECK, BST_CHECKED, 0);
                g_review.phase = 0;
                rebuildPhaseList();
                selectPhase(std::min<std::size_t>(2, reviewRaw().size() - 1));
            }
            return 0;
        case IDC_PHASE_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                const int row = static_cast<int>(SendMessageW(g_review.phaseList, LB_GETCURSEL, 0, 0));
                if (row != LB_ERR) selectPhase(static_cast<std::size_t>(SendMessageW(g_review.phaseList, LB_GETITEMDATA, row, 0)));
            }
            return 0;
        case IDC_QUEUE_LOG:
            if (HIWORD(wParam) == BN_CLICKED) {
                createQueueLogWindow(window);
            }
            return 0;
        case IDC_GARBAGE_LINES:
            if (HIWORD(wParam) == EN_CHANGE && !g_review.updatingGarbageControls && g_review.phase > 0) {
                wchar_t value[32]{};
                GetWindowTextW(g_review.garbageLines, value, static_cast<int>(std::size(value)));
                const int lines = std::clamp(static_cast<int>(wcstol(value, nullptr, 10)), 0, tr::VisibleRows);
                if (lines != g_review.garbageEditor.lines) {
                    g_review.garbageEditor.lines = lines;
                    g_review.garbageEditor.manuallySpecified = true;
                    g_review.garbageOverrideEnabled = true;
                    normalizeGarbageEditor(g_review.garbageEditor);
                    g_review.placementCells.clear();
                    refreshGarbagePatternView();
                    refreshCandidates(true);
                }
            }
            return 0;
        case IDC_GARBAGE_AUTO:
            if (HIWORD(wParam) == BN_CLICKED) {
                g_review.garbageEditor = automaticGarbageForCurrentPhase();
                g_review.garbageOverrideEnabled = false;
                normalizeGarbageEditor(g_review.garbageEditor);
                g_review.placementCells.clear();
                refreshGarbagePatternView();
                refreshCandidates(true);
            }
            return 0;
        case IDC_VIDEO_PHASE:
            if (HIWORD(wParam) == BN_CLICKED) {
                const auto& raw = reviewRaw();
                if (!raw.empty() && g_review.phase < raw.size()) {
                    // Keep a deliberate "show the lead-in" control separate
                    // from row selection, which seeks exactly to its label.
                    seekVideo(std::max(0.0, raw[g_review.phase].startSeconds - .75), true);
                }
            }
            return 0;
        case IDC_VIDEO_PLAY_PAUSE:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (!g_video.ready) syncVideoToSelectedPhase(true);
                else if (g_video.playing) {
                    g_video.player->Pause();
                    g_video.playing = false;
                    updateVideoInfo();
                } else {
                    g_video.player->Play();
                    g_video.playing = true;
                    updateVideoInfo();
                }
            }
            return 0;
        case IDC_VIDEO_BACK:
            if (HIWORD(wParam) == BN_CLICKED) seekVideo(g_video.requestedSeconds - .5, false);
            return 0;
        case IDC_VIDEO_FORWARD:
            if (HIWORD(wParam) == BN_CLICKED) seekVideo(g_video.requestedSeconds + .5, false);
            return 0;
        case IDC_CLEAR_FILTER:
            if (HIWORD(wParam) == BN_CLICKED) {
                g_review.requiredCells.clear();
                g_review.placementCells.clear();
                g_review.selectedCandidate = -1;
                refreshCandidates(true);
            }
            return 0;
        case IDC_APPLY_CANDIDATE:
            if (HIWORD(wParam) == BN_CLICKED) applySelectedCandidate(window);
            return 0;
        case IDC_RESTORE_AUTOMATIC:
            if (HIWORD(wParam) == BN_CLICKED) restoreAutomaticFromHere(window);
            return 0;
        case IDC_EXPORT:
            if (HIWORD(wParam) == BN_CLICKED) exportApprovedResult(window);
            return 0;
        case IDC_FULLSCREEN:
            if (HIWORD(wParam) == BN_CLICKED) toggleFullscreen(window);
            return 0;
        case IDC_EXIT:
            if (HIWORD(wParam) == BN_CLICKED) DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (g_worker.joinable() && !g_status.done.load()) {
            g_status.cancel.store(true);
            setText(g_analysisStatus, L"Cancellation requested...");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        UnregisterHotKey(window, 0xF11);
        if (g_queueLog.window) DestroyWindow(g_queueLog.window);
        releaseVideoPreview();
        if (g_worker.joinable()) g_worker.join();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }

    g_input = commandLineVideo();
    if (g_input.empty()) g_input = chooseVideo();
    if (g_input.empty() || !std::filesystem::exists(g_input)) { MFShutdown(); CoUninitialize(); return 0; }

    std::string settingsError;
    const auto root = packageRoot();
    if (!tr::loadSettings(root / L"config" / L"tetris_recover.ini", g_settings, settingsError)) {
        MessageBoxW(nullptr, widen(settingsError).c_str(), L"Tetris Video Recovery", MB_OK | MB_ICONERROR);
        MFShutdown(); CoUninitialize(); return 1;
    }
    // Keep generated reconstructions separate from source, simulator, and
    // obsolete diagnostic files. Each approved export gets its own dataset
    // folder under data/exports.
    g_outputDir = root / L"data" / L"exports" / g_input.stem();
    g_status.setMessage("Selected video: " + g_input.u8string());

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&controls);

    WNDCLASSW boardClass{};
    boardClass.lpfnWndProc = boardWindowProc;
    boardClass.hInstance = instance;
    boardClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    boardClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    boardClass.lpszClassName = BoardClass;
    RegisterClassW(&boardClass);

    WNDCLASSW galleryClass{};
    galleryClass.lpfnWndProc = candidateGalleryWindowProc;
    galleryClass.hInstance = instance;
    galleryClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    galleryClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    galleryClass.lpszClassName = CandidateGalleryClass;
    RegisterClassW(&galleryClass);

    WNDCLASSW queueLogClass{};
    queueLogClass.lpfnWndProc = queueLogWindowProc;
    queueLogClass.hInstance = instance;
    queueLogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    queueLogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    queueLogClass.lpszClassName = QueueLogClass;
    RegisterClassW(&queueLogClass);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = ApplicationClass;
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, ApplicationClass, L"Tetris Video Recovery",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 780, 230, nullptr, nullptr, instance, nullptr);
    if (!window) { MFShutdown(); CoUninitialize(); return 1; }
    g_mainWindow = window;
    RegisterHotKey(window, 0xF11, MOD_NOREPEAT, VK_F11);
    ShowWindow(window, SW_SHOWMAXIMIZED);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    MFShutdown();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
