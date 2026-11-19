#include "recovery.hpp"

#include <onnxruntime_cxx_api.h>

#ifndef __EMSCRIPTEN__

#include <windows.h>

#if defined(TR_NVDEC_DEFAULT)
#include <cuda_runtime_api.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>

namespace tr {
namespace {

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

PixelRegion mapNameRegion(int width, int height, const LayoutRect& rectangle,
                          const PlayerNameOcrSettings& ocr) {
    if (rectangle.w <= 0 || rectangle.h <= 0 ||
        ocr.referenceWidth <= 0 || ocr.referenceHeight <= 0) return {};
    const double referenceAspect = ocr.referenceWidth / ocr.referenceHeight;
    const double sourceAspect = static_cast<double>(width) / std::max(1, height);
    double contentWidth = width;
    double contentHeight = height;
    double offsetX = 0;
    double offsetY = 0;
    if (sourceAspect > referenceAspect) {
        contentWidth = height * referenceAspect;
        offsetX = (width - contentWidth) * .5;
    } else {
        contentHeight = width / referenceAspect;
        offsetY = (height - contentHeight) * .5;
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(
        offsetX + rectangle.x * contentWidth / ocr.referenceWidth)), 0, width);
    const int y0 = std::clamp(static_cast<int>(std::floor(
        offsetY + rectangle.y * contentHeight / ocr.referenceHeight)), 0, height);
    const int x1 = std::clamp(static_cast<int>(std::ceil(
        offsetX + (rectangle.x + rectangle.w) * contentWidth /
            ocr.referenceWidth)), 0, width);
    const int y1 = std::clamp(static_cast<int>(std::ceil(
        offsetY + (rectangle.y + rectangle.h) * contentHeight /
            ocr.referenceHeight)), 0, height);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

void trim(std::string& value);

class PaddleNameRecognizer {
public:
    PaddleNameRecognizer(const std::filesystem::path& modelPath,
                         const std::filesystem::path& dictionaryPath,
                         std::string& error)
        : environment_(ORT_LOGGING_LEVEL_WARNING, "tetris-player-name-ocr"),
          session_(nullptr) {
        try {
            std::ifstream dictionary(dictionaryPath, std::ios::binary);
            if (!dictionary) {
                error = "Could not open the player-name OCR dictionary";
                return;
            }
            characters_.push_back("blank");
            std::string line;
            while (std::getline(dictionary, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                characters_.push_back(line);
            }
            // RapidOCR/PaddleOCR appends a printable space after the model's
            // embedded dictionary and reserves class zero for the CTC blank.
            characters_.push_back(" ");
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(
                GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(1);
            options.SetInterOpNumThreads(1);
            session_ = Ort::Session(environment_, modelPath.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            inputName_ = session_.GetInputNameAllocated(0, allocator).get();
            outputName_ = session_.GetOutputNameAllocated(0, allocator).get();
            ready_ = true;
            error.clear();
        } catch (const Ort::Exception& exception) {
            error = std::string("Could not initialize player-name ONNX OCR: ") +
                exception.what();
        }
    }

    bool ready() const { return ready_; }

    bool recognize(const Frame& frame, const PixelRegion& crop,
                   PlayerNameRecognition& result,
                   std::string& error) {
        if (!ready_ || crop.width <= 0 || crop.height <= 0) {
            error = "Player-name ONNX OCR is not ready";
            return false;
        }
        try {
            constexpr int InputHeight = 48;
            const double ratio = static_cast<double>(crop.width) / crop.height;
            // Match PaddleOCR exactly: the dynamic canvas width is truncated,
            // while the resized content width is rounded up and then clipped
            // to that canvas. A one-pixel difference changes the recognizer's
            // sequence length and materially hurts very short handles.
            const int inputWidth = std::max(
                320, static_cast<int>(InputHeight * ratio));
            const int resizedWidth = std::clamp(
                static_cast<int>(std::ceil(InputHeight * ratio)),
                1, inputWidth);
            const std::size_t plane = static_cast<std::size_t>(InputHeight) *
                inputWidth;
            // PaddleOCR pads with normalized zero (mid-gray in source RGB).
            std::vector<float> input(plane * 3, 0.0f);
            const auto luminance = [&](int sourceX, int sourceY) -> float {
                const int localX = sourceX - frame.sourceX;
                const int localY = sourceY - frame.sourceY;
                if (localX < 0 || localY < 0 || localX >= frame.width ||
                    localY >= frame.height) return 127.5f;
                const std::size_t offset =
                    (static_cast<std::size_t>(localY) * frame.width + localX) * 4;
                const float blue = frame.bgra[offset];
                const float green = frame.bgra[offset + 1];
                const float red = frame.bgra[offset + 2];
                return (29.0f * blue + 150.0f * green + 77.0f * red) /
                    256.0f;
            };
            for (int y = 0; y < InputHeight; ++y) {
                const double sourceY = std::clamp(
                    crop.y + (y + .5) * crop.height / InputHeight - .5,
                    static_cast<double>(crop.y),
                    static_cast<double>(crop.y + crop.height - 1));
                const int y0 = static_cast<int>(std::floor(sourceY));
                const int y1 = std::min(crop.y + crop.height - 1, y0 + 1);
                const double fy = sourceY - y0;
                for (int x = 0; x < resizedWidth; ++x) {
                    const double sourceX = std::clamp(
                        crop.x + (x + .5) * crop.width / resizedWidth - .5,
                        static_cast<double>(crop.x),
                        static_cast<double>(crop.x + crop.width - 1));
                    const int x0 = static_cast<int>(std::floor(sourceX));
                    const int x1 = std::min(crop.x + crop.width - 1, x0 + 1);
                    const double fx = sourceX - x0;
                    const std::size_t destination =
                        static_cast<std::size_t>(y) * inputWidth + x;
                    const double top = luminance(x0, y0) * (1.0 - fx) +
                        luminance(x1, y0) * fx;
                    const double bottom = luminance(x0, y1) * (1.0 - fx) +
                        luminance(x1, y1) * fx;
                    const double value = top * (1.0 - fy) + bottom * fy;
                    const float normalized =
                        static_cast<float>(value / 127.5 - 1.0);
                    for (int component = 0; component < 3; ++component) {
                        input[static_cast<std::size_t>(component) * plane +
                              destination] = normalized;
                    }
                }
            }

            const std::array<std::int64_t, 4> dimensions{
                1, 3, InputHeight, inputWidth};
            auto memory = Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault);
            auto tensor = Ort::Value::CreateTensor<float>(
                memory, input.data(), input.size(), dimensions.data(),
                dimensions.size());
            const char* inputNames[]{inputName_.c_str()};
            const char* outputNames[]{outputName_.c_str()};
            auto outputs = session_.Run(
                Ort::RunOptions{nullptr}, inputNames, &tensor, 1,
                outputNames, 1);
            if (outputs.empty() || !outputs.front().IsTensor()) {
                error = "Player-name ONNX OCR returned no tensor";
                return false;
            }
            const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 ||
                shape[2] != static_cast<std::int64_t>(characters_.size())) {
                error = "Player-name ONNX OCR returned an unexpected shape";
                return false;
            }
            const float* probabilities = outputs.front().GetTensorData<float>();
            const std::size_t timeSteps = static_cast<std::size_t>(shape[1]);
            const std::size_t classes = static_cast<std::size_t>(shape[2]);
            int previous = -1;
            double confidence = 0;
            int emitted = 0;
            result.text.clear();
            for (std::size_t step = 0; step < timeSteps; ++step) {
                const float* row = probabilities + step * classes;
                std::size_t winner = 0;
                for (std::size_t candidate = 1; candidate < classes; ++candidate) {
                    if (row[candidate] > row[winner]) winner = candidate;
                }
                const int current = static_cast<int>(winner);
                const bool duplicate = current == previous;
                previous = current;
                if (current == 0 || duplicate) continue;
                result.text += characters_[winner];
                confidence += row[winner];
                ++emitted;
            }
            trim(result.text);
            if (result.text.empty()) {
                error = "Player-name ONNX OCR recognized no text";
                return false;
            }
            result.confidence = emitted > 0 ? confidence / emitted : 0;
            result.engine = "paddleocr-pp-ocrv6-onnx";
            result.error.clear();
            error.clear();
            return true;
        } catch (const Ort::Exception& exception) {
            error = std::string("Player-name ONNX OCR failed: ") +
                exception.what();
            return false;
        }
    }

private:
    Ort::Env environment_;
    Ort::Session session_;
    std::vector<std::string> characters_;
    std::string inputName_;
    std::string outputName_;
    bool ready_ = false;
};

std::shared_ptr<PaddleNameRecognizer> paddleRecognizer(
        const PlayerNameOcrSettings& settings, std::string& error) {
    static std::mutex mutex;
    static std::shared_ptr<PaddleNameRecognizer> cached;
    static std::filesystem::path cachedModel;
    static std::filesystem::path cachedDictionary;
    std::lock_guard<std::mutex> lock(mutex);
    if (cachedModel == settings.onnxModelPath &&
        cachedDictionary == settings.dictionaryPath) {
        if (cached) return cached;
    }
    auto model = std::make_shared<PaddleNameRecognizer>(
        settings.onnxModelPath, settings.dictionaryPath, error);
    if (!model->ready()) return {};
    cachedModel = settings.onnxModelPath;
    cachedDictionary = settings.dictionaryPath;
    cached = std::move(model);
    return cached;
}

bool writeCropPgm(const Frame& frame, const PixelRegion& crop, int upscale,
                  const std::filesystem::path& destination,
                  std::string& error) {
    if (crop.width <= 0 || crop.height <= 0 || frame.bgra.empty()) {
        error = "Player-name crop is empty";
        return false;
    }
    const int scale = std::clamp(upscale, 1, 6);
    const int outputWidth = crop.width * scale;
    const int outputHeight = crop.height * scale;
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not create the temporary player-name crop";
        return false;
    }
    file << "P5\n" << outputWidth << ' ' << outputHeight << "\n255\n";
    std::vector<unsigned char> row(static_cast<std::size_t>(outputWidth));
    for (int y = 0; y < outputHeight; ++y) {
        const int sourceY = crop.y + y / scale - frame.sourceY;
        for (int x = 0; x < outputWidth; ++x) {
            const int sourceX = crop.x + x / scale - frame.sourceX;
            if (sourceX < 0 || sourceY < 0 || sourceX >= frame.width ||
                sourceY >= frame.height) {
                row[static_cast<std::size_t>(x)] = 128;
                continue;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(sourceY) * frame.width + sourceX) * 4;
            const int blue = frame.bgra[offset];
            const int green = frame.bgra[offset + 1];
            const int red = frame.bgra[offset + 2];
            row[static_cast<std::size_t>(x)] = static_cast<unsigned char>(
                std::clamp((29 * blue + 150 * green + 77 * red + 128) >> 8,
                           0, 255));
        }
        file.write(reinterpret_cast<const char*>(row.data()),
                   static_cast<std::streamsize>(row.size()));
    }
    if (!file) {
        error = "Could not finish the temporary player-name crop";
        return false;
    }
    return true;
}

bool runTesseract(const Settings& settings,
                  const std::filesystem::path& image,
                  const std::filesystem::path& outputBase,
                  std::string& error) {
    const auto& executable = settings.playerNameOcr.executablePath;
    std::wstring command = quoteArgument(executable.wstring()) + L" " +
        quoteArgument(image.wstring()) + L" " + quoteArgument(outputBase.wstring());
    if (!settings.playerNameOcr.tessdataPath.empty()) {
        command += L" --tessdata-dir " +
            quoteArgument(settings.playerNameOcr.tessdataPath.wstring());
    }
    const std::wstring languages(
        settings.playerNameOcr.languages.begin(),
        settings.playerNameOcr.languages.end());
    command += L" -l " + quoteArgument(languages) +
        L" --oem 1 --psm 7 tsv";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                        &startup, &process)) {
        error = "Could not start Tesseract (Windows error " +
            std::to_string(GetLastError()) + ")";
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 124);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait == WAIT_TIMEOUT) {
        error = "Tesseract timed out";
        return false;
    }
    if (wait != WAIT_OBJECT_0 || exitCode != 0) {
        error = "Tesseract failed with exit code " + std::to_string(exitCode);
        return false;
    }
    return true;
}

void trim(std::string& value) {
    while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20)
        value.erase(value.begin());
    while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20)
        value.pop_back();
}

bool parseTsv(const std::filesystem::path& path,
              PlayerNameRecognition& result) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "Tesseract did not produce TSV output";
        return false;
    }
    std::string line;
    double confidenceSum = 0;
    int confidenceCount = 0;
    bool first = true;
    while (std::getline(file, line)) {
        if (first) { first = false; continue; }
        std::array<std::string, 12> fields;
        std::size_t begin = 0;
        bool complete = true;
        for (std::size_t field = 0; field < 11; ++field) {
            const std::size_t tab = line.find('\t', begin);
            if (tab == std::string::npos) { complete = false; break; }
            fields[field] = line.substr(begin, tab - begin);
            begin = tab + 1;
        }
        if (!complete) continue;
        fields[11] = line.substr(begin);
        trim(fields[11]);
        if (fields[0] != "5" || fields[11].empty()) continue;
        try {
            const double confidence = std::stod(fields[10]);
            if (confidence >= 0) {
                confidenceSum += confidence;
                ++confidenceCount;
            }
        } catch (...) {}
        // Player handles do not contain semantic whitespace. Concatenating
        // OCR word boxes is safer for Japanese glyphs that Tesseract split.
        result.text += fields[11];
    }
    trim(result.text);
    result.confidence = confidenceCount > 0
        ? confidenceSum / (confidenceCount * 100.0) : 0;
    if (result.text.empty()) {
        result.error = "No player name was recognized";
        return false;
    }
    result.error.clear();
    return true;
}

void recognizeCrop(const Frame& frame, int sourceWidth, int sourceHeight,
                   const LayoutRect& rectangle,
                   const Settings& settings,
                   const std::filesystem::path& temporaryDirectory,
                   const wchar_t* stem,
                   PlayerNameRecognition& result) {
    result = {};
    if (rectangle.w <= 0 || rectangle.h <= 0) return;
    result.attempted = true;
    result.crop = rectangle;
    const PixelRegion crop = mapNameRegion(
        sourceWidth, sourceHeight, rectangle, settings.playerNameOcr);
    if (std::getenv("TR_PLAYER_NAME_OCR_DIAGNOSTICS")) {
        std::string ignored;
        writeCropPgm(frame, crop, 1,
                     temporaryDirectory /
                         (std::wstring(stem) + L"_native_crop.pgm"),
                     ignored);
    }
    std::string onnxError;
    if (settings.playerNameOcr.preferOnnx &&
        std::filesystem::is_regular_file(settings.playerNameOcr.onnxModelPath) &&
        std::filesystem::is_regular_file(settings.playerNameOcr.dictionaryPath)) {
        auto model = paddleRecognizer(settings.playerNameOcr, onnxError);
        if (model && model->recognize(frame, crop, result, onnxError)) return;
    }
    result.engine = "tesseract-5";
    if (!std::filesystem::is_regular_file(
            settings.playerNameOcr.executablePath)) {
        result.error = onnxError.empty()
            ? "No usable player-name OCR engine was found" : onnxError;
        return;
    }
    const auto image = temporaryDirectory / (std::wstring(stem) + L".pgm");
    const auto outputBase = temporaryDirectory / (std::wstring(stem) + L"_ocr");
    if (!writeCropPgm(frame, crop, settings.playerNameOcr.upscale,
                      image, result.error)) return;
    if (!runTesseract(settings, image, outputBase, result.error)) return;
    parseTsv(outputBase.wstring() + L".tsv", result);
}

void recognizePlayerNamesInFrame(const Frame& frame,
                                 int sourceWidth, int sourceHeight,
                                 const Settings& settings,
                                 RecoveryOutput& output,
                                 std::string& warning) {
    static std::atomic<unsigned long long> serial{0};
    const auto id = std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed));
    const auto temporaryDirectory = std::filesystem::temp_directory_path() /
        (L"tetris_player_ocr_" + id);
    std::error_code filesystemError;
    std::filesystem::create_directories(temporaryDirectory, filesystemError);
    if (filesystemError) {
        warning = "Could not create the player-name OCR temporary directory";
        return;
    }
    recognizeCrop(frame, sourceWidth, sourceHeight, settings.p1.name, settings,
                  temporaryDirectory, L"p1", output.p1PlayerName);
    recognizeCrop(frame, sourceWidth, sourceHeight, settings.p2.name, settings,
                  temporaryDirectory, L"p2", output.p2PlayerName);
    if (!std::getenv("TR_PLAYER_NAME_OCR_DIAGNOSTICS")) {
        std::filesystem::remove_all(temporaryDirectory, filesystemError);
    } else {
        warning = "Player-name OCR diagnostics: " +
            temporaryDirectory.u8string();
    }
    if (!output.p1PlayerName.error.empty() ||
        !output.p2PlayerName.error.empty()) {
        warning = "Player-name OCR was incomplete";
    }
}

} // namespace

bool recognizePlayerNames(const std::filesystem::path& input,
                          double sourceSeconds,
                          const Settings& settings,
                          RecoveryOutput& output,
                          std::string& warning) {
    warning.clear();
    output.p1PlayerName = {};
    output.p2PlayerName = {};
    if (!settings.playerNameOcr.enabled ||
        (settings.p1.name.w <= 0 && settings.p2.name.w <= 0)) return true;
    const bool haveOnnx = settings.playerNameOcr.preferOnnx &&
        std::filesystem::is_regular_file(settings.playerNameOcr.onnxModelPath) &&
        std::filesystem::is_regular_file(settings.playerNameOcr.dictionaryPath);
    const bool haveTesseract = std::filesystem::is_regular_file(
        settings.playerNameOcr.executablePath);
    if (!haveOnnx && !haveTesseract) {
        warning = "Player-name OCR is configured, but no OCR engine is available";
        return true;
    }
    if (!haveOnnx && haveTesseract &&
        !settings.playerNameOcr.tessdataPath.empty() &&
        !std::filesystem::is_directory(settings.playerNameOcr.tessdataPath)) {
        warning = "Player-name OCR is configured, but tessdata is missing";
        return true;
    }

    Frame frame;
    bool found = false;
    int decodedWidth = 0;
    int decodedHeight = 0;
    std::string attemptErrors;
    // Media Foundation's legacy H.264 reader can successfully open some
    // long YouTube/OBS MP4s, but then return only null samples after a seek.
    // The main production binary already carries the native FFmpeg demuxer +
    // NVDEC backend used by stream analysis. Prefer that same in-process path
    // here (also covering AV1), and retain both MF modes as compatibility
    // fallbacks for codecs that NVDEC does not support. No external ffmpeg
    // process or temporary full-frame image is involved.
    struct DecodeAttempt {
        bool advanced = false;
        bool forceLegacy = false;
        bool preferNvdec = false;
        const char* name = nullptr;
    };
    const DecodeAttempt attempts[]{
        {false, false, true, "nvdec"},
        {true, false, false, "mf-advanced"},
        {false, true, false, "mf-legacy"},
    };
    for (const DecodeAttempt& attempt : attempts) {
        VideoReader reader;
        std::string currentError;
        if (!reader.open(input, currentError, attempt.advanced,
                         attempt.forceLegacy, attempt.preferNvdec)) {
            if (!attemptErrors.empty()) attemptErrors += "; ";
            attemptErrors += std::string(attempt.name) + ": " + currentError;
            continue;
        }
        decodedWidth = reader.width();
        decodedHeight = reader.height();
        const double target = std::clamp(
            sourceSeconds, 0.0,
            reader.durationSeconds() > .001
                ? reader.durationSeconds() - .001 : std::max(0.0, sourceSeconds));
        if (!reader.seek(target, currentError)) {
            if (!attemptErrors.empty()) attemptErrors += "; ";
            attemptErrors += std::string(attempt.name) + ": " + currentError;
            continue;
        }
        bool endOfStream = false;
        // YouTube AV1 keyframe intervals can be several seconds. Twelve
        // seconds is deliberately generous while still bounding corrupt
        // streams; ordinary matches stop on the first post-target frame.
        const int frameBudget = std::max(
            240, static_cast<int>(std::ceil(
                std::max(1.0, reader.frameRate()) * 12.0)));
        for (int decoded = 0; decoded < frameBudget && !endOfStream; ++decoded) {
            if (!reader.read(frame, endOfStream, currentError)) break;
            if (!frame.bgra.empty() &&
                static_cast<double>(frame.time100ns) / 10000000.0 + .001 >=
                    target) {
                found = true;
                break;
            }
        }
        if (found) break;
        if (currentError.empty()) {
            currentError = endOfStream
                ? "end of stream before the requested frame"
                : "no decoded frame reached the requested timestamp";
        }
        if (!attemptErrors.empty()) attemptErrors += "; ";
        attemptErrors += std::string(attempt.name) + ": " + currentError;
    }
    if (!found) {
        warning = "No decoded frame was available for player-name OCR";
        if (!attemptErrors.empty()) warning += ": " + attemptErrors;
        return true;
    }
    recognizePlayerNamesInFrame(frame, decodedWidth, decodedHeight, settings,
                                output, warning);
    return true;
}

bool recognizePlayerNamesFromFrame(const Frame& frame,
                                   int sourceWidth, int sourceHeight,
                                   const Settings& settings,
                                   RecoveryOutput& output,
                                   std::string& warning) {
    warning.clear();
    output.p1PlayerName = {};
    output.p2PlayerName = {};
    if (!settings.playerNameOcr.enabled ||
        (settings.p1.name.w <= 0 && settings.p2.name.w <= 0)) return true;
    const bool haveOnnx = settings.playerNameOcr.preferOnnx &&
        std::filesystem::is_regular_file(settings.playerNameOcr.onnxModelPath) &&
        std::filesystem::is_regular_file(settings.playerNameOcr.dictionaryPath);
    const bool haveTesseract = std::filesystem::is_regular_file(
        settings.playerNameOcr.executablePath);
    if (!haveOnnx && !haveTesseract) {
        warning = "Player-name OCR is configured, but no OCR engine is available";
        return true;
    }
    if (frame.bgra.empty() || sourceWidth <= 0 || sourceHeight <= 0) {
        warning = "The reused player-name frame is empty";
        return true;
    }
    recognizePlayerNamesInFrame(frame, sourceWidth, sourceHeight, settings,
                                output, warning);
    return true;
}

bool capturePlayerNameFrameFromNv12(
        const void* nv12, std::size_t pitch,
        int sourceWidth, int sourceHeight, int surfaceHeight,
        int matrix, bool fullRange, void* cudaStream,
        const Settings& settings, Frame& frame, std::string& error) {
#if defined(TR_NVDEC_DEFAULT)
    frame = {};
    error.clear();
    if (!nv12 || pitch == 0 || sourceWidth <= 0 || sourceHeight <= 0 ||
        surfaceHeight < sourceHeight) {
        error = "The reused NVDEC player-name surface is invalid";
        return false;
    }
    std::vector<PixelRegion> regions;
    const auto append = [&](const LayoutRect& rectangle) {
        const PixelRegion region = mapNameRegion(
            sourceWidth, sourceHeight, rectangle, settings.playerNameOcr);
        if (region.width > 0 && region.height > 0) regions.push_back(region);
    };
    append(settings.p1.name);
    append(settings.p2.name);
    if (regions.empty()) {
        error = "The player-name crop is empty";
        return false;
    }
    int x0 = sourceWidth;
    int y0 = sourceHeight;
    int x1 = 0;
    int y1 = 0;
    for (const PixelRegion& region : regions) {
        x0 = std::min(x0, region.x);
        y0 = std::min(y0, region.y);
        x1 = std::max(x1, region.x + region.width);
        y1 = std::max(y1, region.y + region.height);
    }
    // NV12 chroma pairs address two horizontal luma pixels. Expand by at
    // most one pixel so both name rectangles share one compact transfer.
    x0 &= ~1;
    x1 = std::min(sourceWidth & ~1, (x1 + 1) & ~1);
    if (x1 <= x0 || y1 <= y0) {
        error = "The mapped player-name crop is empty";
        return false;
    }
    const int width = x1 - x0;
    const int height = y1 - y0;
    const int uvY0 = y0 / 2;
    const int uvY1 = (y1 + 1) / 2;
    const int uvHeight = uvY1 - uvY0;
    std::vector<std::uint8_t> luma(
        static_cast<std::size_t>(width) * height);
    std::vector<std::uint8_t> chroma(
        static_cast<std::size_t>(width) * uvHeight);
    const auto* source = static_cast<const std::uint8_t*>(nv12);
    cudaStream_t stream = reinterpret_cast<cudaStream_t>(cudaStream);
    cudaError_t status = cudaMemcpy2DAsync(
        luma.data(), static_cast<std::size_t>(width),
        source + static_cast<std::size_t>(y0) * pitch + x0, pitch,
        static_cast<std::size_t>(width), height,
        cudaMemcpyDeviceToHost, stream);
    if (status == cudaSuccess) {
        status = cudaMemcpy2DAsync(
            chroma.data(), static_cast<std::size_t>(width),
            source + static_cast<std::size_t>(surfaceHeight) * pitch +
                static_cast<std::size_t>(uvY0) * pitch + x0,
            pitch, static_cast<std::size_t>(width), uvHeight,
            cudaMemcpyDeviceToHost, stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(stream);
    if (status != cudaSuccess) {
        error = std::string("Could not retain the NVDEC player-name crop: ") +
            cudaGetErrorString(status);
        return false;
    }
    frame.width = width;
    frame.height = height;
    frame.sourceX = x0;
    frame.sourceY = y0;
    frame.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    const auto quantize = [](float value) -> std::uint8_t {
        return static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(std::floor(value + .5f)), 0, 255));
    };
    for (int localY = 0; localY < height; ++localY) {
        const int y = y0 + localY;
        const int uvRow = y / 2 - uvY0;
        for (int localX = 0; localX < width; ++localX) {
            const int x = x0 + localX;
            const float yy = luma[
                static_cast<std::size_t>(localY) * width + localX];
            const int uvColumn = (x & ~1) - x0;
            const std::size_t uvOffset =
                static_cast<std::size_t>(uvRow) * width + uvColumn;
            const float uu = chroma[uvOffset] - 128.0f;
            const float vv = chroma[uvOffset + 1] - 128.0f;
            const float c = fullRange
                ? yy : std::max(0.0f, yy - 16.0f) * (255.0f / 219.0f);
            float red = 0;
            float green = 0;
            float blue = 0;
            if (matrix == 1) {
                red = c + 1.5748f * vv;
                green = c - 0.187324f * uu - 0.468124f * vv;
                blue = c + 1.8556f * uu;
            } else {
                red = c + 1.402f * vv;
                green = c - 0.344136f * uu - 0.714136f * vv;
                blue = c + 1.772f * uu;
            }
            const std::size_t destination =
                (static_cast<std::size_t>(localY) * width + localX) * 4;
            frame.bgra[destination] = quantize(blue);
            frame.bgra[destination + 1] = quantize(green);
            frame.bgra[destination + 2] = quantize(red);
            frame.bgra[destination + 3] = 255;
        }
    }
    return true;
#else
    (void)nv12;
    (void)pitch;
    (void)sourceWidth;
    (void)sourceHeight;
    (void)surfaceHeight;
    (void)matrix;
    (void)fullRange;
    (void)cudaStream;
    (void)settings;
    frame = {};
    error = "NVDEC player-name capture is unavailable in this build";
    return false;
#endif
}

} // namespace tr

#else

namespace tr {
bool recognizePlayerNames(const std::filesystem::path&, double,
                          const Settings&, RecoveryOutput&,
                          std::string& warning) {
    warning.clear();
    return true;
}
bool recognizePlayerNamesFromFrame(const Frame&, int, int,
                                   const Settings&, RecoveryOutput&,
                                   std::string& warning) {
    warning.clear();
    return true;
}
} // namespace tr

#endif
