#pragma once

#include "common.hpp"

#include <memory>

#ifdef __EMSCRIPTEN__

namespace tr {

// Browser builds run the identical feature vector through ONNX Runtime Web
// from JavaScript.  Keeping this stub lets the native recovery translation
// unit be compiled unchanged so its queue/beam/output code is shared by both
// targets.
class OnnxBoardModel {
public:
    OnnxBoardModel(const std::filesystem::path&, std::string&) {}
    ~OnnxBoardModel() = default;
    OnnxBoardModel(const OnnxBoardModel&) = delete;
    OnnxBoardModel& operator=(const OnnxBoardModel&) = delete;

    bool ready() const { return false; }
    const std::string& inputName() const { return empty_; }
    const std::string& outputName() const { return empty_; }
    std::vector<Cell> infer(const std::vector<float>&, std::string& error) const {
        error = "ONNX inference is supplied by ONNX Runtime Web";
        return std::vector<Cell>(BoardWidth * VisibleRows, Cell::Empty);
    }

private:
    std::string empty_;
};

} // namespace tr

#else

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

#endif
