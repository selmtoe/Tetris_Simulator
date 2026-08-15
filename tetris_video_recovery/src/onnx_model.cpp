#include "onnx_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

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
        session_ = std::make_unique<Ort::Session>(env_, modelPath.c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = session_->GetInputNameAllocated(0, allocator);
        auto outputName = session_->GetOutputNameAllocated(0, allocator);
        inputName_ = inputName.get();
        outputName_ = outputName.get();
        inputShape_ = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto outputInfo = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        outputShape_ = outputInfo.GetShape();
        outputType_ = outputInfo.GetElementType();

        // The checked-in model has a dynamic first dimension. We always use
        // 200 because one inference covers the complete visible board.
        if (inputShape_.size() != 2 ||
            (inputShape_[0] != -1 && inputShape_[0] != 200) || inputShape_[1] != 63) {
            error = "ONNX input shape is invalid: " + shapeText(inputShape_) +
                    " (expected [-1,63] or [200,63])";
            session_.reset();
            return;
        }
        if (outputShape_.empty() || (outputShape_.size() > 2 && outputShape_[0] != 200)) {
            error = "ONNX output shape is invalid: " + shapeText(outputShape_);
            session_.reset();
            return;
        }
    } catch (const Ort::Exception& exception) {
        error = "ONNX Runtime could not load the model: " + std::string(exception.what());
        session_.reset();
    }
}

OnnxBoardModel::~OnnxBoardModel() = default;

std::vector<Cell> OnnxBoardModel::infer(const std::vector<float>& features, std::string& error) const {
    std::vector<Cell> result(200, Cell::Empty);
    if (!session_) {
        error = "ONNX model is not initialized";
        return result;
    }
    if (features.size() != 200 * 63) {
        error = "ONNX feature vector must contain exactly 12,600 values";
        return result;
    }

    try {
        std::array<int64_t, 2> shape{200, 63};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memoryInfo_, const_cast<float*>(features.data()), features.size(), shape.data(), shape.size());
        const char* inputNames[] = {inputName_.c_str()};
        const char* outputNames[] = {outputName_.c_str()};
        auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, &input, 1, outputNames, 1);
        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto actualShape = info.GetShape();
        const auto actualType = info.GetElementType();
        const std::size_t elementCount = info.GetElementCount();
        if (elementCount < 200) {
            error = "ONNX output contains fewer than 200 cell predictions";
            return result;
        }

        auto valueAt = [&](std::size_t offset) -> double {
            switch (actualType) {
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
                return outputs[0].GetTensorData<float>()[offset];
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
                return outputs[0].GetTensorData<double>()[offset];
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
                return outputs[0].GetTensorData<std::int64_t>()[offset];
            case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
                return outputs[0].GetTensorData<std::int32_t>()[offset];
            default:
                return 0;
            }
        };

        // The model currently returns [200] integer labels. Keep the
        // [200,classes] path as a compatibility path for a retrained model.
        if (actualShape.size() == 2 && (actualShape[0] == -1 || actualShape[0] == 200) && actualShape[1] > 1) {
            const int classes = static_cast<int>(actualShape[1]);
            for (int cell = 0; cell < 200; ++cell) {
                int best = 0;
                double bestValue = valueAt(static_cast<std::size_t>(cell) * classes);
                for (int c = 1; c < classes; ++c) {
                    const double value = valueAt(static_cast<std::size_t>(cell) * classes + c);
                    if (value > bestValue) { bestValue = value; best = c; }
                }
                result[cell] = labelToCell(best);
            }
        } else {
            for (int cell = 0; cell < 200; ++cell) result[cell] = labelToCell(valueAt(cell));
        }
    } catch (const Ort::Exception& exception) {
        error = "ONNX inference failed: " + std::string(exception.what());
    }
    return result;
}

} // namespace tr
