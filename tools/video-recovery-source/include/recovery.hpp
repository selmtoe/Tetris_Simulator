#pragma once

#include "common.hpp"
#include "tetris_engine.hpp"
#include "video_reader.hpp"

namespace tr {

struct TrajectoryRepairReport {
    bool attempted = false;
    bool adopted = false;
    bool garbageHypothesesSearched = false;
    bool neuralRankerUsed = false;
    int player = 0;
    std::size_t firstSuspiciousPhase = 0;
    double firstSuspiciousSeconds = 0;
    int initialBeamWidth = 0;
    int finalBeamWidth = 0;
    int initialSeverePhases = 0;
    int finalSeverePhases = 0;
    double initialMinimumMatchRatio = 1;
    double finalMinimumMatchRatio = 1;
    // Set when the optional wall-clock ceiling stopped the repair ladder before
    // it could compare every hypothesis. The fast-pass timeline is kept as-is,
    // so the caller must report the match as needing correction rather than
    // treat the unchanged trajectory as a verified result.
    bool timeBudgetExceeded = false;
    double searchSeconds = 0;
    // placement-lookahead, garbage-rise-backtracking, or no-improvement.
    std::string cause;
};

struct PlayerNameRecognition {
    bool attempted = false;
    std::string text;
    double confidence = 0;
    std::string engine;
    std::string error;
    LayoutRect crop{};
};

struct RecoveryOutput {
    // Kept separately from the solved timelines so a correction can retain
    // ONNX observations and re-run only the downstream beam search.
    std::vector<TimelineStep> rawP1;
    std::vector<TimelineStep> rawP2;
    std::vector<TimelineStep> p1;
    std::vector<TimelineStep> p2;
    // Exact beam frontiers for differential continuation/reanalysis. These
    // are runtime-only caches and are intentionally absent from exports.
    std::shared_ptr<BeamSearchCache> beamCacheP1;
    std::shared_ptr<BeamSearchCache> beamCacheP2;
    double videoDurationSeconds = 0;
    // When native analysis is intentionally paused, this records how much of
    // the source has already been analyzed while videoDurationSeconds keeps
    // the full source duration for continuation/export.
    double analyzedUntilSeconds = 0;
    // A range analysis keeps its reconstruction timeline relative to the
    // beginning of the match while the review player opens the original
    // stream.  This offset maps a relative timeline second back to the
    // source-video timestamp without creating a clipped/re-encoded video.
    double sourceStartSeconds = 0;
    double sourceEndSeconds = 0;
    double sourceVideoDurationSeconds = 0;
    std::vector<QueueRecognitionSample> queueObservationsP1;
    std::vector<QueueRecognitionSample> queueObservationsP2;
    std::vector<QueueRecognitionSample> originalQueueObservationsP1;
    std::vector<QueueRecognitionSample> originalQueueObservationsP2;
    // Incremented for each queue-log edit so reanalysis can freeze the
    // already reviewed prefix before the newest edit.
    std::uint64_t queueEditRevision = 0;
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
    std::filesystem::path trainingIndexPath;
    // Filled only when visible-board agreement triggered a bounded local
    // re-search. It explains where the trajectory first became suspicious
    // and whether wider placement/garbage hypotheses repaired it.
    TrajectoryRepairReport trajectoryRepair;
    std::string p1Url;
    std::string p2Url;
    std::string combinedUrl;
    PlayerNameRecognition p1PlayerName;
    PlayerNameRecognition p2PlayerName;
    // Runtime-only signal for the range analyzer. When READY/GO pixels made
    // the decoded opening non-empty, the batch wrapper may probe the three
    // immediately preceding frames for a genuinely empty start without
    // disturbing matches whose opening was already recognised correctly.
    bool forcedEmptyOpeningApplied = false;
    // Set only by the interactive editor's explicit export action.  Command
    // line compatibility exports remain useful predictions but are not
    // silently promoted to human-approved training labels.
    bool humanApproved = false;
    // The review UI defaults to a self-contained training sample: it adds a
    // hard link to the source video when possible, and copies it otherwise.
    // This can be turned off for users who only want the annotation JSON.
    bool includeSourceVideoInTraining = true;
};

// Read only the first decoded frame at `sourceSeconds`, crop the two fixed
// name regions from the centered 16:9 image, and run the configured OCR.
// OCR is metadata-only: a missing engine/model or unreadable name never
// invalidates an otherwise successful board reconstruction.
bool recognizePlayerNames(const std::filesystem::path& input,
                          double sourceSeconds,
                          const Settings& settings,
                          RecoveryOutput& output,
                          std::string& warning);

// Fast range-analysis path: recognize the two fixed names from a frame that
// was already decoded by the queue pass. `frame` may contain only the compact
// union of both name strips; sourceX/sourceY preserve full-frame addressing.
bool recognizePlayerNamesFromFrame(const Frame& frame,
                                   int sourceWidth, int sourceHeight,
                                   const Settings& settings,
                                   RecoveryOutput& output,
                                   std::string& warning);

#ifndef __EMSCRIPTEN__
// Copy only the two name strips from the NV12 surface already delivered by
// NVDEC and convert that small union to BGRA. This avoids opening a second
// demuxer/decoder per match. The CUDA stream is the queue pass's existing
// stream and is synchronized exactly once for this metadata frame.
bool capturePlayerNameFrameFromNv12(
    const void* nv12, std::size_t pitch, int sourceWidth, int sourceHeight,
    int surfaceHeight, int matrix, bool fullRange, void* cudaStream,
    const Settings& settings, Frame& frame, std::string& error);
#endif

// Diagnostics for the bounded opening-only fallback used by stream range
// analysis.  The ordinary fast path never performs this scan: it is invoked
// only when the first recovered board/HOLD state is not empty for both
// players.  A contiguous run is reported instead of trusting one accidental
// all-empty CNN frame.
struct OpeningStartSearchResult {
    bool found = false;
    double startSeconds = 0;
    double endSeconds = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t testedFrames = 0;
    int consecutiveFrames = 0;
};

struct RecoveryQualityIssue {
    int player = 0;
    std::size_t firstPhase = 0;
    std::size_t lastPhase = 0;
    double startSeconds = 0;
    double endSeconds = 0;
    double minimumMatchRatio = 1;
    int consecutivePhases = 0;
    std::size_t lastMovePhase = 0;
    std::size_t movesThroughEnd = 0;
};

struct RecoveryQualityReport {
    // A warning is intentionally conservative: at least two consecutive
    // solved/observed boards must agree in fewer than 80% of visible cells.
    // The reviewer stores the whole detected match when the user confirms it.
    double matchRatioThreshold = .70;   // 0.8は厳しすぎる（発光エフェクトで盤面が隠れる数フレームを拾っていた）
    int requiredConsecutivePhases = 2;
    std::vector<RecoveryQualityIssue> issues;
    // Only a completed match whose every issue is confined to the affected
    // player's final six moves is exempt from the visible quality warning.
    // Keep those issues as match-end detection training evidence.
    static constexpr std::size_t TerminalMoveLimit = 6;
    std::vector<RecoveryQualityIssue> terminalIssues;

    bool suspicious() const { return !issues.empty(); }
    bool terminalAnomaly() const { return !terminalIssues.empty(); }
    std::size_t totalIssueCount() const { return issues.size() + terminalIssues.size(); }
};

// 修復の意思決定用（従来の厳しい判定）
RecoveryQualityReport evaluateRecoveryQuality(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2);
// 出力（品質警告）用。ライン消去直後の読めない区間を除外する。
RecoveryQualityReport evaluateRecoveryQualityForReport(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2);

RecoveryQualityReport evaluateRecoveryQuality(
    const std::vector<TimelineStep>& p1,
    const std::vector<TimelineStep>& p2);

// Stream segmentation establishes a new-game invariant independently of the
// pixels covered by READY/GO effects. Apply it to the persisted/reviewed
// time-zero state without rewriting later placements.
// With settings and TR_OPENING_RESOLVE=1, a player whose already-solved
// phase 1 kept board cells without a placement is solved again (see .cpp).
void forceRecoveryOpeningEmpty(RecoveryOutput& output, const Settings* settings = nullptr);

// Returns the earliest phase where automatic legal-move recovery stopped.
// Unlike a decoder/model failure, this state is intentionally reviewable and
// is persisted in autosaves so the correction UI can resume at the problem.
bool firstRecoveryUnresolved(const RecoveryOutput& output,
                             int& player,
                             std::size_t& phase);

bool loadSettings(const std::filesystem::path& path, Settings& settings, std::string& error);
// Analysis stops at an editable, source-compatible solved timeline.  It does
// not write files or launch a browser; the native review UI decides when the
// user has approved the result.
bool analyzeVideo(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status, RecoveryOutput& output, std::string& error,
                  double analysisEndSeconds = 0);
// Speed-prioritized native path used only by the separate GPU fast-final
// executable. It establishes the delivery-order-sensitive opening with the
// accepted legacy reader, then analyzes the remaining range through D3D11.
// The ordinary analyzeVideo path and executable remain unchanged.
bool analyzeVideoFastFinal(const std::filesystem::path& input,
                           const std::filesystem::path& modelPath,
                           const Settings& settings, Status& status,
                           RecoveryOutput& output, std::string& error,
                           double analysisEndSeconds = 0,
                           double legacyPrefixSeconds = 20.0);

// High-throughput range analyzer for stream segmentation.  The object owns
// reusable CUDA/ONNX sessions, so constructing it once and calling analyze()
// for every match avoids model initialization per match.  Returned timeline
// timestamps are relative to startSeconds; sourceStartSeconds retains the
// exact mapping needed by the review player.
// One instance is confined to one worker: models and persistent inference
// bindings are mutable. Independent instances may run concurrently. Configure
// the process environment before creating any workers and keep it immutable.
class VideoRangeAnalyzer {
public:
    VideoRangeAnalyzer(std::filesystem::path input,
                       std::filesystem::path modelPath,
                       Settings settings);
    ~VideoRangeAnalyzer();
    VideoRangeAnalyzer(VideoRangeAnalyzer&&) noexcept;
    VideoRangeAnalyzer& operator=(VideoRangeAnalyzer&&) noexcept;
    VideoRangeAnalyzer(const VideoRangeAnalyzer&) = delete;
    VideoRangeAnalyzer& operator=(const VideoRangeAnalyzer&) = delete;

    bool analyze(double startSeconds, double endSeconds,
                 Status& status, RecoveryOutput& output,
                 std::string& error);
    // Decode one small ceremony window once and locate the beginning of the
    // latest contiguous run where both visible boards and both HOLD slots are
    // empty. Board sessions are reused from analyze(); no full match recovery
    // or beam search is performed by this fallback.
    bool findEmptyOpeningStart(double searchStartSeconds,
                               double searchEndSeconds,
                               int requiredConsecutiveFrames,
                               OpeningStartSearchResult& result,
                               std::string& error);
    // v76: the first frame after GO where the HOLD/NEXT slots that the GO
    // graphic never covers (P1 HOLD/NEXT1/NEXT2, P2 HOLD/NEXT1-5) change from
    // the first GO frame. That is the first spawn (the queue advances), so it
    // is used as the match start. found=false when no such frame exists.
    bool findQueueMoveStart(double goFirstSeconds,
                            double searchEndSeconds,
                            OpeningStartSearchResult& result,
                            std::string& error);
    std::uint64_t createdModelSessions() const;
    std::uint64_t reusedModelSessions() const;
    double modelInitializationSeconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool continueVideoAnalysis(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                           const Settings& settings, Status& status, RecoveryOutput& output,
                           std::string& error, double resumeFromSeconds = 0,
                           double analysisEndSeconds = 0);
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
// Browser-friendly split form of recoverObservations.  Preparing the raw
// timelines and solving each player separately lets the UI yield between the
// two synchronous WASM beam calls and show which side is currently running.
bool prepareRecoveryObservations(double videoDurationSeconds, const Settings& settings,
                                 std::vector<QueueRecognitionSample> p1Queue,
                                 std::vector<QueueRecognitionSample> p2Queue,
                                 std::vector<BoardObservation> p1Boards,
                                 std::vector<BoardObservation> p2Boards,
                                 RecoveryOutput& output, std::string& error,
                                 bool preserveManualCorrections = false,
                                 double preserveManualUntilSeconds = -1);
bool solveRecoveryPlayer(RecoveryOutput& output, int player, const Settings& settings, std::string& error);
// Split the usable portion of one NEXT phase into equal sections and return
// the first instant of every section. The phase start is therefore always the
// first board observation.
std::vector<double> boardSampleTimes(double startSeconds, double endSeconds, int requestedSamples);
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
// Apply the same manual legal-candidate correction used by the native review
// window.  The browser review calls this through the WASM bridge so candidate
// generation, anchoring, and downstream beam recomputation cannot drift.
bool applyCorrectionCandidate(RecoveryOutput& output, int player, std::size_t phase,
                              std::size_t candidateIndex, const Settings& settings,
                              const std::optional<GarbageRise>& overrideGarbage,
                              std::string& error);
// Remove only the manual correction at `phase` and restore the native beam
// search up to the next immutable manual anchor.
bool restoreAutomaticFrom(RecoveryOutput& output, int player, std::size_t phase,
                          const Settings& settings, std::string& error);
// Atomically persist an editable session in the ordinary training/v2 format.
// It can be passed back to the EXE directly after a crash or restart and does
// not hash/copy the large source video on every edit.
bool writeRecoveryAutosave(const std::filesystem::path& input,
                           const std::filesystem::path& autosavePath,
                           const RecoveryOutput& output,
                           std::string& error);
// Persist the currently approved/corrected timeline as simulator shortcuts,
// JSON and an HTML review report.
bool writeRecoveredOutput(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                          const Settings& settings, RecoveryOutput& output, std::string& error);
// Write the same simulator shortcuts/links as an approved export without
// copying or hashing the source video. Batch analysis uses this immediately
// after its lightweight autosave so every detected match is ready to open.
bool writeRecoveredSimulatorLinks(const std::filesystem::path& input,
                                  const std::filesystem::path& outputDirectory,
                                  const Settings& settings,
                                  RecoveryOutput& output,
                                  std::string& error);
#ifdef TR_CONSUMER_OUTPUT
// Produce the same final replay links without saving observations or editor data.
bool prepareConsumerSimulatorLinks(const Settings& settings, RecoveryOutput& output,
                                   std::string& error);
#endif
// Compatibility entry point for non-interactive callers/tests: analyze then
// immediately write.  The desktop application uses analyzeVideo instead.
bool recoverVideo(const std::filesystem::path& input, const std::filesystem::path& outputDirectory, const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status, RecoveryOutput& output, std::string& error);

} // namespace tr
