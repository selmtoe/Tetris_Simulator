#include "onnx_model.hpp"
#include "cuda_provider_env.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace tr {

namespace {

std::string shapeText(const std::vector<int64_t>& shape) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i) out << ',';
        out << shape[i];
    }
    out << ']';
    return out.str();
}

Cell labelToCell(double label) {
    const int value = static_cast<int>(std::lround(label));
    // This is the class order used by the original browser model:
    // ['null', 'G', 'S', 'Z', 'L', 'J', 'O', 'I', 'T'].
    switch (value) {
    case 1: return Cell::Garbage;
    case 2: return Cell::S;
    case 3: return Cell::Z;
    case 4: return Cell::L;
    case 5: return Cell::J;
    case 6: return Cell::O;
    case 7: return Cell::I;
    case 8: return Cell::T;
    default: return Cell::Empty;
    }
}

bool cudaBoardInferenceRequested() {
#ifdef TR_ORT_CUDA_DEFAULT
    bool enabled = true;
#else
    bool enabled = false;
#endif
    if (const char* configured = std::getenv("TR_GPU_ONNX")) {
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

} // namespace

OnnxBoardModel::OnnxBoardModel(const std::filesystem::path& modelPath, std::string& error)
    : env_(ORT_LOGGING_LEVEL_ERROR, "tetris-video-recovery"),
      memoryInfo_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) {
    try {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.DisableMemPattern();
        const bool useCuda = cudaBoardInferenceRequested();
        cudaEnabled_ = useCuda;
        if (useCuda) {
            // Correctness-first CUDA configuration.  In particular, TF32 is
            // disabled so Ampere-and-newer GPUs retain full FP32 mantissas.
            // CUDA graphs and FP16 are deliberately excluded until the final
            // 1.mov trajectory hash has passed.
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
            std::clog << "Board CNN execution provider: "
                      << (useCuda ? "CUDA FP32 (TF32 off)" : "CPU") << '\n';
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = session_->GetInputNameAllocated(0, allocator);
        auto outputName = session_->GetOutputNameAllocated(0, allocator);
        inputName_ = inputName.get();
        outputName_ = outputName.get();
        inputShape_ = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto outputInfo = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        outputShape_ = outputInfo.GetShape();
        outputType_ = outputInfo.GetElementType();

        // One batched inference covers all 200 visible cells. Each item is a
        // row-major RGB 32x32 crop normalized to [0, 1].
        if (inputShape_.size() != 4 ||
            (inputShape_[0] != -1 && inputShape_[0] != 200) ||
            inputShape_[1] != 3 || inputShape_[2] != 32 || inputShape_[3] != 32) {
            error = "ONNX input shape is invalid: " + shapeText(inputShape_) +
                    " (expected [-1,3,32,32] or [200,3,32,32])";
            session_.reset();
            return;
        }
        if (outputShape_.size() != 2 ||
            (outputShape_[0] != -1 && outputShape_[0] != 200) || outputShape_[1] != 9) {
            error = "ONNX output shape is invalid: " + shapeText(outputShape_) +
                    " (expected [-1,9] or [200,9])";
            session_.reset();
            return;
        }
        if (outputType_ != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            outputType_ != ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE) {
            error = "ONNX output must contain floating-point logits";
            session_.reset();
            return;
        }
    } catch (const Ort::Exception& exception) {
        error = "ONNX Runtime could not load the model: " + std::string(exception.what());
        session_.reset();
    }
}

OnnxBoardModel::~OnnxBoardModel() = default;

BoardProbabilities OnnxBoardModel::inferProbabilities(const std::vector<float>& features,
                                                      std::string& error) const {
    BoardProbabilities result{};
    if (!session_) {
        error = "ONNX model is not initialized";
        return result;
    }
    constexpr std::size_t ValuesPerCell = 3 * 32 * 32;
    if (features.size() != 200 * ValuesPerCell) {
        error = "ONNX RGB cell batch must contain exactly 614,400 values";
        return result;
    }

    try {
        std::array<int64_t, 4> shape{200, 3, 32, 32};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memoryInfo_, const_cast<float*>(features.data()), features.size(), shape.data(), shape.size());
        const char* inputNames[] = {inputName_.c_str()};
        const char* outputNames[] = {outputName_.c_str()};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 1);
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto actualShape = info.GetShape();
        const auto actualType = info.GetElementType();
        const std::size_t elementCount = info.GetElementCount();
        if (actualShape.size() != 2 || actualShape[0] != 200 || actualShape[1] != 9 ||
            elementCount != 200 * 9) {
            error = "ONNX output shape changed at inference: " + shapeText(actualShape);
            return result;
        }

        auto valueAt = [&](std::size_t offset) -> double {
            switch (actualType) {
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
                return outputs[0].GetTensorData<float>()[offset];
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
                return outputs[0].GetTensorData<double>()[offset];
            default:
                return 0;
            }
        };

        constexpr int Classes = CellClassCount;
        for (int cell = 0; cell < 200; ++cell) {
            const std::size_t base = static_cast<std::size_t>(cell) * Classes;
            double maximum = valueAt(base);
            for (int c = 1; c < Classes; ++c) maximum = std::max(maximum, valueAt(base + c));
            double total = 0;
            std::array<double, Classes> exponentials{};
            for (int c = 0; c < Classes; ++c) {
                exponentials[static_cast<std::size_t>(c)] = std::exp(valueAt(base + c) - maximum);
                total += exponentials[static_cast<std::size_t>(c)];
            }
            if (!(total > 0) || !std::isfinite(total)) {
                error = "ONNX output contained invalid logits";
                return {};
            }
            for (int c = 0; c < Classes; ++c) {
                const Cell mapped = labelToCell(c);
                result[probabilityIndex(cell, mapped)] =
                    static_cast<float>(exponentials[static_cast<std::size_t>(c)] / total);
            }
        }
    } catch (const Ort::Exception& exception) {
        error = "ONNX inference failed: " + std::string(exception.what());
    }
    return result;
}

std::vector<BoardProbabilities> OnnxBoardModel::inferCudaDevice(
    float* deviceFeatures,
    std::size_t boardCount,
    std::string& error) const {
    constexpr std::size_t CellsPerBoard = BoardWidth * VisibleRows;
    constexpr std::size_t ValuesPerCell = 3 * 32 * 32;
    error.clear();
    if (!session_) {
        error = "ONNX model is not initialized";
        return {};
    }
    if (!cudaEnabled_) {
        error = "Board ONNX session was not created with the CUDA provider";
        return {};
    }
    if (!deviceFeatures || boardCount == 0 ||
        boardCount > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max()) / CellsPerBoard) {
        error = "Board CUDA input is empty or too large";
        return {};
    }
    const std::size_t cellCount = boardCount * CellsPerBoard;
    if ((inputShape_[0] != -1 &&
         inputShape_[0] != static_cast<std::int64_t>(cellCount)) ||
        (outputShape_[0] != -1 &&
         outputShape_[0] != static_cast<std::int64_t>(cellCount))) {
        error = "Board ONNX model has a fixed batch incompatible with the CUDA request";
        return {};
    }

    // The persistent bindings own mutable CPU output buffers and are reused
    // across calls. Current recovery paths invoke this synchronously, but the
    // model is also an independently reusable component; serialize CUDA Runs
    // so a future caller cannot race cache insertion/eviction or overwrite an
    // output buffer while another call is converting its logits.
    std::lock_guard<std::mutex> inferenceLock(cudaInferenceMutex_);
    try {
        std::array<std::int64_t, 4> shape{
            static_cast<std::int64_t>(cellCount), 3, 32, 32
        };
        std::array<std::int64_t, 2> outputShape{
            static_cast<std::int64_t>(cellCount), CellClassCount
        };
        Ort::MemoryInfo cudaMemory(
            "Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        const bool persistentBinding = [] {
            const char* configured = std::getenv("TR_ORT_PERSISTENT_BINDING");
            return configured && std::string(configured) != "0";
        }();
        std::vector<Ort::Value> transientOutputs;
        const float* floatOutput = nullptr;
        const double* doubleOutput = nullptr;
        if (persistentBinding) {
            CudaBindingCacheEntry* cache = nullptr;
            for (const auto& candidate : cachedCudaBindings_) {
                if (candidate->input == deviceFeatures &&
                    candidate->boardCount == boardCount) {
                    cache = candidate.get();
                    break;
                }
            }
            if (!cache) {
                // A session normally sees at most 32 scalar tensor offsets
                // plus 32 full-batch shapes. Bound stale entries in case a
                // long-lived GUI session recreates CUDA preprocessors at new
                // device addresses across analyses.
                constexpr std::size_t MaximumCudaBindingCacheEntries = 64;
                if (cachedCudaBindings_.size() >=
                    MaximumCudaBindingCacheEntries) {
                    cachedCudaBindings_.clear();
                }
                auto candidate = std::make_unique<CudaBindingCacheEntry>();
                candidate->input = deviceFeatures;
                candidate->boardCount = boardCount;
                candidate->binding = std::make_unique<Ort::IoBinding>(*session_);
                candidate->inputValue = std::make_unique<Ort::Value>(
                    Ort::Value::CreateTensor<float>(
                        cudaMemory, deviceFeatures, cellCount * ValuesPerCell,
                        shape.data(), shape.size()));
                if (outputType_ == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    candidate->floatOutput.resize(
                        cellCount * CellClassCount);
                    candidate->outputValue = std::make_unique<Ort::Value>(
                        Ort::Value::CreateTensor<float>(
                            memoryInfo_, candidate->floatOutput.data(),
                            candidate->floatOutput.size(), outputShape.data(),
                            outputShape.size()));
                } else {
                    candidate->doubleOutput.resize(
                        cellCount * CellClassCount);
                    candidate->outputValue = std::make_unique<Ort::Value>(
                        Ort::Value::CreateTensor<double>(
                            memoryInfo_, candidate->doubleOutput.data(),
                            candidate->doubleOutput.size(), outputShape.data(),
                            outputShape.size()));
                }
                candidate->binding->BindInput(
                    inputName_.c_str(), *candidate->inputValue);
                candidate->binding->BindOutput(
                    outputName_.c_str(), *candidate->outputValue);
                cache = candidate.get();
                cachedCudaBindings_.push_back(std::move(candidate));
            }
            session_->Run(Ort::RunOptions{nullptr}, *cache->binding);
            floatOutput = cache->floatOutput.empty()
                ? nullptr : cache->floatOutput.data();
            doubleOutput = cache->doubleOutput.empty()
                ? nullptr : cache->doubleOutput.data();
        } else {
            Ort::Value input = Ort::Value::CreateTensor<float>(
                cudaMemory, deviceFeatures, cellCount * ValuesPerCell,
                shape.data(), shape.size());
            Ort::IoBinding binding(*session_);
            binding.BindInput(inputName_.c_str(), input);
            binding.BindOutput(outputName_.c_str(), memoryInfo_);
            session_->Run(Ort::RunOptions{nullptr}, binding);
            transientOutputs = binding.GetOutputValues();
            if (transientOutputs.size() != 1) {
                error = "Board ONNX I/O binding returned an unexpected output count";
                return {};
            }
            const auto info = transientOutputs[0].GetTensorTypeAndShapeInfo();
            const auto actualShape = info.GetShape();
            const auto actualType = info.GetElementType();
            if (actualShape.size() != 2 ||
                actualShape[0] != static_cast<std::int64_t>(cellCount) ||
                actualShape[1] != CellClassCount ||
                info.GetElementCount() != cellCount * CellClassCount ||
                (actualType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
                 actualType != ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE)) {
                error = "Board ONNX CUDA output shape/type changed at inference";
                return {};
            }
            floatOutput = actualType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
                ? transientOutputs[0].GetTensorData<float>() : nullptr;
            doubleOutput = actualType == ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE
                ? transientOutputs[0].GetTensorData<double>() : nullptr;
        }

        const auto valueAt = [&](std::size_t offset) -> double {
            return floatOutput ? floatOutput[offset] : doubleOutput[offset];
        };
        std::vector<BoardProbabilities> result(boardCount);
        for (std::size_t board = 0; board < boardCount; ++board) {
            for (std::size_t cell = 0; cell < CellsPerBoard; ++cell) {
                const std::size_t sourceBase =
                    (board * CellsPerBoard + cell) * CellClassCount;
                double maximum = valueAt(sourceBase);
                for (int label = 1; label < CellClassCount; ++label) {
                    maximum = std::max(maximum, valueAt(sourceBase + label));
                }
                std::array<double, CellClassCount> exponentials{};
                double total = 0;
                for (int label = 0; label < CellClassCount; ++label) {
                    exponentials[static_cast<std::size_t>(label)] =
                        std::exp(valueAt(sourceBase + label) - maximum);
                    total += exponentials[static_cast<std::size_t>(label)];
                }
                if (!(total > 0) || !std::isfinite(total)) {
                    error = "Board ONNX CUDA output contained invalid logits";
                    return {};
                }
                for (int label = 0; label < CellClassCount; ++label) {
                    const Cell mapped = labelToCell(label);
                    result[board][probabilityIndex(
                        static_cast<int>(cell), mapped)] = static_cast<float>(
                            exponentials[static_cast<std::size_t>(label)] / total);
                }
            }
        }
        return result;
    } catch (const Ort::Exception& exception) {
        error = "Board CUDA I/O-binding inference failed: " +
            std::string(exception.what());
        return {};
    }
}

std::vector<Cell> OnnxBoardModel::infer(const std::vector<float>& features, std::string& error) const {
    const BoardProbabilities probabilities = inferProbabilities(features, error);
    std::vector<Cell> result(BoardWidth * VisibleRows, Cell::Empty);
    if (!error.empty()) return result;
    for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
        Cell best = Cell::Empty;
        float bestProbability = probabilities[probabilityIndex(cell, best)];
        for (int value = 1; value < CellClassCount; ++value) {
            const Cell candidate = static_cast<Cell>(value);
            const float probability = probabilities[probabilityIndex(cell, candidate)];
            if (probability > bestProbability) {
                best = candidate;
                bestProbability = probability;
            }
        }
        result[static_cast<std::size_t>(cell)] = best;
    }
    return result;
}

} // namespace tr
