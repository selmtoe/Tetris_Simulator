#pragma once

#include "common.hpp"

#include <memory>

#ifndef __EMSCRIPTEN__
#include <onnxruntime_cxx_api.h>
#endif

namespace tr {

struct QueueCnnPrediction {
    Cell piece = Cell::Empty;
    float pieceConfidence = 0;
    float unclearProbability = 0;
};

#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
class OnnxQueueModel {
public:
    OnnxQueueModel(const std::filesystem::path&, std::string&);
    bool ready() const { return ready_; }
    std::vector<QueueCnnPrediction> infer(const std::vector<float>&, std::size_t,
                                         std::string&) const;
private:
    bool ready_ = false;
};
#elif defined(__EMSCRIPTEN__)

class OnnxQueueModel {
public:
    OnnxQueueModel(const std::filesystem::path&, std::string&) {}
    bool ready() const { return false; }
    std::vector<QueueCnnPrediction> infer(const std::vector<float>&, std::size_t,
                                          std::string& error) const {
        error = "Queue ONNX inference is unavailable in the WASM build";
        return {};
    }
};

#else

class OnnxQueueModel {
public:
    OnnxQueueModel(const std::filesystem::path& modelPath, std::string& error);
    ~OnnxQueueModel();
    OnnxQueueModel(const OnnxQueueModel&) = delete;
    OnnxQueueModel& operator=(const OnnxQueueModel&) = delete;

    bool ready() const { return session_ != nullptr; }
    std::vector<QueueCnnPrediction> infer(const std::vector<float>& images,
                                          std::size_t batch,
                                          std::string& error) const;
    // The pointer is non-owning CUDA device memory and must remain valid until
    // this synchronous I/O-binding call returns.
    std::vector<QueueCnnPrediction> inferCudaDevice(float* deviceImages,
                                                    std::size_t batch,
                                                    std::string& error) const;

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memoryInfo_;
    std::string inputName_;
    std::string pieceOutputName_;
    std::string unclearOutputName_;
    mutable float* cachedCudaInput_ = nullptr;
    mutable std::size_t cachedCudaBatch_ = 0;
    mutable std::unique_ptr<Ort::Value> cachedCudaInputValue_;
    mutable std::unique_ptr<Ort::Value> cachedPieceOutputValue_;
    mutable std::unique_ptr<Ort::Value> cachedUnclearOutputValue_;
    mutable std::unique_ptr<Ort::IoBinding> cachedCudaBinding_;
    mutable std::vector<float> cachedPieceOutput_;
    mutable std::vector<float> cachedUnclearOutput_;
};

#endif

} // namespace tr
