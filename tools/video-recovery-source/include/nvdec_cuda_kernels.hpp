#pragma once

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

struct TrCudaBoardSample {
    int x0, y0, x1, y1;
    double fx, fy;
};

struct TrCudaResizeSample {
    std::uint32_t offset00, offset10, offset01, offset11;
    double fx, fy;
};

struct TrCudaClassicRect {
    std::uint32_t boardBase, boardWidth, x0, y0;
    std::uint32_t x1, y1, unused0, unused1;
};

struct TrCudaQueueSample {
    int x0, y0, x1, y1;
    float fx, fy;
    std::uint32_t valid, unused;
};

struct TrCudaLegacyPoint {
    int x, y;
    std::uint32_t slot, unused;
};

struct TrCudaCropRect {
    int x, y, width, height;
};

extern "C" int trNv12RetainBoards(
    const void* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, int fullRange,
    const TrCudaBoardSample* samples, std::size_t sampleCount, int fastFp32,
    std::uint32_t* boardPixels, cudaStream_t stream);

extern "C" int trNv12AppendQueue(
    const void* source, std::size_t pitch, int width, int sourceHeight,
    int matrix, int fullRange,
    const TrCudaQueueSample* samples, std::size_t sampleCount,
    const TrCudaLegacyPoint* legacyPoints, std::size_t legacyPointCount,
    const TrCudaCropRect* cropRects, int maxCropWidth, int maxCropHeight,
    int darkPixelLimit, std::size_t frameIndex,
    float* tensor, std::uint32_t* statistics, cudaStream_t stream);

extern "C" int trBoardPixelsToTensor(
    const std::uint32_t* boardPixels,
    const TrCudaResizeSample* samples, std::size_t sampleCount,
    float* tensor, int fastFp32, cudaStream_t stream);

extern "C" int trBoardClassicSums(
    const std::uint32_t* boardPixels,
    const TrCudaClassicRect* rects, std::size_t rectCount,
    std::uint32_t* sums, cudaStream_t stream);

// Resize the largest centered 16:9 rectangle of one NV12 frame directly into
// the scene classifier's [batch,1,90,160] normalized CUDA tensor. For the
// ordinary 1280x720/1920x1080 Switch captures this exactly reproduces area
// downsampling by averaging each integer source block.
extern "C" int trNv12AppendSceneLuma(
    const void* source, std::size_t pitch,
    int visibleWidth, int visibleHeight, int surfaceHeight,
    int matrix, int fullRange, std::size_t batchIndex,
    float* tensor, cudaStream_t stream);

// One-pass range analysis: convert exactly one rectangle of a retained raw
// NV12 board crop into the same BGRA bytes the legacy range reader produces.
extern "C" int trNv12ToBgraLinear(
    const void* source, std::size_t pitch, int width, int sourceHeight,
    int regionX, int regionY, int regionWidth, int regionHeight,
    int matrix, int fullRange, void* destination,
    std::size_t destinationPitch, cudaStream_t stream);

#endif
