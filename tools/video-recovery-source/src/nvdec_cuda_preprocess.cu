#include "nvdec_cuda_kernels.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace {

constexpr int QueueSlots = 12;
constexpr int PixelsPerCrop = 48 * 48;
constexpr int FloatsPerCrop = 3 * PixelsPerCrop;
constexpr int StatisticFields = 5;
constexpr int SceneWidth = 160;
constexpr int SceneHeight = 90;
constexpr int ScenePixels = SceneWidth * SceneHeight;

struct RgbBytes {
    unsigned char r, g, b;
};

__device__ unsigned char quantize(float value) {
    value = fminf(255.0f, fmaxf(0.0f, value));
    return static_cast<unsigned char>(floorf(value + 0.5f));
}

__device__ RgbBytes nv12Rgb(const unsigned char* source,
                            std::size_t pitch,
                            int width,
                            int sourceHeight,
                            int x,
                            int y,
                            int matrix,
                            bool fullRange) {
    x = max(0, min(width - 1, x));
    if (y < 0 || y >= sourceHeight) return {0, 0, 0};
    const float yy = source[static_cast<std::size_t>(y) * pitch + x];
    const std::size_t uvOffset = static_cast<std::size_t>(sourceHeight) * pitch +
        static_cast<std::size_t>(y / 2) * pitch + (x & ~1);
    const float uu = source[uvOffset] - 128.0f;
    const float vv = source[uvOffset + 1] - 128.0f;
    const float c = fullRange ? yy : fmaxf(0.0f, yy - 16.0f) * (255.0f / 219.0f);
    float r, g, b;
    if (matrix == 1) {
        r = c + 1.5748f * vv;
        g = c - 0.187324f * uu - 0.468124f * vv;
        b = c + 1.8556f * uu;
    } else {
        r = c + 1.402f * vv;
        g = c - 0.344136f * uu - 0.714136f * vv;
        b = c + 1.772f * uu;
    }
    return {quantize(r), quantize(g), quantize(b)};
}

__device__ unsigned int canvasByte(double value) {
    value = fmin(255.0, fmax(0.0, value));
    const unsigned int lower = static_cast<unsigned int>(floor(value));
    const double fraction = value - static_cast<double>(lower);
    return fraction > 0.5 || (fraction == 0.5 && (lower & 1u))
        ? lower + 1u : lower;
}

__device__ unsigned int canvasByteFast(float value) {
    return __float2uint_rn(fminf(255.0f, fmaxf(0.0f, value)));
}

__device__ double blend(double a, double b, double c, double d,
                        double fx, double fy) {
    const double top = a * (1.0 - fx) + b * fx;
    const double bottom = c * (1.0 - fx) + d * fx;
    return top * (1.0 - fy) + bottom * fy;
}

__device__ float blendFast(float a, float b, float c, float d,
                           float fx, float fy) {
    const float top = fmaf(b - a, fx, a);
    const float bottom = fmaf(d - c, fx, c);
    return fmaf(bottom - top, fy, top);
}

__device__ double packedChannel(std::uint32_t packed, unsigned shift) {
    return static_cast<double>((packed >> shift) & 255u);
}

__global__ void retainBoardsKernel(
    const unsigned char* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, bool fullRange,
    const TrCudaBoardSample* samples, std::size_t sampleCount,
    std::uint32_t* output) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= sampleCount) return;
    const TrCudaBoardSample s = samples[index];
    const RgbBytes p00 = nv12Rgb(source, pitch, width, sourceHeight,
                                 s.x0, s.y0, matrix, fullRange);
    RgbBytes p10 = p00, p01 = p00, p11 = p00;
    if (s.fx != 0.0 || s.fy != 0.0) {
        p10 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x1, s.y0, matrix, fullRange);
        p01 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x0, s.y1, matrix, fullRange);
        p11 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x1, s.y1, matrix, fullRange);
    }
    const unsigned int r = canvasByte(blend(
        p00.r, p10.r, p01.r, p11.r, s.fx, s.fy));
    const unsigned int g = canvasByte(blend(
        p00.g, p10.g, p01.g, p11.g, s.fx, s.fy));
    const unsigned int b = canvasByte(blend(
        p00.b, p10.b, p01.b, p11.b, s.fx, s.fy));
    output[index] = r | (g << 8) | (b << 16);
}

__global__ void retainBoardsFastKernel(
    const unsigned char* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, bool fullRange,
    const TrCudaBoardSample* samples, std::size_t sampleCount,
    std::uint32_t* output) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= sampleCount) return;
    const TrCudaBoardSample s = samples[index];
    const RgbBytes p00 = nv12Rgb(source, pitch, width, sourceHeight,
                                 s.x0, s.y0, matrix, fullRange);
    RgbBytes p10 = p00, p01 = p00, p11 = p00;
    if (s.fx != 0.0 || s.fy != 0.0) {
        p10 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x1, s.y0, matrix, fullRange);
        p01 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x0, s.y1, matrix, fullRange);
        p11 = nv12Rgb(source, pitch, width, sourceHeight,
                      s.x1, s.y1, matrix, fullRange);
    }
    const float fx = static_cast<float>(s.fx);
    const float fy = static_cast<float>(s.fy);
    const unsigned int r = canvasByteFast(blendFast(
        p00.r, p10.r, p01.r, p11.r, fx, fy));
    const unsigned int g = canvasByteFast(blendFast(
        p00.g, p10.g, p01.g, p11.g, fx, fy));
    const unsigned int b = canvasByteFast(blendFast(
        p00.b, p10.b, p01.b, p11.b, fx, fy));
    output[index] = r | (g << 8) | (b << 16);
}

__global__ void queueTensorKernel(
    const unsigned char* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, bool fullRange,
    const TrCudaQueueSample* samples, std::size_t sampleCount,
    std::size_t frameIndex, float* tensor) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= sampleCount) return;
    const TrCudaQueueSample sp = samples[index];
    const std::size_t slot = index / PixelsPerCrop;
    const std::size_t pixel = index - slot * PixelsPerCrop;
    const std::size_t base = (frameIndex * QueueSlots + slot) * FloatsPerCrop;
    if (!sp.valid) {
        tensor[base + pixel] = -1.0f;
        tensor[base + PixelsPerCrop + pixel] = -1.0f;
        tensor[base + 2 * PixelsPerCrop + pixel] = -1.0f;
        return;
    }
    const RgbBytes p00 = nv12Rgb(source, pitch, width, sourceHeight,
                                 sp.x0, sp.y0, matrix, fullRange);
    const RgbBytes p10 = nv12Rgb(source, pitch, width, sourceHeight,
                                 sp.x1, sp.y0, matrix, fullRange);
    const RgbBytes p01 = nv12Rgb(source, pitch, width, sourceHeight,
                                 sp.x0, sp.y1, matrix, fullRange);
    const RgbBytes p11 = nv12Rgb(source, pitch, width, sourceHeight,
                                 sp.x1, sp.y1, matrix, fullRange);
    const float topR = p00.r * (1.0f - sp.fx) + p10.r * sp.fx;
    const float topG = p00.g * (1.0f - sp.fx) + p10.g * sp.fx;
    const float topB = p00.b * (1.0f - sp.fx) + p10.b * sp.fx;
    const float bottomR = p01.r * (1.0f - sp.fx) + p11.r * sp.fx;
    const float bottomG = p01.g * (1.0f - sp.fx) + p11.g * sp.fx;
    const float bottomB = p01.b * (1.0f - sp.fx) + p11.b * sp.fx;
    tensor[base + pixel] =
        (topR * (1.0f - sp.fy) + bottomR * sp.fy) / 127.5f - 1.0f;
    tensor[base + PixelsPerCrop + pixel] =
        (topG * (1.0f - sp.fy) + bottomG * sp.fy) / 127.5f - 1.0f;
    tensor[base + 2 * PixelsPerCrop + pixel] =
        (topB * (1.0f - sp.fy) + bottomB * sp.fy) / 127.5f - 1.0f;
}

__global__ void queueLegacyKernel(
    const unsigned char* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, bool fullRange,
    const TrCudaLegacyPoint* points, std::size_t pointCount,
    std::size_t frameIndex, std::uint32_t* statistics) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= pointCount) return;
    const TrCudaLegacyPoint point = points[index];
    const RgbBytes rgb = nv12Rgb(source, pitch, width, sourceHeight,
                                 point.x, point.y, matrix, fullRange);
    if (rgb.r > 50 || rgb.g > 50 || rgb.b > 50) {
        const std::size_t base = (frameIndex * QueueSlots + point.slot) *
            StatisticFields;
        atomicAdd(statistics + base, static_cast<std::uint32_t>(rgb.r));
        atomicAdd(statistics + base + 1, static_cast<std::uint32_t>(rgb.g));
        atomicAdd(statistics + base + 2, static_cast<std::uint32_t>(rgb.b));
        atomicAdd(statistics + base + 3, 1u);
    }
}

__global__ void queueDarkKernel(
    const unsigned char* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, bool fullRange,
    const TrCudaCropRect* rects, int darkPixelLimit,
    std::size_t frameIndex, std::uint32_t* statistics) {
    const int slot = static_cast<int>(blockIdx.z);
    if (slot >= QueueSlots) return;
    const TrCudaCropRect rect = rects[slot];
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= rect.width || y >= rect.height) return;
    const RgbBytes rgb = nv12Rgb(source, pitch, width, sourceHeight,
                                 rect.x + x, rect.y + y, matrix, fullRange);
    if (max(rgb.r, max(rgb.g, rgb.b)) <= darkPixelLimit) {
        const std::size_t base = (frameIndex * QueueSlots + slot) *
            StatisticFields;
        atomicAdd(statistics + base + 4, 1u);
    }
}

__global__ void sceneLumaKernel(
    const unsigned char* source, std::size_t pitch,
    int visibleWidth, int visibleHeight, int surfaceHeight,
    int matrix, bool fullRange, std::size_t batchIndex, float* tensor) {
    const int outputIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (outputIndex >= ScenePixels) return;
    const int outputX = outputIndex % SceneWidth;
    const int outputY = outputIndex / SceneWidth;

    const int unit = min(visibleWidth / 16, visibleHeight / 9);
    const int cropWidth = max(1, unit * 16);
    const int cropHeight = max(1, unit * 9);
    const int cropX = max(0, (visibleWidth - cropWidth) / 2);
    const int cropY = max(0, (visibleHeight - cropHeight) / 2);
    int x0 = cropX + outputX * cropWidth / SceneWidth;
    int x1 = cropX + (outputX + 1) * cropWidth / SceneWidth;
    int y0 = cropY + outputY * cropHeight / SceneHeight;
    int y1 = cropY + (outputY + 1) * cropHeight / SceneHeight;
    x0 = max(0, min(visibleWidth - 1, x0));
    y0 = max(0, min(visibleHeight - 1, y0));
    x1 = max(x0 + 1, min(visibleWidth, x1));
    y1 = max(y0 + 1, min(visibleHeight, y1));

    unsigned int sum = 0;
    unsigned int count = 0;
    for (int y = y0; y < y1; ++y) {
        const unsigned char* row =
            source + static_cast<std::size_t>(y) * pitch;
        for (int x = x0; x < x1; ++x) {
            sum += row[x];
            ++count;
        }
    }
    // cv::resize(INTER_AREA) returns uint8 here. Preserve that rounding point
    // before expanding limited-range NV12 luma to the classifier's byte
    // domain; keeping a fractional byte caused boundary-only class drift.
    float luma = count
        ? static_cast<float>((sum + count / 2) / count) : 0.0f;
    if (!fullRange) luma = (luma - 16.0f) * (255.0f / 219.0f);
    luma = floorf(fminf(255.0f, fmaxf(0.0f, luma)) + 0.5f);
    tensor[batchIndex * ScenePixels + static_cast<std::size_t>(outputIndex)] =
        luma * (1.0f / 127.5f) - 1.0f;
    (void)matrix;
    (void)surfaceHeight;
}

__global__ void boardTensorKernel(
    const std::uint32_t* boardPixels,
    const TrCudaResizeSample* samples, std::size_t sampleCount,
    float* tensor) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= sampleCount) return;
    const TrCudaResizeSample s = samples[index];
    const std::uint32_t p00 = boardPixels[s.offset00];
    const std::uint32_t p10 = boardPixels[s.offset10];
    const std::uint32_t p01 = boardPixels[s.offset01];
    const std::uint32_t p11 = boardPixels[s.offset11];
    const std::size_t y = index / 320;
    const std::size_t x = index % 320;
    const std::size_t cell = (y / 32) * 10 + x / 32;
    const std::size_t pixel = (y % 32) * 32 + x % 32;
    const std::size_t base = cell * 3072 + pixel;
    const unsigned int r = canvasByte(blend(
        packedChannel(p00, 0), packedChannel(p10, 0),
        packedChannel(p01, 0), packedChannel(p11, 0),
        s.fx, s.fy));
    const unsigned int g = canvasByte(blend(
        packedChannel(p00, 8), packedChannel(p10, 8),
        packedChannel(p01, 8), packedChannel(p11, 8),
        s.fx, s.fy));
    const unsigned int b = canvasByte(blend(
        packedChannel(p00, 16), packedChannel(p10, 16),
        packedChannel(p01, 16), packedChannel(p11, 16),
        s.fx, s.fy));
    tensor[base] = r / 255.0f;
    tensor[base + 1024] = g / 255.0f;
    tensor[base + 2048] = b / 255.0f;
}

__global__ void boardTensorFastKernel(
    const std::uint32_t* boardPixels,
    const TrCudaResizeSample* samples, std::size_t sampleCount,
    float* tensor) {
    const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (index >= sampleCount) return;
    const TrCudaResizeSample s = samples[index];
    const std::uint32_t p00 = boardPixels[s.offset00];
    const std::uint32_t p10 = boardPixels[s.offset10];
    const std::uint32_t p01 = boardPixels[s.offset01];
    const std::uint32_t p11 = boardPixels[s.offset11];
    const std::size_t y = index / 320;
    const std::size_t x = index % 320;
    const std::size_t cell = (y / 32) * 10 + x / 32;
    const std::size_t pixel = (y % 32) * 32 + x % 32;
    const std::size_t base = cell * 3072 + pixel;
    const float fx = static_cast<float>(s.fx);
    const float fy = static_cast<float>(s.fy);
    const unsigned int r = canvasByteFast(blendFast(
        static_cast<float>(p00 & 255u), static_cast<float>(p10 & 255u),
        static_cast<float>(p01 & 255u), static_cast<float>(p11 & 255u), fx, fy));
    const unsigned int g = canvasByteFast(blendFast(
        static_cast<float>((p00 >> 8) & 255u), static_cast<float>((p10 >> 8) & 255u),
        static_cast<float>((p01 >> 8) & 255u), static_cast<float>((p11 >> 8) & 255u), fx, fy));
    const unsigned int b = canvasByteFast(blendFast(
        static_cast<float>((p00 >> 16) & 255u), static_cast<float>((p10 >> 16) & 255u),
        static_cast<float>((p01 >> 16) & 255u), static_cast<float>((p11 >> 16) & 255u), fx, fy));
    tensor[base] = r / 255.0f;
    tensor[base + 1024] = g / 255.0f;
    tensor[base + 2048] = b / 255.0f;
}

__global__ void classicSumsKernel(
    const std::uint32_t* boardPixels,
    const TrCudaClassicRect* rects, std::size_t rectCount,
    std::uint32_t* sums) {
    const std::size_t cell = static_cast<std::size_t>(blockIdx.x) * blockDim.x +
        threadIdx.x;
    if (cell >= rectCount) return;
    const TrCudaClassicRect rect = rects[cell];
    std::uint32_t sumR = 0, sumG = 0, sumB = 0;
    for (std::uint32_t y = rect.y0; y < rect.y1; ++y) {
        for (std::uint32_t x = rect.x0; x < rect.x1; ++x) {
            const std::uint32_t pixel =
                boardPixels[rect.boardBase + y * rect.boardWidth + x];
            sumR += pixel & 255u;
            sumG += (pixel >> 8) & 255u;
            sumB += (pixel >> 16) & 255u;
        }
    }
    sums[cell * 3] = sumR;
    sums[cell * 3 + 1] = sumG;
    sums[cell * 3 + 2] = sumB;
}

} // namespace

extern "C" int trNv12RetainBoards(
    const void* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, int fullRange,
    const TrCudaBoardSample* samples, std::size_t sampleCount, int fastFp32,
    std::uint32_t* boardPixels, cudaStream_t stream) {
    const unsigned blocks = static_cast<unsigned>((sampleCount + 255) / 256);
    if (fastFp32) {
        retainBoardsFastKernel<<<blocks, 256, 0, stream>>>(
            static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
            matrix, fullRange != 0, samples, sampleCount, boardPixels);
    } else {
        retainBoardsKernel<<<blocks, 256, 0, stream>>>(
            static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
            matrix, fullRange != 0, samples, sampleCount, boardPixels);
    }
    return static_cast<int>(cudaGetLastError());
}

extern "C" int trNv12AppendQueue(
    const void* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, int fullRange,
    const TrCudaQueueSample* samples, std::size_t sampleCount,
    const TrCudaLegacyPoint* legacyPoints, std::size_t legacyPointCount,
    const TrCudaCropRect* cropRects, int maxCropWidth, int maxCropHeight,
    int darkPixelLimit, std::size_t frameIndex,
    float* tensor, std::uint32_t* statistics, cudaStream_t stream) {
    std::uint32_t* frameStatistics = statistics +
        frameIndex * QueueSlots * StatisticFields;
    cudaError_t status = cudaMemsetAsync(
        frameStatistics, 0,
        QueueSlots * StatisticFields * sizeof(std::uint32_t), stream);
    if (status != cudaSuccess) return static_cast<int>(status);
    queueTensorKernel<<<static_cast<unsigned>((sampleCount + 255) / 256), 256,
                        0, stream>>>(
        static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
        matrix, fullRange != 0, samples, sampleCount, frameIndex, tensor);
    status = cudaGetLastError();
    if (status != cudaSuccess) return static_cast<int>(status);
    queueLegacyKernel<<<static_cast<unsigned>((legacyPointCount + 255) / 256),
                        256, 0, stream>>>(
        static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
        matrix, fullRange != 0, legacyPoints, legacyPointCount,
        frameIndex, statistics);
    status = cudaGetLastError();
    if (status != cudaSuccess) return static_cast<int>(status);
    const dim3 block(8, 8, 1);
    const dim3 grid((maxCropWidth + 7) / 8, (maxCropHeight + 7) / 8,
                    QueueSlots);
    queueDarkKernel<<<grid, block, 0, stream>>>(
        static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
        matrix, fullRange != 0, cropRects, darkPixelLimit,
        frameIndex, statistics);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int trBoardPixelsToTensor(
    const std::uint32_t* boardPixels,
    const TrCudaResizeSample* samples, std::size_t sampleCount,
    float* tensor, int fastFp32, cudaStream_t stream) {
    const unsigned blocks = static_cast<unsigned>((sampleCount + 255) / 256);
    if (fastFp32) {
        boardTensorFastKernel<<<blocks, 256, 0, stream>>>(
            boardPixels, samples, sampleCount, tensor);
    } else {
        boardTensorKernel<<<blocks, 256, 0, stream>>>(
            boardPixels, samples, sampleCount, tensor);
    }
    return static_cast<int>(cudaGetLastError());
}

extern "C" int trBoardClassicSums(
    const std::uint32_t* boardPixels,
    const TrCudaClassicRect* rects, std::size_t rectCount,
    std::uint32_t* sums, cudaStream_t stream) {
    classicSumsKernel<<<static_cast<unsigned>((rectCount + 63) / 64), 64,
                        0, stream>>>(
        boardPixels, rects, rectCount, sums);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int trNv12AppendSceneLuma(
    const void* source, std::size_t pitch,
    int visibleWidth, int visibleHeight, int surfaceHeight,
    int matrix, int fullRange, std::size_t batchIndex,
    float* tensor, cudaStream_t stream) {
    if (!source || !tensor || pitch == 0 || visibleWidth <= 0 ||
        visibleHeight <= 0 || surfaceHeight < visibleHeight) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    constexpr int Threads = 256;
    sceneLumaKernel<<<(ScenePixels + Threads - 1) / Threads, Threads, 0, stream>>>(
        static_cast<const unsigned char*>(source), pitch,
        visibleWidth, visibleHeight, surfaceHeight,
        matrix, fullRange != 0, batchIndex, tensor);
    return static_cast<int>(cudaGetLastError());
}

#endif
