#pragma once

#include "common.hpp"
#include "onnx_model.hpp"
#include "queue_onnx_model.hpp"

namespace tr {

struct BoardAnalysisInput {
    // 200 row-major RGB cell tensors, each [3,32,32] in the [0,1] range.
    std::vector<float> features;
    VisibleBoard classic{};
};

// One horizontal crop offset is applied to the entire 10x20 playfield. The
// score therefore measures agreement over all 200 cells; the suspected
// garbage rows only decide whether the more expensive retry search is needed.
struct HorizontalAlignmentScore {
    int targetRows = 0;
    int exactOccupiedCells = 0;
    int exactEmptyCells = 0;
    int occupiedColorMismatches = 0;
    int occupancyMismatches = 0;
    int exactGarbageRows = 0;
    int cleanGarbageRows = 0;
    int garbagePieceCells = 0;

    int value() const;
    bool needsRetry() const;
};

std::vector<int> horizontalAlignmentRows(const VisibleBoard& classic);
HorizontalAlignmentScore scoreHorizontalAlignment(
    const VisibleBoard& classic,
    const BoardObservation& observation,
    const std::vector<int>& targetRows);

// Recognition-layer missing-data handling. An unclear CNN frame cannot
// advance HOLD/NEXT and never consults the legacy colour classifier.
QueueObservation stabilizeQueueCnnObservation(
    QueueObservation observation,
    std::optional<QueueObservation>& trusted);

class VisionAnalyzer {
public:
    VisionAnalyzer(int frameWidth, int frameHeight, const PlayerLayout& layout, const OnnxBoardModel* model);

    // Exact ports of analyzeNextQueue and analyzeBoardOnly in 動画解析.html.
    QueueObservation observeQueue(const Frame& frame) const;
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    // Rebuild the exact legacy HOLD/NEXT observation from GPU-reduced sums.
    // Each slot stores {sumR, sumG, sumB, nonBlackPixelCount}; pixels whose
    // three channels are all <= 50 are deliberately excluded, matching
    // observeQueue(Frame) byte-for-byte after its final rounding step.
    QueueObservation observeQueueFromColorStats(
        const std::array<std::array<std::uint64_t, 4>, 6>& stats) const;
    // Append this player's six normalized 48x48 RGB crops to one NCHW batch.
    // Both players are appended before ONNX inference so one call covers all
    // 12 HOLD/NEXT slots in a decoded video frame.
    void appendQueueCnnInput(const Frame& frame, std::vector<float>& batch,
                             std::vector<double>& darkRatios,
                             int darkPixelLimit) const;
    // Smallest source rectangle containing every legacy colour sample and
    // every bilinear CNN input pixel for this player's HOLD/NEXT slots.
    PixelRegion queuePixelRegion() const;
    // Per-slot version used by sparse readback.  Six small rectangles avoid
    // copying the gaps between HOLD and vertically separated NEXT slots.
    std::vector<PixelRegion> queuePixelRegions() const;
    QueueObservation applyQueueCnn(const QueueObservation& legacy,
                                   const std::vector<QueueCnnPrediction>& predictions,
                                   std::size_t predictionOffset,
                                   const std::vector<double>& darkRatios,
                                   std::size_t darkRatioOffset,
                                   const Settings& settings,
                                   const std::vector<QueueCnnPrediction>* labelPredictions = nullptr) const;
    // 採否（unclear・確信度）は predictions、ピースの値だけ labelPredictions から取る
    // 実験用フック。labelPredictions == nullptr のときは従来と完全に同じ挙動。
#endif
    BoardObservation analyzeBoard(const Frame& frame) const;
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
    BoardObservation analyzeBrowserGpuBoard(const Frame& frame) const;
    BoardObservation analyzeBrowserGpuRetainedBoard(const Frame& frame) const;
#endif
    // Losslessly retain only the source pixels that board inference samples.
    // The returned compact frame can be analyzed later with the exact same
    // four neighbours and interpolation fractions as the full source frame.
    Frame retainBoardFrame(const Frame& frame) const;
    void retainBoardFrame(const Frame& frame, Frame& retained) const;
    PixelRegion boardPixelRegion() const;
    BoardObservation analyzeRetainedBoard(const Frame& retained) const;
    // Browser/WASM bridge: CNN tensor extraction and label application remain
    // in this C++ implementation; only the ONNX tensor execution is supplied
    // by ONNX Runtime Web.
    std::vector<float> boardFeatures(const Frame& frame) const;
    // CNN tensor extraction and the classic palette fallback consume the same
    // resampled board image. Returning them together avoids rebuilding that
    // image after ONNX inference without changing either calculation.
    BoardAnalysisInput boardAnalysisInput(const Frame& frame,
                                          double horizontalShiftPixels = 0) const;
    BoardObservation analyzeBoardWithLabels(const Frame& frame, const std::vector<Cell>& labels,
                                            double horizontalShiftPixels = 0,
                                            bool enforceLegalGarbageRows = true) const;
    BoardObservation analyzeBoardWithLabels(const Frame& frame, const std::vector<Cell>& labels,
                                            const VisibleBoard& classic,
                                            bool enforceLegalGarbageRows = true) const;
    BoardObservation analyzeBoardWithProbabilities(const Frame& frame,
                                                   const BoardProbabilities& probabilities,
                                                   const VisibleBoard& classic,
                                                   bool enforceLegalGarbageRows = true) const;
    BoardObservation analyze(const Frame& frame) const;
    // Classic palette sampling is kept available for diagnostics and for
    // validating garbage rows when ONNX is affected by a transient effect.
    VisibleBoard classicBoard(const Frame& frame, double horizontalShiftPixels = 0) const;

    static BoardObservation aggregate(const std::vector<BoardObservation>& samples);
    static Board makeFullBoard(const VisibleBoard& visible);

private:
    struct Crop {
        double x = 0;
        double y = 0;
        double scale = 1;
    };

#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    struct QueueCnnPixel {
        std::array<std::size_t, 4> offsets{};
        double fx = 0;
        double fy = 0;
        std::uint8_t x = 0;
        std::uint8_t y = 0;
    };

    struct QueueCnnCrop {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        std::vector<QueueCnnPixel> pixels;
    };

#endif

    int frameWidth_;
    int frameHeight_;
    PlayerLayout layout_;
    Crop crop_;
    const OnnxBoardModel* model_ = nullptr;
    int boardImageWidth_ = 0;
    int boardImageHeight_ = 0;
    // These tables are pure geometry.  Precomputing them removes repeated
    // floor/clamp/address calculations without changing any sampled pixel or
    // floating-point blend operation.
    std::vector<std::array<std::size_t, 4>> boardSampleOffsets_;
    std::vector<std::array<std::size_t, 4>> retainedBoardSampleOffsets_;
    std::vector<std::array<double, 2>> boardSampleFractions_;
    int retainedBoardX_ = 0;
    int retainedBoardY_ = 0;
    int retainedBoardWidth_ = 0;
    int retainedBoardHeight_ = 0;
    std::array<std::vector<std::size_t>, 6> queueSampleOffsets_;
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    std::array<QueueCnnCrop, 6> queueCnnCrops_;
#endif

    std::vector<float> extractBoardFeatures(const Frame& frame) const;
};

} // namespace tr
