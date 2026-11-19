#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tr {

class NeuralRanker;

constexpr int BoardWidth = 10;
constexpr int BoardHeight = 40;
constexpr int VisibleRows = 20;
constexpr int CellClassCount = 9;

enum class Cell : std::uint8_t {
    Empty = 0,
    I = 1,
    L = 2,
    O = 3,
    Z = 4,
    T = 5,
    J = 6,
    S = 7,
    Garbage = 8,
};

constexpr std::array<Cell, 7> Pieces{
    Cell::I, Cell::L, Cell::O, Cell::Z, Cell::T, Cell::J, Cell::S
};

using Board = std::array<Cell, BoardWidth * BoardHeight>;
using VisibleBoard = std::array<Cell, BoardWidth * VisibleRows>;
using Confidence = std::array<std::uint8_t, BoardWidth * VisibleRows>;
// Cell-major probabilities indexed by the Cell enum. Unlike the ONNX class
// order, this layout is stable throughout aggregation and beam scoring.
using BoardProbabilities = std::array<float, BoardWidth * VisibleRows * CellClassCount>;

inline int index(int x, int y) { return y * BoardWidth + x; }
inline std::size_t probabilityIndex(int visibleCell, Cell cell) {
    return static_cast<std::size_t>(visibleCell) * CellClassCount + static_cast<std::size_t>(cell);
}
inline bool isPiece(Cell c) { return c >= Cell::I && c <= Cell::S; }
inline bool isOccupied(Cell c) { return c != Cell::Empty; }

inline const char* cellName(Cell c) {
    switch (c) {
    case Cell::I: return "I";
    case Cell::L: return "L";
    case Cell::O: return "O";
    case Cell::Z: return "Z";
    case Cell::T: return "T";
    case Cell::J: return "J";
    case Cell::S: return "S";
    case Cell::Garbage: return "G";
    default: return "_";
    }
}

inline char cellChar(Cell c) { return cellName(c)[0]; }

inline std::string pieceString(const std::vector<Cell>& pieces) {
    std::string result;
    result.reserve(pieces.size());
    for (Cell p : pieces) if (isPiece(p)) result.push_back(cellChar(p));
    return result;
}

struct Frame {
    int width = 0;
    int height = 0;
    // Origin in the decoded source texture. Full frames use (0,0); the GPU
    // board pass can return a compact union ROI without changing its pixels.
    int sourceX = 0;
    int sourceY = 0;
    std::int64_t time100ns = 0;
    std::vector<std::uint8_t> bgra;
};

struct PixelRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct LayoutRect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

struct PlayerLayout {
    LayoutRect board;
    // Queue coordinates are the original video's 1280-wide coordinate
    // system. Board coordinates use the original 1920-wide system.
    std::array<double, 2> hold{};
    std::array<std::array<double, 2>, 5> next{};
    double queueCoordinateWidth = 1280.0;
    double queueSampleRadius = 5.0;
    // Exact crop rectangles selected for the HOLD/NEXT CNN. They use the
    // 850x478 training screenshot coordinate system and are mapped onto the
    // centered 16:9 video content, not onto letterbox bars.
    double queueCnnReferenceWidth = 850.0;
    double queueCnnReferenceHeight = 478.0;
    LayoutRect queueCnnHold{};
    std::array<LayoutRect, 5> queueCnnNext{};
    // Player-name crop in the centered 16:9 reference image.  It is kept
    // separate from the board/queue coordinates because the user calibrates
    // this once for a fixed broadcast layout.
    LayoutRect name{};
};

struct PlayerNameOcrSettings {
    bool enabled = true;
    bool preferOnnx = true;
    // Name rectangles are authored against this centered 16:9 canvas and
    // mapped to the decoded frame after removing pillar/letterbox bars.
    double referenceWidth = 1920.0;
    double referenceHeight = 1080.0;
    int upscale = 3;
    std::string languages = "jpn+eng";
    std::filesystem::path onnxModelPath;
    std::filesystem::path dictionaryPath;
    std::filesystem::path executablePath;
    std::filesystem::path tessdataPath;
};

struct ScoreWeights {
    double exactMatch = 10.0;
    double emptyMatch = 1.0;
    double colorMismatch = -2.0;
    double missingBlock = -5.0;
    double extraBlock = -5.0;
    // The placed piece is a much stronger observation than a generic board
    // cell match.  These are deliberately separate so the scorer can learn
    // them from approved placement labels later.
    double placedColorMatch = 60.0;
    double placedColorMismatch = -80.0;
    double placedColorComplete = 160.0;
    double garbageHoleMatch = 100.0;
    // A wrong hole changes the physical board even if the other nine cells
    // look like garbage, so it must outweigh their ordinary cell matches.
    double garbageHoleMismatch = -600.0;
};

struct Settings {
    // These defaults reproduce 動画解析.html's controls.
    double sampleIntervalSeconds = 0.01;
    // Native analysis can expose a prefix for review before processing the
    // remainder. Zero keeps the original full-video behavior.
    double initialAnalysisSeconds = 0;
    int queueConfirmSamples = 2;
    // NEXT is decoded over the complete video rather than confirmed row by
    // row. This beam runs over compressed queue changes, not every scan.
    int queueBeamWidth = 96;
    bool queueUseSevenBag = true;
    // Native analysis uses the compact RGB CNN for HOLD/NEXT. The legacy
    // colour classifier remains the fallback and the complete-history queue
    // decoder remains authoritative around unclear/effect frames.
    bool queueCnnEnabled = true;
    // false: HOLD frames the CNN marked unclear are not HOLD evidence in the
    // logical queue decoder (used by the HOLD-unclear repair lane).
    bool queueUnclearHoldIsEvidence = true;
    double queueCnnUnclearThreshold = .975;
    // A softmax winner below 50% is not a usable categorical observation.
    // It is treated as missing evidence before the unchanged queue decoder.
    double queueCnnPieceConfidenceThreshold = .5;
    int queueCnnDarkPixelLimit = 40;
    double queueCnnDarkRatioThreshold = .0794;
    // If a legal transition exactly matches the established visible CNN rows,
    // keep only exact candidates. Rows inserted by the same garbage-rise
    // transition remain hypotheses until later observations resolve them.
    bool forceExactOnnxMatch = true;
    // A stream range confirmed by READY/GO is a new match, so its recovery
    // anchor is known to have empty locked fields and empty HOLD slots even
    // when the fading ceremony graphic overlaps the board CNN crop. Generic
    // single-video analysis keeps this off because it may start mid-game.
    bool forceEmptyOpening = false;
    // PERFECT CLEAR's white/orange board effect is systematically decoded as
    // a tall L/G stack.  Experimental builds can quarantine only those short
    // observation runs, retain both rise/no-rise physics, and let the first
    // clean frame choose the trajectory.  It is off in existing builds.
#ifdef TR_PC_EFFECT_GUARD_DEFAULT
    bool perfectClearEffectGuard = true;
#else
    bool perfectClearEffectGuard = false;
#endif
    int perfectClearEffectLookbackSteps = 2;
    // The effect frame carries little board evidence. Keep only a small,
    // explicitly ranked recovery lane here; the ordinary beam width remains
    // untouched before and after the short quarantine.
    int perfectClearEffectBeamWidth = 32;
    double perfectClearEffectObservationWeight = 1.0;
    int onnxSamples = 5;
    int beamWidth = 500;
    double shiftThreshold = .60;
    // Ordinary recovery uses the zero-G/sonic graph. Candidate exhaustion
    // retries only the affected player with intermediate soft-drop states.
    bool completeLegalMoveSearch = false;
    // Enabled only by the anomaly retry. Besides the locally best detected
    // garbage rise, retain plausible alternate line counts (and no-rise) so
    // later clean observations can correct an earlier effect-obscured guess.
    bool garbageBacktracking = false;
    // v80: 12 (was 8). _ySYK 89: a 10-row rise whose top row (hole under a
    // vertical I) was cleared by the next lock was only ever seen as 9 rows.
    // TR_GARBAGE_BACKTRACK_MAX_LINES overrides.
    int garbageBacktrackingMaxLines = 12;
    // Second-stage garbage retry only: also enumerate hole columns for the
    // alternate line counts (not just one guess per count).
    bool garbageAltHoleVariants = false;
    // v79, repair ladder only: when spawn rows y=20/19/18 are all blocked on a
    // high stack, let the piece spawn up to 4 rows higher (TR_SPAWN_EXTRA_ROWS).
    bool extraSpawnRows = false;
    // The fast pass never pays for this model. It is enabled only on a
    // bounded retry after an unresolved phase or sustained board mismatch.
    bool accurateFallbackEnabled = true;
    bool useAccurateFallbackRanker = false;
    int accurateFallbackBeamWidth = 256;
    std::filesystem::path accurateFallbackRankerPath;
    std::shared_ptr<const NeuralRanker> accurateFallbackRanker;
    // Wall-clock ceiling for one trajectory-repair ladder, i.e. every widened /
    // garbage / ranker trial for a single match. Zero keeps the historical
    // unbounded behaviour. A ladder re-runs the whole match at beam 128-256
    // with the garbage graph and the accurate ranker, which measures 6-14x the
    // fast pass; on a trajectory that cannot be repaired those trials are pure
    // overhead and can dominate a batch. When the ceiling is reached the
    // fast-pass timeline is kept unchanged and the match is reported as
    // needing correction instead of shipping an unverified repair.
    double repairTimeBudgetSeconds = 0.0;
    // When two consecutive identical minos can be placed in a different
    // order and still reach the same following board, require a human anchor
    // if the alternate intermediate board is this close to the observation.
    double orderAmbiguityScoreMargin = 40.0;
    ScoreWeights weights;
    PlayerNameOcrSettings playerNameOcr;
    bool player1Enabled = true;
    bool player2Enabled = true;
    PlayerLayout p1{
        {304, 157, 366, 725}, {160, 155},
        {{{500, 122}, {500, 175}, {500, 225}, {500, 275}, {500, 325}}},
        1280.0, 5.0, 850.0, 478.0,
        {87, 89, 41, 22},
        {{{309, 72, 46, 27}, {309, 105, 46, 27}, {309, 138, 46, 27},
          {309, 171, 46, 27}, {309, 204, 46, 27}}}
    };
    PlayerLayout p2{
        {1250, 157, 363, 725}, {790, 155},
        {{{1130, 122}, {1130, 175}, {1130, 225}, {1130, 275}, {1130, 325}}},
        1280.0, 5.0, 850.0, 478.0,
        {507, 88, 38, 22},
        {{{727, 72, 46, 27}, {727, 105, 46, 27}, {727, 138, 46, 27},
          {727, 171, 46, 27}, {727, 204, 46, 27}}}
    };
};

struct QueueObservation {
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    std::vector<std::array<std::uint8_t, 3>> colors;
    // CNN metadata is deliberately separate from the provisional piece
    // values. The recognition adapter freezes unclear frames at the previous
    // trusted CNN observation before the unchanged history decoder sees them.
    bool cnnUsed = false;
    bool holdUnclear = false;
    std::array<bool, 5> nextUnclear{};
    std::uint8_t holdPieceConfidence = 255;
    std::array<std::uint8_t, 5> nextPieceConfidence{{255, 255, 255, 255, 255}};
    std::uint8_t holdUnclearProbability = 0;
    std::array<std::uint8_t, 5> nextUnclearProbability{};
};

// One raw queue recognition made at a concrete video scan time. These samples
// intentionally survive the queue stabilizer: reviewers can correct the
// exact recognition that was wrong instead of guessing which phase it came
// from.
struct QueueRecognitionSample {
    double timeSeconds = 0;
    // The current mino inferred internally from the queue transition state at
    // this scan. It is kept in the raw log so a reviewer can correct the
    // hidden active-piece assumption together with Hold/Next.
    Cell active = Cell::Empty;
    // Raw colour-classifier output at this exact timestamp. This is never
    // overwritten by the global sequence decoder.
    QueueObservation observation;
    bool stable = false;
    bool manuallyEdited = false;
    // The recognizer produced this row, but it did not fit the legal queue
    // transition model and was excluded from the stabilized timeline.
    bool rejected = false;
    // The 5-piece window reconstructed from the complete history using
    // bounded legal slides, stable visual re-synchronization, and the 7-bag
    // generator. Phase construction uses this value; `observation` remains
    // the raw evidence.
    QueueObservation decoded;
    bool sequenceCorrected = false;
    // The raw Hold/ACTIVE pair did not form a legal atomic Hold transition,
    // so one or both derived values were corrected.
    bool holdCorrected = false;
    // Reserved for backward-compatible logs. ACTIVE is always inferred from
    // the legal queue/HOLD state; it is never promoted from a visual redraw
    // to an authoritative correction.
    bool activeCorrected = false;
    // Monotonically increasing revision assigned when a reviewer edits this
    // row. It lets the decoder distinguish the newest edit from older manual
    // anchors, so a later edit cannot rewrite the already reviewed prefix.
    std::uint64_t manualRevision = 0;
};

struct BoardObservation {
    VisibleBoard board{};
    Confidence confidence{};
    BoardProbabilities probabilities{};
    QueueObservation queue;
    double timeSeconds = 0;
    double quality = 0;
    // ライン消去などの白いエフェクトが盤面を覆っている割合。
    // 1.0 に近いほど「読めていない」フレーム。
    float effectRatio = 0.0f;
    std::string recognitionError;
};

// Garbage is represented independently from the resulting board so a human
// reviewer can repair a rise even when the incoming rows were hidden by an
// effect. `holeMasks` is ordered from the upper to lower inserted line; a set
// bit is the empty hole and every other bit is a garbage cell. Tetris garbage
// in this recovery mode always has exactly one set bit per row.
struct GarbageRise {
    int lines = 0;
    std::vector<std::uint16_t> holeMasks;
    double matchRatio = 0;
    bool manuallySpecified = false;
    // True only when every automatically inferred garbage row was observed
    // as exactly nine garbage cells plus one empty cell.  When false, the
    // masks are hypotheses and must not be treated as copied ground truth.
    bool holesCertain = false;
};

struct TimelineStep {
    double startSeconds = 0;
    double timeSeconds = 0;
    // This is the active piece of the phase, matching `active` in the
    // original browser implementation.
    Cell piece = Cell::Empty;
    std::string action;
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    Board observed{};
    Confidence confidence{};
    BoardProbabilities probabilities{};
    Board board{};
    Board fullBoard{};
    GarbageRise garbage;
    // The exact lock that produced this state.  Keeping it alongside the
    // board makes an approved reconstruction a direct supervised label rather
    // than a board-difference heuristic during dataset preparation.
    Cell placedPiece = Cell::Empty;
    int placementX = 0;
    int placementY = 0;
    int placementRotation = 0;
    int clearedLines = 0;
    double score = 0;
    bool uncertain = false;
    // Internal, anomaly-only request for the board solver to compare all
    // seven minos (and a no-lock hypothesis). This must not reuse
    // `uncertain`: ordinary low-confidence observations legitimately set that
    // flag and must retain the historical single-piece fast path.
    bool pieceIdentityFlexible = false;
    bool manuallyFixed = false;
    // Automatic legal-move recovery stopped before this state.  The board is
    // only the last known legal board carried forward for review; it must
    // never be exported as a recovered placement.  A reviewer can fix an
    // earlier legal candidate and re-run the downstream segment.
    bool recoveryUnresolved = false;
    // Adaptive per-frame sampling has selected the order of the commuting
    // identical-piece pair beginning at this step. This automatic decision
    // must survive the split GPU -> review JSON so it is not warned again.
    bool orderAutomaticallyResolved = false;
    // The active piece was obtained by pressing HOLD before this phase.  The
    // flag belongs to the page state, while the operation for the page is the
    // lock that produces the following state.
    bool holdUsed = false;
    // The queue/hold fields were explicitly corrected in the review UI.
    // This is separate from manuallyFixed, which means a legal board move.
    bool queueManuallyFixed = false;
};

struct CandidateMove {
    Board board{};
    Board fullBoard{};
    Cell piece = Cell::Empty;
    int x = 0;
    int y = 0;
    int rotation = 0;
    int clearedLines = 0;
    std::array<std::array<int, 2>, 4> cells{};
    int cellCount = 0;
};

// A selectable candidate in the manual-correction editor.  `move.board` is
// the board after line clears and `move.fullBoard` is the landing board before
// clears, so the final simulator timeline can retain clear animations.
struct CorrectionCandidate {
    CandidateMove move{};
    GarbageRise garbage;
    double observationScore = 0;
};

// A pair of consecutive identical minos whose placement order cannot be
// distinguished reliably from the intermediate video observation.  The
// The alternate path reaches the exact same endpoint after the second mino.
// The adaptive recovery pass can therefore select it directly without
// rewriting any later board or manual anchor.
struct PlacementOrderAmbiguity {
    std::size_t firstStep = 0;
    std::size_t secondStep = 0;
    double selectedScore = 0;
    double alternateScore = 0;
    CandidateMove alternateFirstMove{};
};

struct BoardShift {
    int lines = 0;
    double ratio = 0;
};

struct Status {
    std::atomic<int> progress{0};
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::atomic<bool> cancel{false};
    mutable std::mutex messageMutex;
    std::string message;
    std::filesystem::path outputDirectory;

    void setMessage(const std::string& value) { std::lock_guard<std::mutex> lock(messageMutex); message = value; }
    std::string getMessage() const { std::lock_guard<std::mutex> lock(messageMutex); return message; }
};

} // namespace tr
