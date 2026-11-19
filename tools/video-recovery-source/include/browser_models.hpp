#pragma once
// Full browser builds call ONNX Runtime Web asynchronously. These declarations
// are never used by the native CUDA build.
#include "common.hpp"
namespace tr {
class OnnxBoardModel {
public:
    OnnxBoardModel(const std::filesystem::path&, std::string&);
    bool ready() const { return ready_; }
    bool cudaReady() const { return false; }
    BoardProbabilities inferProbabilities(const std::vector<float>&, std::string&) const;
    std::vector<Cell> infer(const std::vector<float>&, std::string&) const;
private:
    bool ready_ = false;
};
}
