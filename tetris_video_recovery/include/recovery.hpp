#pragma once

#include "common.hpp"
#include "video_reader.hpp"

namespace tr {

struct RecoveryOutput {
    // Kept separately from the solved timelines so a correction can retain
    // ONNX observations and re-run only the downstream beam search.
    std::vector<TimelineStep> rawP1;
    std::vector<TimelineStep> rawP2;
    std::vector<TimelineStep> p1;
    std::vector<TimelineStep> p2;
    double videoDurationSeconds = 0;
    std::vector<QueueRecognitionSample> queueObservationsP1;
    std::vector<QueueRecognitionSample> queueObservationsP2;
    std::vector<QueueRecognitionSample> originalQueueObservationsP1;
    std::vector<QueueRecognitionSample> originalQueueObservationsP2;
    std::vector<BoardObservation> boardObservationsP1;
    std::vector<BoardObservation> boardObservationsP2;
    std::filesystem::path jsonPath;
    std::filesystem::path p1UrlPath;
    std::filesystem::path p2UrlPath;
    std::filesystem::path combinedUrlPath;
    std::filesystem::path linksPath;
    std::filesystem::path reportPath;
    std::filesystem::path trainingDatasetDirectory;
    std::filesystem::path trainingAnnotationPath;
    std::filesystem::path trainingVideoPath;
    std::filesystem::path trainingManifestPath;
    std::string p1Url;
    std::string p2Url;
    std::string combinedUrl;
    // Set only by the interactive editor's explicit export action.  Command
    // line compatibility exports remain useful predictions but are not
    // silently promoted to human-approved training labels.
    bool humanApproved = false;
    // The review UI defaults to a self-contained training sample: it adds a
    // hard link to the source video when possible, and copies it otherwise.
    // This can be turned off for users who only want the annotation JSON.
    bool includeSourceVideoInTraining = true;
};

bool loadSettings(const std::filesystem::path& path, Settings& settings, std::string& error);
// Analysis stops at an editable, source-compatible solved timeline.  It does
// not write files or launch a browser; the native review UI decides when the
// user has approved the result.
bool analyzeVideo(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status, RecoveryOutput& output, std::string& error);
// Browser/WASM entry point. The browser supplies the exact raw queue and
// board observations produced by the shared VisionAnalyzer; all queue
// decoding, phase construction, legal-move reconstruction, and beam scoring
// then run through the same C++ implementation as the native tool.
bool recoverObservations(double videoDurationSeconds, const Settings& settings,
                         std::vector<QueueRecognitionSample> p1Queue,
                         std::vector<QueueRecognitionSample> p2Queue,
                         std::vector<BoardObservation> p1Boards,
                         std::vector<BoardObservation> p2Boards,
                         RecoveryOutput& output, std::string& error);
// Decode the complete raw queue history using the native queue decoder and
// return the exact ONNX request times generated from its phase boundaries.
// The browser uses this between its queue and board passes; the returned
// queue vectors contain the same decoded metadata that the native pipeline
// keeps for its JSON/review output.
bool prepareObservationRequests(double videoDurationSeconds, const Settings& settings,
                                std::vector<QueueRecognitionSample>& p1Queue,
                                std::vector<QueueRecognitionSample>& p2Queue,
                                std::vector<double>& p1BoardTimes,
                                std::vector<double>& p2BoardTimes,
                                std::string& error);
// Rebuild phase boundaries and legal-move timelines from edited raw queue
// samples while reusing the already-computed ONNX board observations.
bool reanalyzeQueueObservations(const Settings& settings, RecoveryOutput& output, std::string& error);
// Persist the currently approved/corrected timeline as simulator shortcuts,
// JSON and an HTML review report.
bool writeRecoveredOutput(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                          const Settings& settings, RecoveryOutput& output, std::string& error);
// Compatibility entry point for non-interactive callers/tests: analyze then
// immediately write.  The desktop application uses analyzeVideo instead.
bool recoverVideo(const std::filesystem::path& input, const std::filesystem::path& outputDirectory, const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status, RecoveryOutput& output, std::string& error);

} // namespace tr
