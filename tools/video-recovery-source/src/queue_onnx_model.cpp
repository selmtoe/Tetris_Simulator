#include "queue_onnx_model.hpp"
#include "cuda_provider_env.hpp"

#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#if defined(__CUDACC__) || defined(TR_CUDA_ENABLED)
#include <cuda_runtime.h>
#endif
#include <filesystem>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace tr {

namespace {

Cell queueClassToCell(int value) {
    // Training class order: [empty, I, O, T, L, J, S, Z].
    switch (value) {
    case 1: return Cell::I;
    case 2: return Cell::O;
    case 3: return Cell::T;
    case 4: return Cell::L;
    case 5: return Cell::J;
    case 6: return Cell::S;
    case 7: return Cell::Z;
    default: return Cell::Empty;
    }
}

std::string shapeText(const std::vector<std::int64_t>& shape) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i) out << ',';
        out << shape[i];
    }
    return out.str() + ']';
}

bool cudaQueueInferenceRequested() {
#ifdef TR_ORT_CUDA_QUEUE_DEFAULT
    bool enabled = true;
#else
    bool enabled = false;
#endif
    if (const char* configured = std::getenv("TR_GPU_QUEUE_ONNX")) {
        enabled = std::string(configured) != "0";
    }
    return enabled;
}

bool gpuDiagnosticsEnabled() {
    if (const char* configured = std::getenv("TR_GPU_DIAGNOSTICS")) {
        return std::string(configured) != "0";
    }
    return false;
}

std::vector<QueueCnnPrediction> decodeQueueOutputs(const Ort::Value& pieceOutput,
                                                   const Ort::Value& unclearOutput,
                                                   std::size_t batch,
                                                   std::string& error) {
    std::vector<QueueCnnPrediction> result;
    const auto pieceInfo = pieceOutput.GetTensorTypeAndShapeInfo();
    const auto unclearInfo = unclearOutput.GetTensorTypeAndShapeInfo();
    if (pieceInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        unclearInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        pieceInfo.GetElementCount() != batch * 8 || unclearInfo.GetElementCount() != batch) {
        error = "Queue ONNX returned unexpected tensor types or sizes";
        return result;
    }
    const float* logits = pieceOutput.GetTensorData<float>();
    const float* unclear = unclearOutput.GetTensorData<float>();
    result.resize(batch);
    for (std::size_t item = 0; item < batch; ++item) {
        const float* row = logits + item * 8;
        const int best = static_cast<int>(std::max_element(row, row + 8) - row);
        const float maximum = row[best];
        double denominator = 0;
        for (int klass = 0; klass < 8; ++klass) denominator += std::exp(row[klass] - maximum);
        result[item].piece = queueClassToCell(best);
        result[item].pieceConfidence = static_cast<float>(1.0 / denominator);
        const double value = std::clamp(static_cast<double>(unclear[item]), -80.0, 80.0);
        result[item].unclearProbability = static_cast<float>(1.0 / (1.0 + std::exp(-value)));
    }
    return result;
}

} // namespace

OnnxQueueModel::OnnxQueueModel(const std::filesystem::path& modelPath, std::string& error)
    : env_(ORT_LOGGING_LEVEL_ERROR, "tetris-next-hold-cnn"),
      memoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    try {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const bool useCuda = cudaQueueInferenceRequested();
        if (useCuda) {
            Ort::CUDAProviderOptions cudaOptions;
            cudaOptions.Update(tr::cudaProviderOptionsWithEnv({
                {"device_id", "0"},
                {"use_tf32", "0"},
                {"enable_cuda_graph", "0"},
                {"do_copy_in_default_stream", "1"},
            }));
            options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
        }
        session_ = std::make_unique<Ort::Session>(env_, modelPath.c_str(), options);
        if (gpuDiagnosticsEnabled()) {
            std::clog << "Queue CNN execution provider: "
                      << (useCuda ? "CUDA FP32 (TF32 off)" : "CPU") << '\n';
        }

        if (session_->GetInputCount() != 1 || session_->GetOutputCount() < 2) {
            error = "Queue ONNX must have one input and two outputs";
            session_.reset();
            return;
        }
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = session_->GetInputNameAllocated(0, allocator);
        auto pieceName = session_->GetOutputNameAllocated(0, allocator);
        auto unclearName = session_->GetOutputNameAllocated(1, allocator);
        inputName_ = inputName.get();
        pieceOutputName_ = pieceName.get();
        unclearOutputName_ = unclearName.get();

        const auto inputShape = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto pieceShape = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto unclearShape = session_->GetOutputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
        if (inputShape.size() != 4 || inputShape[1] != 3 || inputShape[2] != 48 || inputShape[3] != 48) {
            error = "Queue ONNX input shape is invalid: " + shapeText(inputShape) +
                    " (expected [batch,3,48,48])";
            session_.reset();
            return;
        }
        if (pieceShape.size() != 2 || pieceShape[1] != 8 || unclearShape.size() != 1) {
            error = "Queue ONNX output shapes are invalid: " + shapeText(pieceShape) +
                    " and " + shapeText(unclearShape);
            session_.reset();
        }
    } catch (const Ort::Exception& exception) {
        error = "Queue ONNX Runtime could not load the model: " + std::string(exception.what());
        session_.reset();
    }
}

OnnxQueueModel::~OnnxQueueModel() = default;

std::vector<QueueCnnPrediction> OnnxQueueModel::infer(const std::vector<float>& images,
                                                       std::size_t batch,
                                                       std::string& error) const {
    constexpr std::size_t ValuesPerImage = 3 * 48 * 48;
    std::vector<QueueCnnPrediction> result;
    if (!session_) {
        error = "Queue ONNX model is not initialized";
        return result;
    }
    if (batch == 0 || images.size() != batch * ValuesPerImage) {
        error = "Queue ONNX input does not match batch*3*48*48";
        return result;
    }

    try {
        std::array<std::int64_t, 4> shape{
            static_cast<std::int64_t>(batch), 3, 48, 48
        };
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memoryInfo_, const_cast<float*>(images.data()), images.size(), shape.data(), shape.size());
        const char* inputNames[]{inputName_.c_str()};
        const char* outputNames[]{pieceOutputName_.c_str(), unclearOutputName_.c_str()};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 2);
        result = decodeQueueOutputs(outputs[0], outputs[1], batch, error);
    } catch (const Ort::Exception& exception) {
        error = "Queue ONNX inference failed: " + std::string(exception.what());
        return {};
    }
    return result;
}

std::vector<QueueCnnPrediction> OnnxQueueModel::inferCudaDevice(
    float* deviceImages, std::size_t batch, std::string& error) const {
    constexpr std::size_t ValuesPerImage = 3 * 48 * 48;
    if (!session_) {
        error = "Queue ONNX model is not initialized";
        return {};
    }
    if (!deviceImages || batch == 0) {
        error = "Queue CUDA input is empty";
        return {};
    }

    try {
        std::array<std::int64_t, 4> shape{
            static_cast<std::int64_t>(batch), 3, 48, 48
        };
        std::array<std::int64_t, 2> pieceShape{
            static_cast<std::int64_t>(batch), 8
        };
        std::array<std::int64_t, 1> unclearShape{
            static_cast<std::int64_t>(batch)
        };
        Ort::MemoryInfo cudaMemory("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        const bool persistentBinding = [] {
            const char* configured = std::getenv("TR_ORT_PERSISTENT_BINDING");
            return configured && std::string(configured) != "0";
        }();
        if (persistentBinding) {
            if (!cachedCudaBinding_ || cachedCudaInput_ != deviceImages ||
                cachedCudaBatch_ != batch) {
                cachedCudaBinding_ = std::make_unique<Ort::IoBinding>(*session_);
                cachedCudaInputValue_ = std::make_unique<Ort::Value>(
                    Ort::Value::CreateTensor<float>(
                        cudaMemory, deviceImages, batch * ValuesPerImage,
                        shape.data(), shape.size()));
                cachedPieceOutput_.resize(batch * 8);
                cachedUnclearOutput_.resize(batch);
                cachedPieceOutputValue_ = std::make_unique<Ort::Value>(
                    Ort::Value::CreateTensor<float>(
                        memoryInfo_, cachedPieceOutput_.data(),
                        cachedPieceOutput_.size(), pieceShape.data(),
                        pieceShape.size()));
                cachedUnclearOutputValue_ = std::make_unique<Ort::Value>(
                    Ort::Value::CreateTensor<float>(
                        memoryInfo_, cachedUnclearOutput_.data(),
                        cachedUnclearOutput_.size(), unclearShape.data(),
                        unclearShape.size()));
                cachedCudaBinding_->BindInput(
                    inputName_.c_str(), *cachedCudaInputValue_);
                cachedCudaBinding_->BindOutput(
                    pieceOutputName_.c_str(), *cachedPieceOutputValue_);
                cachedCudaBinding_->BindOutput(
                    unclearOutputName_.c_str(), *cachedUnclearOutputValue_);
                cachedCudaInput_ = deviceImages;
                cachedCudaBatch_ = batch;
            }
            session_->Run(Ort::RunOptions{nullptr}, *cachedCudaBinding_);
            return decodeQueueOutputs(
                *cachedPieceOutputValue_, *cachedUnclearOutputValue_, batch, error);
        }
        Ort::Value input = Ort::Value::CreateTensor<float>(
            cudaMemory, deviceImages, batch * ValuesPerImage,
            shape.data(), shape.size());
        Ort::IoBinding binding(*session_);
        binding.BindInput(inputName_.c_str(), input);
        binding.BindOutput(pieceOutputName_.c_str(), memoryInfo_);
        binding.BindOutput(unclearOutputName_.c_str(), memoryInfo_);
        session_->Run(Ort::RunOptions{nullptr}, binding);
        auto outputs = binding.GetOutputValues();
        if (outputs.size() != 2) {
            error = "Queue ONNX I/O binding returned an unexpected output count";
            return {};
        }
        return decodeQueueOutputs(outputs[0], outputs[1], batch, error);
    } catch (const Ort::Exception& exception) {
        error = "Queue CUDA I/O-binding inference failed: " + std::string(exception.what());
        return {};
    }
}

} // namespace tr

#endif
