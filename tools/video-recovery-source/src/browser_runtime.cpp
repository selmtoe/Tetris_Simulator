#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
#include "recovery.hpp"
#include "onnx_model.hpp"
#include "queue_onnx_model.hpp"
#include "scene_detector.hpp"
#include "vision.hpp"
#include "browser_ocr.hpp"
#include <emscripten.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>

// Asyncify suspends the calling C++ stack while the browser decodes or runs
// ONNX. The ordinary range analyzer, including its additional frame reads
// during placement-order repair, therefore runs without a second algorithm.
EM_ASYNC_JS(int, browser_model, (int kind, const float* input, int count, float* output), {
    try { await Module.browserIO.infer(kind, input, count, output); return 1; }
    catch (e) { Module.browserError = String(e.stack || e); return 0; }
});
EM_JS(int, browser_model_ready, (int kind), { return Module.browserIO.sessions[kind] ? 1 : 0; });
EM_JS(void, browser_error, (char* output, int capacity), {
    stringToUTF8(Module.browserError || 'Browser operation failed', output, capacity);
});
EM_ASYNC_JS(int, browser_open, (double* metadata), {
    try {
        const id = await Module.browserIO.open();
        const m = Module.browserIO.metadata;
        HEAPF64.set([m.width, m.height, m.duration, m.fps], metadata / 8);
        return id;
    } catch (e) { Module.browserError = String(e.stack || e); return 0; }
});
EM_ASYNC_JS(int, browser_seek, (int id, double time), {
    try { await Module.browserIO.readers.get(id).seek(time); return 1; }
    catch (e) { Module.browserError = String(e.stack || e); return 0; }
});
EM_ASYNC_JS(int, browser_next, (int id, double* time), {
    try {
        const frame = await Module.browserIO.readers.get(id).next();
        if (!frame) return 0;
        HEAPF64[time / 8] = Module.browserIO.readers.get(id).currentTimestamp100ns;
        return 1;
    } catch (e) { Module.browserError = String(e.stack || e); return -1; }
});
EM_ASYNC_JS(int, browser_pixels, (int id, unsigned char* destination, int bytes), {
    try { await Module.browserIO.readers.get(id).pixels(destination, bytes, Module); return 1; }
    catch (e) { Module.browserError = String(e.stack || e); return 0; }
});
EM_JS(void, browser_close, (int id), {
    Module.browserIO.readers.get(id)?.close(); Module.browserIO.readers.delete(id);
});
EM_JS(void, browser_mf_conversion, (int id, int enabled), {
    Module.browserIO.readers.get(id).legacyMfColor = !!enabled;
});

namespace {
#include "opening_guard.inc"
std::string lastError;
std::string getBrowserError() { char buffer[4096]; browser_error(buffer, sizeof(buffer)); return buffer; }
tr::Settings settings;
tr::Status status;
tr::RecoveryOutput recovery;
std::unique_ptr<tr::VideoRangeAnalyzer> analyzer;
DetectedRange browserRange;
StartDecision browserStartDecision;
std::string browserDecisionJson;
}
namespace tr {
struct VideoReader::Impl { int id = 0; };
VideoReader::VideoReader() = default;
VideoReader::~VideoReader() { close(); }
bool VideoReader::usesD3D11() const { return false; }
bool VideoReader::usesNvdecCuda() const { return false; }
bool VideoReader::open(const std::filesystem::path&, std::string& error, bool, bool, bool) {
    close(); double metadata[4]{}; const int id = browser_open(metadata);
    if (!id) { error = getBrowserError(); return false; }
    impl_ = std::make_unique<Impl>(); impl_->id = id;
    width_ = metadata[0]; height_ = metadata[1]; durationSeconds_ = metadata[2]; frameRate_ = metadata[3];
    return true;
}
bool VideoReader::seek(double time, std::string& error) {
    if (!impl_ || !browser_seek(impl_->id, time)) { error = getBrowserError(); return false; }
    return true;
}
void VideoReader::useBrowserMfConversion(bool enabled) {
    if (impl_) browser_mf_conversion(impl_->id, enabled ? 1 : 0);
}
bool VideoReader::readFiltered(Frame& frame, bool& eos, std::string& error,
                              const std::function<bool(std::int64_t)>& needPixels) {
    if (!impl_) { error = "Video reader is closed"; return false; }
    double time = 0; int result = browser_next(impl_->id, &time);
    if (result < 0) { error = getBrowserError(); return false; }
    eos = result == 0; frame.bgra.clear();
    frame.width = width_; frame.height = height_; frame.sourceX = frame.sourceY = 0;
    frame.time100ns = static_cast<std::int64_t>(std::llround(time));
    if (eos || !needPixels(frame.time100ns)) return true;
    frame.bgra.resize(static_cast<std::size_t>(width_) * height_ * 4);
    if (!browser_pixels(impl_->id, frame.bgra.data(), frame.bgra.size())) { error = getBrowserError(); return false; }
    return true;
}
bool VideoReader::read(Frame& frame, bool& eos, std::string& error) {
    return readFiltered(frame, eos, error, [](std::int64_t) { return true; });
}
bool VideoReader::readFilteredRegion(Frame& frame, bool& eos, std::string& error,
                                    const PixelRegion&, const std::function<bool(std::int64_t)>& filter) {
    return readFiltered(frame, eos, error, filter);
}
bool VideoReader::readSparseRegions(Frame& frame, bool& eos, std::string& error,
                                   const std::vector<PixelRegion>&) { return read(frame, eos, error); }
void VideoReader::setGpuConversionRegions(const std::vector<PixelRegion>&) {}
void VideoReader::close() {
    if (impl_) browser_close(impl_->id);
    impl_.reset(); width_ = height_ = 0; durationSeconds_ = frameRate_ = 0;
}

OnnxBoardModel::OnnxBoardModel(const std::filesystem::path&, std::string& error) {
    ready_ = browser_model_ready(0); if (!ready_) error = "Browser board model is not ready";
}
BoardProbabilities OnnxBoardModel::inferProbabilities(const std::vector<float>& input, std::string& error) const {
    if (input.size() != 200 * 3 * 32 * 32) { error = "Invalid board tensor"; return {}; }
    std::array<float, 200 * 9> logits{};
    if (!browser_model(0, input.data(), 200, logits.data())) { error = getBrowserError(); return {}; }
    constexpr Cell mapping[] = {Cell::Empty, Cell::Garbage, Cell::S, Cell::Z, Cell::L, Cell::J, Cell::O, Cell::I, Cell::T};
    BoardProbabilities result{};
    for (int cell = 0; cell < 200; ++cell) {
        const float* row = logits.data() + cell * 9;
        const double maximum = *std::max_element(row, row + 9);
        double total = 0; std::array<double, 9> weights{};
        for (int c = 0; c < 9; ++c) { weights[c] = std::exp(static_cast<double>(row[c]) - maximum); total += weights[c]; }
        if (!(total > 0) || !std::isfinite(total)) { error = "Non-finite board output"; return {}; }
        for (int c = 0; c < 9; ++c) result[probabilityIndex(cell, mapping[c])] = static_cast<float>(weights[c] / total);
    }
    return result;
}
std::vector<Cell> OnnxBoardModel::infer(const std::vector<float>& input, std::string& error) const {
    auto probabilities = inferProbabilities(input, error); std::vector<Cell> result(200);
    for (int i = 0; i < 200; ++i) {
        auto row = probabilities.begin() + i * 9;
        result[i] = static_cast<Cell>(std::max_element(row, row + 9) - row);
    }
    return result;
}
OnnxQueueModel::OnnxQueueModel(const std::filesystem::path&, std::string& error) {
    ready_ = browser_model_ready(1); if (!ready_) error = "Browser queue model is not ready";
}
std::vector<QueueCnnPrediction> OnnxQueueModel::infer(const std::vector<float>& input, std::size_t batch, std::string& error) const {
    if (batch == 0 || input.size() != batch * 3 * 48 * 48) { error = "Invalid queue tensor"; return {}; }
    std::vector<float> output(batch * 9);
    if (!browser_model(1, input.data(), batch, output.data())) { error = getBrowserError(); return {}; }
    constexpr Cell mapping[] = {Cell::Empty, Cell::I, Cell::O, Cell::T, Cell::L, Cell::J, Cell::S, Cell::Z};
    std::vector<QueueCnnPrediction> result(batch);
    for (std::size_t i = 0; i < batch; ++i) {
        const float* row = output.data() + i * 8;
        int best = std::max_element(row, row + 8) - row; float maximum = row[best]; double denominator = 0;
        for (int c = 0; c < 8; ++c) denominator += std::exp(row[c] - maximum);
        const double unclear = std::clamp(static_cast<double>(output[batch * 8 + i]), -80.0, 80.0);
        if (!std::isfinite(denominator) || !std::isfinite(unclear)) { error = "Non-finite queue output"; return {}; }
        result[i] = {mapping[best], static_cast<float>(1.0 / denominator), static_cast<float>(1.0 / (1.0 + std::exp(-unclear)))};
    }
    return result;
}
}

namespace {
// Generated from the native scene source at build time; no rewritten rules.
#include "scene_rules.inc"
std::vector<Prediction> scenePredictions;
std::string sceneJson;
}
extern "C" {
EMSCRIPTEN_KEEPALIVE const char* br_error() { return lastError.c_str(); }
EMSCRIPTEN_KEEPALIVE int br_progress() { return status.progress.load(); }
EMSCRIPTEN_KEEPALIVE const char* br_message() { return status.message.c_str(); }
EMSCRIPTEN_KEEPALIVE int br_init(const char* inputName) {
    lastError.clear(); analyzer.reset();
    if (!tr::loadSettings("/config/tetris_recover.ini", settings, lastError)) return 0;
    settings.onnxSamples = 3;
    settings.beamWidth = 32;
    settings.forceEmptyOpening = true;
    analyzer = std::make_unique<tr::VideoRangeAnalyzer>(inputName, "/models/tetris.onnx", settings);
    return 1;
}
EMSCRIPTEN_KEEPALIVE void br_range_context(double fps, double legacy, double goExit, double goDuration) {
    browserRange = {};
    browserRange.index = 1; browserRange.fps = fps;
    browserRange.legacyStartSeconds = legacy;
    browserRange.legacyStartFrame = static_cast<std::int64_t>(std::llround(legacy * fps));
    browserRange.goExitSeconds = goExit; browserRange.goDurationSeconds = goDuration;
    browserRange.goExitFrame = goExit < 0 ? -1 : static_cast<std::int64_t>(std::llround(goExit * fps));
}
EMSCRIPTEN_KEEPALIVE const char* br_start_decision() { return browserDecisionJson.c_str(); }
EMSCRIPTEN_KEEPALIVE void br_diagnostics(int enabled) {
    if (enabled) setenv("TR_BROWSER_PARITY_EXPORT", "/output/parity-boards.json", 1);
    else unsetenv("TR_BROWSER_PARITY_EXPORT");
}
EMSCRIPTEN_KEEPALIVE int br_analyze(double start, double end, const char* inputName) {
    lastError.clear();
    if (!analyzer) { lastError = "Analyzer is not initialized"; return 0; }
    if (!analyzer->analyze(start, end, status, recovery, lastError)) return 0;
    browserRange.startSeconds = start; browserRange.endSeconds = end;
    browserRange.startFrame = static_cast<std::int64_t>(std::llround(start * browserRange.fps));
    browserStartDecision = {};
    browserStartDecision.initialStartSeconds = browserStartDecision.finalStartSeconds = start;
    browserStartDecision.initialRule = browserStartDecision.finalRule = usableGoExit(browserRange) ? "adaptive-go-exit" : "legacy-analysis-start";
    browserStartDecision.initialState = browserStartDecision.finalState = inspectOpeningState(recovery, settings);
    if (!applyBrowserOpeningGuard(*analyzer, browserRange, browserStartDecision, settings, status, recovery, lastError)) return 0;
    {
        const auto& d = browserStartDecision;
        std::ostringstream out; out.precision(12);
        out << "{\"initialStartSeconds\":" << d.initialStartSeconds << ",\"finalStartSeconds\":" << d.finalStartSeconds
            << ",\"initialRule\":\"" << d.initialRule << "\",\"finalRule\":\"" << d.finalRule
            << "\",\"guardSearched\":" << (d.guardSearched ? "true" : "false")
            << ",\"guardReanalyzed\":" << (d.guardReanalyzed ? "true" : "false")
            << ",\"guardFallback\":" << (d.guardFallback ? "true" : "false")
            << ",\"guardTargetSeconds\":" << d.guardTargetSeconds << "}";
        browserDecisionJson = out.str();
    }
    status.setMessage("Reading player names");
    tr::browserRecognizeNames(inputName, browserRange.startSeconds, settings, recovery);
    if (!tr::writeRecoveryAutosave(inputName, "/output/autosave.json", recovery, lastError)) return 0;
    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    // An editable incomplete match is a normal outcome, not an inference
    // failure. Preserve it and continue with the next match like the PC app.
    if (tr::firstRecoveryUnresolved(recovery, unresolvedPlayer, unresolvedPhase)) {
        lastError.clear();
        return 2;
    }
    return tr::writeRecoveredSimulatorLinks(inputName, "/output", settings, recovery, lastError) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void br_scene_reset() { scenePredictions.clear(); }
EMSCRIPTEN_KEEPALIVE void br_scene_logits(const float* logits, int count) {
    for (int i = 0; i < count; ++i) scenePredictions.push_back(classify(logits + i * 8));
}
EMSCRIPTEN_KEEPALIVE void br_scene_frame(const float* logits, int index) {
    if (index < 0) return;
    if (static_cast<std::size_t>(index) >= scenePredictions.size()) scenePredictions.resize(index + 1);
    scenePredictions[index] = classify(logits);
}
EMSCRIPTEN_KEEPALIVE const char* br_scene_finish(double fps, double minimum) {
    std::vector<tr::SceneMatchRange> matches;
    assembleMatches(scenePredictions, fps, std::max(1, static_cast<int>(std::ceil(fps * minimum))), matches);
    std::ostringstream out; out.precision(12); out << "[";
    for (std::size_t i = 0; i < matches.size(); ++i) {
        const auto& m = matches[i]; if (i) out << ',';
        out << "{\"index\":" << m.index << ",\"start_seconds\":" << m.startSeconds
            << ",\"end_seconds\":" << m.endSeconds << ",\"start_frame\":" << m.startFrame
            << ",\"end_frame\":" << m.endFrame << ",\"start_rule\":\"" << m.startRule
            << "\",\"end_rule\":\"" << m.endRule << "\"";
        const double goExit = static_cast<double>(m.goLastFrame + 1) / fps;
        const double goDuration = static_cast<double>(m.goLastFrame - m.goFirstFrame + 1) / fps;
        // Same default start policy as range_batch_main.cpp::usableGoExit.
        const bool usableGo = m.goLastFrame >= 0 && goExit >= 0 &&
            goDuration + .000001 >= .25 && goExit <= m.startSeconds + .000001;
        out << ",\"analysis_start_frame\":" << (usableGo ? m.goLastFrame + 1 : m.startFrame)
            << ",\"analysis_start_seconds\":" << (usableGo ? goExit : m.startSeconds)
            << ",\"analysis_start_rule\":\"" << (usableGo ? "adaptive-go-exit" : "legacy-analysis-start") << "\""
            << ",\"legacy_start_seconds\":" << m.startSeconds
            << ",\"go_exit_seconds\":" << goExit << ",\"go_duration_seconds\":" << goDuration << "}";
    }
    out << ']'; sceneJson = out.str(); return sceneJson.c_str();
}
EMSCRIPTEN_KEEPALIVE void br_luma(const unsigned char* source, int stride, int width, int height, int fullRange, float* output) {
    const int unit = std::min(width / 16, height / 9), cw = std::max(1, unit * 16), ch = std::max(1, unit * 9);
    const int cx = std::max(0, (width - cw) / 2), cy = std::max(0, (height - ch) / 2);
    for (int oy = 0; oy < 90; ++oy) for (int ox = 0; ox < 160; ++ox) {
        const int x0 = std::clamp(cx + ox * cw / 160, 0, width - 1);
        const int x1 = std::clamp(cx + (ox + 1) * cw / 160, x0 + 1, width);
        const int y0 = std::clamp(cy + oy * ch / 90, 0, height - 1);
        const int y1 = std::clamp(cy + (oy + 1) * ch / 90, y0 + 1, height);
        unsigned int sum = 0; const int count = (x1 - x0) * (y1 - y0);
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) sum += source[y * stride + x];
        float value = static_cast<float>((sum + count / 2) / count);
        if (!fullRange) value = (value - 16.0f) * (255.0f / 219.0f);
        value = std::floor(std::clamp(value, 0.0f, 255.0f) + 0.5f);
        output[oy * 160 + ox] = value * (1.0f / 127.5f) - 1.0f;
    }
}
EMSCRIPTEN_KEEPALIVE void br_yuv(const unsigned char* yp, const unsigned char* up, const unsigned char* vp,
        int ys, int us, int vs, int interleaved, int width, int height, int matrix, int fullRange, unsigned char* output,
        const int* regions, int regionCount, int legacyMf = 0) {
    const auto quantize = [](float value) { return static_cast<unsigned char>(std::floor(std::clamp(value, 0.0f, 255.0f) + 0.5f)); };
    for (int region = 0; region < regionCount; ++region) {
    const int* bounds = regions + region * 4;
    const int x0 = std::clamp(bounds[0], 0, width), y0 = std::clamp(bounds[1], 0, height);
    const int x1 = std::clamp(bounds[0] + bounds[2], x0, width), y1 = std::clamp(bounds[1] + bounds[3], y0, height);
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        float yy = yp[y * ys + x];
        const int cx = interleaved ? (x & ~1) : x / 2;
        const float u = up[(y / 2) * us + cx] - 128.0f;
        const float v = vp[(y / 2) * vs + cx] - 128.0f;
        if (legacyMf) {
            // Windows' advanced RGB32 reader linearly reconstructs chroma
            // at half-pixel centres and expands limited-range chroma. The
            // main CUDA reader instead uses nearest chroma and its existing
            // coefficients. Keep these two established input paths distinct.
            const float sx = std::clamp(x * .5f - .25f, 0.0f, (width - 1) / 2.0f);
            const float sy = std::clamp(y * .5f - .25f, 0.0f, (height - 1) / 2.0f);
            const int ax = std::min(static_cast<int>(sx), (width - 1) / 2);
            const int ay = std::min(static_cast<int>(sy), (height - 1) / 2);
            const int bx = std::min(ax + 1, (width - 1) / 2), by = std::min(ay + 1, (height - 1) / 2);
            const float fx = sx - ax, fy = sy - ay;
            const auto chroma = [&](const unsigned char* p, int stride) {
                const int step = interleaved ? 2 : 1;
                const float top = p[ay * stride + ax * step] * (1 - fx) + p[ay * stride + bx * step] * fx;
                const float bottom = p[by * stride + ax * step] * (1 - fx) + p[by * stride + bx * step] * fx;
                return top * (1 - fy) + bottom * fy - 128.0f;
            };
            const float uu = chroma(up, us), vv = chroma(vp, vs);
            const float luma = fullRange ? yy : (yy - 16.0f) * (255.0f / 219.0f);
            const float scale = fullRange ? 1.0f : 255.0f / 224.0f;
            const float rr = luma + (matrix == 1 ? 1.5748f : 1.402f) * scale * vv;
            const float gg = luma - (matrix == 1 ? .187324f : .344136f) * scale * uu - (matrix == 1 ? .468124f : .714136f) * scale * vv;
            const float bb = luma + (matrix == 1 ? 1.8556f : 1.772f) * scale * uu;
            const int i = (y * width + x) * 4;
            output[i] = quantize(bb); output[i + 1] = quantize(gg); output[i + 2] = quantize(rr); output[i + 3] = 255;
            continue;
        }
        const float c = fullRange ? yy : std::max(0.0f, yy - 16.0f) * (255.0f / 219.0f);
        float r, g, b;
        // For these bounded 8-bit inputs, a double holds the exact product
        // of a float coefficient and integer chroma plus float luma. Casting
        // once reproduces the native kernel's fused float multiply-add.
        const auto fused = [](float a, float b, float c) {
            return static_cast<float>(static_cast<double>(a) * b + c);
        };
        if (matrix == 1) { r = c + 1.5748f * v; g = fused(-0.468124f, v, fused(-0.187324f, u, c)); b = c + 1.8556f * u; }
        else { r = c + 1.402f * v; g = fused(-0.714136f, v, fused(-0.344136f, u, c)); b = c + 1.772f * u; }
        const int i = (y * width + x) * 4;
        output[i] = quantize(b); output[i + 1] = quantize(g); output[i + 2] = quantize(r); output[i + 3] = 255;
    }
    }
}
EMSCRIPTEN_KEEPALIVE int br_regions(int width, int height, int* result, int capacity) {
    std::vector<tr::PixelRegion> regions;
    std::string modelError;
    tr::OnnxBoardModel geometry("/models/tetris.onnx", modelError);
    for (const auto& layout : {settings.p1, settings.p2}) {
        tr::VisionAnalyzer vision(width, height, layout, &geometry);
        regions.push_back(vision.boardPixelRegion());
        const auto queue = vision.queuePixelRegions(); regions.insert(regions.end(), queue.begin(), queue.end());
    }
    const auto names = tr::browserNameRegions(width, height, settings);
    regions.insert(regions.end(), names.begin(), names.end());
    if (capacity < static_cast<int>(regions.size())) return -1;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].width <= 0 || regions[i].height <= 0) return -1;
        result[i * 4] = regions[i].x; result[i * 4 + 1] = regions[i].y;
        result[i * 4 + 2] = regions[i].width; result[i * 4 + 3] = regions[i].height;
    }
    return regions.size();
}
}
#endif
