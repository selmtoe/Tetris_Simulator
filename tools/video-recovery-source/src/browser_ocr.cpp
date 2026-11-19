#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
#include "browser_ocr.hpp"
#include <emscripten.h>
#include <algorithm>
#include <cmath>

EM_ASYNC_JS(int, browser_ocr_infer, (const float* input, int count, int width,
        char* text, int capacity, double* confidence, char* error), {
    try {
        const result = await Module.browserIO.recognizeName(HEAPF32.slice(input / 4, input / 4 + count), width);
        stringToUTF8(result.text, text, capacity); HEAPF64[confidence / 8] = result.confidence;
        return 1;
    } catch (e) { stringToUTF8(String(e.message || e), error, capacity); return 0; }
});

namespace tr {
namespace {
// Generated directly from player_name_ocr.cpp's crop geometry and model
// input construction. Keep the Windows image preparation authoritative.
#include "ocr_pixels.inc"
}
std::vector<PixelRegion> browserNameRegions(int width, int height, const Settings& settings) {
    std::vector<PixelRegion> result;
    if (!settings.playerNameOcr.enabled) return result;
    for (const auto& rect : {settings.p1.name, settings.p2.name}) {
        auto crop = mapNameRegion(width, height, rect, settings.playerNameOcr);
        if (crop.width > 0 && crop.height > 0) result.push_back(crop);
    }
    return result;
}
void browserRecognizeNames(const std::filesystem::path& input, double seconds,
                          const Settings& settings, RecoveryOutput& output) {
    output.p1PlayerName = {}; output.p2PlayerName = {};
    if (!settings.playerNameOcr.enabled) return;
    VideoReader reader; Frame frame; std::string error; bool eos = false, found = false;
    if (reader.open(input, error) && reader.seek(seconds, error)) {
        const int budget = std::max(240, static_cast<int>(std::ceil(std::max(1.0, reader.frameRate()) * 12)));
        for (int i = 0; i < budget && !eos; ++i) {
            if (!reader.read(frame, eos, error)) break;
            if (!eos && !frame.bgra.empty() && frame.time100ns / 1e7 + .000001 >= seconds) { found = true; break; }
        }
    }
    for (int side = 0; side < 2; ++side) {
        const auto& rect = side ? settings.p2.name : settings.p1.name;
        auto& result = side ? output.p2PlayerName : output.p1PlayerName;
        if (rect.w <= 0 || rect.h <= 0) continue;
        result.attempted = true; result.crop = rect;
        if (!found) { result.error = error.empty() ? "No decoded frame for player name" : error; continue; }
        const auto crop = mapNameRegion(reader.width(), reader.height(), rect, settings.playerNameOcr);
        int width = 0; auto tensor = prepareNameInput(frame, crop, width);
        char text[8192]{}, failure[8192]{}; double confidence = 0;
        if (browser_ocr_infer(tensor.data(), tensor.size(), width, text, sizeof(text), &confidence, failure)) {
            result.text = text; result.confidence = confidence; result.engine = "paddleocr-pp-ocrv6-onnx";
        } else { result.error = failure; }
    }
}
}
#endif
