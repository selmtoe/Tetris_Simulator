#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tr {

constexpr int BoardWidth = 10;
constexpr int BoardHeight = 40;
constexpr int VisibleRows = 20;

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

inline int index(int x, int y) { return y * BoardWidth + x; }
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
    std::int64_t time100ns = 0;
    std::vector<std::uint8_t> bgra;
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
};

struct ScoreWeights {
    double exactMatch = 10.0;
    double emptyMatch = 1.0;
    double colorMismatch = -2.0;
    double missingBlock = -5.0;
    double extraBlock = -5.0;
};

struct Settings {
    // These defaults reproduce 動画解析.html's controls.
    double sampleIntervalSeconds = 0.01;
    int queueConfirmSamples = 2;
    // NEXT is decoded over the complete video rather than confirmed row by
    // row. This beam runs over compressed queue changes, not every scan.
    int queueBeamWidth = 96;
    bool queueUseSevenBag = true;
    // If a legal placement exactly recreates the ONNX 20x10 observation,
    // image evidence is definitive for that transition. Do not let an older
    // cumulative beam score choose a merely similar board instead.
    bool forceExactOnnxMatch = true;
    int onnxSamples = 5;
    int beamWidth = 500;
    double shiftThreshold = .60;
    ScoreWeights weights;
    bool player1Enabled = true;
    bool player2Enabled = true;
    PlayerLayout p1{
        {304, 157, 366, 725}, {160, 155},
        {{{500, 122}, {500, 175}, {500, 225}, {500, 275}, {500, 325}}}
    };
    PlayerLayout p2{
        {1250, 157, 363, 725}, {790, 155},
        {{{1130, 122}, {1130, 175}, {1130, 225}, {1130, 275}, {1130, 325}}}
    };
};

struct QueueObservation {
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    std::vector<std::array<std::uint8_t, 3>> colors;
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
    // legal 0/1/2 slides and the 7-bag generator. Phase construction uses
    // this value; `observation` remains the raw evidence.
    QueueObservation decoded;
    bool sequenceCorrected = false;
    // The raw Hold colour briefly changed, but the following NEXT transition
    // proved that the change could not have been a legal hold state.
    bool holdCorrected = false;
};

struct BoardObservation {
    VisibleBoard board{};
    Confidence confidence{};
    QueueObservation queue;
    double timeSeconds = 0;
    double quality = 0;
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
    bool manuallyFixed = false;
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
