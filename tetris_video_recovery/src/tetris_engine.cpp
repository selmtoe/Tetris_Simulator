#include "tetris_engine.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <set>
#include <unordered_set>

namespace tr {

namespace {

struct ReachState { int x; int y; int rotation; };

std::uint64_t stateKey(int x, int y, int rotation) {
    return (static_cast<std::uint64_t>(rotation & 3) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 16) |
           static_cast<std::uint16_t>(y);
}

std::string boardKey(const Board& board) {
    return std::string(reinterpret_cast<const char*>(board.data()), board.size());
}

using OriginalShape = std::array<std::array<int, 2>, 4>;

OriginalShape originalShape(std::initializer_list<std::array<int, 2>> cells) {
    OriginalShape result{};
    std::copy(cells.begin(), cells.end(), result.begin());
    return result;
}

std::vector<OriginalShape> originalShapes(Cell piece) {
    switch (piece) {
    case Cell::I:
        return {originalShape({{-1,0},{0,0},{1,0},{2,0}}), originalShape({{0,-1},{0,0},{0,1},{0,2}})};
    case Cell::J:
        return {originalShape({{-1,-1},{-1,0},{0,0},{1,0}}), originalShape({{0,-1},{1,-1},{0,0},{0,1}}),
                originalShape({{-1,0},{0,0},{1,0},{1,1}}), originalShape({{0,-1},{0,0},{-1,1},{0,1}})};
    case Cell::L:
        return {originalShape({{1,-1},{-1,0},{0,0},{1,0}}), originalShape({{0,-1},{0,0},{0,1},{1,1}}),
                originalShape({{-1,0},{0,0},{1,0},{-1,1}}), originalShape({{-1,-1},{0,-1},{0,0},{0,1}})};
    case Cell::O:
        return {originalShape({{0,0},{1,0},{0,1},{1,1}})};
    case Cell::S:
        return {originalShape({{0,-1},{1,-1},{-1,0},{0,0}}), originalShape({{0,-1},{0,0},{1,0},{1,1}})};
    case Cell::T:
        return {originalShape({{0,-1},{-1,0},{0,0},{1,0}}), originalShape({{0,-1},{0,0},{1,0},{0,1}}),
                originalShape({{-1,0},{0,0},{1,0},{0,1}}), originalShape({{0,-1},{-1,0},{0,0},{0,1}})};
    case Cell::Z:
        return {originalShape({{-1,-1},{0,-1},{0,0},{1,0}}), originalShape({{1,-1},{0,0},{1,0},{0,1}})};
    default:
        return {};
    }
}

BoardShift sourceBoardShift(const Board& previous, const Board& observed, double threshold) {
    std::string previousPattern;
    std::string observedPattern;
    const auto appendPattern = [](std::string& pattern, const Board& board, int row) {
        int garbage = 0;
        int filled = 0;
        std::string holes;
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell cell = board[index(x, row)];
            if (cell == Cell::Garbage) ++garbage;
            if (cell != Cell::Empty) ++filled;
            if (cell != Cell::Garbage) {
                if (!holes.empty()) holes += ',';
                holes += std::to_string(x);
            }
        }
        if (garbage > 0 && filled < BoardWidth) pattern += holes + '|';
    };
    for (int y = BoardHeight - 1; y >= 0; --y) {
        appendPattern(previousPattern, previous, y);
        appendPattern(observedPattern, observed, y);
    }
    if (!previousPattern.empty() && previousPattern == observedPattern) return {};

    BoardShift best;
    for (int shift = 1; shift <= VisibleRows; ++shift) {
        int matched = 0;
        int blocks = 0;
        for (int y = VisibleRows + shift; y < BoardHeight; ++y) {
            for (int x = 0; x < BoardWidth; ++x) {
                const Cell oldCell = previous[index(x, y)];
                if (oldCell != Cell::Empty && oldCell != Cell::Garbage) {
                    ++blocks;
                    if (oldCell == observed[index(x, y - shift)]) ++matched;
                }
            }
        }
        int garbage = 0;
        for (int y = BoardHeight - shift; y < BoardHeight; ++y) {
            for (int x = 0; x < BoardWidth; ++x) {
                if (observed[index(x, y)] == Cell::Garbage) ++garbage;
            }
        }
        if (blocks > 0 && garbage > 0) {
            const double ratio = static_cast<double>(matched) / blocks;
            if (ratio >= threshold && ratio > best.ratio) best = {shift, ratio};
        }
    }
    return best;
}

constexpr std::uint16_t FullBoardMask = static_cast<std::uint16_t>((1u << BoardWidth) - 1u);

std::uint16_t normalizedHoleMask(std::uint16_t mask) {
    const std::uint16_t candidates = static_cast<std::uint16_t>(mask & FullBoardMask);
    // A noisy observed row can expose several apparent gaps.  The game rule
    // for this recovery mode is one garbage hole per row, so choose one
    // deterministic candidate rather than carrying an impossible row into
    // legal-move generation.  Center is the neutral fallback when no gap was
    // visible at all.
    int selected = 4;
    int bestDistance = BoardWidth + 1;
    for (int x = 0; x < BoardWidth; ++x) {
        if ((candidates & (1u << x)) == 0) continue;
        const int distance = std::abs(x - 4);
        if (distance < bestDistance) {
            selected = x;
            bestDistance = distance;
        }
    }
    return static_cast<std::uint16_t>(1u << selected);
}

std::vector<std::uint16_t> holesFromObserved(const Board& observed, int lines) {
    lines = std::clamp(lines, 0, BoardHeight);
    std::vector<std::uint16_t> masks;
    masks.reserve(static_cast<std::size_t>(lines));
    for (int row = BoardHeight - lines; row < BoardHeight; ++row) {
        std::uint16_t candidates = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            // Only confirmed garbage is solid. An effect can make several
            // cells look open, but normalizedHoleMask enforces the actual
            // one-hole-per-row garbage rule before it reaches the editor or
            // legal-move generator.
            if (observed[index(x, row)] != Cell::Garbage) {
                candidates = static_cast<std::uint16_t>(candidates | (1u << x));
            }
        }
        masks.push_back(normalizedHoleMask(candidates));
    }
    return masks;
}

Board boardWithManualGarbage(const Board& previous, const GarbageRise& requested);

Board sourceShiftedBoard(const Board& previous, const Board& observed, int lines) {
    if (lines <= 0) return previous;
    GarbageRise rise;
    rise.lines = std::clamp(lines, 0, BoardHeight);
    rise.holeMasks = holesFromObserved(observed, rise.lines);
    return boardWithManualGarbage(previous, rise);
}

Board boardWithManualGarbage(const Board& previous, const GarbageRise& requested) {
    const int lines = std::clamp(requested.lines, 0, BoardHeight);
    if (lines == 0) return previous;
    Board shifted{};
    for (int y = 0; y < BoardHeight - lines; ++y) {
        for (int x = 0; x < BoardWidth; ++x) shifted[index(x, y)] = previous[index(x, y + lines)];
    }
    for (int row = 0; row < lines; ++row) {
        // A malformed/incomplete manual request should remain visibly
        // repairable instead of silently creating a solid garbage line.
        const std::uint16_t holes = row < static_cast<int>(requested.holeMasks.size())
            ? normalizedHoleMask(requested.holeMasks[static_cast<std::size_t>(row)])
            : static_cast<std::uint16_t>(1u << 4);
        const int y = BoardHeight - lines + row;
        for (int x = 0; x < BoardWidth; ++x) {
            shifted[index(x, y)] = (holes & (1u << x)) ? Cell::Empty : Cell::Garbage;
        }
    }
    return shifted;
}

GarbageRise automaticGarbageRise(const Board& observed, const BoardShift& shift) {
    GarbageRise rise;
    rise.lines = shift.lines;
    rise.holeMasks = holesFromObserved(observed, shift.lines);
    rise.matchRatio = shift.ratio;
    return rise;
}

double sourceScore(const Board& predicted, const Board& observed, const ScoreWeights& weights) {
    double value = 0;
    for (int y = 0; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell predictedCell = predicted[index(x, y)];
            const Cell observedCell = observed[index(x, y)];
            if (predictedCell == observedCell) {
                value += predictedCell == Cell::Empty ? weights.emptyMatch : weights.exactMatch;
            } else if (predictedCell != Cell::Empty && observedCell != Cell::Empty) {
                value += weights.colorMismatch;
            } else if (predictedCell == Cell::Empty) {
                value += weights.extraBlock;
            } else {
                value += weights.missingBlock;
            }
        }
    }
    return value;
}

// ONNX predicts the 10x20 visible playfield only. The hidden rows are not
// evidence, so an exact match deliberately compares just those 200 labels.
// This also keeps a just-cleared pre-clear row out of the comparison.
bool exactVisibleOnnxMatch(const Board& predicted, const Board& observed) {
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            if (predicted[index(x, y)] != observed[index(x, y)]) return false;
        }
    }
    return true;
}

} // namespace

TetrisEngine::Shape TetrisEngine::shape(Cell piece, int rotation) {
    Shape result;
    rotation &= 3;
    std::array<std::array<int, 2>, 4> base{};
    double centerX = 0, centerY = 0;
    switch (piece) {
    case Cell::I: base = {{{0,0},{1,0},{2,0},{3,0}}}; centerX = 1.5; centerY = .5; break;
    case Cell::O: base = {{{0,0},{1,0},{0,-1},{1,-1}}}; centerX = .5; centerY = -.5; break;
    case Cell::T: base = {{{0,0},{-1,0},{0,-1},{1,0}}}; break;
    case Cell::L: base = {{{-1,0},{0,0},{1,0},{1,-1}}}; break;
    case Cell::J: base = {{{0,0},{-1,0},{1,0},{-1,-1}}}; break;
    case Cell::S: base = {{{1,-1},{-1,0},{0,0},{0,-1}}}; break;
    case Cell::Z: base = {{{0,0},{1,0},{0,-1},{-1,-1}}}; break;
    default: return result;
    }
    if (piece == Cell::O || rotation == 0) {
        result.cells = base;
        return result;
    }
    for (std::size_t i = 0; i < base.size(); ++i) {
        double x = base[i][0] - centerX;
        double y = base[i][1] - centerY;
        for (int r = 0; r < rotation; ++r) {
            const double oldX = x;
            x = -y;
            y = oldX;
        }
        result.cells[i] = {static_cast<int>(std::lround(x + centerX)), static_cast<int>(std::lround(y + centerY))};
    }
    return result;
}

bool TetrisEngine::fits(const Board& board, const Shape& currentShape, int x, int y) {
    for (const auto& cell : currentShape.cells) {
        const int nx = x + cell[0];
        const int ny = y + cell[1];
        if (nx < 0 || nx >= BoardWidth || ny >= BoardHeight) return false;
        if (ny >= 0 && isOccupied(board[index(nx, ny)])) return false;
    }
    return true;
}

std::array<int, 2> TetrisEngine::spawnPosition(Cell piece) {
    // This follows simulator/app/player-engine.js exactly:
    // floor(BOARD_WIDTH / 2) - floor(center.x) - 1.
    return piece == Cell::I ? std::array<int,2>{3, 20} : std::array<int,2>{4, 20};
}

std::vector<TetrisEngine::Offset> TetrisEngine::rotationOffsets(Cell piece, int from, int to) {
    from &= 3; to &= 3;
    if ((to - from + 4) % 4 == 1) {
        if (piece == Cell::I) {
            switch (from) {
            case 0: return {{0,0},{-2,0},{1,0},{-2,-1},{1,2}};
            case 1: return {{0,0},{-1,0},{2,0},{-1,2},{2,-1}};
            case 2: return {{0,0},{2,0},{-1,0},{2,1},{-1,-2}};
            default: return {{0,0},{1,0},{-2,0},{1,-2},{-2,1}};
            }
        }
        switch (from) {
        case 0: return {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}};
        case 1: return {{0,0},{1,0},{1,-1},{0,2},{1,2}};
        case 2: return {{0,0},{1,0},{1,1},{0,-2},{1,-2}};
        default: return {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}};
        }
    }
    // The simulator uses these exact reverse-transition values for CCW rotation.
    if (piece == Cell::I) {
        switch (from) {
        case 0: return {{0,0},{-1,0},{2,0},{-1,2},{2,-1}};
        case 1: return {{0,0},{2,0},{-1,0},{2,1},{-1,-2}};
        case 2: return {{0,0},{1,0},{-2,0},{1,-2},{-2,1}};
        default: return {{0,0},{-2,0},{1,0},{-2,-1},{1,2}};
        }
    }
    switch (from) {
    case 0: return {{0,0},{1,0},{1,1},{0,-2},{1,-2}};
    case 1: return {{0,0},{1,0},{1,-1},{0,2},{1,2}};
    case 2: return {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}};
    default: return {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}};
    }
}

CandidateMove TetrisEngine::lock(const Board& board, Cell piece, const Shape& currentShape, int x, int y, int rotation) {
    CandidateMove result;
    result.board = board;
    result.fullBoard = board;
    result.piece = piece; result.x = x; result.y = y; result.rotation = rotation;
    for (int i = 0; i < 4; ++i) {
        const int nx = x + currentShape.cells[i][0];
        const int ny = y + currentShape.cells[i][1];
        result.cells[i] = {nx, ny};
        result.cellCount++;
        if (ny >= 0 && ny < BoardHeight) result.fullBoard[index(nx, ny)] = piece;
    }
    int write = BoardHeight - 1;
    for (int row = BoardHeight - 1; row >= 0; --row) {
        bool full = true;
        for (int xCell = 0; xCell < BoardWidth; ++xCell) if (!isOccupied(result.fullBoard[index(xCell, row)])) { full = false; break; }
        if (full) { result.clearedLines++; continue; }
        for (int xCell = 0; xCell < BoardWidth; ++xCell) result.board[index(xCell, write)] = result.fullBoard[index(xCell, row)];
        --write;
    }
    while (write >= 0) {
        for (int xCell = 0; xCell < BoardWidth; ++xCell) result.board[index(xCell, write)] = Cell::Empty;
        --write;
    }
    return result;
}

std::vector<CandidateMove> TetrisEngine::originalLegalMoves(const Board& board, Cell piece) {
    // Verbatim behavior of generateLegalMoves() from 動画解析.html: enumerate
    // every in-bounds grounded orientation/anchor, then clear full rows.
    std::vector<CandidateMove> moves;
    if (!isPiece(piece)) return moves;
    const auto shapes = originalShapes(piece);
    for (int rotation = 0; rotation < static_cast<int>(shapes.size()); ++rotation) {
        const auto& current = shapes[rotation];
        for (int x = 0; x < BoardWidth; ++x) {
            for (int y = 0; y < BoardHeight; ++y) {
                bool canPlace = true;
                for (const auto& [dx, dy] : current) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || nx >= BoardWidth || ny < 0 || ny >= BoardHeight || isOccupied(board[index(nx, ny)])) {
                        canPlace = false;
                        break;
                    }
                }
                if (!canPlace) continue;

                bool grounded = false;
                for (const auto& [dx, dy] : current) {
                    const int nx = x + dx;
                    const int ny = y + dy + 1;
                    if (ny >= BoardHeight || (ny >= 0 && isOccupied(board[index(nx, ny)]))) {
                        grounded = true;
                        break;
                    }
                }
                if (!grounded) continue;

                CandidateMove move;
                move.board = board;
                move.fullBoard = board;
                move.piece = piece;
                move.x = x;
                move.y = y;
                move.rotation = rotation;
                for (int i = 0; i < 4; ++i) {
                    const int nx = x + current[i][0];
                    const int ny = y + current[i][1];
                    move.cells[i] = {nx, ny};
                    move.fullBoard[index(nx, ny)] = piece;
                    ++move.cellCount;
                }

                int write = BoardHeight - 1;
                for (int row = BoardHeight - 1; row >= 0; --row) {
                    bool full = true;
                    for (int column = 0; column < BoardWidth; ++column) {
                        if (!isOccupied(move.fullBoard[index(column, row)])) { full = false; break; }
                    }
                    if (full) { ++move.clearedLines; continue; }
                    for (int column = 0; column < BoardWidth; ++column) {
                        move.board[index(column, write)] = move.fullBoard[index(column, row)];
                    }
                    --write;
                }
                while (write >= 0) {
                    for (int column = 0; column < BoardWidth; ++column) move.board[index(column, write)] = Cell::Empty;
                    --write;
                }
                moves.push_back(std::move(move));
            }
        }
    }
    return moves;
}

std::vector<CandidateMove> TetrisEngine::legalMoves(const Board& board, Cell piece) {
    std::vector<CandidateMove> moves;
    if (!isPiece(piece)) return moves;
    const auto spawn = spawnPosition(piece);
    const Shape spawnShape = shape(piece, 0);
    if (!fits(board, spawnShape, spawn[0], spawn[1])) return moves;

    std::deque<ReachState> pending;
    std::unordered_set<std::uint64_t> visited;
    pending.push_back({spawn[0], spawn[1], 0});
    visited.insert(stateKey(spawn[0], spawn[1], 0));
    std::unordered_set<std::string> resultBoards;

    auto enqueue = [&](int x, int y, int rotation) {
        if (!fits(board, shape(piece, rotation), x, y)) return;
        const auto key = stateKey(x, y, rotation);
        if (visited.insert(key).second) pending.push_back({x,y,rotation});
    };

    while (!pending.empty()) {
        const ReachState state = pending.front(); pending.pop_front();
        const Shape current = shape(piece, state.rotation);
        int hardY = state.y;
        while (fits(board, current, state.x, hardY + 1)) ++hardY;
        if (hardY >= -4) {
            CandidateMove move = lock(board, piece, current, state.x, hardY, state.rotation);
            const auto key = boardKey(move.board);
            if (resultBoards.insert(key).second) moves.push_back(move);
        }

        enqueue(state.x - 1, state.y, state.rotation);
        enqueue(state.x + 1, state.y, state.rotation);
        enqueue(state.x, state.y + 1, state.rotation);
        if (piece != Cell::O) {
            const int cwRotation = (state.rotation + 1) & 3;
            for (const auto& offset : rotationOffsets(piece, state.rotation, cwRotation))
                if (fits(board, shape(piece, cwRotation), state.x + offset.x, state.y - offset.y)) {
                    enqueue(state.x + offset.x, state.y - offset.y, cwRotation); break;
                }
            const int ccwRotation = (state.rotation + 3) & 3;
            for (const auto& offset : rotationOffsets(piece, state.rotation, ccwRotation))
                if (fits(board, shape(piece, ccwRotation), state.x + offset.x, state.y - offset.y)) {
                    enqueue(state.x + offset.x, state.y - offset.y, ccwRotation); break;
                }
        }
    }
    return moves;
}

std::optional<Board> TetrisEngine::garbageShift(const Board& previous, const Board& observed, int lines, double& ratio) {
    ratio = 0;
    if (lines <= 0 || lines > BoardHeight) return std::nullopt;
    int total = 0, matched = 0;
    for (int y = 0; y < BoardHeight - lines; ++y) for (int x = 0; x < BoardWidth; ++x) {
        const Cell oldCell = previous[index(x, y + lines)];
        const Cell nowCell = observed[index(x, y)];
        if (oldCell != Cell::Empty && oldCell != Cell::Garbage) { ++total; if (oldCell == nowCell || (isOccupied(nowCell) && nowCell != Cell::Garbage)) ++matched; }
    }
    int garbageCells = 0;
    for (int y = BoardHeight - lines; y < BoardHeight; ++y) for (int x = 0; x < BoardWidth; ++x) if (observed[index(x,y)] == Cell::Garbage) ++garbageCells;
    ratio = total ? static_cast<double>(matched) / total : 0;
    if (garbageCells < lines * 5 || ratio < .48) return std::nullopt;
    Board shifted{};
    for (int y = 0; y < BoardHeight - lines; ++y) for (int x = 0; x < BoardWidth; ++x) shifted[index(x,y)] = previous[index(x,y+lines)];
    for (int y = BoardHeight - lines; y < BoardHeight; ++y) for (int x = 0; x < BoardWidth; ++x) shifted[index(x,y)] = observed[index(x,y)] == Cell::Garbage ? Cell::Garbage : Cell::Empty;
    return shifted;
}

double TetrisEngine::observationScore(const Board& predicted, const Board& observed, const Confidence& confidence) {
    double score = 0;
    for (int y = 0; y < VisibleRows; ++y) for (int x = 0; x < BoardWidth; ++x) {
        const int vi = y * BoardWidth + x;
        const int bi = (y + VisibleRows) * BoardWidth + x;
        const double weight = std::clamp(confidence[vi] / 255.0, .12, 1.0);
        const Cell p = predicted[bi], o = observed[bi];
        if (p == o) score += (p == Cell::Empty ? .30 : 7.0) * weight;
        else if (isOccupied(p) && isOccupied(o)) score += 1.1 * weight;
        else if (p == Cell::Empty && o == Cell::Empty) score += .2;
        else if (p != Cell::Empty) score -= 5.2 * weight;
        else score -= 3.8 * weight;
    }
    return score;
}

BoardShift TetrisEngine::detectBoardShift(const Board& previous, const Board& observed, double threshold) {
    return sourceBoardShift(previous, observed, threshold);
}

double TetrisEngine::sourceObservationScore(const Board& predicted, const Board& observed, const ScoreWeights& weights) {
    return sourceScore(predicted, observed, weights);
}

Board TetrisEngine::applyGarbageRise(const Board& previous, const GarbageRise& rise) {
    return boardWithManualGarbage(previous, rise);
}

std::vector<std::uint16_t> TetrisEngine::garbageHolesFromObserved(const Board& observed, int lines) {
    return holesFromObserved(observed, lines);
}

std::vector<CorrectionCandidate> TetrisEngine::correctionCandidates(const std::vector<TimelineStep>& raw,
                                                                     const std::vector<TimelineStep>& solved,
                                                                     std::size_t index,
                                                                     const Settings& settings,
                                                                     const std::optional<GarbageRise>& overrideGarbage) {
    std::vector<CorrectionCandidate> candidates;
    if (index == 0 || index >= raw.size() || index >= solved.size()) return candidates;

    const TimelineStep& previousRaw = raw[index - 1];
    const TimelineStep& currentRaw = raw[index];
    const BoardShift shift = sourceBoardShift(solved[index - 1].board, currentRaw.observed, settings.shiftThreshold);
    GarbageRise rise = automaticGarbageRise(currentRaw.observed, shift);
    Board base = sourceShiftedBoard(solved[index - 1].board, currentRaw.observed, shift.lines);
    if (overrideGarbage.has_value()) {
        rise = *overrideGarbage;
        rise.lines = std::clamp(rise.lines, 0, BoardHeight);
        rise.manuallySpecified = true;
        if (rise.holeMasks.size() > static_cast<std::size_t>(rise.lines)) rise.holeMasks.resize(rise.lines);
        base = boardWithManualGarbage(solved[index - 1].board, rise);
    }

    // A step represents the state after placing the preceding active mino.
    // Keep the exact same active/next fallback as runBeamSearch().
    if (previousRaw.piece == Cell::Empty) {
        CandidateMove noPlacement;
        noPlacement.board = base;
        noPlacement.fullBoard = base;
        noPlacement.piece = Cell::Empty;
        candidates.push_back({noPlacement, rise, sourceScore(base, currentRaw.observed, settings.weights)});
        return candidates;
    }

    Cell piece = previousRaw.piece;
    if (!isPiece(piece) && !previousRaw.next.empty() && isPiece(previousRaw.next[0])) piece = previousRaw.next[0];
    if (!isPiece(piece)) return candidates;

    for (CandidateMove move : originalLegalMoves(base, piece)) {
        const double score = sourceScore(move.board, currentRaw.observed, settings.weights);
        candidates.push_back({std::move(move), rise, score});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        return left.observationScore > right.observationScore;
    });
    return candidates;
}

namespace {

struct SourceBeamNode {
    Board board{};
    Board fullBoard{};
    double score = 0;
    int parent = -1;
    std::size_t step = 0;
    GarbageRise garbage;
    CandidateMove placement;
};

CandidateMove timelinePlacement(const TimelineStep& step) {
    CandidateMove placement;
    placement.board = step.board;
    placement.fullBoard = step.fullBoard;
    placement.piece = step.placedPiece;
    placement.x = step.placementX;
    placement.y = step.placementY;
    placement.rotation = step.placementRotation;
    placement.clearedLines = step.clearedLines;
    return placement;
}

CandidateMove noPlacement(const Board& board) {
    CandidateMove placement;
    placement.board = board;
    placement.fullBoard = board;
    return placement;
}

std::vector<TimelineStep> runSourceBeam(const std::vector<TimelineStep>& raw,
                                        const Settings& settings,
                                        std::size_t anchorIndex,
                                        std::vector<TimelineStep> result) {
    if (raw.empty() || anchorIndex >= raw.size()) return result;
    if (result.size() != raw.size()) result.resize(raw.size());

    const TimelineStep anchor = result[anchorIndex];
    std::vector<SourceBeamNode> nodes;
    nodes.reserve(std::max<std::size_t>((raw.size() - anchorIndex) * static_cast<std::size_t>(settings.beamWidth), 1));
    nodes.push_back({anchor.board, anchor.fullBoard, anchor.score, -1, anchorIndex,
                     anchor.garbage, timelinePlacement(anchor)});
    std::vector<int> beam{0};

    for (std::size_t step = anchorIndex + 1; step < raw.size(); ++step) {
        const TimelineStep& item = raw[step];
        const TimelineStep& previous = raw[step - 1];
        const BoardShift shift = sourceBoardShift(previous.observed, item.observed, settings.shiftThreshold);
        const bool skipPlacement = previous.piece == Cell::Empty;
        std::vector<Cell> possible;
        if (!skipPlacement) possible.push_back(previous.piece);
        if (!skipPlacement && possible.empty() && !previous.next.empty() && isPiece(previous.next[0])) {
            possible.push_back(previous.next[0]);
        }

        std::vector<SourceBeamNode> candidates;
        candidates.reserve(beam.size() * (skipPlacement ? 1 : 40));
        if (item.manuallyFixed) {
            // A correction is an explicit anchor.  Preserve the landing board
            // before a line clear as well as the board after it.
            if (!beam.empty()) {
                const SourceBeamNode& parent = nodes[beam.front()];
                candidates.push_back({item.board, item.fullBoard, parent.score + 100000.0,
                                      beam.front(), step, item.garbage, timelinePlacement(item)});
            }
        } else {
            for (int parentIndex : beam) {
                const SourceBeamNode& parent = nodes[parentIndex];
                const Board base = sourceShiftedBoard(parent.board, item.observed, shift.lines);

                if (skipPlacement) {
                    candidates.push_back({base, base, parent.score + sourceScore(base, item.observed, settings.weights),
                                          parentIndex, step, automaticGarbageRise(item.observed, shift), noPlacement(base)});
                    continue;
                }

                for (Cell piece : possible) {
                    // This is deliberately the same candidate universe as
                    // 動画解析.html, not a looser image-only correction.
                    for (const CandidateMove& move : TetrisEngine::originalLegalMoves(base, piece)) {
                        candidates.push_back({move.board, move.fullBoard,
                                              parent.score + sourceScore(move.board, item.observed, settings.weights),
                                              parentIndex, step, automaticGarbageRise(item.observed, shift), move});
                    }
                }
            }
        }

        if (candidates.empty()) {
            // Retain the original browser fallback but do not discard an
            // already-approved prefix when this happens in a repaired tail.
            int fallbackParent = -1;
            for (std::size_t fallbackStep = anchorIndex; fallbackStep <= step; ++fallbackStep) {
                TimelineStep history = fallbackStep == anchorIndex ? anchor : raw[fallbackStep];
                if (fallbackStep != anchorIndex) {
                    history.board = history.observed;
                    history.fullBoard = history.observed;
                    history.score = 0;
                    history.garbage = {};
                }
                nodes.push_back({history.board, history.fullBoard, history.score, fallbackParent, fallbackStep,
                                 history.garbage, timelinePlacement(history)});
                fallbackParent = static_cast<int>(nodes.size() - 1);
            }
            beam.assign(1, fallbackParent);
            continue;
        }

        // An exact legal result for the current ONNX frame is a hard
        // transition constraint. Previously candidates were sorted only by
        // their cumulative score, so a perfect current board could lose to a
        // merely similar board that had scored better on older frames.
        // Explicit manual corrections remain anchors and are never replaced.
        if (!item.manuallyFixed && settings.forceExactOnnxMatch) {
            const bool hasExactOnnxCandidate = std::any_of(candidates.begin(), candidates.end(),
                [&item](const SourceBeamNode& candidate) {
                    return exactVisibleOnnxMatch(candidate.board, item.observed);
                });
            if (hasExactOnnxCandidate) {
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                    [&item](const SourceBeamNode& candidate) {
                        return !exactVisibleOnnxMatch(candidate.board, item.observed);
                    }), candidates.end());
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const SourceBeamNode& left, const SourceBeamNode& right) {
            return left.score > right.score;
        });
        if (candidates.size() > static_cast<std::size_t>(settings.beamWidth)) candidates.resize(settings.beamWidth);
        beam.clear();
        beam.reserve(candidates.size());
        for (auto& candidate : candidates) {
            nodes.push_back(std::move(candidate));
            beam.push_back(static_cast<int>(nodes.size() - 1));
        }
    }

    int node = beam.front();
    while (node >= 0) {
        const SourceBeamNode& current = nodes[node];
        TimelineStep history = current.step == anchorIndex ? anchor : raw[current.step];
        history.board = current.board;
        history.fullBoard = current.fullBoard;
        history.garbage = current.garbage;
        history.placedPiece = current.placement.piece;
        history.placementX = current.placement.x;
        history.placementY = current.placement.y;
        history.placementRotation = current.placement.rotation;
        history.clearedLines = current.placement.clearedLines;
        history.score = current.score;
        result[current.step] = std::move(history);
        node = current.parent;
    }
    return result;
}

} // namespace

std::vector<TimelineStep> TetrisEngine::beamSearch(const std::vector<TimelineStep>& raw, const Settings& settings) {
    if (raw.empty()) return {};
    std::vector<TimelineStep> result(raw.size());
    TimelineStep initial = raw.front();
    if (!initial.manuallyFixed) {
        initial.board = initial.observed;
        initial.fullBoard = initial.observed;
        initial.garbage = {};
    }
    initial.score = 0;
    result[0] = std::move(initial);
    return runSourceBeam(raw, settings, 0, std::move(result));
}

std::vector<TimelineStep> TetrisEngine::recomputeFrom(const std::vector<TimelineStep>& raw,
                                                       const std::vector<TimelineStep>& solved,
                                                       std::size_t firstStep,
                                                       const Settings& settings) {
    if (raw.empty()) return {};
    if (firstStep == 0 || firstStep >= raw.size() || solved.size() != raw.size() || !raw[firstStep].manuallyFixed) {
        return beamSearch(raw, settings);
    }

    std::vector<TimelineStep> result = solved;
    TimelineStep anchor = raw[firstStep];
    anchor.score = result[firstStep - 1].score + sourceScore(anchor.board, anchor.observed, settings.weights);
    result[firstStep] = std::move(anchor);
    return runSourceBeam(raw, settings, firstStep, std::move(result));
}

} // namespace tr
