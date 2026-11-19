#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tr {

struct SceneMatchRange {
    int index = 0;
    std::int64_t readyFrame = -1;
    std::int64_t goFirstFrame = -1;
    std::int64_t goLastFrame = -1;
    std::int64_t startFrame = -1;
    std::int64_t endFrame = -1;
    double startSeconds = 0;
    double endSeconds = 0;
    float startConfidence = 0;
    float endConfidence = 0;
    std::string startRule;
    std::string endRule;
};

struct SceneLabelRun {
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
    int label = 0;
    // 終了3クラスで読み替える前の、シーン10クラスのラベル（-1 は未設定）。
    // 勝敗が「引き分け」になったときの裏取りに使う。
    int originalLabel = -1;
    double confidenceSum = 0;
    float minimumConfidence = 1;
    std::int64_t frameCount = 0;
};

struct SceneDetectionResult {
    int width = 0;
    int height = 0;
    double fps = 0;
    double durationSeconds = 0;
    std::int64_t decodedFrames = 0;
    std::int64_t classifiedFrames = 0;
    double elapsedSeconds = 0;
    double modelInitializationSeconds = 0;
    std::vector<SceneMatchRange> matches;
    std::vector<SceneLabelRun> classificationRuns;
};

// Production stream scanner. Every decoded frame follows the direct
// NVDEC -> CUDA luma resize -> ONNX Runtime CUDA path; Python and OpenCV are
// intentionally absent from this API.
// Called in chronological order only after the next complete ceremony makes a
// range immutable. The last range is delivered at EOF. All frames are classified.
using SceneMatchCallback = std::function<void(const SceneMatchRange&, double)>;
bool detectSceneMatches(const std::filesystem::path& source,
                        const std::filesystem::path& sceneModel,
                        double endMinimumSeconds,
                        SceneDetectionResult& result,
                        std::string& error,
                        const SceneMatchCallback& onMatch = {});

bool writeSceneDetection(const std::filesystem::path& destination,
                         const std::filesystem::path& source,
                         const std::filesystem::path& sceneModel,
                         const SceneDetectionResult& result,
                         std::string& error);

// A cached detection is accepted only when both the source stream and scene
// model have identical byte counts and filesystem modification stamps.
bool sceneDetectionCacheValid(const std::filesystem::path& detection,
                              const std::filesystem::path& source,
                              const std::filesystem::path& sceneModel);

} // namespace tr
