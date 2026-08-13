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

    std::vector<float> extractBoardFeatures(const Frame& frame) const;
    VisibleBoard classicBoard(const Frame& frame) const;
};

} // namespace tr
