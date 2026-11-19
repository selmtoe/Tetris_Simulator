#include "scene_detector.hpp"
#include "cuda_provider_env.hpp"
#include "scene_boundaries.hpp"
#include "batch_runtime.hpp"

#include "nvdec_cuda_kernels.hpp"
#include "video_reader.hpp"

#include <onnxruntime_cxx_api.h>

#if defined(TR_NVDEC_DEFAULT)
#include <cuda_runtime.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tr {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t InputWidth = 160;
constexpr std::size_t InputHeight = 90;
constexpr std::size_t InputValues = InputWidth * InputHeight;
constexpr std::size_t ClassCount = 10;   // モデルが持ち得る最大クラス数
constexpr std::size_t MinClassCount = 8; // 従来モデルは8クラス
constexpr std::size_t TargetBatch = 512;
constexpr std::size_t BatchCapacity = 1024;
using namespace scene_detail;

std::string shapeText(const std::vector<std::int64_t>& shape) {
    std::ostringstream value;
    value << '[';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index) value << ',';
        value << shape[index];
    }
    value << ']';
    return value.str();
}

std::string cudaText(cudaError_t status) {
    const char* value = cudaGetErrorString(status);
    return value ? value : "unknown CUDA error";
}

class SceneModel {
public:
    explicit SceneModel(const std::filesystem::path& path, std::string& error,
                        std::size_t minClassCount = MinClassCount,
                        std::size_t maxClassCount = ClassCount)
        : env_(ORT_LOGGING_LEVEL_ERROR, "ppt-native-scene") {
#if !defined(TR_NVDEC_DEFAULT)
        (void)path;
        error = "Native scene detection requires the NVDEC/CUDA build";
#else
        const auto started = Clock::now();
        try {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(1);
            options.SetInterOpNumThreads(1);
            Ort::CUDAProviderOptions cudaOptions;
            cudaOptions.Update(tr::cudaProviderOptionsWithEnv({
                {"device_id", "0"},
                {"use_tf32", "1"},
                {"enable_cuda_graph", "0"},
                {"do_copy_in_default_stream", "1"},
            }));
            options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
            session_ = std::make_unique<Ort::Session>(env_, path.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            auto inputName = session_->GetInputNameAllocated(0, allocator);
            auto outputName = session_->GetOutputNameAllocated(0, allocator);
            inputName_ = inputName.get();
            outputName_ = outputName.get();
            inputShape_ = session_->GetInputTypeInfo(0)
                .GetTensorTypeAndShapeInfo().GetShape();
            outputShape_ = session_->GetOutputTypeInfo(0)
                .GetTensorTypeAndShapeInfo().GetShape();
            if (inputShape_.size() != 4 ||
                (inputShape_[0] != -1 && inputShape_[0] != 1) ||
                inputShape_[1] != 1 || inputShape_[2] != InputHeight ||
                inputShape_[3] != InputWidth) {
                error = "Scene ONNX input is " + shapeText(inputShape_) +
                    ", expected [-1,1,90,160]";
                session_.reset();
                return;
            }
            if (outputShape_.size() != 2 ||
                (outputShape_[0] != -1 && outputShape_[0] != 1) ||
                outputShape_[1] < static_cast<std::int64_t>(minClassCount) ||
                outputShape_[1] > static_cast<std::int64_t>(maxClassCount)) {
                error = "Scene ONNX output is " + shapeText(outputShape_) +
                    ", expected [-1," + std::to_string(minClassCount) + ".." +
                    std::to_string(maxClassCount) + "]";
                session_.reset();
                return;
            }
            classCount_ = static_cast<std::size_t>(outputShape_[1]);
            cudaError_t status = cudaMalloc(
                reinterpret_cast<void**>(&input_),
                BatchCapacity * InputValues * sizeof(float));
            if (status != cudaSuccess) {
                error = "Scene CUDA input allocation failed: " + cudaText(status);
                session_.reset();
                return;
            }
            status = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
            if (status != cudaSuccess) {
                error = "Scene CUDA stream creation failed: " + cudaText(status);
                cudaFree(input_);
                input_ = nullptr;
                session_.reset();
                return;
            }
        } catch (const Ort::Exception& exception) {
            error = "Scene ONNX Runtime initialization failed: " +
                std::string(exception.what());
            session_.reset();
        }
        initializationSeconds_ = std::chrono::duration<double>(
            Clock::now() - started).count();
#endif
    }

    ~SceneModel() {
#if defined(TR_NVDEC_DEFAULT)
        if (stream_) cudaStreamDestroy(stream_);
        if (input_) cudaFree(input_);
#endif
    }

    bool ready() const { return session_ && input_ && stream_; }
    float* input() const { return input_; }
    cudaStream_t stream() const { return stream_; }
    std::size_t classCount() const { return classCount_; }
    double initializationSeconds() const { return initializationSeconds_; }

    bool infer(std::size_t count, std::vector<float>& output,
               std::string& error) const {
        if (!ready() || count == 0 || count > BatchCapacity) {
            error = "Scene inference received an invalid batch";
            return false;
        }
        // Preprocessing uses a nonblocking stream, ORT uses its own stream.
        // Explicitly finish even a partial final batch before handing it off.
        const auto synchronized = cudaStreamSynchronize(stream_);
        if (synchronized != cudaSuccess) {
            error = "Scene CUDA preprocessing failed: " + cudaText(synchronized);
            return false;
        }
        try {
            output.assign(count * classCount_, 0.0f);
            std::array<std::int64_t, 4> inputShape{
                static_cast<std::int64_t>(count), 1,
                static_cast<std::int64_t>(InputHeight),
                static_cast<std::int64_t>(InputWidth)};
            std::array<std::int64_t, 2> outputShape{
                static_cast<std::int64_t>(count),
                static_cast<std::int64_t>(classCount_)};
            Ort::MemoryInfo cudaMemory(
                "Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);
            Ort::MemoryInfo cpuMemory = Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value inputValue = Ort::Value::CreateTensor<float>(
                cudaMemory, input_, count * InputValues,
                inputShape.data(), inputShape.size());
            Ort::Value outputValue = Ort::Value::CreateTensor<float>(
                cpuMemory, output.data(), output.size(),
                outputShape.data(), outputShape.size());
            Ort::IoBinding binding(*session_);
            binding.BindInput(inputName_.c_str(), inputValue);
            binding.BindOutput(outputName_.c_str(), outputValue);
            session_->Run(Ort::RunOptions{nullptr}, binding);
            return true;
        } catch (const Ort::Exception& exception) {
            error = "Scene ONNX inference failed: " +
                std::string(exception.what());
            return false;
        }
    }

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    std::string inputName_;
    std::string outputName_;
    std::vector<std::int64_t> inputShape_;
    std::vector<std::int64_t> outputShape_;
    std::size_t classCount_ = MinClassCount;
    float* input_ = nullptr;
    cudaStream_t stream_ = nullptr;
    double initializationSeconds_ = 0;
};

enum class MatchState { Searching, Ready, Go, Playing, Ended };

struct Run {
    int count = 0;
    std::int64_t firstFrame = -1;
    float firstConfidence = 0;
    void clear() { count = 0; firstFrame = -1; firstConfidence = 0; }
    void push(std::int64_t frame, float confidence) {
        if (count == 0) {
            firstFrame = frame;
            firstConfidence = confidence;
        }
        ++count;
    }
};

class BoundaryMachine {
public:
    BoundaryMachine(double fps, int blackoutEndRequired,
                    std::vector<SceneMatchRange>& matches)
        : fps_(fps),
          endWindowFrames_(std::max(1, static_cast<int>(
              std::lround(fps * EndWindowSeconds)))),
          endRequiredVotes_(std::max(1, static_cast<int>(
              std::ceil(endWindowFrames_ * EndRatio)))),
          blackoutEndRequired_(std::max(1, blackoutEndRequired)),
          matches_(matches) {}

    void push(int label, std::int64_t frame, float confidence,
              float endProbability) {
        switch (state_) {
        case MatchState::Ended:
            if (label == 7) {
                reset();
            } else if (label == 2) {
                state_ = MatchState::Searching;
                beginReady(frame, confidence);
            }
            return;
        case MatchState::Searching:
            if (label != 2) {
                ready_.clear();
                return;
            }
            ready_.push(frame, confidence);
            if (ready_.count >= 2) {
                state_ = MatchState::Ready;
                readyFrame_ = ready_.firstFrame;
                go_.clear();
            }
            return;
        case MatchState::Ready:
            // Analysis-start-looking frames before GO are intentionally ignored.
            if (label == 3) {
                go_.push(frame, confidence);
                if (go_.count >= 2) {
                    state_ = MatchState::Go;
                    goFirstFrame_ = go_.firstFrame;
                    goLastFrame_ = frame;
                    ready_.clear();
                    start_.clear();
                }
            } else {
                go_.clear();
            }
            return;
        case MatchState::Go:
            if (label == 3) {
                goLastFrame_ = frame;
                ready_.clear();
                start_.clear();
                return;
            }
            if (label == 4) {
                ready_.clear();
                start_.push(frame, confidence);
                if (start_.count >= 2) {
                    state_ = MatchState::Playing;
                    clearEndWindow();
                    gameplaySeen_ = false;
                    pendingStartFrame_ = start_.firstFrame;
                    pendingStartConfidence_ = start_.firstConfidence;
                }
                return;
            }
            start_.clear();
            if (label == 2) {
                ready_.push(frame, confidence);
                if (ready_.count >= 2) {
                    state_ = MatchState::Ready;
                    readyFrame_ = ready_.firstFrame;
                    go_.clear();
                    goFirstFrame_ = -1;
                    goLastFrame_ = -1;
                    ready_.clear();
                }
            } else {
                ready_.clear();
            }
            return;
        case MatchState::Playing:
            if (label == 5 && !gameplaySeen_) {
                gameplaySeen_ = true;
                clearEndWindow();
            }
            if (!gameplaySeen_) {
                clearEndWindow();
                return;
            }
            pushEndVote(frame, label == 6 || label >= 8, endProbability);
            if (endWindow_.size() >= static_cast<std::size_t>(endWindowFrames_) &&
                endVotes_ >= endRequiredVotes_) {
                completeAt(frame, averageEndProbability());
            } else if (label == 7 && endVotes_ >= blackoutEndRequired_) {
                // A blackout is a strong sequence cue. It may confirm an end
                // whose image labels were too noisy to reach the 70% ratio.
                completeAt(frame, averageEndProbability());
            }
            return;
        }
    }

private:
    void reset() {
        state_ = MatchState::Searching;
        ready_.clear(); go_.clear(); start_.clear(); clearEndWindow();
        readyFrame_ = goFirstFrame_ = goLastFrame_ = -1;
        pendingStartFrame_ = -1;
        pendingStartConfidence_ = 0;
        gameplaySeen_ = false;
    }

    void beginReady(std::int64_t frame, float confidence) {
        ready_.clear(); ready_.push(frame, confidence);
        go_.clear(); start_.clear(); clearEndWindow();
        readyFrame_ = goFirstFrame_ = goLastFrame_ = -1;
        pendingStartFrame_ = -1;
        gameplaySeen_ = false;
    }

    struct EndVote {
        std::int64_t frame = -1;
        bool isEnd = false;
        float probability = 0;
    };

    void clearEndWindow() {
        endWindow_.clear();
        endVotes_ = 0;
        endProbabilitySum_ = 0;
    }

    void pushEndVote(std::int64_t frame, bool isEnd, float probability) {
        const std::int64_t minimumFrame = frame - endWindowFrames_ + 1;
        while (!endWindow_.empty() && endWindow_.front().frame < minimumFrame) {
            endVotes_ -= endWindow_.front().isEnd ? 1 : 0;
            endProbabilitySum_ -= endWindow_.front().probability;
            endWindow_.pop_front();
        }
        endWindow_.push_back({frame, isEnd, probability});
        endVotes_ += isEnd ? 1 : 0;
        endProbabilitySum_ += probability;
    }

    float averageEndProbability() const {
        return endWindow_.empty() ? 0.0f : static_cast<float>(
            endProbabilitySum_ / static_cast<double>(endWindow_.size()));
    }

    void completeAt(std::int64_t endFrame, float endConfidence) {
        if (pendingStartFrame_ < 0 || endFrame <= pendingStartFrame_) {
            state_ = MatchState::Ended;
            return;
        }
        SceneMatchRange match;
        match.index = static_cast<int>(matches_.size() + 1);
        match.readyFrame = readyFrame_;
        match.goFirstFrame = goFirstFrame_;
        match.goLastFrame = goLastFrame_;
        match.startFrame = pendingStartFrame_;
        match.endFrame = endFrame;
        match.startSeconds = static_cast<double>(match.startFrame) / fps_;
        match.endSeconds = static_cast<double>(match.endFrame) / fps_;
        match.startConfidence = pendingStartConfidence_;
        match.endConfidence = endConfidence;
        matches_.push_back(match);
        state_ = MatchState::Ended;
    }

    double fps_ = 60;
    int endWindowFrames_ = 180;
    int endRequiredVotes_ = 126;
    int blackoutEndRequired_ = 15;
    std::vector<SceneMatchRange>& matches_;
    MatchState state_ = MatchState::Searching;
    Run ready_, go_, start_;
    std::deque<EndVote> endWindow_;
    int endVotes_ = 0;
    double endProbabilitySum_ = 0;
    std::int64_t readyFrame_ = -1;
    std::int64_t goFirstFrame_ = -1;
    std::int64_t goLastFrame_ = -1;
    std::int64_t pendingStartFrame_ = -1;
    float pendingStartConfidence_ = 0;
    bool gameplaySeen_ = false;
};

Prediction classify(const float* logits, std::size_t classCount) {
    int label = 0;
    float maximum = logits[0];
    for (int candidate = 1; candidate < static_cast<int>(classCount); ++candidate) {
        if (logits[candidate] > maximum) {
            maximum = logits[candidate];
            label = candidate;
        }
    }
    std::array<double, ClassCount> weights{};
    double total = 0;
    for (std::size_t candidate = 0; candidate < classCount; ++candidate) {
        weights[candidate] = std::exp(
            static_cast<double>(logits[candidate] - maximum));
        total += weights[candidate];
    }
    Prediction prediction;
    prediction.label = label;
    if (total > 0) {
        prediction.confidence = static_cast<float>(
            weights[static_cast<std::size_t>(label)] / total);
        prediction.endProbability = static_cast<float>(
            (weights[6] + (classCount > 8 ? weights[8] : 0.0) +
             (classCount > 9 ? weights[9] : 0.0)) / total);
    }
    return prediction;
}

std::int64_t fileStamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    return error ? std::numeric_limits<std::int64_t>::min()
                 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
}

std::uintmax_t fileBytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    return error ? std::numeric_limits<std::uintmax_t>::max() : bytes;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(byte)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(byte);
            }
        }
    }
    return output.str();
}

std::optional<long long> integerField(const std::string& json,
                                      const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t position = json.find(marker);
    if (position == std::string::npos) return std::nullopt;
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return std::nullopt;
    const char* begin = json.c_str() + position + 1;
    char* end = nullptr;
    const long long value = std::strtoll(begin, &end, 10);
    if (end == begin) return std::nullopt;
    return value;
}

} // namespace

bool detectSceneMatches(const std::filesystem::path& source,
                        const std::filesystem::path& sceneModel,
                        double endMinimumSeconds,
                        SceneDetectionResult& result,
                        std::string& error,
                        const SceneMatchCallback& onMatch) {
#if !defined(TR_NVDEC_DEFAULT)
    (void)source; (void)sceneModel; (void)endMinimumSeconds; (void)result;
    error = "Native scene detection requires the NVDEC/CUDA build";
    return false;
#else
    result = {};
    const auto started = Clock::now();
    SceneModel model(sceneModel, error);
    if (!model.ready()) return false;
    result.modelInitializationSeconds = model.initializationSeconds();

    // 任意の2本目: 結果画面だけを 引き分け/P1/P2 に分類する3クラスモデル。
    // TR_END_STATE_MODEL が指定されたときだけ、シーン分類が 6/8/9 を出した
    // フレームに追加適用する。境界検出用の10クラスモデルはそのまま。
    std::unique_ptr<SceneModel> endStateModel;
    std::filesystem::path endStateModelPath;
    if (const char* configured = std::getenv("TR_END_STATE_MODEL")) {
        if (configured && *configured) {
            endStateModelPath = std::filesystem::absolute(configured);
            endStateModel = std::make_unique<SceneModel>(
                endStateModelPath, error, 3, 3);
            if (!endStateModel->ready()) return false;
            result.modelInitializationSeconds +=
                endStateModel->initializationSeconds();
        }
    }

    VideoReader reader;
    if (!reader.open(source, error, false, false, true)) return false;
    if (!reader.usesNvdecCuda()) {
        error = "Native scene detector did not receive an NVDEC CUDA stream";
        return false;
    }
    result.width = reader.width();
    result.height = reader.height();
    result.fps = reader.frameRate();
    result.durationSeconds = reader.durationSeconds();
    if (!(result.fps > 0) || !std::isfinite(result.fps)) {
        error = "Native scene detector could not determine the video frame rate";
        return false;
    }
    const int blackoutEndRequired = std::max(
        1, static_cast<int>(std::ceil(result.fps * endMinimumSeconds)));
    // Optional sub-sampling of the classification pass (TR_SCENE_STRIDE, default 1).
    // Every frame is still decoded -- the bitstream needs it -- but only frames on
    // the stride grid are classified.  Skipped frames inherit the previous label so
    // the prediction array and the run list stay dense and match assembly sees the
    // same shape as an every-frame scan.
    std::int64_t classificationStride = 1;
    if (const char* configured = std::getenv("TR_SCENE_STRIDE")) {
        classificationStride = std::max<std::int64_t>(1, std::atoll(configured));
    }
    std::int64_t lastPredictionIndex = -1;
    std::vector<Prediction> predictions;
    std::vector<std::int64_t> frameIndices;
    frameIndices.reserve(BatchCapacity);
    std::vector<float> logits;
    std::size_t batchCount = 0;
    std::int64_t nextFrame = 0;
    int lastProgress = -1;
    const std::int64_t expectedFrames = static_cast<std::int64_t>(
        std::llround(result.durationSeconds * result.fps));
    if (expectedFrames > 0) {
        predictions.reserve(static_cast<std::size_t>(expectedFrames));
    }
    std::vector<SceneMatchRange> published;
    double decodeSeconds = 0, inferenceSeconds = 0, boundarySeconds = 0;
    std::int64_t lastAssembly = 0;
    auto publish = [&](bool final) -> bool {
        const auto assemblyStarted = Clock::now();
        std::vector<SceneMatchRange> stable;
        assembleMatches(predictions, result.fps, blackoutEndRequired, stable, final);
        if (stable.size() < published.size()) {
            error = "Streaming scene prefix shrank";
            return false;
        }
        for (std::size_t i = 0; i < published.size(); ++i) {
            const auto& a = published[i];
            const auto& b = stable[i];
            if (a.index != b.index || a.readyFrame != b.readyFrame ||
                a.goFirstFrame != b.goFirstFrame || a.goLastFrame != b.goLastFrame ||
                a.startFrame != b.startFrame || a.endFrame != b.endFrame ||
                a.startRule != b.startRule || a.endRule != b.endRule ||
                a.startConfidence != b.startConfidence || a.endConfidence != b.endConfidence) {
                error = "Streaming scene prefix changed at match " + std::to_string(i + 1);
                return false;
            }
        }
        for (std::size_t i = published.size(); i < stable.size(); ++i)
            if (onMatch) onMatch(stable[i], result.fps);
        published = std::move(stable);
        if (final) result.matches = published;
        // TR_SCENE_DUMP=<path>（診断用、既定なし）: 最終確定時に全フレームの判定を TSV で書き出す。
        if (final) {
            if (const char* dumpPath = std::getenv("TR_SCENE_DUMP")) {
                std::ofstream dump(dumpPath);
                dump << "frame\tlabel\tconfidence\tend_probability\n";
                for (std::size_t i = 0; i < predictions.size(); ++i)
                    dump << i << '\t' << predictions[i].label << '\t'
                         << predictions[i].confidence << '\t'
                         << predictions[i].endProbability << '\n';
            }
        }
        lastAssembly = result.classifiedFrames;
        boundarySeconds += std::chrono::duration<double>(Clock::now() - assemblyStarted).count();
        return true;
    };
    auto flush = [&]() -> bool {
        if (!batchCount) return true;
        const auto inferenceStarted = Clock::now();
        if (!model.infer(batchCount, logits, error)) return false;
        inferenceSeconds += std::chrono::duration<double>(Clock::now() - inferenceStarted).count();
        std::vector<Prediction> batchPredictions(batchCount);
        for (std::size_t index = 0; index < batchCount; ++index) {
            batchPredictions[index] = classify(
                logits.data() + index * model.classCount(), model.classCount());
        }
        // 境界判定はシーン10クラスの元ラベル（predictions）で行い、
        // 終了3クラスモデルの結果は classificationRuns の出力だけに使う。
        // こうすると勝敗の追加判定が試合境界を動かさない。
        std::vector<Prediction> outputPredictions = batchPredictions;
        if (endStateModel && endStateModel->ready()) {
            std::vector<std::size_t> endIndices;
            endIndices.reserve(batchCount);
            for (std::size_t index = 0; index < batchCount; ++index) {
                const int label = outputPredictions[index].label;
                if (label == 6 || label >= 8) endIndices.push_back(index);
            }
            if (!endIndices.empty()) {
                for (std::size_t slot = 0; slot < endIndices.size(); ++slot) {
                    const cudaError_t status = cudaMemcpyAsync(
                        endStateModel->input() + slot * InputValues,
                        model.input() + endIndices[slot] * InputValues,
                        InputValues * sizeof(float),
                        cudaMemcpyDeviceToDevice,
                        endStateModel->stream());
                    if (status != cudaSuccess) {
                        error = "End-state model input copy failed: " +
                            cudaText(status);
                        return false;
                    }
                }
                const cudaError_t synchronized =
                    cudaStreamSynchronize(endStateModel->stream());
                if (synchronized != cudaSuccess) {
                    error = "End-state model input sync failed: " +
                        cudaText(synchronized);
                    return false;
                }
                std::vector<float> endLogits;
                if (!endStateModel->infer(endIndices.size(), endLogits, error))
                    return false;
                const std::size_t endClasses = endStateModel->classCount();
                for (std::size_t slot = 0; slot < endIndices.size(); ++slot) {
                    const float* row = endLogits.data() + slot * endClasses;
                    int best = 0;
                    for (std::size_t candidate = 1; candidate < endClasses; ++candidate) {
                        if (row[candidate] > row[best])
                            best = static_cast<int>(candidate);
                    }
                    const double maximum = row[best];
                    double sum = 0.0;
                    for (std::size_t candidate = 0; candidate < endClasses; ++candidate) {
                        sum += std::exp(static_cast<double>(row[candidate]) - maximum);
                    }
                    const float confidence = static_cast<float>(
                        1.0 / std::max(sum, 1e-9));
                    const int mapped = best == 0 ? 6 : (best == 1 ? 8 : 9);
                    outputPredictions[endIndices[slot]].label = mapped;
                    outputPredictions[endIndices[slot]].confidence = confidence;
                }
            }
        }
        Prediction lastOutputPrediction;
        for (std::size_t index = 0; index < batchCount; ++index) {
            const auto& prediction = batchPredictions[index];
            const auto& outputPrediction = outputPredictions[index];
            const auto frameIndex = frameIndices[index];
            if (frameIndex >= static_cast<std::int64_t>(predictions.size())) {
                predictions.resize(static_cast<std::size_t>(frameIndex + 1));
            }
            // Fill the frames the stride skipped with the previous label, and let
            // that run cover them, before recording this frame.
            if (classificationStride > 1 && lastPredictionIndex >= 0 &&
                lastPredictionIndex + 1 < frameIndex) {
                const auto held = predictions[static_cast<std::size_t>(lastPredictionIndex)];
                const std::int64_t fillBegin = lastPredictionIndex + 1;
                const std::int64_t fillEnd = frameIndex;
                for (std::int64_t fill = fillBegin; fill < fillEnd; ++fill) {
                    predictions[static_cast<std::size_t>(fill)] = held;
                }
                if (!result.classificationRuns.empty() &&
                    result.classificationRuns.back().endFrame == fillBegin) {
                    auto& run = result.classificationRuns.back();
                    run.endFrame = fillEnd;
                    run.frameCount += fillEnd - fillBegin;
                    run.confidenceSum += lastOutputPrediction.confidence *
                        static_cast<double>(fillEnd - fillBegin);
                }
            }
            predictions[static_cast<std::size_t>(frameIndex)] = prediction;
            lastPredictionIndex = frameIndex;
            lastOutputPrediction = outputPrediction;
            if (result.classificationRuns.empty() ||
                result.classificationRuns.back().label != outputPrediction.label ||
                result.classificationRuns.back().endFrame != frameIndex) {
                SceneLabelRun run;
                run.startFrame = frameIndex;
                run.endFrame = frameIndex + 1;
                run.label = outputPrediction.label;
                // 読み替え前のシーン10クラスのラベル（引き分けの裏取り用）
                run.originalLabel = prediction.label;
                run.confidenceSum = outputPrediction.confidence;
                run.minimumConfidence = outputPrediction.confidence;
                run.frameCount = 1;
                result.classificationRuns.push_back(run);
            } else {
                auto& run = result.classificationRuns.back();
                run.endFrame = frameIndex + 1;
                run.confidenceSum += outputPrediction.confidence;
                run.minimumConfidence = std::min(
                    run.minimumConfidence, outputPrediction.confidence);
                ++run.frameCount;
            }
        }
        result.classifiedFrames += static_cast<std::int64_t>(batchCount);
        batchCount = 0;
        frameIndices.clear();
        if (onMatch && result.classifiedFrames - lastAssembly >= result.fps * 10 &&
            !publish(false)) return false;
        return true;
    };

    // Streaming sources: while a downloader is still appending to the file, EOF
    // means "wait for more data", not "the video ended".  The reader latches EOF
    // permanently, so the only reliable way to continue is to reopen the file and
    // resume just after the last frame that was classified.  Frame indices are
    // derived from the frame timestamp, so the resumed scan keeps numbering
    // correctly; frames re-decoded from the previous keyframe are skipped.
    // Everything stays inert unless TR_STREAM_DONE_FILE is set.
    const std::string streamDonePath = [] {
        const char* configured = std::getenv("TR_STREAM_DONE_FILE");
        return configured && *configured ? std::string(configured) : std::string();
    }();
    const auto streamFinished = [&streamDonePath]() -> bool {
        if (streamDonePath.empty()) return true;
        std::ifstream file(streamDonePath);
        if (!file) return false;
        std::string value;
        std::getline(file, value);
        return !value.empty();
    };
    std::int64_t lastTimestamp100ns = -1;
    // Frame indices must not depend on the container's absolute PTS origin.
    // The NVDEC demuxer already normalises, so this is a second line of defence
    // for the other reader backends: indices are counted from the first frame
    // that actually arrives.
    std::int64_t firstFrameTimestamp100ns = -1;
    bool resumedAfterCompletion = false;

    bool endOfStream = false;
    const bool pixelDiag = std::getenv("TR_STREAM_PIXEL_DIAG") != nullptr;
    // Pacing: never read past the part of the file the downloader has finished.
    // Without it the scan runs ahead to EOF, and the reopen needed to continue
    // re-creates the decoder, which changes the pixels of exactly one frame at
    // every boundary.
    const char* readyPath = std::getenv("TR_STREAM_READY_FILE");
    const std::string streamReadyPath = readyPath && *readyPath ? readyPath : "";
    // Must exceed the reader's decode look-ahead, otherwise the demuxer runs
    // into the un-written tail and latches EOF anyway.
    double readMarginSeconds = 30.0;
    if (const char* configured = std::getenv("TR_STREAM_READ_MARGIN_SECONDS")) {
        readMarginSeconds = std::max(0.0, std::atof(configured));
    }
    const auto streamReadySeconds = [&streamReadyPath]() -> double {
        if (streamReadyPath.empty()) return 0.0;
        std::ifstream file(streamReadyPath);
        if (!file) return 0.0;
        std::string value;
        std::getline(file, value);
        try {
            return std::stod(value);
        } catch (...) {
            return 0.0;
        }
    };
    while (!endOfStream) {
        if (!streamReadyPath.empty() && !streamFinished()) {
            const double ready = streamReadySeconds();
            // The reader decodes ahead by its internal pending buffer, so keep a
            // margin that is comfortably larger than that read-ahead.  With a
            // 1 s margin the demuxer still reached the written end and latched
            // EOF, which forced a reopen (and a different first frame).
            if (ready <= 0.0 ||
                static_cast<double>(nextFrame) / std::max(1.0, result.fps) >
                    ready - readMarginSeconds) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }
        }
        std::int64_t timestamp = 0;
        const auto decodeStarted = Clock::now();
        if (!reader.readCudaNv12Sample(
                timestamp, endOfStream, error,
                [&](const void* nv12, std::size_t pitch,
                    int surfaceHeight, int matrix, bool fullRange,
                    std::int64_t frameTimestamp, std::string& callbackError) {
                    if (batchCount >= BatchCapacity) {
                        callbackError = "Native scene CUDA batch overflow";
                        return false;
                    }
                    if (frameTimestamp > 0 && firstFrameTimestamp100ns < 0) {
                        firstFrameTimestamp100ns = frameTimestamp;
                    }
                    const std::int64_t relativeTimestamp =
                        frameTimestamp > 0 && firstFrameTimestamp100ns > 0
                        ? frameTimestamp - firstFrameTimestamp100ns
                        : frameTimestamp;
                    const std::int64_t timeIndex = relativeTimestamp > 0
                        ? static_cast<std::int64_t>(std::llround(
                              static_cast<double>(relativeTimestamp) *
                              result.fps / 10000000.0))
                        : nextFrame;
                    // After reopening a growing file the decoder replays frames
                    // from the previous keyframe.  They must be dropped before
                    // the luma is appended, otherwise the batch would contain
                    // duplicates that no frame index accounts for.
                    if (!streamDonePath.empty() && timeIndex < nextFrame) {
                        if (frameTimestamp > 0) lastTimestamp100ns = frameTimestamp;
                        return true;
                    }
                    const std::int64_t frameIndex = std::max(nextFrame, timeIndex);
                    if (classificationStride > 1 &&
                        (frameIndex % classificationStride) != 0) {
                        // Not classified: advance the numbering without paying for
                        // the luma preprocess or an inference slot.
                        nextFrame = frameIndex + 1;
                        result.decodedFrames = std::max(
                            result.decodedFrames, frameIndex + 1);
                        if (frameTimestamp > 0) lastTimestamp100ns = frameTimestamp;
                        return true;
                    }
                    const int status = trNv12AppendSceneLuma(
                        nv12, pitch, result.width, result.height, surfaceHeight,
                        matrix, fullRange ? 1 : 0, batchCount,
                        model.input(), model.stream());
                    if (status != static_cast<int>(cudaSuccess)) {
                        std::ostringstream details;
                        details << "Native scene luma preprocessing failed: "
                                << cudaText(static_cast<cudaError_t>(status))
                                << " source=" << nv12
                                << " pitch=" << pitch
                                << " visible=" << result.width << 'x'
                                << result.height
                                << " surface_height=" << surfaceHeight
                                << " input=" << model.input();
                        callbackError = details.str();
                        return false;
                    }
                    if (pixelDiag && frameIndex >= 2700 && frameIndex <= 2810) {
                        // The luma lives in device memory; copy a window of
                        // frames back so two runs can be compared byte for byte.
                        const std::size_t planeBytes =
                            static_cast<std::size_t>(pitch) *
                            static_cast<std::size_t>(result.height);
                        std::vector<std::uint8_t> host(planeBytes);
                        cudaStreamSynchronize(model.stream());
                        if (cudaMemcpy(host.data(), nv12, planeBytes,
                                       cudaMemcpyDeviceToHost) == cudaSuccess) {
                            std::uint64_t hash = 1469598103934665603ull;
                            for (std::size_t offset = 0; offset < planeBytes; ++offset) {
                                hash ^= host[offset];
                                hash *= 1099511628211ull;
                            }
                            BatchLog() << "frame_pixel=" << frameIndex
                                       << ' ' << std::hex << hash << std::dec << '\n';
                        }
                    }
                    // Remember the newest timestamp for every classified frame,
                    // not just the ones dropped after a reopen. Without this the
                    // first EOF after a growing-file pause left lastTimestamp100ns
                    // at -1, so the resume below reopened the reader and decoded
                    // the stream from frame 0 again (21,683 wasted frames in the
                    // MT6d stream run).
                    if (frameTimestamp > 0) lastTimestamp100ns = frameTimestamp;
                    frameIndices.push_back(frameIndex);
                    nextFrame = frameIndex + 1;
                    ++batchCount;
                    result.decodedFrames = std::max(
                        result.decodedFrames, frameIndex + 1);
                    return true;
                }, reinterpret_cast<void*>(model.stream()))) {
            return false;
        }
        decodeSeconds += std::chrono::duration<double>(Clock::now() - decodeStarted).count();
        if (batchCount >= TargetBatch && !flush()) return false;
        if (expectedFrames > 0) {
            const int progress = static_cast<int>(std::clamp<std::int64_t>(
                result.decodedFrames * 100 / expectedFrames, 0, 100));
            if (progress >= lastProgress + 10) {
                BatchLog() << "scene_progress=" << progress
                          << " decoded=" << result.decodedFrames << '\n'
                          << std::flush;
                lastProgress = progress;
            }
        }
        if (endOfStream && !streamDonePath.empty()) {
            // Wait for the downloader, then resume from just after the last
            // classified frame.  The reader caches its own EOF state, so it has
            // to be reopened; the seek lands on a keyframe and the skipped
            // frames are filtered in the callback above.  Once the downloader
            // reported completion one extra pass is still required: the last
            // chunk may have been written after the previous EOF was observed.
            if (streamFinished()) {
                if (resumedAfterCompletion) break;
                resumedAfterCompletion = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            BatchLog() << "scene_stream_resume=1 at_frame=" << nextFrame << '\n';
            // Deliberately keep the pending batch: flushing here would change
            // the inference batch sizes and therefore the last-bit values of the
            // confidences versus an uninterrupted scan of the finished file.
            reader.close();
            if (!reader.open(source, error, false, false, true)) return false;
            if (lastTimestamp100ns > 0) {
                const double resumeSeconds =
                    static_cast<double>(lastTimestamp100ns) / 10000000.0 + 0.001;
                if (!reader.seek(resumeSeconds, error)) return false;
            }
            endOfStream = false;
        }
    }
    if (!flush()) return false;
    if (!publish(true)) return false;
    result.elapsedSeconds = std::chrono::duration<double>(
        Clock::now() - started).count();
    BatchLog() << "scene_timing decode_preprocess_seconds=" << decodeSeconds
               << " inference_seconds=" << inferenceSeconds
               << " boundary_seconds=" << boundarySeconds << '\n';
    return true;
#endif
}

bool writeSceneDetection(const std::filesystem::path& destination,
                         const std::filesystem::path& source,
                         const std::filesystem::path& sceneModel,
                         const SceneDetectionResult& result,
                         std::string& error) {
    std::filesystem::path temporary = destination;
    temporary += L".writing";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not create native scene detection JSON";
        return false;
    }
    const auto sourceBytes = fileBytes(source);
    const auto modelBytes = fileBytes(sceneModel);
    const char* endStateConfigured = std::getenv("TR_END_STATE_MODEL");
    const bool endStateActive = endStateConfigured && *endStateConfigured;
    std::string endStateSettings;
    if (endStateActive) {
        endStateSettings = ",\"end_state_model\":\"" +
            jsonEscape(std::string(endStateConfigured)) +
            "\",\"end_state_labels\":\"draw/p1/p2\"";
    }
    file << "{\"schema\":\"ppt-match-detection/native-v5\""
         << ",\"engine\":\"cpp-nvdec-cuda-onnx-v5\""
         << ",\"source\":\"" << jsonEscape(source.generic_u8string()) << '"'
         << ",\"source_file\":{\"source_bytes\":" << sourceBytes
         << ",\"source_mtime_ticks\":" << fileStamp(source) << "}"
         << ",\"model\":{\"path\":\""
         << jsonEscape(sceneModel.generic_u8string()) << '"'
         << ",\"model_bytes\":" << modelBytes
         << ",\"model_mtime_ticks\":" << fileStamp(sceneModel) << "}"
         << ",\"video\":{\"fps\":" << std::fixed << std::setprecision(9)
         << result.fps << ",\"frame_count\":" << result.decodedFrames
         << ",\"duration_seconds\":" << result.durationSeconds
         << ",\"width\":" << result.width << ",\"height\":"
         << result.height << "}"
         << ",\"settings\":{\"scan\":\"every-frame\""
         << ",\"start_rule\":\"independent-ready-strong-go-post-go-start\""
         << ",\"ready_evidence_window_seconds\":4.0"
         << ",\"ready_evidence_minimum_seconds\":0.25"
         << ",\"strong_go_minimum_seconds\":0.25"
         << ",\"go_exit_trial_seconds\":0.30"
         << ",\"go_exit_lossless_fallback_seconds\":0.25"
         << ",\"start_search_seconds\":2.0"
         << ",\"end_rule\":\"3s-70pct-with-blackout-and-next-ceremony-recovery\""
         << ",\"end_window_seconds\":3.0"
         << ",\"end_ratio\":0.70"
         << ",\"blackout_end_minimum_seconds\":0.25"
         << ",\"crop\":\"largest-centered-16:9\""
         << ",\"input\":\"1x90x160 luma\""
         << endStateSettings << "}"
         << ",\"classification_runs\":{\"schema\":\"ppt-scene-classification-runs/v1\""
         << ",\"encoding\":\"[start_frame,end_frame_exclusive,label,mean_confidence,min_confidence,original_label]\""
         << ",\"labels\":[\"00_unrelated\",\"10_ppt_other\",\"30_ready\","
            "\"40_go\",\"45_analysis_start\",\"50_gameplay\","
         << (endStateActive ? "\"60_end_draw\"" : "\"60_end\"")
         << ",\"70_blackout\",\"80_end_p1\",\"90_end_p2\"],\"items\":[";
    for (std::size_t index = 0; index < result.classificationRuns.size(); ++index) {
        if (index) file << ',';
        const auto& run = result.classificationRuns[index];
        file << '[' << run.startFrame << ',' << run.endFrame << ',' << run.label
             << ',' << (run.confidenceSum / std::max<std::int64_t>(run.frameCount, 1))
             << ',' << run.minimumConfidence << ',' << run.originalLabel << ']';
    }
    file << "]},\"matches\":[";
    // 盤面解析は終了位置の少し後ろまで見る（最後のミノが落ちきる前に切れるのを防ぐ）。
    // 表示上の end は結果画面の頭のまま。
    constexpr double BoardRangePaddingSeconds = 0.0;   // 2026-11-06: 終了=最後の試合中フレームなので余白なし
    for (std::size_t index = 0; index < result.matches.size(); ++index) {
        if (index) file << ',';
        const auto& match = result.matches[index];
        const double boardEndSeconds = std::min(
            match.endSeconds + BoardRangePaddingSeconds,
            static_cast<double>(result.decodedFrames) / std::max(1.0, result.fps));
        const std::int64_t boardEndFrame = std::max(
            match.endFrame,
            static_cast<std::int64_t>(std::llround(boardEndSeconds * result.fps)));
        file << "{\"index\":" << match.index
             << ",\"status\":\"confirmed\""
             << ",\"ready\":{\"frame\":" << match.readyFrame
             << ",\"seconds\":" << static_cast<double>(match.readyFrame) / result.fps << "}"
             << ",\"go\":{\"first_frame\":" << match.goFirstFrame
             << ",\"first_seconds\":" << static_cast<double>(match.goFirstFrame) / result.fps
             << ",\"last_frame\":" << match.goLastFrame
             << ",\"last_seconds\":" << static_cast<double>(match.goLastFrame) / result.fps << "}"
             << ",\"analysis_start\":{\"frame\":" << match.startFrame
             << ",\"seconds\":" << match.startSeconds
             << ",\"confidence\":" << match.startConfidence
             << ",\"rule\":\"" << match.startRule << "\"}"
             << ",\"start_candidates\":{\"legacy_analysis_start\":{\"frame\":"
             << match.startFrame << ",\"seconds\":" << match.startSeconds
             << "},\"go_exit\":{\"frame\":" << (match.goLastFrame + 1)
             << ",\"seconds\":"
             << static_cast<double>(match.goLastFrame + 1) / result.fps
             << ",\"go_duration_seconds\":"
             << static_cast<double>(match.goLastFrame - match.goFirstFrame + 1) /
                    result.fps
             << ",\"meets_0_30_seconds\":"
             << ((match.goLastFrame - match.goFirstFrame + 1) /
                         result.fps + .000001 >= .30 ? "true" : "false")
             << "}}"
             << ",\"end\":{\"frame\":" << match.endFrame
             << ",\"seconds\":" << match.endSeconds
             << ",\"confidence\":" << match.endConfidence
             << ",\"rule\":\"" << match.endRule << "\"}"
             << ",\"analysis_range\":{\"start_frame_inclusive\":"
             << match.startFrame << ",\"end_frame_exclusive\":" << boardEndFrame
             << ",\"start_seconds\":" << match.startSeconds
             << ",\"end_seconds\":" << boardEndSeconds
             << ",\"duration_seconds\":"
             << (boardEndSeconds - match.startSeconds) << "}}";
    }
    file << "] ,\"unresolved\":[]"
         << ",\"scan\":{\"elapsed_seconds\":" << result.elapsedSeconds
         << ",\"source_seconds_per_processing_second\":"
         << result.durationSeconds / std::max(result.elapsedSeconds, 1e-9)
         << ",\"decoded_frames\":" << result.decodedFrames
         << ",\"unique_classified_frames\":" << result.classifiedFrames
         << ",\"classification_runs\":" << result.classificationRuns.size()
         << ",\"model_initialization_seconds\":"
         << result.modelInitializationSeconds << "}}\n";
    file.close();
    if (!file) {
        error = "Could not finish native scene detection JSON";
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::remove(destination, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "Could not finalize native scene detection JSON: " +
            filesystemError.message();
        return false;
    }
    return true;
}

bool sceneDetectionCacheValid(const std::filesystem::path& detection,
                              const std::filesystem::path& source,
                              const std::filesystem::path& sceneModel) {
    std::ifstream file(detection, std::ios::binary);
    if (!file) return false;
    std::ostringstream value;
    value << file.rdbuf();
    const std::string json = value.str();
    if (json.find("\"engine\":\"cpp-nvdec-cuda-onnx-v5\"") == std::string::npos)
        return false;
    const auto sourceBytes = integerField(json, "source_bytes");
    const auto sourceStamp = integerField(json, "source_mtime_ticks");
    const auto modelBytes = integerField(json, "model_bytes");
    const auto modelStamp = integerField(json, "model_mtime_ticks");
    if (json.find("\"classification_runs\"") == std::string::npos)
        return false;
    return sourceBytes && sourceStamp && modelBytes && modelStamp &&
        *sourceBytes == static_cast<long long>(fileBytes(source)) &&
        *sourceStamp == fileStamp(source) &&
        *modelBytes == static_cast<long long>(fileBytes(sceneModel)) &&
        *modelStamp == fileStamp(sceneModel);
}

} // namespace tr
