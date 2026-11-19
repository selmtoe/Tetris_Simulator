#pragma once

#include "common.hpp"

#include <memory>

namespace tr {

constexpr std::size_t NeuralRankFeatureCount = 85;

struct BeamSearchCache;

// Reports one completed transition of the source-compatible beam.  The
// callback is deliberately optional so the hot path used by the reviewer can
// remain unchanged when no progress UI is attached.
using BeamProgressCallback = std::function<void(std::size_t completedSteps,
                                                std::size_t totalSteps,
                                                std::size_t beamSize,
                                                std::size_t candidateCount)>;

class TetrisEngine {
public:
    struct Shape {
        std::array<std::array<int, 2>, 4> cells{};
    };

    static Shape shape(Cell piece, int rotation);
    static bool fits(const Board& board, const Shape& shape, int x, int y);
    // Legacy exhaustive grounded-placement superset retained for regression
    // and performance comparison. It does not prove spawn reachability.
    static std::vector<CandidateMove> originalLegalMoves(const Board& board, Cell piece);
    // Complete simulator-reachable placement generation from the normal spawn,
    // with an extra upward fallback row for high stacks, translations, sonic
    // drop, and CW/CCW SRS kicks.
    static std::vector<CandidateMove> legalMoves(const Board& board, Cell piece);
    // Fast first pass used by the beam. It omits intermediate soft-drop states;
    // candidate exhaustion is retried through legalMoves() automatically.
    static std::vector<CandidateMove> legalMovesFast(const Board& board, Cell piece);
    // v79: the same generators with the high-stack spawn fallback (repair ladder only).
    static std::vector<CandidateMove> legalMovesHighStack(const Board& board, Cell piece,
                                                          bool completeSoftDrop);
    static std::optional<Board> garbageShift(const Board& previous, const Board& observed, int lines, double& ratio);
    static double observationScore(const Board& predicted, const Board& observed, const Confidence& confidence);
    // Same garbage-rise detector and weighted 40-row score used by the
    // source-compatible beam search.
    static BoardShift detectBoardShift(const Board& previous, const Board& observed, double threshold);
    static double sourceObservationScore(const Board& predicted, const Board& observed, const ScoreWeights& weights);
    // Decompose the exact production transition score into the ten linear
    // ScoreWeights coefficients. This is used by the offline rank trainer;
    // the runtime hot path remains unchanged.
    static std::array<double, 10> transitionScoreFeatures(
        const CandidateMove& move, const GarbageRise& rise,
        const Board& observed, const Confidence& confidence,
        const BoardProbabilities& probabilities);
    // Candidate-local evidence and board-shape context used by the accurate
    // fallback ranker. The first ten fields are exactly the production linear
    // score decomposition; the remaining fields are normalized spatial and
    // structural context with no teacher-only information.
    static std::array<float, NeuralRankFeatureCount> neuralRankFeatures(
        const CandidateMove& move, const GarbageRise& rise,
        const Board& observed, const Confidence& confidence,
        const BoardProbabilities& probabilities);
    // Create a pre-placement board after explicitly receiving garbage.  This
    // is deliberately separate from the source-compatible automatic detector
    // so manual correction can work when the bottom rows were never observed.
    static Board applyGarbageRise(const Board& previous, const GarbageRise& rise);
    static std::vector<std::uint16_t> garbageHolesFromObserved(const Board& observed, int lines);
    // A structurally exact Gx9 + empty row has one authoritative hole. For an
    // incomplete/effect-obscured row, every column remains possible and is
    // ordered by the current softmax evidence. The outer vector is one option
    // set per row.
    static std::vector<std::vector<std::uint16_t>> garbageHoleCandidates(const Board& observed, int lines);
    // Enumerate only the legal source-compatible candidates for the selected
    // transition.  The result is based on the already-corrected previous
    // board, so it remains valid after multiple manual corrections.
    static std::vector<CorrectionCandidate> correctionCandidates(const std::vector<TimelineStep>& raw,
                                                                   const std::vector<TimelineStep>& solved,
                                                                   std::size_t index,
                                                                   const Settings& settings,
                                                                   const std::optional<GarbageRise>& overrideGarbage = std::nullopt);
    static std::vector<TimelineStep> beamSearch(const std::vector<TimelineStep>& raw, const Settings& settings,
                                                BeamProgressCallback progress = {});
    // Reuse the exact beam frontier for an unchanged raw prefix, then expand
    // only newly added or changed phases.  The retained arena includes every
    // parent needed for backtracking, so this is bit-for-bit equivalent to a
    // fresh beam search; incompatible settings or inputs fall back safely.
    static std::vector<TimelineStep> beamSearchIncremental(
        const std::vector<TimelineStep>& raw, const Settings& settings,
        std::shared_ptr<BeamSearchCache>& cache, BeamProgressCallback progress = {});
    // Recompute the selected phase and all following phases while preserving
    // the fixed history before `firstStep`.
    static std::vector<TimelineStep> recomputeFrom(const std::vector<TimelineStep>& raw,
                                                   const std::vector<TimelineStep>& solved,
                                                   std::size_t firstStep,
                                                   const Settings& settings);
    // Find unresolved order swaps for consecutive identical minos.  A manual
    // anchor on the intermediate step is treated as an explicit resolution.
    static std::vector<PlacementOrderAmbiguity> placementOrderAmbiguities(
        const std::vector<TimelineStep>& raw,
        const std::vector<TimelineStep>& solved,
        const Settings& settings);
    // Resolve a detected identical-piece order directly when the alternate
    // commuting path has stronger CNN probability evidence. Both placements
    // are rewritten together and the already-selected endpoint is preserved,
    // so no later board or queue phase is changed.
    static std::size_t resolvePlacementOrderAmbiguities(
        const std::vector<TimelineStep>& raw,
        std::vector<TimelineStep>& solved,
        const Settings& settings,
        double minimumScoreGain = 0.0);

private:
    struct Offset { int x; int y; };
    static std::array<Offset, 5> rotationOffsets(Cell piece, int from, int to);
    static std::array<int, 2> spawnPosition(Cell piece);
    static CandidateMove lock(const Board& board, Cell piece, const Shape& shape, int x, int y, int rotation);
    static std::vector<CandidateMove> legalMovesImpl(const Board& board, Cell piece,
                                                     bool completeSoftDrop,
                                                     bool extraSpawnRows = false);
};

} // namespace tr
