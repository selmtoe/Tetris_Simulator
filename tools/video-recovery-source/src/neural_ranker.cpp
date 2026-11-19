#include "neural_ranker.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>

namespace tr {
namespace {

template <typename Value>
bool readValue(std::ifstream& input, Value& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool readFloats(std::ifstream& input, float* destination, std::size_t count) {
    return static_cast<bool>(input.read(
        reinterpret_cast<char*>(destination),
        static_cast<std::streamsize>(count * sizeof(float))));
}

} // namespace

bool NeuralRanker::load(const std::filesystem::path& path, std::string& error) {
    *this = {};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open accurate fallback ranker: " + path.string();
        return false;
    }
    std::array<char, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t inputCount = 0;
    std::uint32_t firstCount = 0;
    std::uint32_t secondCount = 0;
    if (!input.read(magic.data(), magic.size()) ||
        !readValue(input, version) || !readValue(input, inputCount) ||
        !readValue(input, firstCount) || !readValue(input, secondCount) ||
        !readValue(input, scoreScale_)) {
        error = "Accurate fallback ranker header is truncated";
        return false;
    }
    constexpr std::array<char, 8> ExpectedMagic{'T','R','N','R','A','N','K','1'};
    if (magic != ExpectedMagic || version != 1 ||
        inputCount != NeuralRankFeatureCount || firstCount < 1 ||
        secondCount < 1 || firstCount > 512 || secondCount > 512 ||
        !(scoreScale_ > 0)) {
        error = "Accurate fallback ranker header is incompatible";
        return false;
    }
    input_ = static_cast<int>(inputCount);
    hidden1_ = static_cast<int>(firstCount);
    hidden2_ = static_cast<int>(secondCount);
    firstWeights_.resize(static_cast<std::size_t>(hidden1_) * input_);
    firstBias_.resize(static_cast<std::size_t>(hidden1_));
    secondWeights_.resize(static_cast<std::size_t>(hidden2_) * hidden1_);
    secondBias_.resize(static_cast<std::size_t>(hidden2_));
    outputWeights_.resize(static_cast<std::size_t>(hidden2_));
    if (!readFloats(input, baseWeights_.data(), baseWeights_.size()) ||
        !readFloats(input, mean_.data(), mean_.size()) ||
        !readFloats(input, inverseStd_.data(), inverseStd_.size()) ||
        !readFloats(input, firstWeights_.data(), firstWeights_.size()) ||
        !readFloats(input, firstBias_.data(), firstBias_.size()) ||
        !readFloats(input, secondWeights_.data(), secondWeights_.size()) ||
        !readFloats(input, secondBias_.data(), secondBias_.size()) ||
        !readFloats(input, outputWeights_.data(), outputWeights_.size()) ||
        !readValue(input, outputBias_)) {
        error = "Accurate fallback ranker weights are truncated";
        *this = {};
        return false;
    }
    char trailing = 0;
    if (input.read(&trailing, 1)) {
        error = "Accurate fallback ranker contains unexpected trailing data";
        *this = {};
        return false;
    }
    path_ = path;
    return true;
}

bool NeuralRanker::valid() const {
    return input_ == static_cast<int>(NeuralRankFeatureCount) &&
           hidden1_ > 0 && hidden2_ > 0 && !firstWeights_.empty();
}

ScoreWeights NeuralRanker::baselineWeights() const {
    ScoreWeights result;
    if (!valid()) return result;
    result.exactMatch = baseWeights_[0];
    result.emptyMatch = baseWeights_[1];
    result.colorMismatch = baseWeights_[2];
    result.missingBlock = baseWeights_[3];
    result.extraBlock = baseWeights_[4];
    result.placedColorMatch = baseWeights_[5];
    result.placedColorMismatch = baseWeights_[6];
    result.placedColorComplete = baseWeights_[7];
    result.garbageHoleMatch = baseWeights_[8];
    result.garbageHoleMismatch = baseWeights_[9];
    return result;
}

double NeuralRanker::score(
    const std::array<float, NeuralRankFeatureCount>& features) const {
    if (!valid()) return 0;
    std::array<float, NeuralRankFeatureCount> normalized{};
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        normalized[index] = (features[index] - mean_[index]) * inverseStd_[index];
    }
    // The scorer is called for every legal candidate in a wide fallback
    // beam. Heap allocation here used to dominate failed-match recovery
    // (millions of two-vector allocations per match). The file loader caps
    // both layers at 512, so fixed scratch storage is exact and allocation-free.
    std::array<float, 512> first;
    for (int output = 0; output < hidden1_; ++output) {
        float value = firstBias_[static_cast<std::size_t>(output)];
        const float* weights = firstWeights_.data() +
            static_cast<std::size_t>(output) * input_;
        for (int input = 0; input < input_; ++input) {
            value += weights[input] * normalized[static_cast<std::size_t>(input)];
        }
        first[static_cast<std::size_t>(output)] = std::max(0.0f, value);
    }
    std::array<float, 512> second;
    for (int output = 0; output < hidden2_; ++output) {
        float value = secondBias_[static_cast<std::size_t>(output)];
        const float* weights = secondWeights_.data() +
            static_cast<std::size_t>(output) * hidden1_;
        for (int input = 0; input < hidden1_; ++input) {
            value += weights[input] * first[static_cast<std::size_t>(input)];
        }
        second[static_cast<std::size_t>(output)] = std::max(0.0f, value);
    }
    float residual = outputBias_;
    for (int input = 0; input < hidden2_; ++input) {
        residual += outputWeights_[static_cast<std::size_t>(input)] *
                    second[static_cast<std::size_t>(input)];
    }
    float baseline = 0;
    for (std::size_t index = 0; index < baseWeights_.size(); ++index) {
        baseline += baseWeights_[index] * features[index];
    }
    return static_cast<double>(baseline + scoreScale_ * residual);
}

} // namespace tr
