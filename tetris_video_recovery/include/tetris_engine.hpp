#pragma once

#include "common.hpp"

namespace tr {

class TetrisEngine {
public:
    struct Shape {
        std::array<std::array<int, 2>, 4> cells{};
    };

    static Shape shape(Cell piece, int rotation);
    static bool fits(const Board& board, const Shape& shape, int x, int y);
    // Exact grounded-placement enumeration used by 動画解析.html.
    static std::vector<CandidateMove> originalLegalMoves(const Board& board, Cell piece);
    // SRS reachability remains available for a future simulator-only mode.
    static std::vector<CandidateMove> legalMoves(const Board& board, Cell piece);
    static std::optional<Board> garbageShift(const Board& previous, const Board& observed, int lines, double& ratio);
    static double observationScore(const Board& predicted, const Board& observed, const Confidence& confidence);
    // Same garbage-rise detector and weighted 40-row score used by the
    // source-compatible beam search.
    static BoardShift detectBoardShift(const Board& previous, const Board& observed, double threshold);
    static double sourceObservationScore(const Board& predicted, const Board& observed, const ScoreWeights& weights);
    // Create a pre-placement board after explicitly receiving garbage.  This
    // is deliberately separate from the source-compatible automatic detector
    // so manual correction can work when the bottom rows were never observed.
    static Board applyGarbageRise(const Board& previous, const GarbageRise& rise);
    static std::vector<std::uint16_t> garbageHolesFromObserved(const Board& observed, int lines);
    // Enumerate only the legal source-compatible candidates for the selected
    // transition.  The result is based on the already-corrected previous
    // board, so it remains valid after multiple manual corrections.
    static std::vector<CorrectionCandidate> correctionCandidates(const std::vector<TimelineStep>& raw,
                                                                   const std::vector<TimelineStep>& solved,
                                                                   std::size_t index,
                                                                   const Settings& settings,
                                                                   const std::optional<GarbageRise>& overrideGarbage = std::nullopt);
    static std::vector<TimelineStep> beamSearch(const std::vector<TimelineStep>& raw, const Settings& settings);
    // Recompute the selected phase and all following phases while preserving
    // the fixed history before `firstStep`.
    static std::vector<TimelineStep> recomputeFrom(const std::vector<TimelineStep>& raw,
                                                   const std::vector<TimelineStep>& solved,
                                                   std::size_t firstStep,
                                                   const Settings& settings);

private:
    struct Offset { int x; int y; };
    static std::vector<Offset> rotationOffsets(Cell piece, int from, int to);
    static std::array<int, 2> spawnPosition(Cell piece);
    static CandidateMove lock(const Board& board, Cell piece, const Shape& shape, int x, int y, int rotation);
};

} // namespace tr
