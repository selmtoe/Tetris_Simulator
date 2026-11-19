#pragma once

#include "tetris_engine.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tr {

// Tiny native MLP used only by the accuracy fallback. The ordinary fast path
// never calls it. Its binary format is emitted by train_tetris_neural_ranker.py
// and deliberately avoids Python/ONNX runtime overhead in production.
class NeuralRanker {
public:
    bool load(const std::filesystem::path& path, std::string& error);
    bool valid() const;
    double score(const std::array<float, NeuralRankFeatureCount>& features) const;
    ScoreWeights baselineWeights() const;

    int hidden1() const { return hidden1_; }
    int hidden2() const { return hidden2_; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    int input_ = 0;
    int hidden1_ = 0;
    int hidden2_ = 0;
    float scoreScale_ = 1;
    std::array<float, 10> baseWeights_{};
    std::array<float, NeuralRankFeatureCount> mean_{};
    std::array<float, NeuralRankFeatureCount> inverseStd_{};
    std::vector<float> firstWeights_;
    std::vector<float> firstBias_;
    std::vector<float> secondWeights_;
    std::vector<float> secondBias_;
    std::vector<float> outputWeights_;
    float outputBias_ = 0;
};

} // namespace tr
