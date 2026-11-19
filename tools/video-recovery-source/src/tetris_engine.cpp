#include "tetris_engine.hpp"
#include "neural_ranker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace tr {

namespace {

struct ReachState { int x; int y; int rotation; };

std::uint64_t stateKey(int x, int y, int rotation) {
    // x and y are signed because SRS anchors may sit just outside the field.
    // Keep their bit ranges disjoint from rotation; the previous packing
    // overlapped rotation with the upper bits of a negative x coordinate.
    return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 18) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(y)) << 2) |
           static_cast<std::uint64_t>(rotation & 3);
}

using OriginalShape = std::array<std::array<int, 2>, 4>;

OriginalShape placementKey(const TetrisEngine::Shape& shape, int x, int y) {
    OriginalShape cells{};
    for (std::size_t i = 0; i < shape.cells.size(); ++i) {
        cells[i] = {x + shape.cells[i][0], y + shape.cells[i][1]};
    }
    std::sort(cells.begin(), cells.end());
    return cells;
}

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
        // Keep the source-compatible candidate cells. canonicalAnchor() below
        // converts their anchor to the browser/simulator shape convention
        // before the placement is exported.
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

bool sameCells(const std::array<std::array<int, 2>, 4>& left,
               const std::array<std::array<int, 2>, 4>& right) {
    auto sorted = [](auto cells) {
        std::sort(cells.begin(), cells.end());
        return cells;
    };
    return sorted(left) == sorted(right);
}

std::optional<std::uint16_t> exactGarbageHole(const Board& observed, int row);
bool hasProbabilityEvidence(const BoardProbabilities& probabilities);

struct RowMaskSequence {
    std::array<std::uint16_t, BoardHeight> values{};
    std::size_t count = 0;

    void push_back(std::uint16_t value) { values[count++] = value; }
    bool empty() const { return count == 0; }
    std::size_t size() const { return count; }
    std::uint16_t operator[](std::size_t index) const { return values[index]; }

    bool operator==(const RowMaskSequence& other) const {
        return count == other.count &&
               std::equal(values.begin(), values.begin() + count, other.values.begin());
    }
};

RowMaskSequence exactBottomGarbageStack(const Board& board) {
    RowMaskSequence stack;
    for (int row = BoardHeight - 1; row >= 0; --row) {
        const auto hole = exactGarbageHole(board, row);
        if (!hole.has_value()) break;
        stack.push_back(*hole);
    }
    return stack;
}

struct SourceShiftObservation {
    RowMaskSequence exactBottomHoles;
    RowMaskSequence garbagePattern;
    std::array<int, VisibleRows + 1> bottomGarbage{};
};

SourceShiftObservation sourceShiftObservation(const Board& observed) {
    SourceShiftObservation result;
    result.exactBottomHoles = exactBottomGarbageStack(observed);
    int garbageBelow = 0;
    for (int y = BoardHeight - 1; y >= 0; --y) {
        int garbage = 0;
        int filled = 0;
        std::uint16_t nonGarbage = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell cell = observed[index(x, y)];
            if (cell == Cell::Garbage) ++garbage;
            if (cell != Cell::Empty) ++filled;
            if (cell != Cell::Garbage) nonGarbage |= static_cast<std::uint16_t>(1u << x);
        }
        if (garbage > 0 && filled < BoardWidth) result.garbagePattern.push_back(nonGarbage);
        if (y >= BoardHeight - VisibleRows) {
            garbageBelow += garbage;
            result.bottomGarbage[static_cast<std::size_t>(BoardHeight - y)] = garbageBelow;
        }
    }
    return result;
}

bool hasExactVerticalCluster(const Board& previous, const Board& observed, int shift,
                             int& totalCells, int& matchedCells) {
    totalCells = 0;
    matchedCells = 0;
    const int firstRow = VisibleRows + shift;
    for (int x = 0; x < BoardWidth; ++x) {
        int row = firstRow;
        while (row < BoardHeight) {
            if (!isPiece(previous[index(x, row)])) {
                ++row;
                continue;
            }
            const int start = row;
            while (row < BoardHeight && isPiece(previous[index(x, row)])) ++row;
            const int end = row - 1;
            const int length = end - start + 1;
            if (length < 2) continue;
            totalCells += length;
            bool exact = true;
            for (int sourceRow = start; sourceRow <= end; ++sourceRow) {
                const int targetRow = sourceRow - shift;
                if (targetRow < VisibleRows ||
                    observed[index(x, targetRow)] != previous[index(x, sourceRow)]) {
                    exact = false;
                    break;
                }
            }
            if (exact) matchedCells += length;
        }
    }
    return matchedCells > 0;
}

std::optional<std::array<int, 2>> canonicalAnchor(
    Cell piece, int rotation, const std::array<std::array<int, 2>, 4>& cells) {
    // originalLegalMoves intentionally retains the old browser's candidate
    // set, but its x/y values are exported to the current simulator/editor.
    // Find the anchor in the shared simulator shape that describes the exact
    // same four board cells. This matters for I (anchor differs by rotation)
    // and keeps the conversion correct if a legacy shape table drifts again.
    const TetrisEngine::Shape canonical = TetrisEngine::shape(piece, rotation);
    for (const auto& target : cells) {
        for (const auto& shapeCell : canonical.cells) {
            const int x = target[0] - shapeCell[0];
            const int y = target[1] - shapeCell[1];
            std::array<std::array<int, 2>, 4> translated{};
            for (std::size_t i = 0; i < canonical.cells.size(); ++i) {
                translated[i] = {x + canonical.cells[i][0], y + canonical.cells[i][1]};
            }
            if (sameCells(translated, cells)) return std::array<int, 2>{x, y};
        }
    }
    return std::nullopt;
}

bool shiftStackVetoEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TR_SHIFT_STACK_VETO");
        return !(value && std::string(value) == "0");
    }();
    return enabled;
}

// Retained rows (bottom-up index j in previous == j + shift in observed) whose
// exact hole differs. Rows beyond either exact stack are not compared.
int retainedStackMismatches(const RowMaskSequence& previousHoles,
                            const RowMaskSequence& observedHoles, int shift) {
    if (previousHoles.size() < 2 || observedHoles.size() < 2) return 0;
    const std::size_t s = static_cast<std::size_t>(shift);
    if (observedHoles.size() <= s) return 0;
    const std::size_t n = std::min(previousHoles.size(), observedHoles.size() - s);
    int mismatches = 0;
    for (std::size_t j = 0; j < n; ++j) {
        if (previousHoles[j] != observedHoles[j + s]) ++mismatches;
    }
    return mismatches;
}

BoardShift sourceBoardShift(const Board& previous, const Board& observed, double threshold,
                            const SourceShiftObservation& observation) {
    // A garbage rise is a contiguous block of new bottom rows. Compare the
    // whole hole-mask stack against the previous bottom stack first. This is
    // intentionally stronger than counting matching cells: a vertical column
    // of holes can look similar after several different offsets, while the
    // ordered stack (including its boundaries) cannot.
    const auto previousHoles = exactBottomGarbageStack(previous);
    const auto& observedHoles = observation.exactBottomHoles;
    if (observedHoles.size() > previousHoles.size()) {
        const std::size_t added = observedHoles.size() - previousHoles.size();
        bool retainedStackMatches = true;
        for (std::size_t i = 0; i < previousHoles.size(); ++i) {
            if (previousHoles[i] != observedHoles[i + added]) {
                retainedStackMatches = false;
                break;
            }
        }
        // An exact observed Gx9+hole stack with no retained predecessor is
        // itself sufficient rise evidence. Requiring a pre-existing garbage
        // row made the first incoming garbage on an empty/sparse board vanish.
        if (retainedStackMatches && added <= static_cast<std::size_t>(VisibleRows)) {
            return {static_cast<int>(added), 1.0};
        }
    }

    RowMaskSequence previousPattern;
    const auto appendPattern = [](RowMaskSequence& pattern,
                                  const Board& board, int row) {
        int garbage = 0;
        int filled = 0;
        std::uint16_t nonGarbage = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell cell = board[index(x, row)];
            if (cell == Cell::Garbage) ++garbage;
            if (cell != Cell::Empty) ++filled;
            if (cell != Cell::Garbage) nonGarbage |= static_cast<std::uint16_t>(1u << x);
        }
        if (garbage > 0 && filled < BoardWidth) pattern.push_back(nonGarbage);
    };
    for (int y = BoardHeight - 1; y >= 0; --y) {
        appendPattern(previousPattern, previous, y);
    }
    if (!previousPattern.empty() && previousPattern == observation.garbagePattern) return {};

    BoardShift best;
    int bestClusterCells = 0;
    for (int shift = 1; shift <= VisibleRows; ++shift) {
        // A rise hypothesis is impossible unless the corresponding number of
        // observed bottom rows contains garbage.  Test this before the
        // comparatively expensive coloured-board correlation below.  This is
        // only a reordering of the existing gate and therefore leaves every
        // returned BoardShift bit-for-bit unchanged.
        const int garbage = observation.bottomGarbage[static_cast<std::size_t>(shift)];
#ifndef TR_DISABLE_ZERO_GARBAGE_EARLY_GATE
        if (garbage <= 0) continue;
#endif
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
        if (blocks > 0 && garbage > 0) {
            const double ratio = static_cast<double>(matched) / blocks;
            int clusterCells = 0;
            int matchedClusterCells = 0;
            const bool hasCluster = hasExactVerticalCluster(previous, observed, shift,
                                                             clusterCells, matchedClusterCells);
            // When the board contains a vertical cluster, an offset that only
            // overlaps its interior is not meaningful rise evidence. Without
            // this gate, a tall column makes shift=1 win simply because it
            // overlaps on all but one row. Sparse boards without a cluster
            // retain the cell-based fallback.
            if (clusterCells > 0 && !hasCluster) continue;
            // The exact hole stack is stronger evidence than the coloured
            // correlation. When both boards carry an exact bottom stack and
            // this offset makes at least two retained rows disagree, a weak
            // correlation must not invent the rise (e.g. an early line clear
            // compensated by copying the old bottom row as "new" garbage).
            if (ratio < .8 && shiftStackVetoEnabled() &&
                retainedStackMismatches(previousHoles, observedHoles, shift) >= 2) continue;
            if (ratio >= threshold &&
                (best.lines == 0 || ratio > best.ratio ||
                 (std::abs(ratio - best.ratio) < .000001 &&
                  matchedClusterCells > bestClusterCells))) {
                best = {shift, ratio};
                bestClusterCells = matchedClusterCells;
            }
        }
    }
    return best;
}

BoardShift sourceBoardShift(const Board& previous, const Board& observed, double threshold) {
    return sourceBoardShift(previous, observed, threshold, sourceShiftObservation(observed));
}

constexpr std::uint16_t FullBoardMask = static_cast<std::uint16_t>((1u << BoardWidth) - 1u);

using OccupiedRows = std::array<std::uint16_t, BoardHeight>;

OccupiedRows occupiedRows(const Board& board) {
    OccupiedRows rows{};
    for (int y = 0; y < BoardHeight; ++y) {
        std::uint16_t row = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            if (isOccupied(board[index(x, y)])) row |= static_cast<std::uint16_t>(1u << x);
        }
        rows[static_cast<std::size_t>(y)] = row;
    }
    return rows;
}

std::uint64_t boardPairHash(const Board& board, const Board& fullBoard) {
    // The beam still stores the coloured boards used by scoring and export.
    // The hash is only a compact transposition index; exact array comparison
    // below resolves collisions, so no candidate can be merged incorrectly.
    std::uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](const Board& value) {
        for (const Cell cell : value) {
            hash ^= static_cast<std::uint8_t>(cell);
            hash *= 1099511628211ull;
        }
    };
    append(board);
    append(fullBoard);
    return hash;
}

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

std::optional<std::uint16_t> exactGarbageHole(const Board& observed, int row) {
    int garbage = 0;
    int empty = 0;
    int emptyColumn = -1;
    for (int x = 0; x < BoardWidth; ++x) {
        const Cell cell = observed[index(x, row)];
        if (cell == Cell::Garbage) {
            ++garbage;
        } else if (cell == Cell::Empty) {
            ++empty;
            emptyColumn = x;
        } else {
            // A coloured mino is not evidence for the garbage hole.  It may
            // be the placed piece, an old block exposed by an effect, or a
            // classifier error.  The row is therefore ambiguous.
            return std::nullopt;
        }
    }
    if (garbage == BoardWidth - 1 && empty == 1) {
        return static_cast<std::uint16_t>(1u << emptyColumn);
    }
    return std::nullopt;
}

std::array<double, BoardWidth> garbageHolePosteriors(
    const Board& observed, const BoardProbabilities* probabilities, int row) {
    std::array<double, BoardWidth> scores{};
    const bool useProbabilities = probabilities && row >= VisibleRows &&
                                  hasProbabilityEvidence(*probabilities);
    double maximum = -std::numeric_limits<double>::infinity();
    for (int x = 0; x < BoardWidth; ++x) {
        if (useProbabilities) {
            const int visibleCell = (row - VisibleRows) * BoardWidth + x;
            const double empty = std::max<double>(
                (*probabilities)[probabilityIndex(visibleCell, Cell::Empty)], 1e-6);
            const double garbage = std::max<double>(
                (*probabilities)[probabilityIndex(visibleCell, Cell::Garbage)], 1e-6);
            // Every candidate expects G in the other nine cells, so their
            // common likelihood cancels. The empty-vs-G log odds at this cell
            // are the complete relative likelihood of choosing it as the hole.
            scores[static_cast<std::size_t>(x)] = std::log(empty) - std::log(garbage);
        } else {
            // Imported/legacy observations have no softmax distribution. Keep
            // the hard label only as an ordering preference, never as a reason
            // to delete the other nine legal columns.
            const Cell cell = observed[index(x, row)];
            scores[static_cast<std::size_t>(x)] = cell == Cell::Empty ? 4.0 :
                                                  cell == Cell::Garbage ? -4.0 : 0.0;
        }
        maximum = std::max(maximum, scores[static_cast<std::size_t>(x)]);
    }

    double total = 0;
    for (double& score : scores) {
        score = std::exp(score - maximum);
        total += score;
    }
    if (total > 0 && std::isfinite(total)) {
        for (double& score : scores) score /= total;
    } else {
        scores.fill(1.0 / BoardWidth);
    }
    return scores;
}

// v74: per-row hole column seen clearly in the following frames (-1 = none).
using GarbageHoleHints = std::array<std::int8_t, BoardHeight>;

bool garbageHoleLookaheadEnabled() {
    static const bool value = [] {
        const char* v = std::getenv("TR_GARBAGE_HOLE_LOOKAHEAD");
        return !(v && std::string(v) == "0");
    }();
    return value;
}

int holeColumn(std::uint16_t mask) {
    for (int x = 0; x < BoardWidth; ++x) if (mask & (1u << x)) return x;
    return -1;
}

// A rise is often first observed mid-animation, so one new row reads as e.g.
// GG_______G and all ten hole columns tie.  The same row is normally drawn
// cleanly (Gx9 + one hole) a few frames later.  roDw 3 kept the wrong column
// of such a tie for 22 seconds and its later TETRIS cleared only two lines.
// For each ambiguous garbage-like row, follow it into the next frames using
// the exact rows below it as an anchor (accepting a unique upward shift for a
// further rise) and return the hole only when at least two later frames show
// it exactly and none disagrees.
GarbageHoleHints garbageHoleLookaheadHints(const std::vector<TimelineStep>& raw,
                                           std::size_t step) {
    GarbageHoleHints hints;
    hints.fill(-1);
    if (!garbageHoleLookaheadEnabled() || step >= raw.size()) return hints;
    constexpr std::size_t LookaheadSteps = 8;
    constexpr int MaxShift = 4;
    const Board& current = raw[step].observed;
    for (int y = BoardHeight - 1; y >= BoardHeight - 14; --y) {
        if (exactGarbageHole(current, y)) continue;
        int garbage = 0, coloured = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell cell = current[index(x, y)];
            if (cell == Cell::Garbage) ++garbage;
            else if (cell != Cell::Empty) ++coloured;
        }
        if (garbage < 2 || coloured > 0) continue;
        // Anchor on every exact garbage row around it (above and below):
        // stacked rows with one shared hole alone would align at several
        // shifts, which is rejected below as ambiguous.
        std::vector<std::pair<int, std::uint16_t>> anchors;
        for (int other = BoardHeight - 14; other < BoardHeight; ++other) {
            if (other == y) continue;
            if (const auto hole = exactGarbageHole(current, other)) anchors.push_back({other, *hole});
        }
        if (anchors.size() < 2) continue;
        int agreed = -1, votes = 0;
        bool conflict = false;
        for (std::size_t later = step + 1;
             later < raw.size() && later <= step + LookaheadSteps && !conflict; ++later) {
            const Board& observed = raw[later].observed;
            int matchedShift = -1, matches = 0;
            for (int shift = 0; shift <= MaxShift; ++shift) {
                if (y - shift < VisibleRows) break;
                const bool aligned = std::all_of(anchors.begin(), anchors.end(), [&](const auto& anchor) {
                    if (anchor.first - shift < VisibleRows) return false;
                    const auto hole = exactGarbageHole(observed, anchor.first - shift);
                    return hole && *hole == anchor.second;
                });
                if (aligned) { matchedShift = shift; ++matches; }
            }
            if (matches != 1) continue;
            const auto hole = exactGarbageHole(observed, y - matchedShift);
            if (!hole) continue;
            const int column = holeColumn(*hole);
            if (agreed >= 0 && agreed != column) conflict = true;
            agreed = column;
            ++votes;
        }
        if (!conflict && votes >= 2) hints[static_cast<std::size_t>(y)] = static_cast<std::int8_t>(agreed);
    }
    return hints;
}

std::vector<std::vector<std::uint16_t>> holeCandidatesFromObserved(
    const Board& observed, const BoardProbabilities* probabilities, int lines,
    const GarbageHoleHints* hints = nullptr) {
    lines = std::clamp(lines, 0, BoardHeight);
    std::vector<std::vector<std::uint16_t>> options;
    options.reserve(static_cast<std::size_t>(lines));
    for (int row = BoardHeight - lines; row < BoardHeight; ++row) {
        if (const auto exact = exactGarbageHole(observed, row)) {
            // Gx9 + one empty cell is a complete garbage-row observation.
            // Its hole is authoritative; alternate columns are only retained
            // for rows that are structurally incomplete or effect-obscured.
            options.push_back({*exact});
            continue;
        }
        if (hints && (*hints)[static_cast<std::size_t>(row)] >= 0) {
            options.push_back({static_cast<std::uint16_t>(1u << (*hints)[static_cast<std::size_t>(row)])});
            continue;
        }
        const auto posterior = garbageHolePosteriors(observed, probabilities, row);
        std::vector<std::uint16_t> rowOptions;
        rowOptions.reserve(BoardWidth);
        for (int x = 0; x < BoardWidth; ++x) {
            rowOptions.push_back(static_cast<std::uint16_t>(1u << x));
        }
        const auto columnOf = [](std::uint16_t mask) {
            for (int x = 0; x < BoardWidth; ++x) if (mask & (1u << x)) return x;
            return 0;
        };
        std::stable_sort(rowOptions.begin(), rowOptions.end(), [&posterior, &columnOf](std::uint16_t left,
                                                                                      std::uint16_t right) {
            const int leftColumn = columnOf(left);
            const int rightColumn = columnOf(right);
            return posterior[static_cast<std::size_t>(leftColumn)] >
                   posterior[static_cast<std::size_t>(rightColumn)];
        });
        options.push_back(std::move(rowOptions));
    }
    return options;
}

std::vector<std::vector<std::uint16_t>> holeCandidatesFromObserved(const Board& observed, int lines) {
    return holeCandidatesFromObserved(observed, nullptr, lines);
}

std::vector<std::uint16_t> holesFromObserved(const Board& observed, int lines) {
    const auto options = holeCandidatesFromObserved(observed, lines);
    std::vector<std::uint16_t> masks;
    masks.reserve(options.size());
    for (const auto& rowOptions : options) {
        // This compatibility helper still returns one deterministic mask for
        // callers that need a concrete board.  Automatic beam search uses the
        // complete row option sets through holeCandidatesFromObserved().
        masks.push_back(rowOptions.empty() ? static_cast<std::uint16_t>(1u << 4) : rowOptions.front());
    }
    return masks;
}

// A row with an uncertain hole keeps every column as a live hypothesis, but
// the Cartesian product must not be allowed to multiply by 10^N for every
// beam parent.  The coverage pass below preserves every row/column option;
// this cap only limits combinations of several uncertain rows.
constexpr std::size_t MaxGarbageRiseCombinations = 256;

template <typename Callback>
void forEachGarbageRiseVariant(const Board& observed, const BoardProbabilities& probabilities,
                               const BoardShift& shift, Callback&& callback,
                               const GarbageHoleHints* hints = nullptr) {
    GarbageRise base;
    base.lines = std::clamp(shift.lines, 0, BoardHeight);
    base.matchRatio = shift.ratio;
    if (base.lines == 0) {
        callback(base);
        return;
    }

    const auto options = holeCandidatesFromObserved(observed, &probabilities, base.lines, hints);
    std::size_t combinations = 1;
    for (const auto& row : options) {
        if (row.empty() || combinations > MaxGarbageRiseCombinations / row.size()) {
            combinations = MaxGarbageRiseCombinations + 1;
            break;
        }
        combinations *= row.size();
    }

    // Exact rows are always preserved.  For a pathological multi-row effect
    // with 10^N unresolved combinations, keep a bounded hypothesis set: first
    // emit a representative for every column of every ambiguous row, then
    // fill the remaining budget with combinations.  Ordinary one- and
    // two-row rises are fully enumerated.
    const std::size_t coverageMinimum = options.size() * static_cast<std::size_t>(BoardWidth);
    const std::size_t variantBudget = std::min(
        MaxGarbageRiseCombinations,
        std::max<std::size_t>(100, coverageMinimum));
    const std::size_t limit = std::min(combinations, variantBudget);
    std::vector<std::uint16_t> selected;
    selected.reserve(options.size());
    for (const auto& rowOptions : options) {
        selected.push_back(rowOptions.empty() ? static_cast<std::uint16_t>(1u << 4)
                                              : rowOptions.front());
    }
    std::size_t emitted = 0;
    auto emitSelected = [&] {
        if (emitted >= limit) return;
        GarbageRise rise = base;
        rise.holeMasks = selected;
        rise.holesCertain = true;
        for (const auto& rowOptions : options) {
            if (rowOptions.size() != 1) {
                rise.holesCertain = false;
                break;
            }
        }
        callback(std::move(rise));
        ++emitted;
    };

    if (combinations > limit) {
        // Guarantee that every legal hole column appears in the retained
        // domain even when the Cartesian product itself is too large.
        for (std::size_t row = 0; row < options.size() && emitted < limit; ++row) {
            for (const std::uint16_t mask : options[row]) {
                selected[row] = mask;
                emitSelected();
                if (emitted >= limit) break;
            }
            selected[row] = options[row].empty()
                ? static_cast<std::uint16_t>(1u << 4) : options[row].front();
        }
    }

    std::function<void(std::size_t)> visit = [&](std::size_t row) {
        if (emitted >= limit) return;
        if (row == options.size()) {
            emitSelected();
            return;
        }

        const auto& rowOptions = options[row];
        if (rowOptions.empty()) return;
        for (const std::uint16_t mask : rowOptions) {
            selected[row] = mask;
            visit(row + 1);
            if (emitted >= limit) return;
        }
    };
    visit(0);
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

GarbageRise automaticGarbageRise(const Board& observed, const BoardShift& shift,
                                 const GarbageHoleHints* hints = nullptr) {
    GarbageRise rise;
    rise.lines = shift.lines;
    rise.matchRatio = shift.ratio;
    const auto options = holeCandidatesFromObserved(observed, nullptr, shift.lines, hints);
    for (const auto& rowOptions : options) {
        rise.holeMasks.push_back(rowOptions.empty() ? static_cast<std::uint16_t>(1u << 4) : rowOptions.front());
    }
    rise.holesCertain = !options.empty() && std::all_of(options.begin(), options.end(), [](const auto& row) {
        return row.size() == 1;
    });
    return rise;
}

std::vector<GarbageRise> garbageRiseHypotheses(
        const Board& postClearBoard,
        const Board& observed,
        const BoardProbabilities& probabilities,
        const BoardShift& primary,
        const Settings& settings,
        const GarbageHoleHints* hints = nullptr) {
    std::vector<GarbageRise> result;
    const auto append = [&](GarbageRise rise) {
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const GarbageRise& existing) {
                return existing.lines == rise.lines &&
                       existing.holeMasks == rise.holeMasks;
            });
        if (!duplicate) result.push_back(std::move(rise));
    };
    forEachGarbageRiseVariant(observed, probabilities, primary, append, hints);
    if (!settings.garbageBacktracking) return result;

    // Effects can make a false rise locally outscore the real trajectory, and
    // an obscured bottom can hide the true line count. During the bounded
    // anomaly retry only, keep no-rise plus every structurally plausible
    // count. The ordinary 32-wide fast path remains unchanged.
    append(GarbageRise{});
    static const int maximumOverride = [] {
        const char* v = std::getenv("TR_GARBAGE_BACKTRACK_MAX_LINES");
        return v ? std::atoi(v) : -1;
    }();
    const int maximum = std::clamp(maximumOverride >= 0 ? maximumOverride
                                                        : settings.garbageBacktrackingMaxLines,
                                   0, VisibleRows);
    // Alternative line counts get a single hole guess per row.  When the
    // newest rows are hidden by a clear flash (or cleared in the same phase)
    // that guess can be the wrong column.  The second-stage garbage retry
    // (settings.garbageAltHoleVariants) also enumerates the hole variants,
    // bounded like the primary rise.
    for (int lines = 1; lines <= maximum; ++lines) {
        if (lines == primary.lines) continue;
        double ratio = 0;
        if (!TetrisEngine::garbageShift(postClearBoard, observed, lines, ratio)) continue;
        // Same veto as sourceBoardShift: a weak count the exact hole stack
        // contradicts must not be offered as an alternative either.
        if (ratio < .8 && shiftStackVetoEnabled() &&
            retainedStackMismatches(exactBottomGarbageStack(postClearBoard),
                                    exactBottomGarbageStack(observed), lines) >= 2) continue;
        append(automaticGarbageRise(observed, {lines, ratio}, hints));
        if (settings.garbageAltHoleVariants) forEachGarbageRiseVariant(observed, probabilities, {lines, ratio}, append, hints);
    }
    return result;
}

bool hasProbabilityEvidence(const BoardProbabilities& probabilities) {
    double total = 0;
    for (const float value : probabilities) total += value;
    return total > .5;
}

double sourceScore(const Board& predicted, const Board& observed, const ScoreWeights& weights,
                   const BoardProbabilities* probabilities = nullptr) {
    double value = 0;
    // The ONNX model only observes the visible 20 rows.  The hidden rows in
    // `observed` are synthetic empty cells, not evidence, so comparing them
    // would systematically punish a high stack and a garbage rise.
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell predictedCell = predicted[index(x, y)];
            const Cell observedCell = observed[index(x, y)];
            if (probabilities) {
                const int visibleCell = (y - VisibleRows) * BoardWidth + x;
                const double empty = (*probabilities)[probabilityIndex(visibleCell, Cell::Empty)];
                if (predictedCell == Cell::Empty) {
                    value += empty * weights.emptyMatch + (1.0 - empty) * weights.extraBlock;
                } else {
                    const double exact = (*probabilities)[probabilityIndex(visibleCell, predictedCell)];
                    const double otherOccupied = std::max(0.0, 1.0 - empty - exact);
                    value += exact * weights.exactMatch +
                             otherOccupied * weights.colorMismatch +
                             empty * weights.missingBlock;
                }
            } else {
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
    }
    return value;
}

using SourceScoreTable =
    std::array<double, BoardWidth * VisibleRows * CellClassCount>;

SourceScoreTable makeSourceScoreTable(const Board& observed, const ScoreWeights& weights,
                                      const BoardProbabilities* probabilities) {
    SourceScoreTable table{};
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const int visibleCell = (y - VisibleRows) * BoardWidth + x;
            for (int cell = 0; cell < CellClassCount; ++cell) {
                const Cell predictedCell = static_cast<Cell>(cell);
                double value = 0;
                if (probabilities) {
                    const double empty =
                        (*probabilities)[probabilityIndex(visibleCell, Cell::Empty)];
                    if (predictedCell == Cell::Empty) {
                        value = empty * weights.emptyMatch +
                                (1.0 - empty) * weights.extraBlock;
                    } else {
                        const double exact =
                            (*probabilities)[probabilityIndex(visibleCell, predictedCell)];
                        const double otherOccupied = std::max(0.0, 1.0 - empty - exact);
                        value = exact * weights.exactMatch +
                                otherOccupied * weights.colorMismatch +
                                empty * weights.missingBlock;
                    }
                } else {
                    const Cell observedCell = observed[index(x, y)];
                    if (predictedCell == observedCell) {
                        value = predictedCell == Cell::Empty ? weights.emptyMatch
                                                             : weights.exactMatch;
                    } else if (predictedCell != Cell::Empty && observedCell != Cell::Empty) {
                        value = weights.colorMismatch;
                    } else if (predictedCell == Cell::Empty) {
                        value = weights.extraBlock;
                    } else {
                        value = weights.missingBlock;
                    }
                }
                table[probabilityIndex(visibleCell, predictedCell)] = value;
            }
        }
    }
    return table;
}

double sourceScoreFromTable(const Board& predicted, const SourceScoreTable& table) {
    double value = 0;
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const int visibleCell = (y - VisibleRows) * BoardWidth + x;
            value += table[probabilityIndex(visibleCell, predicted[index(x, y)])];
        }
    }
    return value;
}

double perfectClearEffectMaskedScore(const Board& predicted, const Board& observed,
                                     const SourceScoreTable& table) {
    double value = 0;
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell observedCell = observed[index(x, y)];
            // Orange letters become L and the white flash commonly becomes G.
            // Neither is categorical board evidence during the detected run.
            if (observedCell == Cell::L || observedCell == Cell::Garbage) continue;
            const int visibleCell = (y - VisibleRows) * BoardWidth + x;
            value += table[probabilityIndex(visibleCell, predicted[index(x, y)])];
        }
    }
    return value;
}

std::vector<std::array<int, 2>> survivingPlacedCells(const CandidateMove& move, int garbageLines) {
    std::array<bool, BoardHeight> fullRows{};
    const OccupiedRows occupied = occupiedRows(move.fullBoard);
    for (int y = 0; y < BoardHeight; ++y)
        fullRows[static_cast<std::size_t>(y)] = occupied[static_cast<std::size_t>(y)] == FullBoardMask;

    std::vector<std::array<int, 2>> result;
    result.reserve(4);
    for (int i = 0; i < move.cellCount; ++i) {
        const int x = move.cells[static_cast<std::size_t>(i)][0];
        const int y = move.cells[static_cast<std::size_t>(i)][1];
        if (x < 0 || x >= BoardWidth || y < 0 || y >= BoardHeight || fullRows[static_cast<std::size_t>(y)]) {
            continue;
        }
        int afterClearY = y;
        for (int row = y + 1; row < BoardHeight; ++row) {
            if (fullRows[static_cast<std::size_t>(row)]) ++afterClearY;
        }
        const int afterGarbageY = afterClearY - std::max(0, garbageLines);
        if (afterGarbageY >= 0 && afterGarbageY < BoardHeight) {
            result.push_back({x, afterGarbageY});
        }
    }
    return result;
}

double placedColorScore(const CandidateMove& move, int garbageLines, Cell piece,
                         const Board& observed, const Confidence& confidence,
                         const BoardProbabilities* probabilities,
                         const ScoreWeights& weights) {
    if (!isPiece(piece)) return 0;
    const auto cells = survivingPlacedCells(move, garbageLines);
    if (cells.empty()) return 0;

    double score = 0;
    int visibleCells = 0;
    int exactCells = 0;
    double completeProbability = 1.0;
    for (const auto& [x, y] : cells) {
        if (y < VisibleRows || y >= BoardHeight) continue;
        ++visibleCells;
        const int confidenceIndex = (y - VisibleRows) * BoardWidth + x;
        if (probabilities) {
            const double empty = (*probabilities)[probabilityIndex(confidenceIndex, Cell::Empty)];
            const double exact = (*probabilities)[probabilityIndex(confidenceIndex, piece)];
            const double otherOccupied = std::max(0.0, 1.0 - empty - exact);
            score += weights.placedColorMatch * exact +
                     weights.placedColorMismatch * otherOccupied;
            completeProbability *= exact;
            continue;
        }
        const double confidenceWeight = std::clamp(
            confidence[static_cast<std::size_t>(confidenceIndex)] / 255.0, .25, 1.0);
        const Cell actual = observed[index(x, y)];
        if (actual == piece) {
            score += weights.placedColorMatch * confidenceWeight;
            ++exactCells;
        } else if (isOccupied(actual)) {
            // A visible, occupied cell of another colour is strong evidence
            // against this placement.  An empty cell is treated as unknown:
            // it may be hidden by an effect or sampled before the lock.
            score += weights.placedColorMismatch * confidenceWeight;
        }
    }
    if (probabilities) {
        if (visibleCells > 0) score += weights.placedColorComplete * completeProbability;
    } else if (visibleCells > 0 && exactCells == visibleCells) {
        score += weights.placedColorComplete;
    }
    return score;
}

struct GarbageHoleScoreContext {
    std::array<std::optional<std::uint16_t>, VisibleRows> exact;
    std::array<std::array<double, BoardWidth>, VisibleRows> posterior{};
};

GarbageHoleScoreContext makeGarbageHoleScoreContext(
    const Board& observed, const BoardProbabilities* probabilities) {
    GarbageHoleScoreContext result;
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        const std::size_t row = static_cast<std::size_t>(y - VisibleRows);
        result.exact[row] = exactGarbageHole(observed, y);
        if (!result.exact[row].has_value()) {
            result.posterior[row] = garbageHolePosteriors(observed, probabilities, y);
        }
    }
    return result;
}

double garbageHoleScore(const GarbageRise& rise, const Board& observed,
                        const BoardProbabilities* probabilities,
                        const ScoreWeights& weights,
                        const GarbageHoleScoreContext* context = nullptr) {
    if (rise.lines <= 0 || rise.holeMasks.size() < static_cast<std::size_t>(rise.lines)) return 0;
    double score = 0;
    for (int row = 0; row < rise.lines; ++row) {
        const int y = BoardHeight - rise.lines + row;
        const std::uint16_t mask = rise.holeMasks[static_cast<std::size_t>(row)];
        const auto exact = context
            ? context->exact[static_cast<std::size_t>(y - VisibleRows)]
            : exactGarbageHole(observed, y);
        if (exact.has_value()) {
            // Gx9 + one empty cell is a complete structural observation. A
            // candidate with another hole must not recover the score through
            // the nine surrounding G matches. Keep it in the beam for future
            // correction, but apply the dedicated large mismatch immediately.
            score += mask == *exact ? weights.garbageHoleMatch : weights.garbageHoleMismatch;
            continue;
        }
        int column = -1;
        for (int x = 0; x < BoardWidth; ++x) {
            if (mask & (1u << x)) {
                column = x;
                break;
            }
        }
        if (column < 0) continue;
        const auto posterior = context
            ? context->posterior[static_cast<std::size_t>(y - VisibleRows)]
            : garbageHolePosteriors(observed, probabilities, y);
        const double probability = posterior[static_cast<std::size_t>(column)];
        // With no structurally exact row, 1/10 is neutral. This prevents an
        // effect-obscured row from penalizing every rise hypothesis equally,
        // while still strongly separating informative softmax odds.
        constexpr double uniform = 1.0 / BoardWidth;
        if (probability >= uniform) {
            score += weights.garbageHoleMatch * (probability - uniform) / (1.0 - uniform);
        } else {
            score += weights.garbageHoleMismatch * (uniform - probability) / uniform;
        }
    }
    return score;
}

double transitionObservationScoreWithEvidence(
    const CandidateMove& move, const Board& predictedBoard, const GarbageRise& rise,
    const Board& observed, const Confidence& confidence,
    const BoardProbabilities* evidence, const ScoreWeights& weights,
    const SourceScoreTable* sourceTable = nullptr,
    const GarbageHoleScoreContext* garbageContext = nullptr,
    const NeuralRanker* neuralRanker = nullptr) {
    if (neuralRanker && neuralRanker->valid()) {
        CandidateMove rankedMove = move;
        rankedMove.board = predictedBoard;
        BoardProbabilities categorical{};
        const BoardProbabilities& rankEvidence = evidence ? *evidence : categorical;
        return neuralRanker->score(TetrisEngine::neuralRankFeatures(
            rankedMove, rise, observed, confidence, rankEvidence));
    }
    return (sourceTable ? sourceScoreFromTable(predictedBoard, *sourceTable)
                        : sourceScore(predictedBoard, observed, weights, evidence)) +
           placedColorScore(move, rise.lines, move.piece, observed, confidence, evidence, weights) +
           garbageHoleScore(rise, observed, evidence, weights, garbageContext);
}

double transitionObservationScore(const CandidateMove& move, const GarbageRise& rise,
                                   const Board& observed, const Confidence& confidence,
                                   const BoardProbabilities& probabilities,
                                   const ScoreWeights& weights) {
    const BoardProbabilities* evidence = hasProbabilityEvidence(probabilities) ? &probabilities : nullptr;
    return transitionObservationScoreWithEvidence(
        move, move.board, rise, observed, confidence, evidence, weights);
}

// ONNX predicts the 10x20 visible playfield only. The hidden rows are not
// evidence, so an exact match deliberately compares just those 200 labels.
// This also keeps a just-cleared pre-clear row out of the comparison.
bool exactVisibleOnnxMatch(const Board& predicted, const Board& observed,
                           const GarbageRise& rise) {
    // The hard exact-match correction is authoritative for the reconstructed
    // board, but not for rows that appeared in this same transition. A single
    // CNN hole error in a fresh garbage row must not erase all nine alternate
    // physical hypotheses before later frames can resolve it.
    const int endRow = std::max(VisibleRows, BoardHeight - std::max(0, rise.lines));
    return std::equal(predicted.begin() + BoardWidth * VisibleRows,
                      predicted.begin() + BoardWidth * endRow,
                      observed.begin() + BoardWidth * VisibleRows);
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
    // Keep the simulator's normal spawn anchor. legalMoves() additionally
    // permits one more upward fallback row for high-stack recovery.
    // floor(BOARD_WIDTH / 2) - floor(center.x) - 1.
    return piece == Cell::I ? std::array<int,2>{3, 20} : std::array<int,2>{4, 20};
}

std::array<TetrisEngine::Offset, 5> TetrisEngine::rotationOffsets(Cell piece, int from, int to) {
    from &= 3; to &= 3;
    if ((to - from + 4) % 4 == 1) {
        if (piece == Cell::I) {
            switch (from) {
            case 0: return {{{0,0},{-2,0},{1,0},{-2,-1},{1,2}}};
            case 1: return {{{0,0},{-1,0},{2,0},{-1,2},{2,-1}}};
            case 2: return {{{0,0},{2,0},{-1,0},{2,1},{-1,-2}}};
            default: return {{{0,0},{1,0},{-2,0},{1,-2},{-2,1}}};
            }
        }
        switch (from) {
        case 0: return {{{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}};
        case 1: return {{{0,0},{1,0},{1,-1},{0,2},{1,2}}};
        case 2: return {{{0,0},{1,0},{1,1},{0,-2},{1,-2}}};
        default: return {{{0,0},{-1,0},{-1,-1},{0,2},{-1,2}}};
        }
    }
    // The simulator uses these exact reverse-transition values for CCW rotation.
    if (piece == Cell::I) {
        switch (from) {
        case 0: return {{{0,0},{-1,0},{2,0},{-1,2},{2,-1}}};
        case 1: return {{{0,0},{2,0},{-1,0},{2,1},{-1,-2}}};
        case 2: return {{{0,0},{1,0},{-2,0},{1,-2},{-2,1}}};
        default: return {{{0,0},{-2,0},{1,0},{-2,-1},{1,2}}};
        }
    }
    switch (from) {
    case 0: return {{{0,0},{1,0},{1,1},{0,-2},{1,-2}}};
    case 1: return {{{0,0},{1,0},{1,-1},{0,2},{1,2}}};
    case 2: return {{{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}};
    default: return {{{0,0},{-1,0},{-1,-1},{0,2},{-1,2}}};
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
    const OccupiedRows occupied = occupiedRows(board);
    for (int rotation = 0; rotation < static_cast<int>(shapes.size()); ++rotation) {
        const auto& current = shapes[rotation];
        for (int x = 0; x < BoardWidth; ++x) {
            for (int y = 0; y < BoardHeight; ++y) {
                bool canPlace = true;
                for (const auto& [dx, dy] : current) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || nx >= BoardWidth || ny < 0 || ny >= BoardHeight ||
                        (occupied[static_cast<std::size_t>(ny)] & (1u << nx)) != 0) {
                        canPlace = false;
                        break;
                    }
                }
                if (!canPlace) continue;

                bool grounded = false;
                for (const auto& [dx, dy] : current) {
                    const int nx = x + dx;
                    const int ny = y + dy + 1;
                    if (ny >= BoardHeight ||
                        (ny >= 0 && (occupied[static_cast<std::size_t>(ny)] & (1u << nx)) != 0)) {
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
                OccupiedRows lockedRows = occupied;
                for (int i = 0; i < 4; ++i) {
                    const int nx = x + current[i][0];
                    const int ny = y + current[i][1];
                    move.cells[i] = {nx, ny};
                    move.fullBoard[index(nx, ny)] = piece;
                    lockedRows[static_cast<std::size_t>(ny)] |= static_cast<std::uint16_t>(1u << nx);
                    ++move.cellCount;
                }

                if (const auto anchor = canonicalAnchor(piece, rotation, move.cells)) {
                    move.x = (*anchor)[0];
                    move.y = (*anchor)[1];
                }

                int write = BoardHeight - 1;
                for (int row = BoardHeight - 1; row >= 0; --row) {
                    if (lockedRows[static_cast<std::size_t>(row)] == FullBoardMask) {
                        ++move.clearedLines;
                        continue;
                    }
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

std::vector<CandidateMove> TetrisEngine::legalMovesImpl(
        const Board& board, Cell piece, bool completeSoftDrop, bool extraSpawnRows) {
    std::vector<CandidateMove> moves;
    if (!isPiece(piece)) return moves;
    auto spawn = spawnPosition(piece);
    const std::array<Shape, 4> rotations{
        shape(piece, 0), shape(piece, 1), shape(piece, 2), shape(piece, 3)
    };
    const Shape& spawnShape = rotations[0];
    // Preserve the normal y=20 graph whenever it is available. On a high
    // stack, try y=19 and then the additionally allowed y=18 row while keeping
    // every ordinary match's candidate order byte-for-byte stable.
    if (!fits(board, spawnShape, spawn[0], spawn[1])) {
        --spawn[1];
        if (!fits(board, spawnShape, spawn[0], spawn[1])) {
            --spawn[1];
            if (!fits(board, spawnShape, spawn[0], spawn[1])) {
                // v79: a large garbage rise can push the stack 2-3 rows above
                // the visible field (XTveY 89 P2: a Z at y=18 after a 9-line
                // rise) and the game still spawns the next piece above it. With
                // only y=19/18 the true board had no legal move, so the beam
                // kept a wrong branch that cleared a fake line to make room.
                // Reached only when every ordinary spawn row is blocked, so all
                // other boards keep their exact candidate order. Enabled only
                // by the repair ladder (Settings::extraSpawnRows), so healthy
                // players are solved exactly as before.
                // TR_SPAWN_EXTRA_ROWS=0 restores the old limit there too.
                int extraRows = extraSpawnRows ? 4 : 0;
                if (const char* v = std::getenv("TR_SPAWN_EXTRA_ROWS"); v && extraRows) extraRows = std::clamp(std::atoi(v), 0, 8);
                // Only for a genuinely high stack (>= 80 occupied cells). A
                // nearly empty board blocked by a few stray top cells (bTL5 42's
                // opening) must keep "no legal move".
                int occupiedCells = 0;
                for (const Cell cell : board) occupiedCells += cell != Cell::Empty;
                if (occupiedCells < 80) extraRows = 0;
                bool found = false;
                for (int extra = 0; extra < extraRows && !found; ++extra) {
                    --spawn[1];
                    found = spawn[1] >= 2 && fits(board, spawnShape, spawn[0], spawn[1]);
                }
                if (!found) return moves;
            }
        }
    }

    std::vector<ReachState> pending;
    pending.reserve(256);
    std::unordered_set<std::uint64_t> visited;
    visited.reserve(256);
    pending.push_back({spawn[0], spawn[1], 0});
    visited.insert(stateKey(spawn[0], spawn[1], 0));
    // A piece has at most a few dozen reachable locks.  Preserve the BFS's
    // first-seen order while avoiding one tree allocation and logarithmic
    // comparison walk per lock.  OriginalShape equality is exact, so this is
    // semantically identical to using std::set solely for insert().second.
#ifdef TR_LEGACY_SET_PLACEMENTS
    std::set<OriginalShape> resultPlacements;
#else
    std::vector<OriginalShape> resultPlacements;
    resultPlacements.reserve(64);
#endif
    moves.reserve(64);

    auto enqueue = [&](int x, int y, int rotation) {
        if (!fits(board, rotations[static_cast<std::size_t>(rotation & 3)], x, y)) return;
        const auto key = stateKey(x, y, rotation);
        if (visited.insert(key).second) pending.push_back({x,y,rotation});
    };

    const auto explore = [&](bool includeSoftDrop) {
        // Revisit the already explored zero-G graph when soft drop is enabled.
        // Existing placements remain first and byte-for-byte ordered; only
        // locks that require acting at an intermediate descent height append.
        for (std::size_t nextState = 0; nextState < pending.size(); ++nextState) {
            const ReachState state = pending[nextState];
            const Shape& current = rotations[static_cast<std::size_t>(state.rotation)];
            int hardY = state.y;
            while (fits(board, current, state.x, hardY + 1)) ++hardY;
            const OriginalShape landed = placementKey(current, state.x, hardY);
            const bool hasVisibleCell = std::any_of(landed.begin(), landed.end(), [](const auto& cell) {
                return cell[1] >= VisibleRows;
            });
            // Canonicalize by the four locked cells, not by the post-clear
            // board: distinct pre-clear placements may clear to one board.
            bool newPlacement = false;
            if (hasVisibleCell) {
#ifdef TR_LEGACY_SET_PLACEMENTS
                newPlacement = resultPlacements.insert(landed).second;
#else
                newPlacement = std::find(resultPlacements.begin(), resultPlacements.end(), landed) ==
                               resultPlacements.end();
                if (newPlacement) resultPlacements.push_back(landed);
#endif
            }
            if (newPlacement) {
                moves.push_back(lock(board, piece, current, state.x, hardY, state.rotation));
            }

            enqueue(state.x - 1, state.y, state.rotation);
            enqueue(state.x + 1, state.y, state.rotation);
            if (hardY != state.y) enqueue(state.x, hardY, state.rotation);
            if (piece != Cell::O) {
                const int cwRotation = (state.rotation + 1) & 3;
                for (const auto& offset : rotationOffsets(piece, state.rotation, cwRotation))
                    if (fits(board, rotations[static_cast<std::size_t>(cwRotation)],
                             state.x + offset.x, state.y - offset.y)) {
                        enqueue(state.x + offset.x, state.y - offset.y, cwRotation); break;
                    }
                const int ccwRotation = (state.rotation + 3) & 3;
                for (const auto& offset : rotationOffsets(piece, state.rotation, ccwRotation))
                    if (fits(board, rotations[static_cast<std::size_t>(ccwRotation)],
                             state.x + offset.x, state.y - offset.y)) {
                        enqueue(state.x + offset.x, state.y - offset.y, ccwRotation); break;
                    }
            }
            if (includeSoftDrop) enqueue(state.x, state.y + 1, state.rotation);
        }
    };
    explore(false);
    // The complete retry revisits the zero-G states and appends the locks that
    // require acting at intermediate descent heights. The ordinary beam calls
    // legalMovesFast(), so healthy matches never pay this row-by-row cost.
    if (completeSoftDrop) explore(true);
    return moves;
}

std::vector<CandidateMove> TetrisEngine::legalMoves(const Board& board, Cell piece) {
    return legalMovesImpl(board, piece, true);
}

std::vector<CandidateMove> TetrisEngine::legalMovesFast(const Board& board, Cell piece) {
    return legalMovesImpl(board, piece, false);
}

std::vector<CandidateMove> TetrisEngine::legalMovesHighStack(const Board& board, Cell piece,
                                                             bool completeSoftDrop) {
    return legalMovesImpl(board, piece, completeSoftDrop, true);
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

std::array<double, 10> TetrisEngine::transitionScoreFeatures(
    const CandidateMove& move, const GarbageRise& rise,
    const Board& observed, const Confidence& confidence,
    const BoardProbabilities& probabilities) {
    std::array<double, 10> result{};
    const bool useProbabilities = hasProbabilityEvidence(probabilities);
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell predicted = move.board[index(x, y)];
            const Cell actual = observed[index(x, y)];
            if (useProbabilities) {
                const int visibleCell = (y - VisibleRows) * BoardWidth + x;
                const double empty = probabilities[
                    probabilityIndex(visibleCell, Cell::Empty)];
                if (predicted == Cell::Empty) {
                    result[1] += empty;
                    result[4] += 1.0 - empty;
                } else {
                    const double exact = probabilities[
                        probabilityIndex(visibleCell, predicted)];
                    result[0] += exact;
                    result[2] += std::max(0.0, 1.0 - empty - exact);
                    result[3] += empty;
                }
            } else if (predicted == actual) {
                ++result[predicted == Cell::Empty ? 1 : 0];
            } else if (predicted != Cell::Empty && actual != Cell::Empty) {
                ++result[2];
            } else if (predicted == Cell::Empty) {
                ++result[4];
            } else {
                ++result[3];
            }
        }
    }

    if (isPiece(move.piece)) {
        const auto cells = survivingPlacedCells(move, rise.lines);
        int visibleCells = 0;
        int exactCells = 0;
        double completeProbability = 1.0;
        for (const auto& [x, y] : cells) {
            if (y < VisibleRows || y >= BoardHeight) continue;
            ++visibleCells;
            const int visibleCell = (y - VisibleRows) * BoardWidth + x;
            if (useProbabilities) {
                const double empty = probabilities[
                    probabilityIndex(visibleCell, Cell::Empty)];
                const double exact = probabilities[
                    probabilityIndex(visibleCell, move.piece)];
                result[5] += exact;
                result[6] += std::max(0.0, 1.0 - empty - exact);
                completeProbability *= exact;
            } else {
                const double weight = std::clamp(
                    confidence[static_cast<std::size_t>(visibleCell)] / 255.0,
                    .25, 1.0);
                if (observed[index(x, y)] == move.piece) {
                    result[5] += weight;
                    ++exactCells;
                } else if (isOccupied(observed[index(x, y)])) {
                    result[6] += weight;
                }
            }
        }
        if (useProbabilities) {
            if (visibleCells > 0) result[7] += completeProbability;
        } else if (visibleCells > 0 && exactCells == visibleCells) {
            result[7] += 1.0;
        }
    }

    if (rise.lines > 0 &&
        rise.holeMasks.size() >= static_cast<std::size_t>(rise.lines)) {
        const BoardProbabilities* evidence = useProbabilities ? &probabilities : nullptr;
        for (int row = 0; row < rise.lines; ++row) {
            const int y = BoardHeight - rise.lines + row;
            const std::uint16_t mask = rise.holeMasks[static_cast<std::size_t>(row)];
            if (const auto exact = exactGarbageHole(observed, y)) {
                result[mask == *exact ? 8 : 9] += 1.0;
                continue;
            }
            int column = -1;
            for (int x = 0; x < BoardWidth; ++x) {
                if (mask & (1u << x)) {
                    column = x;
                    break;
                }
            }
            if (column < 0) continue;
            const auto posterior = garbageHolePosteriors(observed, evidence, y);
            constexpr double uniform = 1.0 / BoardWidth;
            const double probability = posterior[static_cast<std::size_t>(column)];
            if (probability >= uniform) {
                result[8] += (probability - uniform) / (1.0 - uniform);
            } else {
                result[9] += (uniform - probability) / uniform;
            }
        }
    }
    return result;
}

std::array<float, NeuralRankFeatureCount> TetrisEngine::neuralRankFeatures(
    const CandidateMove& move, const GarbageRise& rise,
    const Board& observed, const Confidence& confidence,
    const BoardProbabilities& probabilities) {
    std::array<float, NeuralRankFeatureCount> result{};
    std::size_t feature = 0;
    const auto linear = transitionScoreFeatures(
        move, rise, observed, confidence, probabilities);
    for (const double value : linear) result[feature++] = static_cast<float>(value);

    // Spatial agreement lets the nonlinear fallback distinguish a compact
    // animation/effect region from a physically shifted whole-board error.
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        int matched = 0;
        for (int x = 0; x < BoardWidth; ++x) {
            matched += move.board[index(x, y)] == observed[index(x, y)];
        }
        result[feature++] = static_cast<float>(matched) / BoardWidth;
    }
    for (int x = 0; x < BoardWidth; ++x) {
        int matched = 0;
        for (int y = VisibleRows; y < BoardHeight; ++y) {
            matched += move.board[index(x, y)] == observed[index(x, y)];
        }
        result[feature++] = static_cast<float>(matched) / VisibleRows;
    }

    std::array<int, BoardWidth> heights{};
    std::array<int, BoardWidth> holes{};
    int aggregateHeight = 0;
    int maximumHeight = 0;
    int totalHoles = 0;
    for (int x = 0; x < BoardWidth; ++x) {
        int first = BoardHeight;
        for (int y = 0; y < BoardHeight; ++y) {
            if (isOccupied(move.board[index(x, y)])) {
                first = y;
                break;
            }
        }
        heights[static_cast<std::size_t>(x)] = BoardHeight - first;
        bool occupiedAbove = false;
        for (int y = 0; y < BoardHeight; ++y) {
            const bool occupied = isOccupied(move.board[index(x, y)]);
            occupiedAbove = occupiedAbove || occupied;
            if (occupiedAbove && !occupied) ++holes[static_cast<std::size_t>(x)];
        }
        aggregateHeight += heights[static_cast<std::size_t>(x)];
        maximumHeight = std::max(maximumHeight, heights[static_cast<std::size_t>(x)]);
        totalHoles += holes[static_cast<std::size_t>(x)];
    }
    for (const int height : heights) result[feature++] = static_cast<float>(height) / BoardHeight;
    for (const int holeCount : holes) result[feature++] = static_cast<float>(holeCount) / BoardHeight;

    int bumpiness = 0;
    int wells = 0;
    for (int x = 0; x < BoardWidth; ++x) {
        if (x + 1 < BoardWidth) {
            bumpiness += std::abs(heights[static_cast<std::size_t>(x)] -
                                  heights[static_cast<std::size_t>(x + 1)]);
        }
        const int left = x == 0 ? BoardHeight : heights[static_cast<std::size_t>(x - 1)];
        const int right = x + 1 == BoardWidth ? BoardHeight : heights[static_cast<std::size_t>(x + 1)];
        wells += std::max(0, std::min(left, right) - heights[static_cast<std::size_t>(x)]);
    }
    result[feature++] = static_cast<float>(aggregateHeight) / (BoardWidth * BoardHeight);
    result[feature++] = static_cast<float>(maximumHeight) / BoardHeight;
    result[feature++] = static_cast<float>(bumpiness) / ((BoardWidth - 1) * BoardHeight);
    result[feature++] = static_cast<float>(totalHoles) / (BoardWidth * BoardHeight);
    result[feature++] = static_cast<float>(wells) / (BoardWidth * BoardHeight);

    int observedOccupied = 0;
    int candidateOccupied = 0;
    int occupancyMismatch = 0;
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        for (int x = 0; x < BoardWidth; ++x) {
            const bool candidate = isOccupied(move.board[index(x, y)]);
            const bool actual = isOccupied(observed[index(x, y)]);
            observedOccupied += actual;
            candidateOccupied += candidate;
            occupancyMismatch += candidate != actual;
        }
    }
    result[feature++] = static_cast<float>(observedOccupied) / (BoardWidth * VisibleRows);
    result[feature++] = static_cast<float>(candidateOccupied) / (BoardWidth * VisibleRows);
    result[feature++] = static_cast<float>(occupancyMismatch) / (BoardWidth * VisibleRows);
    result[feature++] = std::none_of(move.board.begin(), move.board.end(),
        [](Cell cell) { return isOccupied(cell); }) ? 1.0f : 0.0f;

    for (const Cell piece : Pieces) result[feature++] = move.piece == piece ? 1.0f : 0.0f;
    for (int rotation = 0; rotation < 4; ++rotation) {
        result[feature++] = (move.rotation & 3) == rotation ? 1.0f : 0.0f;
    }
    result[feature++] = static_cast<float>(move.x + 2) / 13.0f;
    result[feature++] = static_cast<float>(move.y) / (BoardHeight - 1);
    result[feature++] = static_cast<float>(std::clamp(move.clearedLines, 0, 4)) / 4.0f;
    result[feature++] = static_cast<float>(std::clamp(rise.lines, 0, VisibleRows)) / VisibleRows;
    result[feature++] = static_cast<float>(survivingPlacedCells(move, rise.lines).size()) / 4.0f;

    return result;
}

Board TetrisEngine::applyGarbageRise(const Board& previous, const GarbageRise& rise) {
    return boardWithManualGarbage(previous, rise);
}

std::vector<std::uint16_t> TetrisEngine::garbageHolesFromObserved(const Board& observed, int lines) {
    return holesFromObserved(observed, lines);
}

std::vector<std::vector<std::uint16_t>> TetrisEngine::garbageHoleCandidates(const Board& observed, int lines) {
    return holeCandidatesFromObserved(observed, lines);
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
    // Incoming garbage is applied after the preceding active piece locks.
    // Generate the lock from the pre-rise board so its exported coordinates
    // remain in the frame where the player actually placed the piece.
    Board base = solved[index - 1].board;
    const auto risesAfter = [&](const Board& postClearBoard) {
        std::vector<GarbageRise> rises;
        if (overrideGarbage.has_value()) {
            GarbageRise rise = *overrideGarbage;
            rise.lines = std::clamp(rise.lines, 0, BoardHeight);
            rise.manuallySpecified = true;
            if (rise.holeMasks.size() > static_cast<std::size_t>(rise.lines)) {
                rise.holeMasks.resize(rise.lines);
            }
            rises.push_back(std::move(rise));
        } else {
            // Compare against the legal lock's post-clear board. This makes an
            // exact Gx9+hole row force a rise even when the same transition
            // cleared exactly as many old garbage rows.
            const BoardShift shift = sourceBoardShift(postClearBoard, currentRaw.observed,
                                                      settings.shiftThreshold);
            const GarbageHoleHints holeHints = garbageHoleLookaheadHints(raw, index);
            forEachGarbageRiseVariant(currentRaw.observed, currentRaw.probabilities, shift,
                                      [&](GarbageRise rise) { rises.push_back(std::move(rise)); },
                                      &holeHints);
        }
        return rises;
    };

    // A step represents the state after placing the preceding active mino.
    // Keep the exact same active/next fallback as runBeamSearch().
    if (previousRaw.piece == Cell::Empty) {
        const auto rises = risesAfter(base);
        for (const GarbageRise& rise : rises) {
            CandidateMove noPlacement;
            noPlacement.board = boardWithManualGarbage(base, rise);
            noPlacement.fullBoard = noPlacement.board;
            noPlacement.piece = Cell::Empty;
            candidates.push_back({noPlacement, rise,
                                  transitionObservationScore(noPlacement, rise, currentRaw.observed,
                                                              currentRaw.confidence,
                                                              currentRaw.probabilities,
                                                              settings.weights)});
        }
        return candidates;
    }

    Cell piece = previousRaw.piece;
    if (!isPiece(piece) && !previousRaw.next.empty() && isPiece(previousRaw.next[0])) piece = previousRaw.next[0];
    if (!isPiece(piece)) return candidates;

    for (CandidateMove move : legalMoves(base, piece)) {
        const auto rises = risesAfter(move.board);
        for (const GarbageRise& rise : rises) {
            CandidateMove result = move;
            result.board = boardWithManualGarbage(move.board, rise);
            const double score = transitionObservationScore(result, rise, currentRaw.observed,
                                                            currentRaw.confidence,
                                                            currentRaw.probabilities,
                                                            settings.weights);
            candidates.push_back({std::move(result), rise, score});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        return left.observationScore > right.observationScore;
    });
    return candidates;
}

namespace {

struct SourcePlacement {
    Cell piece = Cell::Empty;
    int x = 0;
    int y = 0;
    int rotation = 0;
    int clearedLines = 0;
};

struct SourceBeamNode {
    Board board{};
    Board fullBoard{};
    double score = 0;
    int parent = -1;
    std::size_t step = 0;
    GarbageRise garbage;
    SourcePlacement placement;
};

struct LegalMoveCacheKey {
    Board board{};
    Cell piece = Cell::Empty;

    bool operator==(const LegalMoveCacheKey& other) const {
        return piece == other.piece && board == other.board;
    }
};

struct LegalMoveCacheKeyHash {
    std::size_t operator()(const LegalMoveCacheKey& key) const {
        std::uint64_t hash = 1469598103934665603ull;
        hash ^= static_cast<std::uint8_t>(key.piece);
        hash *= 1099511628211ull;
        for (const Cell cell : key.board) {
            hash ^= static_cast<std::uint8_t>(cell);
            hash *= 1099511628211ull;
        }
        return static_cast<std::size_t>(hash);
    }
};

} // namespace

struct BeamSearchCache {
    Settings settings;
    bool initialized = false;
    std::vector<TimelineStep> raw;
    std::vector<TimelineStep> solved;
    std::vector<SourceBeamNode> nodes;
    std::vector<std::vector<int>> beams;
    std::vector<std::size_t> nodeEnds;
};

namespace {

bool sameBeamSettings(const Settings& left, const Settings& right) {
    const auto& a = left.weights;
    const auto& b = right.weights;
    return left.beamWidth == right.beamWidth &&
           left.shiftThreshold == right.shiftThreshold &&
           left.forceExactOnnxMatch == right.forceExactOnnxMatch &&
           left.completeLegalMoveSearch == right.completeLegalMoveSearch &&
           left.garbageBacktracking == right.garbageBacktracking &&
           left.useAccurateFallbackRanker == right.useAccurateFallbackRanker &&
           left.accurateFallbackRanker.get() == right.accurateFallbackRanker.get() &&
           left.garbageBacktrackingMaxLines == right.garbageBacktrackingMaxLines &&
           left.garbageAltHoleVariants == right.garbageAltHoleVariants &&
           left.extraSpawnRows == right.extraSpawnRows &&
           left.perfectClearEffectGuard == right.perfectClearEffectGuard &&
           left.perfectClearEffectLookbackSteps == right.perfectClearEffectLookbackSteps &&
           left.perfectClearEffectBeamWidth == right.perfectClearEffectBeamWidth &&
           left.perfectClearEffectObservationWeight == right.perfectClearEffectObservationWeight &&
           a.exactMatch == b.exactMatch &&
           a.emptyMatch == b.emptyMatch &&
           a.colorMismatch == b.colorMismatch &&
           a.missingBlock == b.missingBlock &&
           a.extraBlock == b.extraBlock &&
           a.placedColorMatch == b.placedColorMatch &&
           a.placedColorMismatch == b.placedColorMismatch &&
           a.placedColorComplete == b.placedColorComplete &&
           a.garbageHoleMatch == b.garbageHoleMatch &&
           a.garbageHoleMismatch == b.garbageHoleMismatch;
}

bool looksLikePerfectClearEffect(const Board& observed) {
    int lCells = 0;
    int occupiedCells = 0;
    int firstOccupiedRow = BoardHeight;
    int lastOccupiedRow = -1;
    for (int y = VisibleRows; y < BoardHeight; ++y) {
        bool occupiedRow = false;
        for (int x = 0; x < BoardWidth; ++x) {
            const Cell cell = observed[index(x, y)];
            if (!isOccupied(cell)) continue;
            occupiedRow = true;
            ++occupiedCells;
            if (cell == Cell::L) ++lCells;
        }
        if (occupiedRow) {
            firstOccupiedRow = std::min(firstOccupiedRow, y);
            lastOccupiedRow = y;
        }
    }
    // The classifier maps the orange PERFECT CLEAR letters to an implausible
    // L-majority column spanning most of the field.  Requiring all three
    // properties avoids reacting to an ordinary stack containing several Ls.
    const int occupiedSpan = lastOccupiedRow >= firstOccupiedRow
        ? lastOccupiedRow - firstOccupiedRow + 1 : 0;
    return lCells >= 24 && lCells * 2 >= occupiedCells && occupiedSpan >= 10;
}

std::vector<bool> perfectClearEffectProtectedSteps(
    const std::vector<TimelineStep>& raw, const Settings& settings,
    std::size_t anchorIndex, std::size_t lastStepInclusive) {
    std::vector<bool> protectedSteps(raw.size(), false);
    if (!settings.perfectClearEffectGuard || raw.empty()) return protectedSteps;
    for (std::size_t step = anchorIndex + 1; step <= lastStepInclusive; ++step) {
        if (!looksLikePerfectClearEffect(raw[step].observed)) continue;
        const std::size_t lookback = static_cast<std::size_t>(
            std::max(0, settings.perfectClearEffectLookbackSteps));
        const std::size_t first = step > lookback ? step - lookback : anchorIndex + 1;
        for (std::size_t guarded = std::max(first, anchorIndex + 1);
             guarded <= step; ++guarded) {
            protectedSteps[guarded] = true;
        }
    }
    return protectedSteps;
}

bool sameGarbageRise(const GarbageRise& left, const GarbageRise& right) {
    return left.lines == right.lines && left.holeMasks == right.holeMasks &&
           left.matchRatio == right.matchRatio &&
           left.manuallySpecified == right.manuallySpecified &&
           left.holesCertain == right.holesCertain;
}

bool sameRawStep(const TimelineStep& left, const TimelineStep& right) {
    return left.startSeconds == right.startSeconds &&
           left.timeSeconds == right.timeSeconds &&
           left.piece == right.piece && left.action == right.action &&
           left.hold == right.hold && left.next == right.next &&
           left.observed == right.observed && left.confidence == right.confidence &&
           left.probabilities == right.probabilities &&
           left.board == right.board && left.fullBoard == right.fullBoard &&
           sameGarbageRise(left.garbage, right.garbage) &&
           left.placedPiece == right.placedPiece &&
           left.placementX == right.placementX && left.placementY == right.placementY &&
           left.placementRotation == right.placementRotation &&
           left.clearedLines == right.clearedLines && left.score == right.score &&
           left.uncertain == right.uncertain &&
           left.pieceIdentityFlexible == right.pieceIdentityFlexible &&
           left.manuallyFixed == right.manuallyFixed &&
           left.recoveryUnresolved == right.recoveryUnresolved &&
           left.orderAutomaticallyResolved == right.orderAutomaticallyResolved &&
           left.holdUsed == right.holdUsed &&
           left.queueManuallyFixed == right.queueManuallyFixed;
}

SourcePlacement placementInfo(const TimelineStep& step) {
    return {step.placedPiece, step.placementX, step.placementY,
            step.placementRotation, step.clearedLines};
}

SourcePlacement placementInfo(const CandidateMove& move) {
    return {move.piece, move.x, move.y, move.rotation, move.clearedLines};
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
                                        std::vector<TimelineStep> result,
                                        const BeamProgressCallback& progress,
                                        BeamSearchCache* cache = nullptr,
                                        std::size_t lastStepInclusive = std::numeric_limits<std::size_t>::max()) {
    if (raw.empty() || anchorIndex >= raw.size()) return result;
    if (result.size() != raw.size()) result.resize(raw.size());
    lastStepInclusive = std::min(lastStepInclusive, raw.size() - 1);
    if (lastStepInclusive < anchorIndex) return result;
    // Incremental caches describe a complete raw timeline. Segment-local
    // reviewer recomputations deliberately leave the later tail untouched.
    if (lastStepInclusive + 1 != raw.size()) cache = nullptr;

    const TimelineStep anchor = result[anchorIndex];
    std::vector<SourceBeamNode> nodes;
    std::vector<int> beam;
    std::vector<std::vector<int>> checkpointBeams;
    std::vector<std::size_t> checkpointNodeEnds;
    std::size_t firstStep = anchorIndex + 1;

    if (cache && anchorIndex == 0 && cache->initialized &&
        sameBeamSettings(cache->settings, settings)) {
        std::size_t common = 0;
        const std::size_t limit = std::min(raw.size(), cache->raw.size());
        while (common < limit && sameRawStep(raw[common], cache->raw[common])) ++common;
        if (common == raw.size() && common == cache->raw.size() &&
            cache->solved.size() == raw.size()) {
            if (progress) progress(raw.size() - 1, raw.size() - 1,
                                   cache->beams.empty() ? 0 : cache->beams.back().size(), 0);
            return cache->solved;
        }
        if (common > 0 && common <= cache->beams.size() &&
            common <= cache->nodeEnds.size() &&
            cache->nodeEnds[common - 1] <= cache->nodes.size()) {
            nodes = std::move(cache->nodes);
            nodes.resize(cache->nodeEnds[common - 1]);
            beam = cache->beams[common - 1];
            checkpointBeams.assign(cache->beams.begin(), cache->beams.begin() + common);
            checkpointNodeEnds.assign(cache->nodeEnds.begin(), cache->nodeEnds.begin() + common);
            firstStep = common;
        }
    }
    if (beam.empty()) {
        nodes.reserve(std::max<std::size_t>((raw.size() - anchorIndex) * static_cast<std::size_t>(settings.beamWidth), 1));
        nodes.push_back({anchor.board, anchor.fullBoard, anchor.score, -1, anchorIndex,
                         anchor.garbage, placementInfo(anchor)});
        beam = {0};
        checkpointBeams.resize(anchorIndex + 1);
        checkpointNodeEnds.resize(anchorIndex + 1);
        checkpointBeams[anchorIndex] = beam;
        checkpointNodeEnds[anchorIndex] = nodes.size();
        firstStep = anchorIndex + 1;
    }
    const std::size_t totalSteps = lastStepInclusive - anchorIndex;
    if (progress) progress(firstStep - anchorIndex - 1, totalSteps, beam.size(), 0);
    const std::vector<bool> pcEffectProtected = perfectClearEffectProtectedSteps(
        raw, settings, anchorIndex, lastStepInclusive);

    // Different placement histories can retain the same post-clear board but
    // a different pre-clear board. Legal move generation depends only on the
    // former and the active piece, so reuse it on an exact key match within a
    // transition. Clearing at each step bounds the cache while preserving the
    // original move ordering byte-for-byte.
    std::unordered_map<LegalMoveCacheKey, std::vector<CandidateMove>, LegalMoveCacheKeyHash>
        legalMoveCache;

    for (std::size_t step = firstStep; step <= lastStepInclusive; ++step) {
        const TimelineStep& item = raw[step];
        const TimelineStep& previous = raw[step - 1];
        const bool protectFromPcEffect = pcEffectProtected[step];
        // The incoming beam is already score-ranked. Shrink it before the
        // first quarantined transition, rather than generating moves from as
        // many as 500 parents and throwing almost all of them away afterward.
        // The ordinary beam is untouched outside this short window.
        if (protectFromPcEffect &&
            (step == anchorIndex + 1 || !pcEffectProtected[step - 1]) &&
            beam.size() > static_cast<std::size_t>(settings.perfectClearEffectBeamWidth)) {
            beam.resize(static_cast<std::size_t>(settings.perfectClearEffectBeamWidth));
        }
        const BoardProbabilities* itemEvidence = hasProbabilityEvidence(item.probabilities)
            ? &item.probabilities : nullptr;
        const SourceShiftObservation shiftObservation = sourceShiftObservation(item.observed);
        const SourceScoreTable sourceTable =
            makeSourceScoreTable(item.observed, settings.weights, itemEvidence);
        const GarbageHoleScoreContext garbageScoreContext =
            makeGarbageHoleScoreContext(item.observed, itemEvidence);
        const GarbageHoleHints holeHints = garbageHoleLookaheadHints(raw, step);
        // The previous ONNX image may itself contain an effect.  Shift
        // detection is therefore evaluated against each reconstructed parent
        // below, not once from two noisy observations.
        // `pieceIdentityFlexible` is used only by the bounded board-driven
        // queue repair.
        // The ordinary decoder supplies exactly one active mino and keeps the
        // historical fast path byte-for-byte.  Once a sustained board
        // mismatch proves that the NEXT-derived path is suspect, the repair
        // pass may ask this transition to compare every mino (and no lock)
        // against the already collected board evidence.
        const bool flexiblePiece = previous.pieceIdentityFlexible &&
            !previous.manuallyFixed;
        const bool skipPlacement = previous.piece == Cell::Empty && !flexiblePiece;
        std::vector<Cell> possible;
        if (!skipPlacement && isPiece(previous.piece)) possible.push_back(previous.piece);
        if (!skipPlacement && possible.empty() && !previous.next.empty() && isPiece(previous.next[0])) {
            possible.push_back(previous.next[0]);
        }
        if (flexiblePiece) {
            for (const Cell piece : Pieces) {
                if (std::find(possible.begin(), possible.end(), piece) == possible.end()) {
                    possible.push_back(piece);
                }
            }
        }

        legalMoveCache.clear();
        legalMoveCache.reserve(beam.size() * std::max<std::size_t>(possible.size(), 1));
        const auto cachedLegalMoves = [&](const Board& board, Cell piece)
            -> const std::vector<CandidateMove>& {
            LegalMoveCacheKey key{board, piece};
            const auto found = legalMoveCache.find(key);
            if (found != legalMoveCache.end()) return found->second;
            auto inserted = legalMoveCache.emplace(
                std::move(key), settings.extraSpawnRows
                    ? TetrisEngine::legalMovesHighStack(board, piece, settings.completeLegalMoveSearch)
                    : settings.completeLegalMoveSearch
                    ? TetrisEngine::legalMoves(board, piece)
                    : TetrisEngine::legalMovesFast(board, piece));
            return inserted.first->second;
        };

        std::vector<SourceBeamNode> candidates;
        candidates.reserve(beam.size() * (skipPlacement ? 1 : 40));
        if (item.manuallyFixed) {
            // A correction is an explicit anchor, but it must still select a
            // parent that can legally reach that anchor.  Taking beam.front()
            // unconditionally made every later manual correction look right
            // in isolation while silently leaving a discontinuity immediately
            // before it after an earlier edit.
            for (int parentIndex : beam) {
                const SourceBeamNode& parent = nodes[parentIndex];
                bool reachesAnchor = false;
                if (!isPiece(item.placedPiece)) {
                    reachesAnchor = boardWithManualGarbage(parent.board, item.garbage) == item.board;
                } else {
                    for (const CandidateMove& move : cachedLegalMoves(parent.board, item.placedPiece)) {
                        if (move.x != item.placementX || move.y != item.placementY ||
                            move.rotation != item.placementRotation ||
                            move.clearedLines != item.clearedLines || move.fullBoard != item.fullBoard) continue;
                        if (boardWithManualGarbage(move.board, item.garbage) == item.board) {
                            reachesAnchor = true;
                            break;
                        }
                    }
                }
                if (!reachesAnchor) continue;
                candidates.push_back({item.board, item.fullBoard, parent.score + 100000.0,
                                      parentIndex, step, item.garbage, placementInfo(item)});
            }
            if (candidates.empty() && !beam.empty()) {
                // Preserve an explicitly reviewed board even when no retained
                // parent can reach it.  The continuity validator will keep
                // export guarded and the reviewer can repair the preceding
                // segment, while no approved anchor is ever discarded.
                const SourceBeamNode& parent = nodes[beam.front()];
                candidates.push_back({item.board, item.fullBoard, parent.score + 100000.0,
                                      beam.front(), step, item.garbage, placementInfo(item)});
            }
        } else {
            for (int parentIndex : beam) {
                const SourceBeamNode& parent = nodes[parentIndex];
                // A rise belongs to the state after this transition.  Keep
                // legal-move generation on the pre-rise parent board, then
                // shift only the locked/cleared board.  Otherwise every
                // exported placement coordinate is spuriously lifted.
                const Board& base = parent.board;

                const auto appendNoPlacement = [&](double correctionPenalty) {
                    const BoardShift shift = sourceBoardShift(
                        base, item.observed, settings.shiftThreshold, shiftObservation);
                    std::vector<GarbageRise> rises = garbageRiseHypotheses(
                        base, item.observed, item.probabilities, shift, settings, &holeHints);
                    if (protectFromPcEffect && shift.lines > 0) {
                        // PERFECT CLEAR can paint an exact-looking Gx9 row,
                        // but real incoming garbage is also common here. Keep
                        // the detected rise and the no-rise hypothesis; the
                        // first clean observation decides between them.
                        if (std::none_of(rises.begin(), rises.end(),
                                [](const GarbageRise& rise) { return rise.lines == 0; })) {
                            rises.push_back({});
                        }
                    }
                    for (const GarbageRise& rise : rises) {
                        const Board shifted = boardWithManualGarbage(base, rise);
                        CandidateMove placement = noPlacement(shifted);
                        const double observationScore = protectFromPcEffect
                            ? perfectClearEffectMaskedScore(shifted, item.observed, sourceTable)
                            : transitionObservationScoreWithEvidence(
                                  placement, shifted, rise, item.observed, item.confidence,
                                  itemEvidence, settings.weights, &sourceTable,
                                  &garbageScoreContext,
                                  settings.useAccurateFallbackRanker
                                      ? settings.accurateFallbackRanker.get() : nullptr);
                        candidates.push_back({shifted, shifted,
                                              parent.score + observationScore *
                                                  (protectFromPcEffect
                                                       ? settings.perfectClearEffectObservationWeight
                                                       : 1.0) - correctionPenalty,
                                              parentIndex, step, rise, placementInfo(placement)});
                    }
                };
                if (skipPlacement) {
                    appendNoPlacement(0.0);
                    continue;
                }

                // A false CNN queue slide creates a phase even though no
                // mino locked. Keep that deletion hypothesis, but make it pay
                // a prior so an ordinary low-information frame cannot erase a
                // real placement. Exact board agreement over later frames is
                // what can overcome the prior.
                // A missed HOLD is observed as a NEXT slide even though no
                // mino locked.  Once the board trajectory is already known
                // to be broken, charging ninety points made that physically
                // necessary deletion unreachable (match 17 counted the held
                // I as a placement).  Keep a small preference for a real lock
                // and let the following settled boards prove the deletion.
                if (flexiblePiece) appendNoPlacement(4.0);

                for (Cell piece : possible) {
                    // Only locks reachable from the simulator's spawn state
                    // through translations and SRS rotations are candidates.
                    for (const CandidateMove& move : cachedLegalMoves(base, piece)) {
                        const bool perfectClearCandidate = protectFromPcEffect &&
                            move.clearedLines > 0 &&
                            std::none_of(move.board.begin(), move.board.end(),
                                [](Cell cell) { return isOccupied(cell); });
                        // Garbage arrives after lock and line clear. Detect it
                        // against this exact post-clear board, not the parent.
                        const BoardShift shift = sourceBoardShift(
                            move.board, item.observed, settings.shiftThreshold, shiftObservation);
                        std::vector<GarbageRise> rises = garbageRiseHypotheses(
                            move.board, item.observed, item.probabilities, shift, settings, &holeHints);
                        if (protectFromPcEffect && shift.lines > 0) {
                            if (std::none_of(rises.begin(), rises.end(),
                                    [](const GarbageRise& rise) { return rise.lines == 0; })) {
                                rises.push_back({});
                            }
                        }
                        for (const GarbageRise& rise : rises) {
                            const Board shifted = boardWithManualGarbage(move.board, rise);
                            const double observationScore = protectFromPcEffect
                                ? perfectClearEffectMaskedScore(shifted, item.observed, sourceTable)
                                : transitionObservationScoreWithEvidence(
                                      move, shifted, rise, item.observed, item.confidence,
                                      itemEvidence, settings.weights, &sourceTable,
                                      &garbageScoreContext,
                                      settings.useAccurateFallbackRanker
                                          ? settings.accurateFallbackRanker.get() : nullptr);
                            // The L-majority effect is direct evidence that a
                            // perfect clear happened somewhere in this short
                            // protected window.  Without this branch prior,
                            // ignoring the bad pixels merely leaves too many
                            // ordinary placement histories for a finite beam.
                            constexpr double PerfectClearEffectBranchBonus = 100000.0;
                            // This branch runs only after a sustained board
                            // mismatch has already proved the queue identity
                            // unreliable.  A large per-piece prior used to
                            // make a one-phase alignment error effectively
                            // permanent: correcting a shifted run of five
                            // pieces cost 5*55 points before the board was
                            // considered.  Keep only a small deterministic
                            // preference for the CNN identity and let the
                            // multi-frame board trajectory decide the run.
                            const double correctionPenalty = flexiblePiece &&
                                isPiece(previous.piece) && piece != previous.piece
                                ? 2.0 : 0.0;
                            candidates.push_back({shifted, move.fullBoard,
                                                  parent.score + observationScore *
                                                      (protectFromPcEffect
                                                           ? settings.perfectClearEffectObservationWeight
                                                           : 1.0) +
                                                      (perfectClearCandidate
                                                           ? PerfectClearEffectBranchBonus : 0.0) -
                                                      correctionPenalty,
                                                  parentIndex, step, rise, placementInfo(move)});
                        }
                    }
                }
            }
        }

        if (candidates.empty()) {
            // A legal history cannot be continued. Preserve the best legal
            // prefix and make the remaining segment explicitly unresolved so
            // the correction UI can repair the placement before this point.
            // Never replace the physical board with the CNN observation: that
            // would silently skip a move and make a corrupted match look
            // recovered.
            if (cache) cache->initialized = false;
            if (progress) progress(step - anchorIndex, totalSteps, 0, 0);

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
                history.recoveryUnresolved = false;
                result[current.step] = std::move(history);
                node = current.parent;
            }

            const SourceBeamNode& lastLegal = nodes[beam.front()];
            for (std::size_t unresolved = step;
                 unresolved <= lastStepInclusive; ++unresolved) {
                TimelineStep pending = raw[unresolved];
                if (!pending.manuallyFixed) {
                    pending.board = lastLegal.board;
                    pending.fullBoard = lastLegal.board;
                    pending.garbage = {};
                    pending.placedPiece = Cell::Empty;
                    pending.placementX = 0;
                    pending.placementY = 0;
                    pending.placementRotation = 0;
                    pending.clearedLines = 0;
                    pending.score = lastLegal.score;
                }
                pending.recoveryUnresolved = true;
                result[unresolved] = std::move(pending);
            }
            return result;
        }

        // Collapse transpositions before applying the beam limit.  The board
        // and pre-clear board include colours, so two paths are merged only
        // when they are equivalent for both future physics and ONNX scoring.
        std::unordered_multimap<std::uint64_t, std::size_t> unique;
        unique.reserve(candidates.size());
        std::vector<SourceBeamNode> deduplicated;
        deduplicated.reserve(candidates.size());
        for (auto& candidate : candidates) {
            const std::uint64_t hash = boardPairHash(candidate.board, candidate.fullBoard);
            const auto range = unique.equal_range(hash);
            std::size_t duplicate = deduplicated.size();
            for (auto found = range.first; found != range.second; ++found) {
                const auto& prior = deduplicated[found->second];
                if (candidate.board == prior.board && candidate.fullBoard == prior.fullBoard) {
                    duplicate = found->second;
                    break;
                }
            }
            if (duplicate == deduplicated.size()) {
                unique.emplace(hash, deduplicated.size());
                deduplicated.push_back(std::move(candidate));
            } else if (candidate.score > deduplicated[duplicate].score) {
                deduplicated[duplicate] = std::move(candidate);
            }
        }
        candidates = std::move(deduplicated);

        // An exact legal result for the current ONNX frame is a hard
        // transition constraint. Previously candidates were sorted only by
        // their cumulative score, so a perfect current board could lose to a
        // merely similar board that had scored better on older frames.
        // Explicit manual corrections remain anchors and are never replaced.
        if (!item.manuallyFixed && settings.forceExactOnnxMatch && !protectFromPcEffect) {
            const bool hasExactOnnxCandidate = std::any_of(candidates.begin(), candidates.end(),
                [&item](const SourceBeamNode& candidate) {
                    return exactVisibleOnnxMatch(candidate.board, item.observed, candidate.garbage);
                });
            if (hasExactOnnxCandidate) {
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                    [&item](const SourceBeamNode& candidate) {
                        return !exactVisibleOnnxMatch(candidate.board, item.observed, candidate.garbage);
                    }), candidates.end());
            }
        }

        const std::size_t candidateCount = candidates.size();
        std::vector<std::size_t> ranking(candidates.size());
        std::iota(ranking.begin(), ranking.end(), 0);
        const auto ranksBefore = [&candidates](std::size_t left, std::size_t right) {
            if (candidates[left].score != candidates[right].score) {
                return candidates[left].score > candidates[right].score;
            }
            // This reproduces stable_sort's original order for score ties and
            // makes partial_sort's result fully deterministic.
            return left < right;
        };
        // Do not let a low-information animation frame carry the whole normal
        // beam into the next transition. The PC branch bonus already ranks the
        // legal empty-board trajectory first, so a small local lane is enough.
        // Ordinary steps continue to use the configured production beam.
        const int stepBeamWidth = protectFromPcEffect
            ? settings.perfectClearEffectBeamWidth
            : settings.beamWidth;
        const std::size_t retained = std::min(
            ranking.size(), static_cast<std::size_t>(stepBeamWidth));
        if (retained < ranking.size()) {
            std::partial_sort(ranking.begin(), ranking.begin() + retained,
                              ranking.end(), ranksBefore);
            ranking.resize(retained);
        } else {
            std::sort(ranking.begin(), ranking.end(), ranksBefore);
        }
        beam.clear();
        beam.reserve(ranking.size());
        for (const std::size_t candidateIndex : ranking) {
            nodes.push_back(std::move(candidates[candidateIndex]));
            beam.push_back(static_cast<int>(nodes.size() - 1));
        }
        if (cache) {
            checkpointBeams.push_back(beam);
            checkpointNodeEnds.push_back(nodes.size());
        }
        if (progress) progress(step - anchorIndex, totalSteps, beam.size(), candidateCount);
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
        history.recoveryUnresolved = false;
        result[current.step] = std::move(history);
        node = current.parent;
    }
    if (progress) progress(totalSteps, totalSteps, beam.size(), 0);
    if (cache) {
        cache->settings = settings;
        cache->initialized = true;
        cache->raw = raw;
        cache->solved = result;
        cache->nodes = std::move(nodes);
        cache->beams = std::move(checkpointBeams);
        cache->nodeEnds = std::move(checkpointNodeEnds);
    }
    return result;
}

} // namespace

std::vector<TimelineStep> TetrisEngine::beamSearch(const std::vector<TimelineStep>& raw, const Settings& settings,
                                                   BeamProgressCallback progress) {
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
    return runSourceBeam(raw, settings, 0, std::move(result), progress);
}

std::vector<TimelineStep> TetrisEngine::beamSearchIncremental(
    const std::vector<TimelineStep>& raw, const Settings& settings,
    std::shared_ptr<BeamSearchCache>& cache, BeamProgressCallback progress) {
    if (raw.empty()) {
        cache.reset();
        return {};
    }
    if (!cache) cache = std::make_shared<BeamSearchCache>();
    std::vector<TimelineStep> result(raw.size());
    TimelineStep initial = raw.front();
    if (!initial.manuallyFixed) {
        initial.board = initial.observed;
        initial.fullBoard = initial.observed;
        initial.garbage = {};
    }
    initial.score = 0;
    result[0] = std::move(initial);
    return runSourceBeam(raw, settings, 0, std::move(result), progress, cache.get());
}

std::vector<TimelineStep> TetrisEngine::recomputeFrom(const std::vector<TimelineStep>& raw,
                                                       const std::vector<TimelineStep>& solved,
                                                       std::size_t firstStep,
                                                       const Settings& settings) {
    if (raw.empty()) return {};
    if (firstStep == 0 || firstStep >= raw.size() || solved.size() != raw.size()) {
        return beamSearch(raw, settings);
    }

    std::vector<TimelineStep> result = solved;
    std::size_t anchorIndex = firstStep;
    if (!raw[firstStep].manuallyFixed) {
        anchorIndex = 0;
        for (std::size_t index = firstStep; index-- > 0;) {
            if (raw[index].manuallyFixed) {
                anchorIndex = index;
                break;
            }
        }
    }

    if (anchorIndex == 0 && !raw[0].manuallyFixed) {
        TimelineStep initial = raw.front();
        initial.board = initial.observed;
        initial.fullBoard = initial.observed;
        initial.garbage = {};
        initial.score = 0;
        result[0] = std::move(initial);
    } else {
        TimelineStep anchor = raw[anchorIndex];
        anchor.score = anchorIndex == 0
            ? 0
            : result[anchorIndex - 1].score + sourceScore(
                  anchor.board, anchor.observed, settings.weights,
                  hasProbabilityEvidence(anchor.probabilities) ? &anchor.probabilities : nullptr);
        result[anchorIndex] = std::move(anchor);
    }

    // Manual corrections divide the timeline into immutable segments. Re-run
    // only as far as the next anchor: its board/placement remains explicit,
    // and every later reviewed segment stays byte-for-byte untouched.
    std::size_t lastStep = raw.size() - 1;
    for (std::size_t index = firstStep + 1; index < raw.size(); ++index) {
        if (raw[index].manuallyFixed) {
            lastStep = index;
            break;
        }
    }
    return runSourceBeam(raw, settings, anchorIndex, std::move(result), {}, nullptr, lastStep);
}

std::vector<PlacementOrderAmbiguity> TetrisEngine::placementOrderAmbiguities(
    const std::vector<TimelineStep>& raw,
    const std::vector<TimelineStep>& solved,
    const Settings& settings) {
    std::vector<PlacementOrderAmbiguity> ambiguities;
    if (raw.size() != solved.size() || solved.size() < 3) return ambiguities;

    const double margin = std::max(0.0, settings.orderAmbiguityScoreMargin);
    for (std::size_t firstStep = 1; firstStep + 1 < solved.size(); ++firstStep) {
        const std::size_t secondStep = firstStep + 1;
        const TimelineStep& selectedFirst = solved[firstStep];
        const TimelineStep& selectedSecond = solved[secondStep];
        if (raw[firstStep].manuallyFixed ||
            selectedFirst.orderAutomaticallyResolved ||
            selectedFirst.recoveryUnresolved || selectedSecond.recoveryUnresolved ||
            !isPiece(selectedFirst.placedPiece) ||
            selectedFirst.placedPiece != selectedSecond.placedPiece) continue;

        const Cell piece = selectedFirst.placedPiece;
        const Board& start = solved[firstStep - 1].board;
        const auto firstMoves = legalMoves(start, piece);
        auto selectedMove = std::find_if(firstMoves.begin(), firstMoves.end(), [&](const CandidateMove& move) {
            return move.x == selectedFirst.placementX && move.y == selectedFirst.placementY &&
                   move.rotation == selectedFirst.placementRotation &&
                   move.clearedLines == selectedFirst.clearedLines &&
                   move.fullBoard == selectedFirst.fullBoard;
        });
        if (selectedMove == firstMoves.end()) continue;

        CandidateMove scoredSelected = *selectedMove;
        scoredSelected.board = selectedFirst.board;
        const double selectedScore = transitionObservationScore(
            scoredSelected, selectedFirst.garbage, raw[firstStep].observed,
            raw[firstStep].confidence, raw[firstStep].probabilities, settings.weights);

        std::optional<CandidateMove> bestAlternate;
        double bestAlternateScore = -std::numeric_limits<double>::infinity();
        for (CandidateMove firstMove : firstMoves) {
            const Board intermediate = boardWithManualGarbage(firstMove.board, selectedFirst.garbage);
            if (intermediate == selectedFirst.board) continue;

            bool reachesSameEndpoint = false;
            for (const CandidateMove& secondMove : legalMoves(intermediate, piece)) {
                if (boardWithManualGarbage(secondMove.board, selectedSecond.garbage) == selectedSecond.board) {
                    reachesSameEndpoint = true;
                    break;
                }
            }
            if (!reachesSameEndpoint) continue;

            firstMove.board = intermediate;
            const double alternateScore = transitionObservationScore(
                firstMove, selectedFirst.garbage, raw[firstStep].observed,
                raw[firstStep].confidence, raw[firstStep].probabilities, settings.weights);
            if (alternateScore > bestAlternateScore) {
                bestAlternateScore = alternateScore;
                bestAlternate = std::move(firstMove);
            }
        }
        if (!bestAlternate.has_value() || bestAlternateScore + margin < selectedScore) continue;
        ambiguities.push_back({firstStep, secondStep, selectedScore,
                               bestAlternateScore, std::move(*bestAlternate)});
    }
    return ambiguities;
}

std::size_t TetrisEngine::resolvePlacementOrderAmbiguities(
    const std::vector<TimelineStep>& raw,
    std::vector<TimelineStep>& solved,
    const Settings& settings,
    double minimumScoreGain) {
    if (raw.size() != solved.size() || solved.size() < 3) return 0;

    const auto issues = placementOrderAmbiguities(raw, solved, settings);
    std::size_t resolved = 0;
    std::size_t lastResolvedSecond = 0;
    const double requiredGain = std::max(0.0, minimumScoreGain);

    for (const PlacementOrderAmbiguity& issue : issues) {
        const std::size_t firstStep = issue.firstStep;
        const std::size_t secondStep = issue.secondStep;
        if (firstStep == 0 || secondStep != firstStep + 1 ||
            secondStep >= solved.size() ||
            (resolved != 0 && firstStep <= lastResolvedSecond) ||
            raw[firstStep].manuallyFixed || raw[secondStep].manuallyFixed) {
            continue;
        }

        const TimelineStep& selectedFirst = solved[firstStep];
        const TimelineStep& selectedSecond = solved[secondStep];
        if (!isPiece(selectedFirst.placedPiece) ||
            selectedFirst.placedPiece != selectedSecond.placedPiece) {
            continue;
        }

        const Cell piece = selectedFirst.placedPiece;
        const Board& start = solved[firstStep - 1].board;
        const Board endpoint = selectedSecond.board;
        const auto firstMoves = legalMoves(start, piece);

        double selectedPairScore = -std::numeric_limits<double>::infinity();
        double bestPairScore = -std::numeric_limits<double>::infinity();
        CandidateMove bestFirst;
        CandidateMove bestSecond;
        bool foundBest = false;

        for (CandidateMove firstMove : firstMoves) {
            const bool selectedFirstMove =
                firstMove.x == selectedFirst.placementX &&
                firstMove.y == selectedFirst.placementY &&
                firstMove.rotation == selectedFirst.placementRotation &&
                firstMove.clearedLines == selectedFirst.clearedLines &&
                firstMove.fullBoard == selectedFirst.fullBoard;
            const Board intermediate = boardWithManualGarbage(
                firstMove.board, selectedFirst.garbage);
            firstMove.board = intermediate;
            const double firstScore = transitionObservationScore(
                firstMove, selectedFirst.garbage, raw[firstStep].observed,
                raw[firstStep].confidence, raw[firstStep].probabilities,
                settings.weights);

            for (CandidateMove secondMove : legalMoves(intermediate, piece)) {
                const bool selectedSecondMove =
                    secondMove.x == selectedSecond.placementX &&
                    secondMove.y == selectedSecond.placementY &&
                    secondMove.rotation == selectedSecond.placementRotation &&
                    secondMove.clearedLines == selectedSecond.clearedLines &&
                    secondMove.fullBoard == selectedSecond.fullBoard;
                const Board finalBoard = boardWithManualGarbage(
                    secondMove.board, selectedSecond.garbage);
                if (finalBoard != endpoint) continue;
                secondMove.board = finalBoard;
                const double pairScore = firstScore + transitionObservationScore(
                    secondMove, selectedSecond.garbage, raw[secondStep].observed,
                    raw[secondStep].confidence, raw[secondStep].probabilities,
                    settings.weights);

                if (selectedFirstMove && selectedSecondMove) {
                    selectedPairScore = std::max(selectedPairScore, pairScore);
                }
                if (!foundBest || pairScore > bestPairScore) {
                    foundBest = true;
                    bestPairScore = pairScore;
                    bestFirst = firstMove;
                    bestSecond = secondMove;
                }
            }
        }

        if (!foundBest || !std::isfinite(selectedPairScore) ||
            bestFirst.board == selectedFirst.board ||
            bestPairScore <= selectedPairScore + requiredGain) {
            continue;
        }

        const double oldSecondCumulativeScore = selectedSecond.score;
        TimelineStep correctedFirst = selectedFirst;
        correctedFirst.board = bestFirst.board;
        correctedFirst.fullBoard = bestFirst.fullBoard;
        correctedFirst.placedPiece = bestFirst.piece;
        correctedFirst.placementX = bestFirst.x;
        correctedFirst.placementY = bestFirst.y;
        correctedFirst.placementRotation = bestFirst.rotation;
        correctedFirst.clearedLines = bestFirst.clearedLines;
        correctedFirst.orderAutomaticallyResolved = true;
        correctedFirst.score = solved[firstStep - 1].score +
            transitionObservationScore(
                bestFirst, correctedFirst.garbage, raw[firstStep].observed,
                raw[firstStep].confidence, raw[firstStep].probabilities,
                settings.weights);

        TimelineStep correctedSecond = selectedSecond;
        correctedSecond.board = bestSecond.board;
        correctedSecond.fullBoard = bestSecond.fullBoard;
        correctedSecond.placedPiece = bestSecond.piece;
        correctedSecond.placementX = bestSecond.x;
        correctedSecond.placementY = bestSecond.y;
        correctedSecond.placementRotation = bestSecond.rotation;
        correctedSecond.clearedLines = bestSecond.clearedLines;
        correctedSecond.score = correctedFirst.score +
            transitionObservationScore(
                bestSecond, correctedSecond.garbage, raw[secondStep].observed,
                raw[secondStep].confidence, raw[secondStep].probabilities,
                settings.weights);

        const double scoreDelta = correctedSecond.score - oldSecondCumulativeScore;
        solved[firstStep] = std::move(correctedFirst);
        solved[secondStep] = std::move(correctedSecond);
        for (std::size_t later = secondStep + 1; later < solved.size(); ++later) {
            solved[later].score += scoreDelta;
        }
        ++resolved;
        lastResolvedSecond = secondStep;
    }
    return resolved;
}

} // namespace tr
