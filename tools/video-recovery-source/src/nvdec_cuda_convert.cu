#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>

namespace {

__device__ unsigned char quantize(float value) {
    value = fminf(255.0f, fmaxf(0.0f, value));
    return static_cast<unsigned char>(floorf(value + 0.5f));
}

__global__ void nv12ToBgraKernel(const unsigned char* source,
                                 std::size_t pitch,
                                 int width,
                                 int sourceHeight,
                                 int regionX,
                                 int regionY,
                                 int regionWidth,
                                 int regionHeight,
                                 int matrix,
                                 bool fullRange,
                                 cudaSurfaceObject_t destination) {
    const int localX = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int localY = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (localX >= regionWidth || localY >= regionHeight) return;
    const int x = regionX + localX;
    const int y = regionY + localY;
    if (x >= width) return;
    if (y >= sourceHeight) {
        surf2Dwrite(make_uchar4(0, 0, 0, 255), destination,
                    x * static_cast<int>(sizeof(uchar4)), y);
        return;
    }
    const float yy = source[static_cast<std::size_t>(y) * pitch + x];
    const std::size_t uvOffset = static_cast<std::size_t>(sourceHeight) * pitch +
        static_cast<std::size_t>(y / 2) * pitch + (x & ~1);
    const float uu = source[uvOffset] - 128.0f;
    const float vv = source[uvOffset + 1] - 128.0f;
    const float c = fullRange ? yy : fmaxf(0.0f, yy - 16.0f) * (255.0f / 219.0f);
    float r, g, b;
    if (matrix == 1) { // ITU-R BT.709
        r = c + 1.5748f * vv;
        g = c - 0.187324f * uu - 0.468124f * vv;
        b = c + 1.8556f * uu;
    } else { // SMPTE 170M / BT.601 and unspecified sources
        r = c + 1.402f * vv;
        g = c - 0.344136f * uu - 0.714136f * vv;
        b = c + 1.772f * uu;
    }
    surf2Dwrite(make_uchar4(quantize(b), quantize(g), quantize(r), 255),
                destination, x * static_cast<int>(sizeof(uchar4)), y);
}

// Linear-memory variant of the same conversion. The one-pass board capture
// keeps raw NV12 crops on the device and must turn exactly one requested
// rectangle into the BGRA bytes the legacy range reader would have produced,
// so the arithmetic below is a copy of nv12ToBgraKernel.
__global__ void nv12ToBgraLinearKernel(const unsigned char* source,
                                       std::size_t pitch,
                                       int width,
                                       int sourceHeight,
                                       int regionX,
                                       int regionY,
                                       int regionWidth,
                                       int regionHeight,
                                       int matrix,
                                       bool fullRange,
                                       unsigned char* destination,
                                       std::size_t destinationPitch) {
    const int localX = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int localY = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (localX >= regionWidth || localY >= regionHeight) return;
    unsigned char* out = destination +
        static_cast<std::size_t>(localY) * destinationPitch +
        static_cast<std::size_t>(localX) * 4;
    const int x = regionX + localX;
    const int y = regionY + localY;
    if (x >= width || y >= sourceHeight) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 255;
        return;
    }
    const float yy = source[static_cast<std::size_t>(y) * pitch + x];
    const std::size_t uvOffset = static_cast<std::size_t>(sourceHeight) * pitch +
        static_cast<std::size_t>(y / 2) * pitch + (x & ~1);
    const float uu = source[uvOffset] - 128.0f;
    const float vv = source[uvOffset + 1] - 128.0f;
    const float c = fullRange ? yy : fmaxf(0.0f, yy - 16.0f) * (255.0f / 219.0f);
    float r, g, b;
    if (matrix == 1) { // ITU-R BT.709
        r = c + 1.5748f * vv;
        g = c - 0.187324f * uu - 0.468124f * vv;
        b = c + 1.8556f * uu;
    } else { // SMPTE 170M / BT.601 and unspecified sources
        r = c + 1.402f * vv;
        g = c - 0.344136f * uu - 0.714136f * vv;
        b = c + 1.772f * uu;
    }
    out[0] = quantize(b);
    out[1] = quantize(g);
    out[2] = quantize(r);
    out[3] = 255;
}

} // namespace

extern "C" int trNv12ToBgraLinear(const void* source,
                                  std::size_t pitch,
                                  int width,
                                  int sourceHeight,
                                  int regionX,
                                  int regionY,
                                  int regionWidth,
                                  int regionHeight,
                                  int matrix,
                                  int fullRange,
                                  void* destination,
                                  std::size_t destinationPitch,
                                  cudaStream_t stream) {
    if (!source || !destination || regionWidth <= 0 || regionHeight <= 0) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    const dim3 block(32, 8);
    const dim3 grid((regionWidth + block.x - 1) / block.x,
                    (regionHeight + block.y - 1) / block.y);
    nv12ToBgraLinearKernel<<<grid, block, 0, stream>>>(
        static_cast<const unsigned char*>(source), pitch, width, sourceHeight,
        regionX, regionY, regionWidth, regionHeight, matrix, fullRange != 0,
        static_cast<unsigned char*>(destination), destinationPitch);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int trNv12ToBgraRegions(void* source,
                                   std::size_t pitch,
                                   int width,
                                   int sourceHeight,
                                   int destinationHeight,
                                   int matrix,
                                   int fullRange,
                                   const int* regions,
                                   int regionCount,
                                   cudaArray_t destination,
                                   cudaStream_t stream) {
    cudaResourceDesc resource{};
    resource.resType = cudaResourceTypeArray;
    resource.res.array.array = destination;
    cudaSurfaceObject_t surface = 0;
    cudaError_t status = cudaCreateSurfaceObject(&surface, &resource);
    if (status != cudaSuccess) return static_cast<int>(status);
    const dim3 block(32, 8);
    const int fallback[4]{0, 0, width, destinationHeight};
    if (!regions || regionCount <= 0) {
        regions = fallback;
        regionCount = 1;
    }
    for (int index = 0; index < regionCount; ++index) {
        const int* region = regions + index * 4;
        if (region[2] <= 0 || region[3] <= 0) continue;
        const dim3 grid((region[2] + block.x - 1) / block.x,
                        (region[3] + block.y - 1) / block.y);
        nv12ToBgraKernel<<<grid, block, 0, stream>>>(
            static_cast<const unsigned char*>(source), pitch, width,
            sourceHeight, region[0], region[1], region[2], region[3],
            matrix, fullRange != 0, surface);
        status = cudaGetLastError();
        if (status != cudaSuccess) break;
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(stream);
    const cudaError_t destroy = cudaDestroySurfaceObject(surface);
    return static_cast<int>(status == cudaSuccess ? destroy : status);
}
