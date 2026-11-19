#pragma once

#include "common.hpp"

#include <memory>

#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
#include "browser_models.hpp"
#elif defined(__EMSCRIPTEN__)

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
    bool cudaReady() const { return false; }
    const std::string& inputName() const { return empty_; }
    const std::string& outputName() const { return empty_; }
    BoardProbabilities inferProbabilities(const std::vector<float>&, std::string& error) const {
        error = "ONNX inference is supplied by ONNX Runtime Web";
        return {};
    }
    std::vector<BoardProbabilities> inferCudaDevice(
        float*, std::size_t, std::string& error) const {
        error = "CUDA ONNX inference is unavailable in the WASM build";
        return {};
    }
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
    bool cudaReady() const { return session_ != nullptr && cudaEnabled_; }
    const std::string& inputName() const { return inputName_; }
    const std::string& outputName() const { return outputName_; }
    BoardProbabilities inferProbabilities(const std::vector<float>& features, std::string& error) const;
    // deviceFeatures is non-owning CUDA memory containing boardCount complete
    // boards in [boardCount*200,3,32,32] order. The synchronous I/O-binding
    // call returns CPU probabilities in the stable Cell enum order.
    std::vector<BoardProbabilities> inferCudaDevice(float* deviceFeatures,
                                                    std::size_t boardCount,
                                                    std::string& error) const;
    std::vector<Cell> infer(const std::vector<float>& features, std::string& error) const;

private:
    struct CudaBindingCacheEntry {
        float* input = nullptr;
        std::size_t boardCount = 0;
        // These buffers must outlive the non-owning Ort::Value wrappers
        // below, so they are declared first (members destruct in reverse).
        std::vector<float> floatOutput;
        std::vector<double> doubleOutput;
        std::unique_ptr<Ort::Value> inputValue;
        std::unique_ptr<Ort::Value> outputValue;
        std::unique_ptr<Ort::IoBinding> binding;
    };

    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memoryInfo_;
    std::string inputName_;
    std::string outputName_;
    std::vector<int64_t> inputShape_;
    std::vector<int64_t> outputShape_;
    ONNXTensorElementDataType outputType_ = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    bool cudaEnabled_ = false;
    // Batch preprocessing can present a small, stable set of device offsets
    // to the same ONNX session. Keep one binding for every exact
    // (input pointer, board count) pair so alternating offsets do not rebuild
    // I/O bindings or their CPU output storage. This does not change tensor
    // shapes, execution order, or numerical work.
    mutable std::mutex cudaInferenceMutex_;
    mutable std::vector<std::unique_ptr<CudaBindingCacheEntry>>
        cachedCudaBindings_;
};

} // namespace tr

#endif
