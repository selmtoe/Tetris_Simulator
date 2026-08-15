#pragma once

#include "common.hpp"
#include "onnx_model.hpp"

namespace tr {

class VisionAnalyzer {
public:
    VisionAnalyzer(int frameWidth, int frameHeight, const PlayerLayout& layout, const OnnxBoardModel* model);

    // Exact ports of analyzeNextQueue and analyzeBoardOnly in 動画解析.html.
    QueueObservation observeQueue(const Frame& frame) const;
    BoardObservation analyzeBoard(const Frame& frame) const;
    // Browser/WASM bridge: feature extraction and label application remain
    // in this C++ implementation; only the ONNX tensor execution is supplied
    // by ONNX Runtime Web.
    std::vector<float> boardFeatures(const Frame& frame) const;
    BoardObservation analyzeBoardWithLabels(const Frame& frame, const std::vector<Cell>& labels) const;
    BoardObservation analyze(const Frame& frame) const;

    static BoardObservation aggregate(const std::vector<BoardObservation>& samples);
    static Board makeFullBoard(const VisibleBoard& visible);

private:
    struct Crop {
        double x = 0;
        double y = 0;
        double scale = 1;
    };

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
    std::vector<std::array<double, 2>> boardSampleFractions_;
    std::array<std::vector<std::size_t>, 6> queueSampleOffsets_;

    std::vector<float> extractBoardFeatures(const Frame& frame) const;
    VisibleBoard classicBoard(const Frame& frame) const;
};

} // namespace tr
