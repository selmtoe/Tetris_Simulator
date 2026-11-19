#include "nvdec_cuda_preprocessor.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include "nvdec_cuda_kernels.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <limits>
#include <string>
#include <memory>
#include <iostream>
#include <utility>
#include <vector>

namespace tr {
namespace {

constexpr int Players = 2;
constexpr int CellsPerBoard = BoardWidth * VisibleRows;
constexpr int CellSize = 32;
constexpr int ResizedWidth = BoardWidth * CellSize;
constexpr int ResizedHeight = VisibleRows * CellSize;
constexpr int TensorValues = CellsPerBoard * 3 * CellSize * CellSize;
constexpr int QueueInputSize = 48;
constexpr int QueuePixels = QueueInputSize * QueueInputSize;
constexpr int QueueFloats = 3 * QueuePixels;
constexpr int QueueSlots = 12;
constexpr int StatisticFields = 5;

struct ContentMapping {
    double x = 0;
    double y = 0;
    double scale = 1;
};

ContentMapping contentMapping(int width, int height) {
    constexpr double targetAspect = 16.0 / 9.0;
    const double sourceAspect = static_cast<double>(width) / std::max(1, height);
    if (sourceAspect > targetAspect) {
        const double cropWidth = height * targetAspect;
        return {(width - cropWidth) / 2.0, 0, cropWidth / 1920.0};
    }
    const double cropWidth = width;
    return {0, (height - cropWidth / targetAspect) / 2.0,
            cropWidth / 1920.0};
}

struct BoardGeometry {
    struct RawCrop {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::size_t pitch = 0;
        std::size_t offset = 0;
        std::size_t bytes = 0;
    };
    std::array<int, Players> widths{};
    std::array<int, Players> heights{};
    std::array<std::uint32_t, Players> bases{};
    std::vector<TrCudaBoardSample> samples;
    std::vector<TrCudaBoardSample> rawSamples;
    std::array<RawCrop, Players> rawCrops{};
    std::array<std::vector<TrCudaResizeSample>, Players> resizeSamples;
    std::vector<TrCudaClassicRect> classicRects;
    std::array<std::uint32_t, Players * CellsPerBoard> classicCounts{};
    std::size_t pixelCount = 0;
};

BoardGeometry buildBoardGeometry(int frameWidth, int frameHeight,
                                 const Settings& settings) {
    BoardGeometry geometry;
    const ContentMapping mapping = contentMapping(frameWidth, frameHeight);
    const std::array<const PlayerLayout*, Players> layouts{
        &settings.p1, &settings.p2
    };
    std::uint32_t boardBase = 0;
    for (int player = 0; player < Players; ++player) {
        const LayoutRect& layout = layouts[player]->board;
        const int width = std::max(10, static_cast<int>(std::floor(layout.w)));
        const int height = std::max(20, static_cast<int>(std::floor(layout.h)));
        geometry.widths[player] = width;
        geometry.heights[player] = height;
        geometry.bases[player] = boardBase;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double sourceX = mapping.x + layout.x * mapping.scale +
                    (x + .5) * mapping.scale - .5;
                const double sourceY = mapping.y + layout.y * mapping.scale +
                    (y + .5) * mapping.scale - .5;
                const double clampedX = std::clamp(
                    sourceX, 0.0, static_cast<double>(frameWidth - 1));
                const double clampedY = std::clamp(
                    sourceY, 0.0, static_cast<double>(frameHeight - 1));
                const int x0 = static_cast<int>(std::floor(clampedX));
                const int y0 = static_cast<int>(std::floor(clampedY));
                geometry.samples.push_back({
                    x0, y0, std::min(frameWidth - 1, x0 + 1),
                    std::min(frameHeight - 1, y0 + 1),
                    clampedX - x0, clampedY - y0
                });
            }
        }
        boardBase += static_cast<std::uint32_t>(width * height);
    }
    geometry.pixelCount = boardBase;

    std::size_t rawOffset = 0;
    for (int player = 0; player < Players; ++player) {
        const std::size_t first = geometry.bases[player];
        const std::size_t count = static_cast<std::size_t>(
            geometry.widths[player] * geometry.heights[player]);
        int minX = frameWidth - 1, minY = frameHeight - 1;
        int maxX = 0, maxY = 0;
        for (std::size_t i = first; i < first + count; ++i) {
            const TrCudaBoardSample& sample = geometry.samples[i];
            minX = std::min(minX, std::min(sample.x0, sample.x1));
            minY = std::min(minY, std::min(sample.y0, sample.y1));
            maxX = std::max(maxX, std::max(sample.x0, sample.x1));
            maxY = std::max(maxY, std::max(sample.y0, sample.y1));
        }
        minX &= ~1;
        minY &= ~1;
        const int endX = std::min(frameWidth, (maxX + 2) & ~1);
        const int endY = std::min(frameHeight, (maxY + 2) & ~1);
        auto& crop = geometry.rawCrops[player];
        crop.x = minX;
        crop.y = minY;
        crop.width = std::max(2, endX - minX);
        crop.height = std::max(2, endY - minY);
        crop.pitch = (static_cast<std::size_t>(crop.width) + 127u) & ~127u;
        crop.offset = rawOffset;
        crop.bytes = crop.pitch * static_cast<std::size_t>(
            crop.height + crop.height / 2);
        rawOffset += crop.bytes;
    }
    geometry.rawSamples = geometry.samples;
    for (int player = 0; player < Players; ++player) {
        const std::size_t first = geometry.bases[player];
        const std::size_t count = static_cast<std::size_t>(
            geometry.widths[player] * geometry.heights[player]);
        const auto& crop = geometry.rawCrops[player];
        for (std::size_t i = first; i < first + count; ++i) {
            auto& sample = geometry.rawSamples[i];
            sample.x0 -= crop.x;
            sample.x1 -= crop.x;
            sample.y0 -= crop.y;
            sample.y1 -= crop.y;
        }
    }

    for (int player = 0; player < Players; ++player) {
        const int width = geometry.widths[player];
        const int height = geometry.heights[player];
        const std::uint32_t base = geometry.bases[player];
        auto& samples = geometry.resizeSamples[player];
        samples.reserve(ResizedWidth * ResizedHeight);
        std::array<int, ResizedWidth> x0{}, x1{};
        std::array<double, ResizedWidth> fx{};
        std::array<int, ResizedHeight> y0{}, y1{};
        std::array<double, ResizedHeight> fy{};
        for (int x = 0; x < ResizedWidth; ++x) {
            const double source = std::clamp(
                (x + .5) * width / ResizedWidth - .5,
                0.0, static_cast<double>(width - 1));
            x0[x] = static_cast<int>(std::floor(source));
            x1[x] = std::min(width - 1, x0[x] + 1);
            fx[x] = source - x0[x];
        }
        for (int y = 0; y < ResizedHeight; ++y) {
            const double source = std::clamp(
                (y + .5) * height / ResizedHeight - .5,
                0.0, static_cast<double>(height - 1));
            y0[y] = static_cast<int>(std::floor(source));
            y1[y] = std::min(height - 1, y0[y] + 1);
            fy[y] = source - y0[y];
        }
        for (int y = 0; y < ResizedHeight; ++y) {
            for (int x = 0; x < ResizedWidth; ++x) {
                samples.push_back({
                    base + static_cast<std::uint32_t>(y0[y] * width + x0[x]),
                    base + static_cast<std::uint32_t>(y0[y] * width + x1[x]),
                    base + static_cast<std::uint32_t>(y1[y] * width + x0[x]),
                    base + static_cast<std::uint32_t>(y1[y] * width + x1[x]),
                    fx[x], fy[y]
                });
            }
        }
        const double cellWidth = static_cast<double>(width) / BoardWidth;
        const double cellHeight = static_cast<double>(height) / VisibleRows;
        const int sampleSize = std::max(
            1, static_cast<int>(std::floor(cellWidth * .25)));
        for (int row = 0; row < VisibleRows; ++row) {
            for (int col = 0; col < BoardWidth; ++col) {
                const double sampleX = (col + .5) * cellWidth;
                const double sampleY = (row + .5) * cellHeight;
                const int rectX0 = std::clamp(static_cast<int>(std::floor(
                    sampleX - sampleSize / 2.0)), 0, width - 1);
                const int rectY0 = std::clamp(static_cast<int>(std::floor(
                    sampleY - sampleSize / 2.0)), 0, height - 1);
                const int rectX1 = std::min(width, rectX0 + sampleSize);
                const int rectY1 = std::min(height, rectY0 + sampleSize);
                geometry.classicRects.push_back({
                    base, static_cast<std::uint32_t>(width),
                    static_cast<std::uint32_t>(rectX0),
                    static_cast<std::uint32_t>(rectY0),
                    static_cast<std::uint32_t>(rectX1),
                    static_cast<std::uint32_t>(rectY1), 0, 0
                });
                geometry.classicCounts[static_cast<std::size_t>(
                    player * CellsPerBoard + row * BoardWidth + col)] =
                    static_cast<std::uint32_t>(
                        (rectX1 - rectX0) * (rectY1 - rectY0));
            }
        }
    }
    return geometry;
}

struct MappedCrop { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

MappedCrop mapCnnCrop(const LayoutRect& rectangle,
                      const PlayerLayout& layout,
                      const ContentMapping& mapping,
                      int frameWidth, int frameHeight) {
    const double contentWidth = mapping.scale * 1920.0;
    const double contentHeight = contentWidth * 9.0 / 16.0;
    return {
        std::clamp(static_cast<int>(std::nearbyint(
            mapping.x + rectangle.x * contentWidth /
                layout.queueCnnReferenceWidth)), 0, frameWidth),
        std::clamp(static_cast<int>(std::nearbyint(
            mapping.y + rectangle.y * contentHeight /
                layout.queueCnnReferenceHeight)), 0, frameHeight),
        std::clamp(static_cast<int>(std::nearbyint(
            mapping.x + (rectangle.x + rectangle.w) * contentWidth /
                layout.queueCnnReferenceWidth)), 0, frameWidth),
        std::clamp(static_cast<int>(std::nearbyint(
            mapping.y + (rectangle.y + rectangle.h) * contentHeight /
                layout.queueCnnReferenceHeight)), 0, frameHeight)
    };
}

struct QueueGeometry {
    std::vector<TrCudaQueueSample> samples;
    std::vector<TrCudaLegacyPoint> legacyPoints;
    std::array<TrCudaCropRect, QueueSlots> cropRects{};
    int maxCropWidth = 0;
    int maxCropHeight = 0;
};

void appendQueuePlayer(QueueGeometry& geometry,
                       const PlayerLayout& layout,
                       std::size_t firstSlot,
                       const ContentMapping& mapping,
                       int frameWidth, int frameHeight) {
    const std::array<LayoutRect, 6> rectangles{{
        layout.queueCnnHold,
        layout.queueCnnNext[0], layout.queueCnnNext[1],
        layout.queueCnnNext[2], layout.queueCnnNext[3],
        layout.queueCnnNext[4]
    }};
    const std::array<std::array<double, 2>, 6> legacy{{
        layout.hold, layout.next[0], layout.next[1], layout.next[2],
        layout.next[3], layout.next[4]
    }};
    for (std::size_t local = 0; local < rectangles.size(); ++local) {
        const std::size_t slot = firstSlot + local;
        const MappedCrop mapped = mapCnnCrop(
            rectangles[local], layout, mapping, frameWidth, frameHeight);
        const int sourceWidth = mapped.x1 - mapped.x0;
        const int sourceHeight = mapped.y1 - mapped.y0;
        geometry.cropRects[slot] = {
            mapped.x0, mapped.y0, sourceWidth, sourceHeight
        };
        geometry.maxCropWidth = std::max(geometry.maxCropWidth, sourceWidth);
        geometry.maxCropHeight = std::max(geometry.maxCropHeight, sourceHeight);
        if (sourceWidth < 1 || sourceHeight < 1) continue;
        int resizedWidth = QueueInputSize;
        int resizedHeight = QueueInputSize;
        if (sourceWidth > sourceHeight) {
            resizedHeight = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceHeight) / sourceWidth * QueueInputSize)));
        } else if (sourceHeight > sourceWidth) {
            resizedWidth = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceWidth) / sourceHeight * QueueInputSize)));
        }
        const int padX = static_cast<int>(std::nearbyint(
            (QueueInputSize - resizedWidth) * .5));
        const int padY = static_cast<int>(std::nearbyint(
            (QueueInputSize - resizedHeight) * .5));
        for (int y = 0; y < resizedHeight; ++y) {
            for (int x = 0; x < resizedWidth; ++x) {
                const double sourceX = mapped.x0 +
                    (x + .5) * sourceWidth / resizedWidth - .5;
                const double sourceY = mapped.y0 +
                    (y + .5) * sourceHeight / resizedHeight - .5;
                const double clampedX = std::clamp(
                    sourceX, static_cast<double>(mapped.x0),
                    static_cast<double>(mapped.x1 - 1));
                const double clampedY = std::clamp(
                    sourceY, static_cast<double>(mapped.y0),
                    static_cast<double>(mapped.y1 - 1));
                const int x0 = static_cast<int>(std::floor(clampedX));
                const int y0 = static_cast<int>(std::floor(clampedY));
                const std::size_t destination = slot * QueuePixels +
                    static_cast<std::size_t>(padY + y) * QueueInputSize +
                    padX + x;
                geometry.samples[destination] = {
                    x0, y0, std::min(mapped.x1 - 1, x0 + 1),
                    std::min(mapped.y1 - 1, y0 + 1),
                    static_cast<float>(clampedX - x0),
                    static_cast<float>(clampedY - y0), 1, 0
                };
            }
        }
    }

    const double queueScale = mapping.scale *
        (1920.0 / layout.queueCoordinateWidth);
    const double radius = layout.queueSampleRadius * queueScale;
    const double radiusSquared = radius * radius;
    const int diameter = static_cast<int>(std::ceil(radius * 2.0));
    for (std::size_t local = 0; local < legacy.size(); ++local) {
        const double centerX = mapping.x + legacy[local][0] * queueScale;
        const double centerY = mapping.y + legacy[local][1] * queueScale;
        const int startX = std::max(
            0, static_cast<int>(std::floor(centerX - radius)));
        const int startY = std::max(
            0, static_cast<int>(std::floor(centerY - radius)));
        const int endX = std::min(frameWidth, startX + diameter);
        const int endY = std::min(frameHeight, startY + diameter);
        for (int y = startY; y < endY; ++y) {
            for (int x = startX; x < endX; ++x) {
                const double dx = x - centerX;
                const double dy = y - centerY;
                if (dx * dx + dy * dy <= radiusSquared) {
                    geometry.legacyPoints.push_back({
                        x, y, static_cast<std::uint32_t>(firstSlot + local), 0
                    });
                }
            }
        }
    }
}

QueueGeometry buildQueueGeometry(int frameWidth, int frameHeight,
                                 const Settings& settings) {
    QueueGeometry geometry;
    geometry.samples.resize(QueueSlots * QueuePixels);
    const ContentMapping mapping = contentMapping(frameWidth, frameHeight);
    appendQueuePlayer(geometry, settings.p1, 0, mapping, frameWidth, frameHeight);
    appendQueuePlayer(geometry, settings.p2, 6, mapping, frameWidth, frameHeight);
    return geometry;
}

struct Rgb { double r = 0, g = 0, b = 0; };

Cell classicCell(std::uint32_t sumR, std::uint32_t sumG,
                 std::uint32_t sumB, std::uint32_t count) {
    if (!count) return Cell::Empty;
    const Rgb input{static_cast<double>(sumR) / count,
                    static_cast<double>(sumG) / count,
                    static_cast<double>(sumB) / count};
    const std::array<std::pair<Cell, std::vector<Rgb>>, 9> palette{{
        {Cell::Empty, {{0,0,0}, {48,40,56}}},
        {Cell::Garbage, {{153,153,153}, {216,216,216}}},
        {Cell::I, {{1,152,153}, {1,153,213}, {0,150,153}}},
        {Cell::O, {{153,154,2}, {249,185,0}}},
        {Cell::T, {{152,0,153}, {135,30,136}}},
        {Cell::L, {{153,103,0}, {245,97,0}}},
        {Cell::J, {{0,0,187}, {0,75,165}}},
        {Cell::S, {{16,151,31}, {92,181,35}}},
        {Cell::Z, {{153,0,0}, {218,24,34}}}
    }};
    const auto distance = [&](const Rgb& candidate) {
        const double dr = input.r - candidate.r;
        const double dg = input.g - candidate.g;
        const double db = input.b - candidate.b;
        return dr * dr + dg * dg + db * db;
    };
    for (const Rgb& candidate : palette[0].second)
        if (distance(candidate) < 6000) return Cell::Empty;
    for (const Rgb& candidate : palette[1].second)
        if (distance(candidate) < 10000) return Cell::Garbage;
    Cell best = Cell::Empty;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t entry = 2; entry < palette.size(); ++entry) {
        for (const Rgb& candidate : palette[entry].second) {
            const double current = distance(candidate);
            if (current < bestDistance) {
                bestDistance = current;
                best = palette[entry].first;
            }
        }
    }
    return bestDistance > 25000 ? Cell::Empty : best;
}

std::string cudaError(const char* operation, cudaError_t status) {
    const char* detail = cudaGetErrorString(status);
    return std::string(operation) + " failed: " +
        (detail ? detail : std::to_string(static_cast<int>(status)));
}

template <typename T>
bool allocateDevice(T*& destination, std::size_t count,
                    const char* label, std::string& error) {
    const cudaError_t status = cudaMalloc(
        reinterpret_cast<void**>(&destination), count * sizeof(T));
    if (status == cudaSuccess) return true;
    error = cudaError(label, status);
    return false;
}

template <typename T>
bool allocateCopy(T*& destination, const std::vector<T>& source,
                  const char* label, std::string& error) {
    if (!allocateDevice(destination, source.size(), label, error)) return false;
    const cudaError_t status = cudaMemcpy(
        destination, source.data(), source.size() * sizeof(T),
        cudaMemcpyHostToDevice);
    if (status == cudaSuccess) return true;
    error = cudaError(label, status);
    return false;
}

} // namespace

struct NvdecCudaPreprocessor::Impl {
    struct PipelineResult {
        D3D11QueueBatchResult batch;
        std::string error;
    };

    struct Slot {
        std::uint32_t generation = 0;
        std::int64_t sourceTime100ns = 0;
        std::uint64_t deliveryOrder = 0;
        int matrix = 0;
        bool fullRange = false;
    };

    ~Impl() {
        if (pipelineFutureActive) {
            try { pipelineFuture.get(); } catch (...) {}
            pipelineFutureActive = false;
        }
        if (stream) cudaStreamSynchronize(stream);
        cudaFree(boardSamples);
        cudaFree(rawBoardSamples);
        cudaFree(resizeSamples[0]);
        cudaFree(resizeSamples[1]);
        cudaFree(classicRects);
        cudaFree(queueSamples);
        cudaFree(legacyPoints);
        cudaFree(cropRects);
        cudaFree(boardRing);
        cudaFree(rawBoardRingData);
        cudaFree(boardScratch);
        cudaFree(boardTensor);
        cudaFree(classicSums);
        cudaFree(queueTensor);
        cudaFree(queuePipelineTensor);
        cudaFree(queueStatistics);
        cudaFree(bgraScratch);
        if (stream) cudaStreamDestroy(stream);
    }

    int frameWidth = 0;
    int frameHeight = 0;
    int darkPixelLimit = 40;
    int cudaDevice = -1;
    bool asyncFrames = false;
    bool fastFp32 = false;
    bool rawBoardRing = false;
    bool adaptiveEnabled = false;
    bool pipelineEnabled = false;
    std::size_t boardBatchCapacity = 1;
    bool boardBatchScalarCnn = false;
    int adaptiveRefreshFrames = 12;
    int adaptiveColorCountDelta = 1;
    double adaptiveRgbDelta = 3.0;
    double adaptiveDarkRatioDelta = 0.005;
    std::size_t nextSlot = 0;
    std::uint32_t nextGeneration = 1;
    std::size_t queueFrames = 0;
    std::size_t frameBytes = 0;
    std::array<bool, QueueSlots> adaptivePreviousStatisticsValid{};
    std::array<bool, QueueSlots> adaptivePreviousPredictionsValid{};
    std::array<int, QueueSlots> adaptiveFramesSinceInference{};
    D3D11QueueFrameStatistics adaptivePreviousStatistics{};
    std::array<QueueCnnPrediction, QueueSlots> adaptivePreviousPredictions{};
    OnnxBoardModel* boardModel = nullptr;
    OnnxQueueModel* queueModel = nullptr;
    // TR_QUEUE_HOLD_MODEL: optional second queue CNN used only for the HOLD
    // slot (slot % 6 == 0: each player contributes HOLD, NEXT1..5).
    std::shared_ptr<OnnxQueueModel> holdModel;
    BoardGeometry boardGeometry;
    QueueGeometry queueGeometry;
    std::vector<Slot> slots;
    cudaStream_t stream = nullptr;
    TrCudaBoardSample* boardSamples = nullptr;
    TrCudaBoardSample* rawBoardSamples = nullptr;
    std::array<TrCudaResizeSample*, Players> resizeSamples{};
    TrCudaClassicRect* classicRects = nullptr;
    TrCudaQueueSample* queueSamples = nullptr;
    TrCudaLegacyPoint* legacyPoints = nullptr;
    TrCudaCropRect* cropRects = nullptr;
    std::uint32_t* boardRing = nullptr;
    std::uint8_t* rawBoardRingData = nullptr;
    std::uint32_t* boardScratch = nullptr;
    float* boardTensor = nullptr;
    std::uint32_t* classicSums = nullptr;
    float* queueTensor = nullptr;
    float* queuePipelineTensor = nullptr;
    std::uint32_t* queueStatistics = nullptr;
    std::uint8_t* bgraScratch = nullptr;
    std::size_t bgraScratchBytes = 0;
    std::future<PipelineResult> pipelineFuture;
    bool pipelineFutureActive = false;
};

NvdecCudaPreprocessor::NvdecCudaPreprocessor() = default;
NvdecCudaPreprocessor::~NvdecCudaPreprocessor() = default;

bool NvdecCudaPreprocessor::initialize(
    int frameWidth, int frameHeight, const Settings& settings,
    OnnxBoardModel* boardModel, OnnxQueueModel* queueModel,
    std::size_t memoryBudgetBytes, std::string& error) {
    error.clear();
    impl_.reset();
    if (frameWidth <= 0 || frameHeight <= 0 || !boardModel ||
        !boardModel->cudaReady() || !queueModel || !queueModel->ready()) {
        error = "Direct NVDEC CUDA preprocessing received invalid inputs";
        return false;
    }
    auto candidate = std::make_unique<Impl>();
    candidate->frameWidth = frameWidth;
    candidate->frameHeight = frameHeight;
    candidate->darkPixelLimit = std::clamp(settings.queueCnnDarkPixelLimit, 0, 255);
    candidate->boardModel = boardModel;
    candidate->queueModel = queueModel;
    if (const char* holdPath = std::getenv("TR_QUEUE_HOLD_MODEL")) {
        if (*holdPath) {
            std::string holdError;
            auto hold = std::make_shared<OnnxQueueModel>(holdPath, holdError);
            if (hold->ready() && holdError.empty()) {
                candidate->holdModel = std::move(hold);
                std::cerr << "queue_hold_model_cuda=" << holdPath << '\n';
            } else {
                std::cerr << "queue_hold_model_cuda_error=" << holdError << '\n';
            }
        }
    }
    if (const char* configured = std::getenv("TR_QUEUE_SAFE_PIPELINE")) {
        candidate->pipelineEnabled = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE")) {
        candidate->adaptiveEnabled = std::string(configured) != "0";
    }
    if (candidate->adaptiveEnabled) candidate->pipelineEnabled = false;
    if (const char* configured = std::getenv(
            "TR_QUEUE_ADAPTIVE_REFRESH_FRAMES")) {
        candidate->adaptiveRefreshFrames = std::clamp(
            std::atoi(configured), 1, 600);
    }
    if (const char* configured = std::getenv(
            "TR_QUEUE_ADAPTIVE_COLOR_COUNT_DELTA")) {
        candidate->adaptiveColorCountDelta = std::clamp(
            std::atoi(configured), 0, 1000000);
    }
    if (const char* configured = std::getenv(
            "TR_QUEUE_ADAPTIVE_RGB_DELTA")) {
        candidate->adaptiveRgbDelta = std::clamp(
            std::atof(configured), 0.0, 255.0);
    }
    if (const char* configured = std::getenv(
            "TR_QUEUE_ADAPTIVE_DARK_RATIO_DELTA")) {
        candidate->adaptiveDarkRatioDelta = std::clamp(
            std::atof(configured), 0.0, 1.0);
    }
    if (const char* configured = std::getenv("TR_NVDEC_ASYNC_FRAMES")) {
        candidate->asyncFrames = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_NVDEC_FAST_FP32")) {
        candidate->fastFp32 = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_NVDEC_RAW_BOARD_RING")) {
        candidate->rawBoardRing = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv(
            "TR_NVDEC_BOARD_BATCH_SCALAR_CNN")) {
        candidate->boardBatchScalarCnn = std::string(configured) != "0";
    }
    bool boardBatchRequested = candidate->boardBatchScalarCnn;
    if (const char* configured = std::getenv("TR_NVDEC_BOARD_BATCH")) {
        boardBatchRequested = boardBatchRequested ||
            std::string(configured) != "0";
    }
    if (boardBatchRequested) {
        candidate->boardBatchCapacity = 8;
        if (const char* batchSize = std::getenv(
                "TR_NVDEC_BOARD_BATCH_SIZE")) {
            candidate->boardBatchCapacity = static_cast<std::size_t>(
                std::clamp(std::atoi(batchSize), 2, 32));
        }
    }
    cudaError_t status = cudaGetDevice(&candidate->cudaDevice);
    if (status != cudaSuccess) {
        error = cudaError("cudaGetDevice(NVDEC direct)", status);
        return false;
    }
    candidate->boardGeometry = buildBoardGeometry(
        frameWidth, frameHeight, settings);
    candidate->queueGeometry = buildQueueGeometry(
        frameWidth, frameHeight, settings);
    if (!candidate->boardGeometry.pixelCount ||
        candidate->queueGeometry.samples.empty()) {
        error = "Direct NVDEC CUDA geometry is empty";
        return false;
    }
    if (candidate->rawBoardRing) {
        const auto& last = candidate->boardGeometry.rawCrops.back();
        candidate->frameBytes = last.offset + last.bytes;
    } else {
        candidate->frameBytes = candidate->boardGeometry.pixelCount *
            sizeof(std::uint32_t);
    }
    // Diagnostics allow a deliberately deeper ring so a whole match can be
    // retained in one pass.  It changes nothing unless the variable is set.
    std::size_t maximumFrames = MaximumFrames;
    if (const char* configured = std::getenv("TR_BOARD_RING_FRAMES")) {
        if (*configured) {
            maximumFrames = static_cast<std::size_t>(
                std::clamp(std::atoi(configured), 32, 16384));
        }
    }
    const std::size_t slotCount = std::min(
        maximumFrames, memoryBudgetBytes / candidate->frameBytes);
    if (!slotCount) {
        error = "Direct NVDEC CUDA board-ring budget is too small";
        return false;
    }
    candidate->slots.resize(slotCount);
    status = cudaStreamCreateWithFlags(
        &candidate->stream, cudaStreamNonBlocking);
    if (status != cudaSuccess) {
        error = cudaError("cudaStreamCreate(NVDEC direct)", status);
        return false;
    }
    if (!allocateCopy(candidate->boardSamples,
                      candidate->boardGeometry.samples,
                      "cudaMalloc/copy(board samples)", error) ||
        (candidate->rawBoardRing &&
         !allocateCopy(candidate->rawBoardSamples,
                       candidate->boardGeometry.rawSamples,
                       "cudaMalloc/copy(raw board samples)", error)) ||
        !allocateCopy(candidate->resizeSamples[0],
                      candidate->boardGeometry.resizeSamples[0],
                      "cudaMalloc/copy(P1 resize samples)", error) ||
        !allocateCopy(candidate->resizeSamples[1],
                      candidate->boardGeometry.resizeSamples[1],
                      "cudaMalloc/copy(P2 resize samples)", error) ||
        !allocateCopy(candidate->classicRects,
                      candidate->boardGeometry.classicRects,
                      "cudaMalloc/copy(classic rects)", error) ||
        !allocateCopy(candidate->queueSamples,
                      candidate->queueGeometry.samples,
                      "cudaMalloc/copy(queue samples)", error) ||
        !allocateCopy(candidate->legacyPoints,
                      candidate->queueGeometry.legacyPoints,
                      "cudaMalloc/copy(queue legacy points)", error)) {
        return false;
    }
    const std::vector<TrCudaCropRect> cropRects(
        candidate->queueGeometry.cropRects.begin(),
        candidate->queueGeometry.cropRects.end());
    if (!allocateCopy(candidate->cropRects, cropRects,
                      "cudaMalloc/copy(queue crop rects)", error) ||
        (!candidate->rawBoardRing &&
         !allocateDevice(candidate->boardRing,
                         slotCount * candidate->boardGeometry.pixelCount,
                         "cudaMalloc(board ring)", error)) ||
        (candidate->rawBoardRing &&
         (!allocateDevice(candidate->rawBoardRingData,
                          slotCount * candidate->frameBytes,
                          "cudaMalloc(raw NV12 board ring)", error) ||
          !allocateDevice(candidate->boardScratch,
                           candidate->boardBatchCapacity *
                               candidate->boardGeometry.pixelCount,
                           "cudaMalloc(board RGB scratch)", error))) ||
        !allocateDevice(candidate->boardTensor,
                        candidate->boardBatchCapacity * TensorValues,
                        "cudaMalloc(board tensor)", error) ||
        !allocateDevice(candidate->classicSums,
                        candidate->boardBatchCapacity * CellsPerBoard * 3,
                        "cudaMalloc(classic sums)", error) ||
        !allocateDevice(candidate->queueTensor,
                        MaximumQueueFrames * QueueSlots * QueueFloats,
                        "cudaMalloc(queue tensor)", error) ||
        ((candidate->pipelineEnabled || candidate->adaptiveEnabled) &&
         !allocateDevice(candidate->queuePipelineTensor,
                         MaximumQueueFrames * QueueSlots * QueueFloats,
                         "cudaMalloc(queue pipeline tensor)", error)) ||
        !allocateDevice(candidate->queueStatistics,
                        MaximumQueueFrames * QueueSlots * StatisticFields,
                        "cudaMalloc(queue statistics)", error)) {
        return false;
    }
    impl_ = std::move(candidate);
    return true;
}

bool NvdecCudaPreprocessor::processFrame(
    const void* nv12, std::size_t pitch, int sourceHeight,
    int matrix, bool fullRange, std::int64_t sourceTime100ns,
    std::uint64_t deliveryOrder, bool retainBoard, bool appendQueue,
    D3D11BoardFrameHandle& handle, std::string& error) {
    error.clear();
    handle = {};
    if (!impl_ || !nv12 || !pitch || sourceHeight <= 0) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (appendQueue && impl_->queueFrames >= MaximumQueueFrames) {
        error = "Direct NVDEC CUDA queue batch reached capacity";
        return false;
    }
    const std::size_t slotIndex = impl_->nextSlot;
    std::uint32_t generation = 0;
    if (retainBoard) {
        generation = impl_->nextGeneration++;
        if (!generation) generation = impl_->nextGeneration++;
    }
    cudaError_t status = cudaSuccess;
    if (retainBoard && impl_->rawBoardRing) {
        const auto* source = static_cast<const std::uint8_t*>(nv12);
        std::uint8_t* frameDestination = impl_->rawBoardRingData +
            slotIndex * impl_->frameBytes;
        for (const auto& crop : impl_->boardGeometry.rawCrops) {
            std::uint8_t* destination = frameDestination + crop.offset;
            const std::uint8_t* sourceY = source +
                static_cast<std::size_t>(crop.y) * pitch + crop.x;
            status = cudaMemcpy2DAsync(
                destination, crop.pitch, sourceY, pitch,
                static_cast<std::size_t>(crop.width), crop.height,
                cudaMemcpyDeviceToDevice, impl_->stream);
            if (status != cudaSuccess) break;
            const std::uint8_t* sourceUv = source +
                static_cast<std::size_t>(sourceHeight) * pitch +
                static_cast<std::size_t>(crop.y / 2) * pitch + crop.x;
            status = cudaMemcpy2DAsync(
                destination + crop.pitch * static_cast<std::size_t>(crop.height),
                crop.pitch, sourceUv, pitch,
                static_cast<std::size_t>(crop.width), crop.height / 2,
                cudaMemcpyDeviceToDevice, impl_->stream);
            if (status != cudaSuccess) break;
        }
    } else if (retainBoard) {
        std::uint32_t* boardDestination = impl_->boardRing +
            slotIndex * impl_->boardGeometry.pixelCount;
        status = static_cast<cudaError_t>(trNv12RetainBoards(
            nv12, pitch, impl_->frameWidth, sourceHeight, matrix,
            fullRange ? 1 : 0, impl_->boardSamples,
            impl_->boardGeometry.samples.size(), impl_->fastFp32 ? 1 : 0,
            boardDestination, impl_->stream));
    }
    if (status == cudaSuccess && appendQueue) {
        status = static_cast<cudaError_t>(trNv12AppendQueue(
            nv12, pitch, impl_->frameWidth, sourceHeight, matrix,
            fullRange ? 1 : 0, impl_->queueSamples,
            impl_->queueGeometry.samples.size(), impl_->legacyPoints,
            impl_->queueGeometry.legacyPoints.size(), impl_->cropRects,
            impl_->queueGeometry.maxCropWidth,
            impl_->queueGeometry.maxCropHeight, impl_->darkPixelLimit,
            impl_->queueFrames, impl_->queueTensor,
            impl_->queueStatistics, impl_->stream));
    }
    if (status == cudaSuccess && !impl_->asyncFrames) {
        status = cudaStreamSynchronize(impl_->stream);
    }
    if (status != cudaSuccess) {
        error = cudaError("direct NVDEC CUDA frame preprocessing", status);
        return false;
    }
    if (retainBoard) {
        Impl::Slot& slot = impl_->slots[slotIndex];
        slot.generation = generation;
        slot.sourceTime100ns = sourceTime100ns;
        slot.deliveryOrder = deliveryOrder;
        slot.matrix = matrix;
        slot.fullRange = fullRange;
        handle.slot = static_cast<std::uint32_t>(slotIndex);
        handle.generation = generation;
        handle.sourceTime100ns = sourceTime100ns;
        handle.deliveryOrder = deliveryOrder;
        impl_->nextSlot = (slotIndex + 1) % impl_->slots.size();
    }
    if (appendQueue) ++impl_->queueFrames;
    return true;
}

bool NvdecCudaPreprocessor::inferBoard(
    const D3D11BoardFrameHandle& handle, bool player1,
    D3D11BoardInference& result, std::string& error) {
    error.clear();
    result = {};
    if (!valid(handle)) {
        error = "Direct NVDEC CUDA board handle is stale";
        return false;
    }
    const int player = player1 ? 0 : 1;
    const Impl::Slot& slot = impl_->slots[handle.slot];
    const std::uint32_t* board = nullptr;
    cudaError_t status = cudaSuccess;
    if (impl_->rawBoardRing) {
        const auto& crop = impl_->boardGeometry.rawCrops[player];
        const std::size_t first = impl_->boardGeometry.bases[player];
        const std::size_t count = static_cast<std::size_t>(
            impl_->boardGeometry.widths[player] *
            impl_->boardGeometry.heights[player]);
        const std::uint8_t* raw = impl_->rawBoardRingData +
            static_cast<std::size_t>(handle.slot) * impl_->frameBytes +
            crop.offset;
        status = static_cast<cudaError_t>(trNv12RetainBoards(
            raw, crop.pitch, crop.width, crop.height,
            slot.matrix, slot.fullRange ? 1 : 0,
            impl_->rawBoardSamples + first, count,
            impl_->fastFp32 ? 1 : 0,
            impl_->boardScratch + first, impl_->stream));
        board = impl_->boardScratch;
    } else {
        board = impl_->boardRing +
            static_cast<std::size_t>(handle.slot) *
            impl_->boardGeometry.pixelCount;
    }
    if (status == cudaSuccess) status = static_cast<cudaError_t>(trBoardPixelsToTensor(
        board, impl_->resizeSamples[player],
        impl_->boardGeometry.resizeSamples[player].size(),
        impl_->boardTensor, impl_->fastFp32 ? 1 : 0, impl_->stream));
    if (status == cudaSuccess) {
        status = static_cast<cudaError_t>(trBoardClassicSums(
            board, impl_->classicRects + player * CellsPerBoard,
            CellsPerBoard, impl_->classicSums, impl_->stream));
    }
    std::array<std::uint32_t, CellsPerBoard * 3> sums{};
    if (status == cudaSuccess) {
        status = cudaMemcpyAsync(
            sums.data(), impl_->classicSums, sizeof(sums),
            cudaMemcpyDeviceToHost, impl_->stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
    if (status != cudaSuccess) {
        error = cudaError("direct NVDEC CUDA board tensor", status);
        return false;
    }
    std::vector<BoardProbabilities> probabilities;
    try {
        probabilities = impl_->boardModel->inferCudaDevice(
            impl_->boardTensor, 1, error);
    } catch (const std::exception& exception) {
        error = "Direct NVDEC CUDA board inference threw: " +
            std::string(exception.what());
    } catch (...) {
        error = "Direct NVDEC CUDA board inference threw an unknown exception";
    }
    if (!error.empty()) return false;
    if (probabilities.size() != 1) {
        error = "Direct NVDEC CUDA board inference returned the wrong batch size";
        return false;
    }
    VisibleBoard classic{};
    for (int cell = 0; cell < CellsPerBoard; ++cell) {
        const std::size_t base = static_cast<std::size_t>(cell) * 3;
        classic[cell] = classicCell(
            sums[base], sums[base + 1], sums[base + 2],
            impl_->boardGeometry.classicCounts[static_cast<std::size_t>(
                player * CellsPerBoard + cell)]);
    }
    result.probabilities = probabilities.front();
    result.classic = classic;
    result.sourceTime100ns = slot.sourceTime100ns;
    result.deliveryOrder = slot.deliveryOrder;
    result.player1 = player1;
    return true;
}

bool NvdecCudaPreprocessor::inferBoards(
    const std::vector<NvdecCudaBoardRequest>& requests,
    std::vector<D3D11BoardInference>& results,
    std::string& error) {
    error.clear();
    results.clear();
    if (!impl_) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (requests.empty()) return true;
    if (requests.size() > impl_->boardBatchCapacity) {
        error = "Direct NVDEC CUDA board batch exceeds configured capacity";
        return false;
    }
    for (const auto& request : requests) {
        if (!valid(request.handle)) {
            error = "Direct NVDEC CUDA board batch contains a stale handle";
            return false;
        }
    }

    using ClassicSums = std::array<std::uint32_t, CellsPerBoard * 3>;
    std::vector<ClassicSums> sums(requests.size());
    cudaError_t status = cudaSuccess;
    for (std::size_t requestIndex = 0;
         requestIndex < requests.size() && status == cudaSuccess;
         ++requestIndex) {
        const auto& request = requests[requestIndex];
        const int player = request.player1 ? 0 : 1;
        const Impl::Slot& slot = impl_->slots[request.handle.slot];
        const std::uint32_t* board = nullptr;
        if (impl_->rawBoardRing) {
            const auto& crop = impl_->boardGeometry.rawCrops[player];
            const std::size_t first = impl_->boardGeometry.bases[player];
            const std::size_t count = static_cast<std::size_t>(
                impl_->boardGeometry.widths[player] *
                impl_->boardGeometry.heights[player]);
            const std::uint8_t* raw = impl_->rawBoardRingData +
                static_cast<std::size_t>(request.handle.slot) *
                    impl_->frameBytes + crop.offset;
            std::uint32_t* scratch = impl_->boardScratch +
                requestIndex * impl_->boardGeometry.pixelCount;
            status = static_cast<cudaError_t>(trNv12RetainBoards(
                raw, crop.pitch, crop.width, crop.height,
                slot.matrix, slot.fullRange ? 1 : 0,
                impl_->rawBoardSamples + first, count,
                impl_->fastFp32 ? 1 : 0, scratch + first, impl_->stream));
            board = scratch;
        } else {
            board = impl_->boardRing +
                static_cast<std::size_t>(request.handle.slot) *
                    impl_->boardGeometry.pixelCount;
        }
        if (status == cudaSuccess) {
            status = static_cast<cudaError_t>(trBoardPixelsToTensor(
                board, impl_->resizeSamples[player],
                impl_->boardGeometry.resizeSamples[player].size(),
                impl_->boardTensor + requestIndex * TensorValues,
                impl_->fastFp32 ? 1 : 0, impl_->stream));
        }
        if (status == cudaSuccess) {
            status = static_cast<cudaError_t>(trBoardClassicSums(
                board, impl_->classicRects + player * CellsPerBoard,
                CellsPerBoard,
                impl_->classicSums + requestIndex * CellsPerBoard * 3,
                impl_->stream));
        }
    }
    if (status == cudaSuccess) {
        status = cudaMemcpyAsync(
            sums.data(), impl_->classicSums,
            requests.size() * sizeof(ClassicSums),
            cudaMemcpyDeviceToHost, impl_->stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
    if (status != cudaSuccess) {
        error = cudaError("direct NVDEC CUDA board batch tensor", status);
        return false;
    }

    std::vector<BoardProbabilities> probabilities;
    try {
        if (impl_->boardBatchScalarCnn) {
            // Keep the exact scalar CNN shape ([200,3,32,32]) and request
            // order while retaining the benefit of one preprocessing/D2H
            // synchronization for the complete request group. In
            // particular, this avoids turning a board request into a very
            // large [N*200,...] convolution batch that can lower GPU clocks
            // on power-limited laptop parts.
            probabilities.reserve(requests.size());
            for (std::size_t requestIndex = 0;
                 requestIndex < requests.size(); ++requestIndex) {
                std::string scalarError;
                auto one = impl_->boardModel->inferCudaDevice(
                    impl_->boardTensor + requestIndex * TensorValues,
                    1, scalarError);
                if (one.size() != 1 || !scalarError.empty()) {
                    error = scalarError.empty()
                        ? "Direct NVDEC CUDA scalar-CNN batch item returned "
                          "the wrong batch size"
                        : std::move(scalarError);
                    probabilities.clear();
                    break;
                }
                probabilities.push_back(std::move(one.front()));
            }
        } else {
            probabilities = impl_->boardModel->inferCudaDevice(
                impl_->boardTensor, requests.size(), error);
        }
    } catch (const std::exception& exception) {
        error = "Direct NVDEC CUDA board batch inference threw: " +
            std::string(exception.what());
    } catch (...) {
        error = "Direct NVDEC CUDA board batch inference threw an unknown exception";
    }
    if (!error.empty()) return false;
    if (probabilities.size() != requests.size()) {
        error = "Direct NVDEC CUDA board inference returned the wrong batch size";
        return false;
    }

    results.resize(requests.size());
    for (std::size_t requestIndex = 0; requestIndex < requests.size();
         ++requestIndex) {
        const auto& request = requests[requestIndex];
        const int player = request.player1 ? 0 : 1;
        const Impl::Slot& slot = impl_->slots[request.handle.slot];
        VisibleBoard classic{};
        for (int cell = 0; cell < CellsPerBoard; ++cell) {
            const std::size_t base = static_cast<std::size_t>(cell) * 3;
            classic[cell] = classicCell(
                sums[requestIndex][base],
                sums[requestIndex][base + 1],
                sums[requestIndex][base + 2],
                impl_->boardGeometry.classicCounts[static_cast<std::size_t>(
                    player * CellsPerBoard + cell)]);
        }
        auto& result = results[requestIndex];
        result.probabilities = probabilities[requestIndex];
        result.classic = classic;
        result.sourceTime100ns = slot.sourceTime100ns;
        result.deliveryOrder = slot.deliveryOrder;
        result.player1 = request.player1;
    }
    return true;
}

bool NvdecCudaPreprocessor::flushQueue(
    D3D11QueueBatchResult& result, std::string& error) {
    error.clear();
    result = {};
    if (!impl_) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (!impl_->queueFrames) return true;

    D3D11QueueBatchResult completedPipelineBatch;
    if (impl_->pipelineEnabled && impl_->pipelineFutureActive) {
        try {
            auto completed = impl_->pipelineFuture.get();
            impl_->pipelineFutureActive = false;
            if (!completed.error.empty()) {
                error = std::move(completed.error);
                return false;
            }
            completedPipelineBatch = std::move(completed.batch);
        } catch (const std::exception& exception) {
            impl_->pipelineFutureActive = false;
            error = "Direct NVDEC CUDA queue pipeline threw: " +
                std::string(exception.what());
            return false;
        } catch (...) {
            impl_->pipelineFutureActive = false;
            error = "Direct NVDEC CUDA queue pipeline threw an unknown exception";
            return false;
        }
    }

    const std::size_t frameCount = impl_->queueFrames;
    const std::size_t batch = frameCount * QueueSlots;
    D3D11QueueBatchResult candidate;
    std::vector<std::uint32_t> statistics(
        batch * StatisticFields);
    cudaError_t status = cudaMemcpyAsync(
        statistics.data(), impl_->queueStatistics,
        statistics.size() * sizeof(std::uint32_t), cudaMemcpyDeviceToHost,
        impl_->stream);
    if (status == cudaSuccess && impl_->pipelineEnabled) {
        status = cudaMemcpyAsync(
            impl_->queuePipelineTensor, impl_->queueTensor,
            batch * QueueFloats * sizeof(float), cudaMemcpyDeviceToDevice,
            impl_->stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
    if (status != cudaSuccess) {
        error = cudaError("cudaMemcpy(queue batch staging)", status);
        return false;
    }
    candidate.frames.resize(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t slot = 0; slot < QueueSlots; ++slot) {
            const std::size_t base =
                (frame * QueueSlots + slot) * StatisticFields;
            auto& destination = candidate.frames[frame].slots[slot];
            destination.sumR = statistics[base];
            destination.sumG = statistics[base + 1];
            destination.sumB = statistics[base + 2];
            destination.colorCount = statistics[base + 3];
            destination.darkCount = statistics[base + 4];
            const TrCudaCropRect& rect = impl_->queueGeometry.cropRects[slot];
            destination.cropPixelCount = static_cast<std::uint32_t>(
                std::max(0, rect.width) * std::max(0, rect.height));
        }
    }
    candidate.adaptiveEnabled = impl_->adaptiveEnabled;
    std::vector<bool> inferSlot(batch, true);
    if (impl_->adaptiveEnabled) {
        const auto statisticsSimilar = [&](const D3D11QueueSlotStatistics& a,
                                           const D3D11QueueSlotStatistics& b) {
            const auto countDifference = static_cast<std::uint64_t>(
                std::llabs(static_cast<long long>(a.colorCount) -
                           static_cast<long long>(b.colorCount)));
            if (countDifference > static_cast<std::uint64_t>(
                    impl_->adaptiveColorCountDelta)) return false;
            const double aDark = a.cropPixelCount
                ? static_cast<double>(a.darkCount) / a.cropPixelCount : 0.0;
            const double bDark = b.cropPixelCount
                ? static_cast<double>(b.darkCount) / b.cropPixelCount : 0.0;
            if (std::abs(aDark - bDark) >
                impl_->adaptiveDarkRatioDelta) return false;
            if ((a.colorCount == 0) != (b.colorCount == 0)) return false;
            if (a.colorCount && b.colorCount) {
                const auto averageDifference = [](std::uint64_t aSum,
                                                  std::uint64_t bSum,
                                                  std::uint32_t aCount,
                                                  std::uint32_t bCount) {
                    return std::abs(static_cast<double>(aSum) / aCount -
                                    static_cast<double>(bSum) / bCount);
                };
                if (averageDifference(a.sumR, b.sumR, a.colorCount,
                                      b.colorCount) > impl_->adaptiveRgbDelta ||
                    averageDifference(a.sumG, b.sumG, a.colorCount,
                                      b.colorCount) > impl_->adaptiveRgbDelta ||
                    averageDifference(a.sumB, b.sumB, a.colorCount,
                                      b.colorCount) > impl_->adaptiveRgbDelta) {
                    return false;
                }
            }
            return true;
        };
        auto planningPredictionValid =
            impl_->adaptivePreviousPredictionsValid;
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            for (std::size_t slot = 0; slot < QueueSlots; ++slot) {
                const std::size_t index = frame * QueueSlots + slot;
                const bool refreshDue =
                    impl_->adaptiveFramesSinceInference[slot] >=
                    impl_->adaptiveRefreshFrames;
                const bool similar =
                    impl_->adaptivePreviousStatisticsValid[slot] &&
                    statisticsSimilar(candidate.frames[frame].slots[slot],
                                      impl_->adaptivePreviousStatistics.slots[slot]);
                inferSlot[index] = !planningPredictionValid[slot] ||
                    !similar || refreshDue;
                impl_->adaptivePreviousStatistics.slots[slot] =
                    candidate.frames[frame].slots[slot];
                impl_->adaptivePreviousStatisticsValid[slot] = true;
                if (inferSlot[index]) {
                    impl_->adaptiveFramesSinceInference[slot] = 0;
                    planningPredictionValid[slot] = true;
                } else {
                    ++impl_->adaptiveFramesSinceInference[slot];
                }
            }
        }
    }
    impl_->queueFrames = 0;

    if (impl_->pipelineEnabled) {
        Impl* pipelineImpl = impl_.get();
        impl_->pipelineFuture = std::async(
            std::launch::async,
            [pipelineImpl, candidate = std::move(candidate), batch]() mutable {
                Impl::PipelineResult completed;
                completed.batch = std::move(candidate);
                const auto started = std::chrono::steady_clock::now();
                const cudaError_t deviceStatus =
                    cudaSetDevice(pipelineImpl->cudaDevice);
                if (deviceStatus != cudaSuccess) {
                    completed.error = cudaError(
                        "cudaSetDevice(direct queue pipeline)", deviceStatus);
                } else {
                    try {
                        completed.batch.predictions =
                            pipelineImpl->queueModel->inferCudaDevice(
                                pipelineImpl->queuePipelineTensor, batch,
                                completed.error);
                    } catch (const std::exception& exception) {
                        completed.error =
                            "Direct NVDEC CUDA queue inference threw: " +
                            std::string(exception.what());
                    } catch (...) {
                        completed.error =
                            "Direct NVDEC CUDA queue inference threw an unknown exception";
                    }
                    if (completed.error.empty() &&
                        completed.batch.predictions.size() != batch) {
                        completed.error =
                            "Direct NVDEC CUDA queue inference returned the wrong batch size";
                    }
                    if (completed.error.empty() && pipelineImpl->holdModel) {
                        std::string holdError;
                        const auto hold = pipelineImpl->holdModel->inferCudaDevice(
                            pipelineImpl->queuePipelineTensor, batch, holdError);
                        if (holdError.empty() && hold.size() == batch) {
                            for (std::size_t i = 0; i < batch; ++i) {
                                if (i % 6 == 0) completed.batch.predictions[i] = hold[i];
                            }
                        }
                    }
                }
                completed.batch.inferredFrames =
                    completed.batch.frames.size();
                completed.batch.inferredSlots = batch;
                completed.batch.inferenceRuns = batch ? 1 : 0;
                completed.batch.backgroundInferenceSeconds =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                return completed;
            });
        impl_->pipelineFutureActive = true;
        result = std::move(completedPipelineBatch);
        return true;
    }

    candidate.predictions.resize(batch);
    std::vector<std::size_t> selectedSlots;
    selectedSlots.reserve(batch);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        bool frameInferred = false;
        for (std::size_t slot = 0; slot < QueueSlots; ++slot) {
            const std::size_t index = frame * QueueSlots + slot;
            if (inferSlot[index]) {
                selectedSlots.push_back(index);
                ++candidate.inferredSlots;
                frameInferred = true;
            } else {
                ++candidate.reusedSlots;
            }
        }
        if (frameInferred) ++candidate.inferredFrames;
        else ++candidate.reusedFrames;
    }
    float* inferenceTensor = impl_->queueTensor;
    if (impl_->adaptiveEnabled && !selectedSlots.empty()) {
        std::size_t compactSlot = 0;
        for (std::size_t selected = 0; selected < selectedSlots.size();) {
            const std::size_t runStart = selectedSlots[selected];
            std::size_t runSlots = 1;
            while (selected + runSlots < selectedSlots.size() &&
                   selectedSlots[selected + runSlots] == runStart + runSlots) {
                ++runSlots;
            }
            status = cudaMemcpyAsync(
                impl_->queuePipelineTensor + compactSlot * QueueFloats,
                impl_->queueTensor + runStart * QueueFloats,
                runSlots * QueueFloats * sizeof(float),
                cudaMemcpyDeviceToDevice, impl_->stream);
            if (status != cudaSuccess) break;
            compactSlot += runSlots;
            selected += runSlots;
        }
        if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
        if (status != cudaSuccess) {
            error = cudaError("cudaMemcpy(adaptive queue compaction)", status);
            return false;
        }
        inferenceTensor = impl_->queuePipelineTensor;
    }
    try {
        const std::size_t inferenceBatch = impl_->adaptiveEnabled
            ? selectedSlots.size() : batch;
        auto selectedPredictions = inferenceBatch
            ? impl_->queueModel->inferCudaDevice(
                  inferenceTensor, inferenceBatch, error)
            : std::vector<QueueCnnPrediction>{};
        if (error.empty() && selectedPredictions.size() != inferenceBatch) {
            error = "Direct NVDEC CUDA queue inference returned the wrong compact batch size";
        }
        if (error.empty() && impl_->holdModel && inferenceBatch) {
            std::string holdError;
            const auto hold = impl_->holdModel->inferCudaDevice(
                inferenceTensor, inferenceBatch, holdError);
            if (holdError.empty() && hold.size() == inferenceBatch) {
                for (std::size_t k = 0; k < inferenceBatch; ++k) {
                    // In adaptive mode the compact batch holds only the
                    // re-inferred slots; map back to the full slot index.
                    const std::size_t full = impl_->adaptiveEnabled ? selectedSlots[k] : k;
                    if (full % 6 == 0) selectedPredictions[k] = hold[k];
                }
            }
        }
        if (error.empty() && impl_->adaptiveEnabled) {
            for (std::size_t selected = 0; selected < selectedSlots.size(); ++selected) {
                candidate.predictions[selectedSlots[selected]] =
                    selectedPredictions[selected];
            }
        } else if (error.empty()) {
            candidate.predictions = std::move(selectedPredictions);
        }
    } catch (const std::exception& exception) {
        error = "Direct NVDEC CUDA queue inference threw: " +
            std::string(exception.what());
    } catch (...) {
        error = "Direct NVDEC CUDA queue inference threw an unknown exception";
    }
    if (!error.empty()) return false;
    if (candidate.predictions.size() != batch) {
        error = "Direct NVDEC CUDA queue inference returned the wrong batch size";
        return false;
    }
    if (impl_->adaptiveEnabled) {
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            for (std::size_t slot = 0; slot < QueueSlots; ++slot) {
                const std::size_t index = frame * QueueSlots + slot;
                if (inferSlot[index]) {
                    impl_->adaptivePreviousPredictions[slot] =
                        candidate.predictions[index];
                    impl_->adaptivePreviousPredictionsValid[slot] = true;
                } else if (impl_->adaptivePreviousPredictionsValid[slot]) {
                    candidate.predictions[index] =
                        impl_->adaptivePreviousPredictions[slot];
                } else {
                    error = "Adaptive direct queue inference has no prior prediction";
                    return false;
                }
            }
        }
        candidate.inferenceRuns = selectedSlots.empty() ? 0 : 1;
    } else {
        candidate.inferredFrames = frameCount;
        candidate.inferredSlots = batch;
        candidate.inferenceRuns = batch ? 1 : 0;
    }
    result = std::move(candidate);
    return true;
}

bool NvdecCudaPreprocessor::pipelineEnabled() const {
    return impl_ && impl_->pipelineEnabled;
}

bool NvdecCudaPreprocessor::finishQueuePipeline(
    D3D11QueueBatchResult& result, std::string& error) {
    error.clear();
    result = {};
    if (!impl_) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (!impl_->pipelineEnabled || !impl_->pipelineFutureActive) return true;
    try {
        auto completed = impl_->pipelineFuture.get();
        impl_->pipelineFutureActive = false;
        if (!completed.error.empty()) {
            error = std::move(completed.error);
            return false;
        }
        result = std::move(completed.batch);
        return true;
    } catch (const std::exception& exception) {
        impl_->pipelineFutureActive = false;
        error = "Direct NVDEC CUDA queue pipeline threw: " +
            std::string(exception.what());
    } catch (...) {
        impl_->pipelineFutureActive = false;
        error = "Direct NVDEC CUDA queue pipeline threw an unknown exception";
    }
    return false;
}

bool NvdecCudaPreprocessor::valid(
    const D3D11BoardFrameHandle& handle) const {
    return impl_ && handle.generation != 0 &&
        handle.slot < impl_->slots.size() &&
        impl_->slots[handle.slot].generation == handle.generation;
}

bool NvdecCudaPreprocessor::convertNv12ToBgra(
    const void* nv12, std::size_t pitch, int sourceHeight, int matrix,
    bool fullRange, int x, int y, int width, int height,
    std::vector<std::uint8_t>& destination, std::string& error) {
    error.clear();
    if (!impl_ || !nv12 || pitch == 0 || sourceHeight <= 0) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (width <= 0 || height <= 0) {
        error = "Direct NVDEC CUDA BGRA region is empty";
        return false;
    }
    const std::size_t destinationPitch = static_cast<std::size_t>(width) * 4;
    const std::size_t bytes = destinationPitch * static_cast<std::size_t>(height);
    if (impl_->bgraScratchBytes < bytes) {
        cudaFree(impl_->bgraScratch);
        impl_->bgraScratch = nullptr;
        impl_->bgraScratchBytes = 0;
        void* allocated = nullptr;
        const cudaError_t allocatedStatus = cudaMalloc(&allocated, bytes);
        if (allocatedStatus != cudaSuccess) {
            error = cudaError("cudaMalloc(board BGRA scratch)",
                              allocatedStatus);
            return false;
        }
        impl_->bgraScratch = static_cast<std::uint8_t*>(allocated);
        impl_->bgraScratchBytes = bytes;
    }
    cudaError_t status = static_cast<cudaError_t>(trNv12ToBgraLinear(
        nv12, pitch, impl_->frameWidth, sourceHeight, x, y, width, height,
        matrix, fullRange ? 1 : 0, impl_->bgraScratch, destinationPitch,
        impl_->stream));
    if (status == cudaSuccess) {
        destination.resize(bytes);
        status = cudaMemcpyAsync(destination.data(), impl_->bgraScratch, bytes,
                                 cudaMemcpyDeviceToHost, impl_->stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
    if (status != cudaSuccess) {
        error = cudaError("direct NVDEC CUDA BGRA region", status);
        return false;
    }
    return true;
}

bool NvdecCudaPreprocessor::readBoardBgra(
    const D3D11BoardFrameHandle& handle, bool player1, int x, int y,
    int width, int height, std::vector<std::uint8_t>& destination,
    std::string& error) {
    error.clear();
    if (!impl_) {
        error = "Direct NVDEC CUDA preprocessor is not ready";
        return false;
    }
    if (!valid(handle)) {
        error = "Direct NVDEC CUDA board handle is stale";
        return false;
    }
    if (!impl_->rawBoardRing) {
        error = "Direct NVDEC CUDA board BGRA export requires the raw board ring";
        return false;
    }
    if (width <= 0 || height <= 0) {
        error = "Direct NVDEC CUDA board BGRA export region is empty";
        return false;
    }
    const int player = player1 ? 0 : 1;
    const auto& crop = impl_->boardGeometry.rawCrops[static_cast<std::size_t>(player)];
    if (x < crop.x || y < crop.y || x + width > crop.x + crop.width ||
        y + height > crop.y + crop.height) {
        error = "Direct NVDEC CUDA board BGRA export region is outside the retained crop";
        return false;
    }
    const std::size_t destinationPitch = static_cast<std::size_t>(width) * 4;
    const std::size_t bytes = destinationPitch * static_cast<std::size_t>(height);
    if (impl_->bgraScratchBytes < bytes) {
        cudaFree(impl_->bgraScratch);
        impl_->bgraScratch = nullptr;
        impl_->bgraScratchBytes = 0;
        void* allocated = nullptr;
        const cudaError_t allocatedStatus = cudaMalloc(&allocated, bytes);
        if (allocatedStatus != cudaSuccess) {
            error = cudaError("cudaMalloc(board BGRA scratch)",
                              allocatedStatus);
            return false;
        }
        impl_->bgraScratch = static_cast<std::uint8_t*>(allocated);
        impl_->bgraScratchBytes = bytes;
    }
    const Impl::Slot& slot = impl_->slots[handle.slot];
    const std::uint8_t* raw = impl_->rawBoardRingData +
        static_cast<std::size_t>(handle.slot) * impl_->frameBytes + crop.offset;
    cudaError_t status = static_cast<cudaError_t>(trNv12ToBgraLinear(
        raw, crop.pitch, crop.width, crop.height, x - crop.x, y - crop.y,
        width, height, slot.matrix, slot.fullRange ? 1 : 0,
        impl_->bgraScratch, destinationPitch, impl_->stream));
    if (status == cudaSuccess) {
        destination.resize(bytes);
        status = cudaMemcpyAsync(destination.data(), impl_->bgraScratch, bytes,
                                 cudaMemcpyDeviceToHost, impl_->stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(impl_->stream);
    if (status != cudaSuccess) {
        error = cudaError("direct NVDEC CUDA board BGRA export", status);
        return false;
    }
    return true;
}

bool NvdecCudaPreprocessor::ready() const { return impl_ != nullptr; }
bool NvdecCudaPreprocessor::rawBoardRingEnabled() const {
    return impl_ && impl_->rawBoardRing;
}
std::size_t NvdecCudaPreprocessor::capacity() const {
    return impl_ ? impl_->slots.size() : 0;
}
std::size_t NvdecCudaPreprocessor::bytesPerFrame() const {
    return impl_ ? impl_->frameBytes : 0;
}
std::size_t NvdecCudaPreprocessor::boardBatchCapacity() const {
    return impl_ ? impl_->boardBatchCapacity : 0;
}
std::size_t NvdecCudaPreprocessor::queueFrameCount() const {
    return impl_ ? impl_->queueFrames : 0;
}
void* NvdecCudaPreprocessor::cudaStreamHandle() const {
    return impl_ ? reinterpret_cast<void*>(impl_->stream) : nullptr;
}

} // namespace tr

#endif
