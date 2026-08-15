#ifdef __EMSCRIPTEN__

#include "recovery.hpp"
#include "onnx_model.hpp"
#include "vision.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tr::BoardObservation;
using tr::Cell;
using tr::Frame;
using tr::QueueRecognitionSample;
using tr::QueueObservation;
using tr::RecoveryOutput;
using tr::Settings;
using tr::VisionAnalyzer;

struct Runtime {
    int width = 0;
    int height = 0;
    Settings settings;
    double duration = 0;
    std::unique_ptr<tr::OnnxBoardModel> geometryModel;
    std::unique_ptr<VisionAnalyzer> p1Vision;
    std::unique_ptr<VisionAnalyzer> p2Vision;
    std::vector<QueueRecognitionSample> p1Queue;
    std::vector<QueueRecognitionSample> p2Queue;
    std::vector<double> p1BoardTimes;
    std::vector<double> p2BoardTimes;
    bool requestsPrepared = false;
    std::vector<BoardObservation> p1Boards;
    std::vector<BoardObservation> p2Boards;
    RecoveryOutput output;
    std::string error;
    std::string outputPath;
};

Runtime runtime;

void setError(const std::string& value) {
    runtime.error = value;
}

int fail(const std::string& value) {
    setError(value);
    return 0;
}

void copyString(const std::string& value, char* destination, int capacity) {
    if (!destination || capacity <= 0) return;
    const std::size_t count = std::min<std::size_t>(value.size(), static_cast<std::size_t>(capacity - 1));
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

bool validFrameInput(const std::uint8_t* rgba, int byteCount) {
    if (!rgba || runtime.width <= 0 || runtime.height <= 0) {
        setError("WASM vision is not initialized");
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(runtime.width) * runtime.height * 4;
    if (byteCount < 0 || static_cast<std::size_t>(byteCount) < expected) {
        setError("Canvas frame has fewer pixels than the initialized video size");
        return false;
    }
    return true;
}

Frame makeFrame(const std::uint8_t* rgba) {
    Frame frame;
    frame.width = runtime.width;
    frame.height = runtime.height;
    const std::size_t pixels = static_cast<std::size_t>(runtime.width) * runtime.height;
    frame.bgra.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        // Canvas ImageData is RGBA; VisionAnalyzer consumes the native
        // VideoReader BGRA layout. Alpha is retained for byte-for-byte frame
        // shape compatibility even though the classifier ignores it.
        frame.bgra[i * 4 + 0] = rgba[i * 4 + 2];
        frame.bgra[i * 4 + 1] = rgba[i * 4 + 1];
        frame.bgra[i * 4 + 2] = rgba[i * 4 + 0];
        frame.bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    return frame;
}

VisionAnalyzer* visionForPlayer(int player) {
    if (player == 1) return runtime.p1Vision.get();
    if (player == 2) return runtime.p2Vision.get();
    setError("Player must be 1 or 2");
    return nullptr;
}

std::vector<QueueRecognitionSample>& queueForPlayer(int player) {
    return player == 1 ? runtime.p1Queue : runtime.p2Queue;
}

std::vector<BoardObservation>& boardsForPlayer(int player) {
    return player == 1 ? runtime.p1Boards : runtime.p2Boards;
}

std::vector<double>& boardTimesForPlayer(int player) {
    return player == 1 ? runtime.p1BoardTimes : runtime.p2BoardTimes;
}

Cell labelToCell(int value) {
    // Same class order as native OnnxBoardModel::labelToCell:
    // null, G, S, Z, L, J, O, I, T.
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

extern "C" {

EMSCRIPTEN_KEEPALIVE
int tr_runtime_init(int width, int height, const char* settingsPath) {
    runtime = Runtime{};
    runtime.width = width;
    runtime.height = height;
    if (width <= 0 || height <= 0) return fail("Video dimensions must be positive");

    if (settingsPath && settingsPath[0] != '\0') {
        if (!tr::loadSettings(std::filesystem::path(settingsPath), runtime.settings, runtime.error)) return 0;
    }
    std::string geometryError;
    runtime.geometryModel = std::make_unique<tr::OnnxBoardModel>(std::filesystem::path{}, geometryError);
    runtime.p1Vision = std::make_unique<VisionAnalyzer>(width, height, runtime.settings.p1, runtime.geometryModel.get());
    runtime.p2Vision = std::make_unique<VisionAnalyzer>(width, height, runtime.settings.p2, runtime.geometryModel.get());
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const char* tr_last_error() {
    return runtime.error.c_str();
}

EMSCRIPTEN_KEEPALIVE
void tr_runtime_reset(double durationSeconds) {
    runtime.duration = durationSeconds;
    runtime.p1Queue.clear();
    runtime.p2Queue.clear();
    runtime.p1BoardTimes.clear();
    runtime.p2BoardTimes.clear();
    runtime.requestsPrepared = false;
    runtime.p1Boards.clear();
    runtime.p2Boards.clear();
    runtime.output = RecoveryOutput{};
    runtime.error.clear();
}

EMSCRIPTEN_KEEPALIVE
int tr_queue_observe_and_add(int player, const std::uint8_t* rgba, int byteCount, double timeSeconds,
                             int* hold, int* next, int nextCapacity,
                             std::uint8_t* colors, int colorsCapacity) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount)) return 0;
    Frame frame = makeFrame(rgba);
    const QueueObservation observation = vision->observeQueue(frame);
    if (hold) *hold = static_cast<int>(observation.hold);
    if (next && nextCapacity > 0) {
        const int count = std::min<int>(nextCapacity, observation.next.size());
        for (int i = 0; i < count; ++i) next[i] = static_cast<int>(observation.next[static_cast<std::size_t>(i)]);
    }
    if (colors && colorsCapacity > 0) {
        const int count = std::min<int>(colorsCapacity / 3, observation.colors.size());
        for (int i = 0; i < count; ++i) {
            colors[i * 3 + 0] = observation.colors[static_cast<std::size_t>(i)][0];
            colors[i * 3 + 1] = observation.colors[static_cast<std::size_t>(i)][1];
            colors[i * 3 + 2] = observation.colors[static_cast<std::size_t>(i)][2];
        }
    }
    QueueRecognitionSample sample;
    sample.timeSeconds = timeSeconds;
    sample.observation = observation;
    queueForPlayer(player).push_back(std::move(sample));
    return static_cast<int>(observation.next.size()) + 1;
}

EMSCRIPTEN_KEEPALIVE
int tr_prepare_board_requests(int player, double* times, int capacity) {
    if (runtime.duration <= 0) return fail("Video duration must be set before queue preparation");
    if (player != 1 && player != 2) return fail("Player must be 1 or 2");
    if (!runtime.requestsPrepared) {
        if (!tr::prepareObservationRequests(runtime.duration, runtime.settings,
                                            runtime.p1Queue, runtime.p2Queue,
                                            runtime.p1BoardTimes, runtime.p2BoardTimes,
                                            runtime.error)) return 0;
        runtime.requestsPrepared = true;
    }
    const auto& source = boardTimesForPlayer(player);
    if (times && capacity > 0) {
        const int count = std::min<int>(capacity, source.size());
        std::copy_n(source.begin(), count, times);
    }
    return static_cast<int>(source.size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_features(int player, const std::uint8_t* rgba, int byteCount, float* output, int outputCapacity) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !output || outputCapacity < 200 * 63) return 0;
    const Frame frame = makeFrame(rgba);
    const auto features = vision->boardFeatures(frame);
    std::copy(features.begin(), features.end(), output);
    return static_cast<int>(features.size());
}

EMSCRIPTEN_KEEPALIVE
int tr_board_finish(int player, const std::uint8_t* rgba, int byteCount, double timeSeconds,
                    const std::uint8_t* classLabels, int labelCount) {
    VisionAnalyzer* vision = visionForPlayer(player);
    if (!vision || !validFrameInput(rgba, byteCount) || !classLabels || labelCount < 200) return 0;
    std::vector<Cell> labels(200, Cell::Empty);
    for (int i = 0; i < 200; ++i) labels[static_cast<std::size_t>(i)] = labelToCell(classLabels[i]);
    const Frame frame = makeFrame(rgba);
    BoardObservation observation = vision->analyzeBoardWithLabels(frame, labels);
    observation.timeSeconds = timeSeconds;
    boardsForPlayer(player).push_back(std::move(observation));
    return static_cast<int>(boardsForPlayer(player).size());
}

EMSCRIPTEN_KEEPALIVE
int tr_recover() {
    if (runtime.duration <= 0) return fail("Video duration must be set before recovery");
    if (!tr::recoverObservations(runtime.duration, runtime.settings,
                                 runtime.p1Queue, runtime.p2Queue,
                                 runtime.p1Boards, runtime.p2Boards,
                                 runtime.output, runtime.error)) return 0;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
double tr_sample_interval() {
    return runtime.settings.sampleIntervalSeconds;
}

EMSCRIPTEN_KEEPALIVE
int tr_onnx_samples() {
    return runtime.settings.onnxSamples;
}

EMSCRIPTEN_KEEPALIVE
int tr_write_output(const char* inputPath, const char* outputDirectory) {
    if (!inputPath || !outputDirectory) {
        return fail("No recovered timeline is available for output");
    }
    runtime.output.includeSourceVideoInTraining = true;
    if (!tr::writeRecoveredOutput(std::filesystem::path(inputPath),
                                  std::filesystem::path(outputDirectory),
                                  runtime.settings, runtime.output, runtime.error)) return 0;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
const char* tr_output_path(int kind) {
    const std::filesystem::path* path = nullptr;
    switch (kind) {
    case 0: path = &runtime.output.jsonPath; break;
    case 1: path = &runtime.output.p1UrlPath; break;
    case 2: path = &runtime.output.p2UrlPath; break;
    case 3: path = &runtime.output.combinedUrlPath; break;
    case 4: path = &runtime.output.linksPath; break;
    case 5: path = &runtime.output.reportPath; break;
    case 6: path = &runtime.output.trainingAnnotationPath; break;
    case 7: path = &runtime.output.trainingManifestPath; break;
    case 8: path = &runtime.output.trainingVideoPath; break;
    default: runtime.outputPath.clear(); return runtime.outputPath.c_str();
    }
    runtime.outputPath = path->u8string();
    return runtime.outputPath.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* tr_output_url(int player) {
    if (player == 1) return runtime.output.p1Url.c_str();
    if (player == 2) return runtime.output.p2Url.c_str();
    if (player == 3) return runtime.output.combinedUrl.c_str();
    return "";
}

EMSCRIPTEN_KEEPALIVE
void tr_copy_error(char* destination, int capacity) {
    copyString(runtime.error, destination, capacity);
}

}

#endif
