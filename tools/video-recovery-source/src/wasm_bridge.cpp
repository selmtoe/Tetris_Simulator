#ifdef __EMSCRIPTEN__

#include "recovery.hpp"
#include "onnx_model.hpp"
#include "tetris_engine.hpp"
#include "vision.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
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
using tr::BoardWidth;
using tr::Cell;
using tr::Frame;
using tr::HorizontalAlignmentScore;
using tr::QueueRecognitionSample;
using tr::QueueObservation;
using tr::RecoveryOutput;
using tr::Settings;
using tr::VisionAnalyzer;
using tr::VisibleRows;

struct Runtime {
    int width = 0;
    int height = 0;
    Frame currentFrame;
    bool frameReady = false;
    std::optional<QueueObservation> p1QueueFrame;
    std::optional<QueueObservation> p2QueueFrame;
    std::optional<tr::VisibleBoard> p1ClassicFrame;
    std::optional<tr::VisibleBoard> p2ClassicFrame;
    std::vector<int> p1AlignmentRows;
    std::vector<int> p2AlignmentRows;
    HorizontalAlignmentScore p1AlignmentScore;
    HorizontalAlignmentScore p2AlignmentScore;
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
    // The source video duration stays fixed, while a chunked browser run can
    // temporarily expose only a prefix to the recovery pipeline.
    double analysisEnd = 0;
    double analysisStart = 0;
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
        << ",\"manual\":" << (garbage.manuallySpecified ? "true" : "false")
        << ",\"holesCertain\":" << (garbage.holesCertain ? "true" : "false") << '}';
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
            << ",\"recoveryUnresolved\":"
            << (step.recoveryUnresolved ? "true" : "false")
            << ",\"orderAutomaticallyResolved\":"
            << (step.orderAutomaticallyResolved ? "true" : "false")
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
            << ",\"activeCorrected\":" << (sample.activeCorrected ? "true" : "false")
            << ",\"rejected\":" << (sample.rejected ? "true" : "false") << '}';
    }
    out << ']';
}

std::string makeReviewSnapshot() {
    std::ostringstream out;
    const auto& p1Queue = runtime.output.queueObservationsP1.empty()
        ? runtime.p1Queue : runtime.output.queueObservationsP1;
    const auto& p2Queue = runtime.output.queueObservationsP2.empty()
        ? runtime.p2Queue : runtime.output.queueObservationsP2;
    out << "{\"duration\":" << runtime.duration << ",\"p1\":{";
    out << "\"raw\":"; writeTimeline(out, runtime.output.rawP1);
    out << ",\"solved\":"; writeTimeline(out, runtime.output.p1);
    out << ",\"queue\":"; writeQueue(out, p1Queue);
    out << ",\"originalQueue\":"; writeQueue(out, runtime.output.originalQueueObservationsP1);
    out << "},\"p2\":{";
    out << "\"raw\":"; writeTimeline(out, runtime.output.rawP2);
    out << ",\"solved\":"; writeTimeline(out, runtime.output.p2);
    out << ",\"queue\":"; writeQueue(out, p2Queue);
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

Cell importedCell(int value) {
    return static_cast<Cell>(std::clamp(value, 0, static_cast<int>(Cell::Garbage)));
}

Cell importedCell(char value) {
    switch (value) {
    case 'I': return Cell::I;
    case 'L': return Cell::L;
    case 'O': return Cell::O;
    case 'Z': return Cell::Z;
    case 'T': return Cell::T;
    case 'J': return Cell::J;
    case 'S': return Cell::S;
    case 'G': return Cell::Garbage;
    default: return Cell::Empty;
    }
}

std::vector<Cell> importedPieces(const int* values, int count) {
    std::vector<Cell> result;
    if (!values || count <= 0) return result;
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const Cell value = importedCell(values[index]);
        if (tr::isPiece(value)) result.push_back(value);
    }
    return result;
}

tr::Board importedBoard(const char* value) {
    tr::Board result{};
    if (!value) return result;
    for (std::size_t index = 0; index < result.size() && value[index] != '\0'; ++index) {
        result[index] = importedCell(value[index]);
    }
    return result;
}

std::vector<tr::TimelineStep>& importedRawFor(int player) {
    return player == 1 ? runtime.output.rawP1 : runtime.output.rawP2;
}

std::vector<tr::TimelineStep>& importedSolvedFor(int player) {
    return player == 1 ? runtime.output.p1 : runtime.output.p2;
}

std::vector<QueueRecognitionSample>& importedQueueFor(int player) {
    return player == 1 ? runtime.output.queueObservationsP1 : runtime.output.queueObservationsP2;
}

std::vector<BoardObservation> importedBoardObservations(const std::vector<tr::TimelineStep>& raw) {
    std::vector<BoardObservation> result;
    result.reserve(raw.size());
    for (const auto& step : raw) {
        BoardObservation observation;
        observation.timeSeconds = step.timeSeconds > step.startSeconds
            ? (step.startSeconds + step.timeSeconds) * .5 : step.startSeconds;
        observation.confidence.fill(255);
        observation.queue.hold = step.hold;
        observation.queue.next = step.next;
        observation.quality = 1;
        for (int y = 0; y < tr::VisibleRows; ++y) {
            for (int x = 0; x < BoardWidth; ++x) {
                observation.board[static_cast<std::size_t>(y * BoardWidth + x)] =
                    step.observed[static_cast<std::size_t>((y + tr::VisibleRows) * BoardWidth + x)];
            }
        }
        result.push_back(std::move(observation));
    }
    return result;
}

tr::CandidateMove importedLock(const tr::Board& board, Cell piece,
                               const tr::TetrisEngine::Shape& shape,
                               int x, int y, int rotation) {
    tr::CandidateMove result;
    result.board = board;
    result.fullBoard = board;
    result.piece = piece;
    result.x = x;
    result.y = y;
    result.rotation = rotation & 3;
    for (std::size_t index = 0; index < shape.cells.size(); ++index) {
        const int nx = x + shape.cells[index][0];
        const int ny = y + shape.cells[index][1];
        result.cells[index] = {nx, ny};
        ++result.cellCount;
        if (ny >= 0 && ny < tr::BoardHeight) result.fullBoard[tr::index(nx, ny)] = piece;
    }
    int write = tr::BoardHeight - 1;
    for (int row = tr::BoardHeight - 1; row >= 0; --row) {
        bool full = true;
        for (int column = 0; column < BoardWidth; ++column) {
            if (!tr::isOccupied(result.fullBoard[tr::index(column, row)])) { full = false; break; }
        }
        if (full) { ++result.clearedLines; continue; }
        for (int column = 0; column < BoardWidth; ++column) {
            result.board[tr::index(column, write)] = result.fullBoard[tr::index(column, row)];
        }
        --write;
    }
    while (write >= 0) {
        for (int column = 0; column < BoardWidth; ++column) result.board[tr::index(column, write)] = Cell::Empty;
        --write;
    }
    return result;
}

int firstContinuityBreak(const std::vector<tr::TimelineStep>& timeline) {
    for (std::size_t index = 1; index < timeline.size(); ++index) {
        const auto& previous = timeline[index - 1];
        const auto& current = timeline[index];
        tr::Board expected = previous.board;
        if (tr::isPiece(current.placedPiece)) {
            const auto shape = tr::TetrisEngine::shape(current.placedPiece, current.placementRotation);
            if (!tr::TetrisEngine::fits(previous.board, shape, current.placementX, current.placementY)) {
                return static_cast<int>(index);
            }
            const auto move = importedLock(previous.board, current.placedPiece, shape,
                                           current.placementX, current.placementY,
                                           current.placementRotation);
            if (move.clearedLines != current.clearedLines || move.fullBoard != current.fullBoard) {
                return static_cast<int>(index);
            }
            expected = move.board;
        }
        expected = tr::TetrisEngine::applyGarbageRise(expected, current.garbage);
        if (expected != current.board) return static_cast<int>(index);
    }
    return -1;
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

bool uploadFrame(const std::uint8_t* rgba, int byteCount) {
    if (!validFrameInput(rgba, byteCount)) return false;
    runtime.currentFrame.width = runtime.width;
    runtime.currentFrame.height = runtime.height;
    runtime.currentFrame.time100ns = 0;
    const std::size_t pixels = static_cast<std::size_t>(runtime.width) * runtime.height;
    runtime.currentFrame.bgra.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        runtime.currentFrame.bgra[i * 4 + 0] = rgba[i * 4 + 2];
        runtime.currentFrame.bgra[i * 4 + 1] = rgba[i * 4 + 1];
        runtime.currentFrame.bgra[i * 4 + 2] = rgba[i * 4 + 0];
        runtime.currentFrame.bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    runtime.frameReady = true;
    runtime.p1QueueFrame.reset();
    runtime.p2QueueFrame.reset();
    runtime.p1ClassicFrame.reset();
    runtime.p2ClassicFrame.reset();
    runtime.p1AlignmentRows.clear();
    runtime.p2AlignmentRows.clear();
    runtime.p1AlignmentScore = {};
    runtime.p2AlignmentScore = {};
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

std::vector<QueueRecognitionSample>& recoveryQueueForPlayer(int player) {
    return player == 1 ? runtime.output.queueObservationsP1 : runtime.output.queueObservationsP2;
}

std::vector<BoardObservation>& boardsForPlayer(int player) {
    return player == 1 ? runtime.p1Boards : runtime.p2Boards;
}

std::vector<double>& boardTimesForPlayer(int player) {
    return player == 1 ? runtime.p1BoardTimes : runtime.p2BoardTimes;
}

std::vector<int>& alignmentRowsForPlayer(int player) {
    return player == 1 ? runtime.p1AlignmentRows : runtime.p2AlignmentRows;
}

HorizontalAlignmentScore& alignmentScoreForPlayer(int player) {
    return player == 1 ? runtime.p1AlignmentScore : runtime.p2AlignmentScore;
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

std::vector<Cell> boardLabels(const std::uint8_t* classLabels, int labelCount) {
    std::vector<Cell> labels(BoardWidth * VisibleRows, Cell::Empty);
    if (!classLabels || labelCount < BoardWidth * VisibleRows) return labels;
    for (int i = 0; i < BoardWidth * VisibleRows; ++i) {
        labels[static_cast<std::size_t>(i)] = labelToCell(classLabels[i]);
    }
    return labels;
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
    runtime.analysisEnd = durationSeconds;
    runtime.analysisStart = 0;
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
int tr_review_import_begin(double durationSeconds) {
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0) {
        return fail("Imported review has no positive duration");
    }
    runtime.duration = durationSeconds;
    runtime.analysisStart = 0;
    runtime.analysisEnd = durationSeconds;
    runtime.p1Queue.clear();
    runtime.p2Queue.clear();
    runtime.p1BoardTimes.clear();
    runtime.p2BoardTimes.clear();
    runtime.p1Boards.clear();
    runtime.p2Boards.clear();
    runtime.requestsPrepared = false;
    runtime.output = RecoveryOutput{};
    runtime.output.videoDurationSeconds = durationSeconds;
    runtime.output.analyzedUntilSeconds = durationSeconds;
    runtime.reviewJson.clear();
    runtime.candidateJson.clear();
    runtime.error.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_import_timeline_step(
    int player, double startSeconds, double timeSeconds, int piece, const char* action,
    int hold, const int* next, int nextCount, const char* observed, const char* board,
    const char* fullBoard, int garbageLines, const std::uint16_t* garbageMasks,
    int garbageMaskCount, double garbageMatchRatio, int garbageFlags, int placedPiece,
    int placementX, int placementY, int placementRotation, int clearedLines, double score,
    int flags, int holdUsed) {
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    if (!std::isfinite(startSeconds) || !std::isfinite(timeSeconds) ||
        startSeconds < 0 || timeSeconds < startSeconds) {
        return fail("Imported timeline time range is invalid");
    }

    tr::TimelineStep step;
    step.startSeconds = startSeconds;
    step.timeSeconds = timeSeconds;
    step.piece = importedCell(piece);
    step.action = action ? action : "";
    step.hold = importedCell(hold);
    step.next = importedPieces(next, std::max(0, nextCount));
    step.observed = importedBoard(observed);
    step.board = importedBoard(board);
    step.fullBoard = importedBoard(fullBoard);
    step.confidence.fill(255);
    step.garbage.lines = std::clamp(garbageLines, 0, tr::VisibleRows);
    step.garbage.matchRatio = std::isfinite(garbageMatchRatio) ? garbageMatchRatio : 0;
    step.garbage.manuallySpecified = (garbageFlags & 1) != 0;
    step.garbage.holesCertain = (garbageFlags & 2) != 0;
    if (garbageMasks && garbageMaskCount > 0) {
        const int count = std::min({garbageMaskCount, step.garbage.lines, tr::VisibleRows});
        step.garbage.holeMasks.assign(garbageMasks, garbageMasks + count);
    }
    step.placedPiece = importedCell(placedPiece);
    step.placementX = placementX;
    step.placementY = placementY;
    step.placementRotation = placementRotation & 3;
    step.clearedLines = std::max(0, clearedLines);
    step.score = std::isfinite(score) ? score : 0;
    step.manuallyFixed = (flags & 1) != 0;
    step.queueManuallyFixed = (flags & 2) != 0;
    step.orderAutomaticallyResolved = (flags & 4) != 0;
    step.recoveryUnresolved = (flags & 8) != 0;
    step.holdUsed = holdUsed != 0;

    importedSolvedFor(player).push_back(step);
    step.recoveryUnresolved = false;
    if (!step.manuallyFixed) {
        step.board = step.observed;
        step.fullBoard = step.observed;
        step.garbage = {};
        step.placedPiece = Cell::Empty;
        step.placementX = 0;
        step.placementY = 0;
        step.placementRotation = 0;
        step.clearedLines = 0;
        step.score = 0;
    }
    importedRawFor(player).push_back(std::move(step));
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_import_queue_sample(int player, double timeSeconds, int active, int hold,
                                  const int* next, int nextCount, int decodedHold,
                                  const int* decodedNext, int decodedNextCount,
                                  int flags, int nextUnclearMask) {
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    if (!std::isfinite(timeSeconds) || timeSeconds < 0) return fail("Imported queue time is invalid");
    QueueRecognitionSample sample;
    sample.timeSeconds = timeSeconds;
    sample.active = importedCell(active);
    sample.observation.hold = importedCell(hold);
    sample.observation.next = importedPieces(next, std::max(0, nextCount));
    sample.decoded.hold = importedCell(decodedHold);
    sample.decoded.next = importedPieces(decodedNext, std::max(0, decodedNextCount));
    sample.stable = (flags & 1) != 0;
    sample.manuallyEdited = (flags & 2) != 0;
    sample.sequenceCorrected = (flags & 4) != 0;
    sample.holdCorrected = (flags & 8) != 0;
    sample.activeCorrected = (flags & 16) != 0;
    sample.observation.cnnUsed = (flags & 32) != 0;
    sample.observation.holdUnclear = (flags & 64) != 0;
    sample.rejected = (flags & 128) != 0;
    for (std::size_t index = 0; index < sample.observation.nextUnclear.size(); ++index) {
        sample.observation.nextUnclear[index] = (nextUnclearMask & (1 << index)) != 0;
    }
    if (sample.manuallyEdited) sample.manualRevision = ++runtime.output.queueEditRevision;
    importedQueueFor(player).push_back(std::move(sample));
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_import_finish() {
    if (runtime.output.rawP1.empty() && runtime.output.rawP2.empty()) {
        return fail("Imported review has no timeline rows");
    }
    runtime.output.originalQueueObservationsP1 = runtime.output.queueObservationsP1;
    runtime.output.originalQueueObservationsP2 = runtime.output.queueObservationsP2;
    runtime.p1Queue = runtime.output.queueObservationsP1;
    runtime.p2Queue = runtime.output.queueObservationsP2;
    runtime.output.boardObservationsP1 = importedBoardObservations(runtime.output.rawP1);
    runtime.output.boardObservationsP2 = importedBoardObservations(runtime.output.rawP2);
    runtime.p1Boards = runtime.output.boardObservationsP1;
    runtime.p2Boards = runtime.output.boardObservationsP2;
    runtime.output.beamCacheP1.reset();
    runtime.output.beamCacheP2.reset();
    runtime.reviewJson.clear();
    runtime.candidateJson.clear();
    runtime.error.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_review_first_discontinuity(int player) {
    if (player != 1 && player != 2) return -1;
    return firstContinuityBreak(player == 1 ? runtime.output.p1 : runtime.output.p2);
}

EMSCRIPTEN_KEEPALIVE
int tr_review_first_order_ambiguity(int player) {
    if (player != 1 && player != 2) return -1;
    const auto& raw = player == 1 ? runtime.output.rawP1 : runtime.output.rawP2;
    const auto& solved = player == 1 ? runtime.output.p1 : runtime.output.p2;
    const auto issues = tr::TetrisEngine::placementOrderAmbiguities(raw, solved, runtime.settings);
    return issues.empty() ? -1 : static_cast<int>(issues.front().firstStep);
}

EMSCRIPTEN_KEEPALIVE
int tr_runtime_set_analysis_end(double endSeconds) {
    if (!(runtime.duration > 0)) return fail("Video duration must be set before choosing an analysis range");
    if (!(endSeconds > 0)) return fail("Analysis end must be positive");
    runtime.analysisEnd = std::clamp(endSeconds, 0.001, runtime.duration);
    runtime.requestsPrepared = false;
    runtime.p1BoardTimes.clear();
    runtime.p2BoardTimes.clear();
    runtime.error.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_runtime_prepare_continuation(double startSeconds, double endSeconds) {
    if (!(runtime.duration > 0)) return fail("Video duration must be set before choosing a continuation range");
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) || startSeconds < 0 || endSeconds <= startSeconds) {
        return fail("Continuation range is invalid");
    }
    runtime.analysisStart = std::clamp(startSeconds, 0.0, runtime.duration);
    runtime.analysisEnd = std::clamp(endSeconds, runtime.analysisStart, runtime.duration);
    if (runtime.analysisEnd <= runtime.analysisStart + .000001) return fail("Continuation range is empty");
    const auto keepBefore = [startSeconds = runtime.analysisStart](const auto& sample) {
        return sample.timeSeconds < startSeconds - .000001;
    };
    runtime.p1Queue.erase(std::remove_if(runtime.p1Queue.begin(), runtime.p1Queue.end(),
                                         [&](const auto& sample) { return !keepBefore(sample); }),
                          runtime.p1Queue.end());
    runtime.p2Queue.erase(std::remove_if(runtime.p2Queue.begin(), runtime.p2Queue.end(),
                                         [&](const auto& sample) { return !keepBefore(sample); }),
                          runtime.p2Queue.end());
    runtime.p1Boards.erase(std::remove_if(runtime.p1Boards.begin(), runtime.p1Boards.end(),
                                          [&](const auto& sample) { return !keepBefore(sample); }),
                           runtime.p1Boards.end());
    runtime.p2Boards.erase(std::remove_if(runtime.p2Boards.begin(), runtime.p2Boards.end(),
                                          [&](const auto& sample) { return !keepBefore(sample); }),
                           runtime.p2Boards.end());
    runtime.requestsPrepared = false;
    runtime.p1BoardTimes.clear();
    runtime.p2BoardTimes.clear();
    runtime.error.clear();
    return 1;
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
    auto& cached = player == 1 ? runtime.p1QueueFrame : runtime.p2QueueFrame;
    if (!cached.has_value()) cached = vision->observeQueue(runtime.currentFrame);
    const QueueObservation& observation = *cached;
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
        if (!tr::prepareObservationRequests(runtime.analysisEnd > 0 ? runtime.analysisEnd : runtime.duration, runtime.settings,
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
int tr_board_features_shift(int player, const std::uint8_t* rgba, int byteCount,
                            double horizontalShiftPixels, float* output, int outputCapacity) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !output || outputCapacity < 200 * 63) return 0;
    if (!runtime.frameReady && !uploadFrame(rgba, byteCount)) return 0;
    auto input = vision->boardAnalysisInput(runtime.currentFrame, horizontalShiftPixels);
    auto& cachedClassic = player == 1 ? runtime.p1ClassicFrame : runtime.p2ClassicFrame;
    cachedClassic = std::move(input.classic);
    std::copy(input.features.begin(), input.features.end(), output);
    return static_cast<int>(input.features.size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_features(int player, const std::uint8_t* rgba, int byteCount,
                      float* output, int outputCapacity) {
    return tr_board_features_shift(player, rgba, byteCount, 0, output, outputCapacity);
}

EMSCRIPTEN_KEEPALIVE
double tr_board_alignment_score(int player, const std::uint8_t* classLabels,
                                int labelCount, int initializeRows) {
    VisionAnalyzer* vision = visionForPlayer(player);
    auto& cachedClassic = player == 1 ? runtime.p1ClassicFrame : runtime.p2ClassicFrame;
    if (!vision || !cachedClassic.has_value() || !classLabels || labelCount < 200) {
        fail("Board alignment score is missing its image or labels");
        return -1.0e15;
    }
    auto& rows = alignmentRowsForPlayer(player);
    if (initializeRows != 0) rows = tr::horizontalAlignmentRows(*cachedClassic);
    const auto labels = boardLabels(classLabels, labelCount);
    const BoardObservation observation =
        vision->analyzeBoardWithLabels(runtime.currentFrame, labels, *cachedClassic, false);
    auto& score = alignmentScoreForPlayer(player);
    score = tr::scoreHorizontalAlignment(*cachedClassic, observation, rows);
    return static_cast<double>(score.value());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_alignment_needs_retry(int player) {
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    return alignmentScoreForPlayer(player).needsRetry() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int tr_board_finish_shift(int player, const std::uint8_t* rgba, int byteCount,
                          double timeSeconds, double horizontalShiftPixels,
                          const std::uint8_t* classLabels, int labelCount) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !classLabels || labelCount < 200) return 0;
    if (!runtime.frameReady && !uploadFrame(rgba, byteCount)) return 0;
    const auto labels = boardLabels(classLabels, labelCount);
    BoardObservation observation = vision->analyzeBoardWithLabels(
        runtime.currentFrame, labels, horizontalShiftPixels, true);
    observation.timeSeconds = timeSeconds;
    boardsForPlayer(player).push_back(std::move(observation));
    return static_cast<int>(boardsForPlayer(player).size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_finish(int player, const std::uint8_t* rgba, int byteCount, double timeSeconds,
                    const std::uint8_t* classLabels, int labelCount) {
    return tr_board_finish_shift(player, rgba, byteCount, timeSeconds, 0,
                                 classLabels, labelCount);
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
int tr_recover_prepare() {
    if (runtime.duration <= 0) return fail("Video duration must be set before recovery");
    if (!tr::prepareRecoveryObservations(runtime.analysisEnd > 0 ? runtime.analysisEnd : runtime.duration, runtime.settings,
                                         runtime.p1Queue, runtime.p2Queue,
                                         runtime.p1Boards, runtime.p2Boards,
                                         runtime.output, runtime.error)) return 0;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_recover_prepare_continuation() {
    if (runtime.duration <= 0) return fail("Video duration must be set before recovery");
    if (!tr::prepareRecoveryObservations(runtime.analysisEnd > 0 ? runtime.analysisEnd : runtime.duration,
                                         runtime.settings, runtime.p1Queue, runtime.p2Queue,
                                         runtime.p1Boards, runtime.p2Boards, runtime.output,
                                         runtime.error, true, runtime.analysisStart)) return 0;
    runtime.reviewJson.clear();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_recover_player(int player) {
    if (!tr::solveRecoveryPlayer(runtime.output, player, runtime.settings, runtime.error)) return 0;
    runtime.reviewJson.clear();
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
    sample.activeCorrected = false;
    sample.manualRevision = ++runtime.output.queueEditRevision;
    auto& recoverySamples = recoveryQueueForPlayer(player);
    if (static_cast<std::size_t>(sampleIndex) < recoverySamples.size()) {
        recoverySamples[static_cast<std::size_t>(sampleIndex)] = sample;
    }
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
    auto& recoverySamples = recoveryQueueForPlayer(player);
    if (static_cast<std::size_t>(sampleIndex) < recoverySamples.size()) {
        recoverySamples[static_cast<std::size_t>(sampleIndex)] = samples[static_cast<std::size_t>(sampleIndex)];
    }
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
    if (firstContinuityBreak(runtime.output.p1) >= 0 ||
        firstContinuityBreak(runtime.output.p2) >= 0) {
        return fail("A discontinuous board transition must be corrected before output");
    }
    if (!tr::TetrisEngine::placementOrderAmbiguities(
            runtime.output.rawP1, runtime.output.p1, runtime.settings).empty() ||
        !tr::TetrisEngine::placementOrderAmbiguities(
            runtime.output.rawP2, runtime.output.p2, runtime.settings).empty()) {
        return fail("An ambiguous identical-piece placement order must be fixed before output");
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
    case 9: path = &runtime.output.trainingIndexPath; break;
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
