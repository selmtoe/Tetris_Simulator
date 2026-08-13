#pragma once

#include "common.hpp"

#include <memory>

#include <onnxruntime_cxx_api.h>

namespace tr {

class OnnxBoardModel {
public:
    OnnxBoardModel(const std::filesystem::path& modelPath, std::string& error);
    ~OnnxBoardModel();
    OnnxBoardModel(const OnnxBoardModel&) = delete;
    OnnxBoardModel& operator=(const OnnxBoardModel&) = delete;

    bool ready() const { return session_ != nullptr; }
    const std::string& inputName() const { return inputName_; }
    const std::string& outputName() const { return outputName_; }
    std::vector<Cell> infer(const std::vector<float>& features, std::string& error) const;

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memoryInfo_;
    std::string inputName_;
    std::string outputName_;
    std::vector<int64_t> inputShape_;
    std::vector<int64_t> outputShape_;
    ONNXTensorElementDataType outputType_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
};

} // namespace tr
