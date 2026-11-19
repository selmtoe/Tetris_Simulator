#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "d3d11_queue_preprocessor.hpp"
#include "d3d11_board_preprocessor.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT)

#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tr {
namespace {

constexpr int InputSize = 48;
constexpr int PixelsPerCrop = InputSize * InputSize;
constexpr int FloatsPerCrop = PixelsPerCrop * 3;
constexpr int SlotsPerFrame = 12;
constexpr int StatisticFields = 5;

enum StatisticField : std::uint32_t {
    SumR = 0,
    SumG = 1,
    SumB = 2,
    ColorCount = 3,
    DarkCount = 4,
};

std::string hrText(HRESULT hr) {
    std::ostringstream out;
    out << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

std::string windowsErrorText(DWORD value) {
    std::ostringstream out;
    out << "Win32 error " << value;
    return out.str();
}

struct MappedCrop {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// StructuredBuffer layouts are explicit and verified against the HLSL types.
struct GpuSample {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    float fx = 0;
    float fy = 0;
    std::uint32_t valid = 0;
    std::uint32_t unused = 0;
};
static_assert(sizeof(GpuSample) == 32);

struct GpuRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
static_assert(sizeof(GpuRect) == 16);

struct GpuLegacyPoint {
    int x = 0;
    int y = 0;
    std::uint32_t slot = 0;
    std::uint32_t unused = 0;
};
static_assert(sizeof(GpuLegacyPoint) == 16);

struct ShaderParameters {
    std::uint32_t cnnSampleCount = 0;
    std::uint32_t legacyPointCount = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t darkPixelLimit = 0;
};
static_assert(sizeof(ShaderParameters) == 16);

struct FusedShaderParameters {
    std::uint32_t boardSampleCount = 0;
    std::uint32_t queueSampleCount = 0;
    std::uint32_t frameIndex = 0;
    std::uint32_t unused0 = 0;
};
static_assert(sizeof(FusedShaderParameters) == 16);

struct Geometry {
    std::vector<GpuSample> cnnSamples;
    std::vector<GpuLegacyPoint> legacyPoints;
    std::array<GpuRect, SlotsPerFrame> cropRects{};
    int maxCropWidth = 0;
    int maxCropHeight = 0;
};

struct ContentMapping {
    double x = 0;
    double y = 0;
    double scale = 1;
};

ContentMapping contentMapping(int width, int height) {
    constexpr double TargetAspect = 16.0 / 9.0;
    const double sourceAspect = static_cast<double>(width) / std::max(1, height);
    double cropWidth = 0;
    double cropX = 0;
    double cropY = 0;
    if (sourceAspect > TargetAspect) {
        cropWidth = height * TargetAspect;
        cropX = (width - cropWidth) / 2.0;
    } else {
        cropWidth = width;
        cropY = (height - cropWidth / TargetAspect) / 2.0;
    }
    return {cropX, cropY, cropWidth / 1920.0};
}

MappedCrop mapCnnCrop(const LayoutRect& rectangle,
                      const PlayerLayout& layout,
                      const ContentMapping& mapping,
                      int frameWidth,
                      int frameHeight) {
    const double contentWidth = mapping.scale * 1920.0;
    const double contentHeight = contentWidth * 9.0 / 16.0;
    MappedCrop result;
    result.x0 = std::clamp(static_cast<int>(std::nearbyint(
        mapping.x + rectangle.x * contentWidth / layout.queueCnnReferenceWidth)),
        0, frameWidth);
    result.y0 = std::clamp(static_cast<int>(std::nearbyint(
        mapping.y + rectangle.y * contentHeight / layout.queueCnnReferenceHeight)),
        0, frameHeight);
    result.x1 = std::clamp(static_cast<int>(std::nearbyint(
        mapping.x + (rectangle.x + rectangle.w) * contentWidth /
            layout.queueCnnReferenceWidth)), 0, frameWidth);
    result.y1 = std::clamp(static_cast<int>(std::nearbyint(
        mapping.y + (rectangle.y + rectangle.h) * contentHeight /
            layout.queueCnnReferenceHeight)), 0, frameHeight);
    return result;
}

void appendPlayerGeometry(Geometry& geometry,
                          const PlayerLayout& layout,
                          std::size_t firstSlot,
                          const ContentMapping& mapping,
                          int frameWidth,
                          int frameHeight) {
    const std::array<LayoutRect, 6> cnnRectangles{{
        layout.queueCnnHold,
        layout.queueCnnNext[0], layout.queueCnnNext[1], layout.queueCnnNext[2],
        layout.queueCnnNext[3], layout.queueCnnNext[4]
    }};
    const std::array<std::array<double, 2>, 6> legacyCoordinates{{
        layout.hold, layout.next[0], layout.next[1], layout.next[2],
        layout.next[3], layout.next[4]
    }};

    for (std::size_t localSlot = 0; localSlot < cnnRectangles.size(); ++localSlot) {
        const std::size_t slot = firstSlot + localSlot;
        const MappedCrop mapped = mapCnnCrop(
            cnnRectangles[localSlot], layout, mapping, frameWidth, frameHeight);
        const int sourceWidth = mapped.x1 - mapped.x0;
        const int sourceHeight = mapped.y1 - mapped.y0;
        geometry.cropRects[slot] = {
            mapped.x0, mapped.y0, sourceWidth, sourceHeight
        };
        geometry.maxCropWidth = std::max(geometry.maxCropWidth, sourceWidth);
        geometry.maxCropHeight = std::max(geometry.maxCropHeight, sourceHeight);
        if (sourceWidth < 1 || sourceHeight < 1) continue;

        int resizedWidth = InputSize;
        int resizedHeight = InputSize;
        if (sourceWidth > sourceHeight) {
            resizedHeight = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceHeight) / sourceWidth * InputSize)));
        } else if (sourceHeight > sourceWidth) {
            resizedWidth = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceWidth) / sourceHeight * InputSize)));
        }
        const int padX = static_cast<int>(std::nearbyint(
            (InputSize - resizedWidth) * .5));
        const int padY = static_cast<int>(std::nearbyint(
            (InputSize - resizedHeight) * .5));
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
                const int x1 = std::min(mapped.x1 - 1, x0 + 1);
                const int y1 = std::min(mapped.y1 - 1, y0 + 1);
                const std::size_t destination =
                    slot * PixelsPerCrop +
                    static_cast<std::size_t>(padY + y) * InputSize + padX + x;
                geometry.cnnSamples[destination] = {
                    x0, y0, x1, y1,
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
    for (std::size_t localSlot = 0; localSlot < legacyCoordinates.size(); ++localSlot) {
        const double centerX = mapping.x + legacyCoordinates[localSlot][0] * queueScale;
        const double centerY = mapping.y + legacyCoordinates[localSlot][1] * queueScale;
        const int startX = std::max(0, static_cast<int>(std::floor(centerX - radius)));
        const int startY = std::max(0, static_cast<int>(std::floor(centerY - radius)));
        const int endX = std::min(frameWidth, startX + diameter);
        const int endY = std::min(frameHeight, startY + diameter);
        for (int y = startY; y < endY; ++y) {
            for (int x = startX; x < endX; ++x) {
                const double dx = x - centerX;
                const double dy = y - centerY;
                if (dx * dx + dy * dy <= radiusSquared) {
                    geometry.legacyPoints.push_back({
                        x, y, static_cast<std::uint32_t>(firstSlot + localSlot), 0
                    });
                }
            }
        }
    }
}

Geometry buildGeometry(int frameWidth, int frameHeight, const Settings& settings) {
    Geometry geometry;
    geometry.cnnSamples.resize(SlotsPerFrame * PixelsPerCrop);
    const ContentMapping mapping = contentMapping(frameWidth, frameHeight);
    appendPlayerGeometry(geometry, settings.p1, 0, mapping, frameWidth, frameHeight);
    appendPlayerGeometry(geometry, settings.p2, 6, mapping, frameWidth, frameHeight);
    return geometry;
}

bool directDecoderTextureEnabled() {
#ifdef TR_FUSED_PREPROCESS_DEFAULT
    bool enabled = true;
#else
    bool enabled = false;
#endif
    if (const char* configured = std::getenv("TR_GPU_FUSED_PREPROCESS")) {
        enabled = std::atoi(configured) != 0;
    }
    return enabled;
}

const char* ResizeShaderSource = R"HLSL(
struct SamplePoint {
    int x0; int y0; int x1; int y1;
    float fx; float fy; uint valid; uint unused;
};
Texture2D<float4> sourceTexture : register(t0);
StructuredBuffer<SamplePoint> samples : register(t1);
RWStructuredBuffer<float> outputTensor : register(u0);
cbuffer Parameters : register(b0) {
    uint cnnSampleCount;
    uint legacyPointCount;
    uint frameIndex;
    uint darkPixelLimit;
};
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= cnnSampleCount) return;
    SamplePoint sp = samples[index];
    uint slot = index / 2304;
    uint pixel = index - slot * 2304;
    uint cropBase = (frameIndex * 12 + slot) * 6912;
    if (sp.valid == 0) {
        outputTensor[cropBase + pixel] = -1.0f;
        outputTensor[cropBase + 2304 + pixel] = -1.0f;
        outputTensor[cropBase + 4608 + pixel] = -1.0f;
        return;
    }
    float3 p00 = sourceTexture.Load(int3(sp.x0, sp.y0, 0)).rgb * 255.0f;
    float3 p10 = sourceTexture.Load(int3(sp.x1, sp.y0, 0)).rgb * 255.0f;
    float3 p01 = sourceTexture.Load(int3(sp.x0, sp.y1, 0)).rgb * 255.0f;
    float3 p11 = sourceTexture.Load(int3(sp.x1, sp.y1, 0)).rgb * 255.0f;
    float3 top = p00 * (1.0f - sp.fx) + p10 * sp.fx;
    float3 bottom = p01 * (1.0f - sp.fx) + p11 * sp.fx;
    float3 value = top * (1.0f - sp.fy) + bottom * sp.fy;
    float3 normalized = value / 127.5f - 1.0f;
    outputTensor[cropBase + pixel] = normalized.r;
    outputTensor[cropBase + 2304 + pixel] = normalized.g;
    outputTensor[cropBase + 4608 + pixel] = normalized.b;
}
)HLSL";

const char* FusedResizeShaderSource = R"HLSL(
struct BoardSample {
    int x0; int y0; int x1; int y1;
    double fx; double fy;
};
struct QueueSample {
    int x0; int y0; int x1; int y1;
    float fx; float fy; uint valid; uint unused;
};
Texture2D<float4> sourceTexture : register(t0);
StructuredBuffer<BoardSample> boardSamples : register(t1);
StructuredBuffer<QueueSample> queueSamples : register(t2);
RWStructuredBuffer<uint> boardPixels : register(u0);
RWStructuredBuffer<float> queueTensor : register(u1);
cbuffer FusedParameters : register(b0) {
    uint boardSampleCount;
    uint queueSampleCount;
    uint frameIndex;
    uint unused0;
};
double sourceByte(float value) { return floor((double)value * 255.0 + 0.5); }
uint canvasByte(double value) {
    value = min(255.0, max(0.0, value));
    uint lower = (uint)floor(value);
    double fraction = value - (double)lower;
    return fraction > 0.5 || (fraction == 0.5 && (lower & 1)) ? lower + 1 : lower;
}
double blend(double a, double b, double c, double d, double fx, double fy) {
    precise double top = a * (1.0 - fx) + b * fx;
    precise double bottom = c * (1.0 - fx) + d * fx;
    precise double value = top * (1.0 - fy) + bottom * fy;
    return value;
}
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index < boardSampleCount) {
        BoardSample s = boardSamples[index];
        float3 p00 = sourceTexture.Load(int3(s.x0, s.y0, 0)).rgb;
        float3 p10 = sourceTexture.Load(int3(s.x1, s.y0, 0)).rgb;
        float3 p01 = sourceTexture.Load(int3(s.x0, s.y1, 0)).rgb;
        float3 p11 = sourceTexture.Load(int3(s.x1, s.y1, 0)).rgb;
        uint r = canvasByte(blend(sourceByte(p00.r), sourceByte(p10.r),
                                  sourceByte(p01.r), sourceByte(p11.r), s.fx, s.fy));
        uint g = canvasByte(blend(sourceByte(p00.g), sourceByte(p10.g),
                                  sourceByte(p01.g), sourceByte(p11.g), s.fx, s.fy));
        uint b = canvasByte(blend(sourceByte(p00.b), sourceByte(p10.b),
                                  sourceByte(p01.b), sourceByte(p11.b), s.fx, s.fy));
        boardPixels[index] = r | (g << 8) | (b << 16);
    }
    if (index < queueSampleCount) {
        QueueSample sp = queueSamples[index];
        uint slot = index / 2304;
        uint pixel = index - slot * 2304;
        uint cropBase = (frameIndex * 12 + slot) * 6912;
        if (sp.valid == 0) {
            queueTensor[cropBase + pixel] = -1.0f;
            queueTensor[cropBase + 2304 + pixel] = -1.0f;
            queueTensor[cropBase + 4608 + pixel] = -1.0f;
        } else {
            float3 q00 = sourceTexture.Load(int3(sp.x0, sp.y0, 0)).rgb * 255.0f;
            float3 q10 = sourceTexture.Load(int3(sp.x1, sp.y0, 0)).rgb * 255.0f;
            float3 q01 = sourceTexture.Load(int3(sp.x0, sp.y1, 0)).rgb * 255.0f;
            float3 q11 = sourceTexture.Load(int3(sp.x1, sp.y1, 0)).rgb * 255.0f;
            float3 top = q00 * (1.0f - sp.fx) + q10 * sp.fx;
            float3 bottom = q01 * (1.0f - sp.fx) + q11 * sp.fx;
            float3 normalized = (top * (1.0f - sp.fy) + bottom * sp.fy) /
                127.5f - 1.0f;
            queueTensor[cropBase + pixel] = normalized.r;
            queueTensor[cropBase + 2304 + pixel] = normalized.g;
            queueTensor[cropBase + 4608 + pixel] = normalized.b;
        }
    }
}
)HLSL";

const char* LegacyShaderSource = R"HLSL(
struct LegacyPoint { int x; int y; uint slot; uint unused; };
Texture2D<float4> sourceTexture : register(t0);
StructuredBuffer<LegacyPoint> points : register(t1);
RWStructuredBuffer<uint> statistics : register(u0);
cbuffer Parameters : register(b0) {
    uint cnnSampleCount;
    uint legacyPointCount;
    uint frameIndex;
    uint darkPixelLimit;
};
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    if (index >= legacyPointCount) return;
    LegacyPoint lp = points[index];
    uint3 bytes = (uint3)(
        sourceTexture.Load(int3(lp.x, lp.y, 0)).rgb * 255.0f + 0.5f);
    if (bytes.r > 50 || bytes.g > 50 || bytes.b > 50) {
        uint base = (frameIndex * 12 + lp.slot) * 5;
        InterlockedAdd(statistics[base + 0], bytes.r);
        InterlockedAdd(statistics[base + 1], bytes.g);
        InterlockedAdd(statistics[base + 2], bytes.b);
        InterlockedAdd(statistics[base + 3], 1);
    }
}
)HLSL";

const char* DarkShaderSource = R"HLSL(
struct CropRect { int x; int y; int width; int height; };
Texture2D<float4> sourceTexture : register(t0);
StructuredBuffer<CropRect> cropRects : register(t1);
RWStructuredBuffer<uint> statistics : register(u0);
cbuffer Parameters : register(b0) {
    uint cnnSampleCount;
    uint legacyPointCount;
    uint frameIndex;
    uint darkPixelLimit;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.z;
    if (slot >= 12) return;
    CropRect rect = cropRects[slot];
    if (id.x >= (uint)rect.width || id.y >= (uint)rect.height) return;
    int2 location = int2(rect.x + id.x, rect.y + id.y);
    uint3 bytes = (uint3)(
        sourceTexture.Load(int3(location, 0)).rgb * 255.0f + 0.5f);
    if (max(bytes.r, max(bytes.g, bytes.b)) <= darkPixelLimit) {
        uint base = (frameIndex * 12 + slot) * 5;
        InterlockedAdd(statistics[base + 4], 1);
    }
}
)HLSL";

using D3DCompileFunction = HRESULT (WINAPI*)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

bool compileShader(D3DCompileFunction compiler,
                   ID3D11Device* device,
                   const char* source,
                   ComPtr<ID3D11ComputeShader>& shader,
                   std::string& error) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compiler(
        source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &errors);
    if (FAILED(hr)) {
        error = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                          errors->GetBufferSize())
            : "D3DCompile failed: " + hrText(hr);
        return false;
    }
    const HRESULT create = device->CreateComputeShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader);
    if (FAILED(create)) {
        error = "CreateComputeShader failed: " + hrText(create);
        return false;
    }
    return true;
}

template <typename T>
bool createImmutableStructuredBuffer(ID3D11Device* device,
                                     const T* values,
                                     std::size_t count,
                                     ComPtr<ID3D11Buffer>& buffer,
                                     ComPtr<ID3D11ShaderResourceView>& srv,
                                     std::string& error) {
    if (count == 0 || count > std::numeric_limits<UINT>::max() / sizeof(T)) {
        error = "D3D11 queue geometry buffer has an invalid size";
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(count * sizeof(T));
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = sizeof(T);
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = values;
    HRESULT hr = device->CreateBuffer(&description, &initial, &buffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(queue geometry) failed: " + hrText(hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = 0;
    view.Buffer.NumElements = static_cast<UINT>(count);
    hr = device->CreateShaderResourceView(buffer.Get(), &view, &srv);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView(queue geometry) failed: " + hrText(hr);
        return false;
    }
    return true;
}

bool createWritableStructuredBuffer(ID3D11Device* device,
                                    UINT count,
                                    UINT stride,
                                    bool cpuReadback,
                                    ComPtr<ID3D11Buffer>& buffer,
                                    ComPtr<ID3D11UnorderedAccessView>& uav,
                                    ComPtr<ID3D11Buffer>& staging,
                                    std::string& error) {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = count * stride;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    HRESULT hr = device->CreateBuffer(&description, nullptr, &buffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(queue output) failed: " + hrText(hr);
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = 0;
    view.Buffer.NumElements = count;
    hr = device->CreateUnorderedAccessView(buffer.Get(), &view, &uav);
    if (FAILED(hr)) {
        error = "CreateUnorderedAccessView(queue output) failed: " + hrText(hr);
        return false;
    }
    if (cpuReadback) {
        D3D11_BUFFER_DESC stagingDescription{};
        stagingDescription.ByteWidth = description.ByteWidth;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateBuffer(&stagingDescription, nullptr, &staging);
        if (FAILED(hr)) {
            error = "CreateBuffer(queue statistics staging) failed: " + hrText(hr);
            return false;
        }
    }
    return true;
}

template <typename Model>
auto inferCudaDevice(Model& model,
                     float* deviceInput,
                     std::size_t batch,
                     std::string& error,
                     int) -> decltype(model.inferCudaDevice(
                         deviceInput, batch, error)) {
    return model.inferCudaDevice(deviceInput, batch, error);
}

// This keeps the new translation unit buildable until OnnxQueueModel gains the
// planned overload. Once it exists, overload resolution selects the function
// above without another change here.
template <typename Model>
std::vector<QueueCnnPrediction> inferCudaDevice(
    Model&, float*, std::size_t, std::string& error, long) {
    error = "OnnxQueueModel::inferCudaDevice(float*, batch, error) is not available";
    return {};
}

} // namespace

struct D3D11QueuePreprocessor::Impl {
    struct PipelineResult {
        D3D11QueueBatchResult batch;
        std::string error;
    };
    using CudaGraphicsResource = void*;
    using CudaStream = void*;
    using CudaD3D11GetDevice = int (WINAPI*)(int*, IDXGIAdapter*);
    using CudaSetDevice = int (WINAPI*)(int);
    using CudaRegisterResource = int (WINAPI*)(
        CudaGraphicsResource*, ID3D11Resource*, unsigned int);
    using CudaUnregisterResource = int (WINAPI*)(CudaGraphicsResource);
    using CudaMapResources = int (WINAPI*)(
        int, CudaGraphicsResource*, CudaStream);
    using CudaUnmapResources = int (WINAPI*)(
        int, CudaGraphicsResource*, CudaStream);
    using CudaGetMappedPointer = int (WINAPI*)(
        void**, std::size_t*, CudaGraphicsResource);
    using CudaGetErrorString = const char* (WINAPI*)(int);
    using CudaMalloc = int (WINAPI*)(void**, std::size_t);
    using CudaFree = int (WINAPI*)(void*);
    using CudaMemcpy = int (WINAPI*)(void*, const void*, std::size_t, int);

    ~Impl() {
        if (pipelineFutureActive) {
            try { pipelineFuture.get(); } catch (...) {}
            pipelineFutureActive = false;
        }
        if (cudaMapped && cudaUnmapResources && tensorCudaResource) {
            CudaGraphicsResource resource = tensorCudaResource;
            cudaUnmapResources(1, &resource, nullptr);
        }
        if (tensorCudaResource && cudaUnregisterResource) {
            cudaUnregisterResource(tensorCudaResource);
        }
        if (adaptiveCompactTensor && cudaFree) {
            if (cudaSetDevice && cudaDevice >= 0) cudaSetDevice(cudaDevice);
            cudaFree(adaptiveCompactTensor);
        }
        if (cudaModule) FreeLibrary(cudaModule);
        if (compilerModule) FreeLibrary(compilerModule);
    }

    template <typename T>
    bool loadCudaFunction(T& destination, const char* name, std::string& error) {
        destination = reinterpret_cast<T>(GetProcAddress(cudaModule, name));
        if (!destination) {
            error = std::string("CUDA runtime does not export ") + name;
            return false;
        }
        return true;
    }

    std::string cudaError(const char* operation, int status) const {
        const char* detail = cudaGetErrorString ? cudaGetErrorString(status) : nullptr;
        return std::string(operation) + " failed: " +
            (detail ? detail : ("CUDA error " + std::to_string(status)));
    }

    bool loadRuntime(std::string& error) {
        cudaModule = LoadLibraryW(L"cudart64_13.dll");
        if (!cudaModule) {
            wchar_t cudaPath[32768]{};
            const DWORD length = GetEnvironmentVariableW(
                L"CUDA_PATH", cudaPath, static_cast<DWORD>(std::size(cudaPath)));
            if (length > 0 && length < std::size(cudaPath)) {
                std::wstring absolute(cudaPath, length);
                absolute += L"\\bin\\x64\\cudart64_13.dll";
                cudaModule = LoadLibraryW(absolute.c_str());
            }
        }
        if (!cudaModule) {
            error = "Could not load cudart64_13.dll: " +
                windowsErrorText(GetLastError());
            return false;
        }
        return loadCudaFunction(cudaD3D11GetDevice, "cudaD3D11GetDevice", error) &&
            loadCudaFunction(cudaSetDevice, "cudaSetDevice", error) &&
            loadCudaFunction(cudaRegisterResource,
                             "cudaGraphicsD3D11RegisterResource", error) &&
            loadCudaFunction(cudaUnregisterResource,
                             "cudaGraphicsUnregisterResource", error) &&
            loadCudaFunction(cudaMapResources,
                             "cudaGraphicsMapResources", error) &&
            loadCudaFunction(cudaUnmapResources,
                             "cudaGraphicsUnmapResources", error) &&
            loadCudaFunction(cudaGetMappedPointer,
                             "cudaGraphicsResourceGetMappedPointer", error) &&
            loadCudaFunction(cudaGetErrorString, "cudaGetErrorString", error) &&
            loadCudaFunction(cudaMalloc, "cudaMalloc", error) &&
            loadCudaFunction(cudaFree, "cudaFree", error) &&
            loadCudaFunction(cudaMemcpy, "cudaMemcpy", error);
    }

    bool loadCompiler(std::string& error) {
        compilerModule = LoadLibraryExW(
            L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compilerModule) {
            error = "Could not load d3dcompiler_47.dll: " +
                windowsErrorText(GetLastError());
            return false;
        }
        d3dCompile = reinterpret_cast<D3DCompileFunction>(
            GetProcAddress(compilerModule, "D3DCompile"));
        if (!d3dCompile) {
            error = "d3dcompiler_47.dll does not export D3DCompile";
            return false;
        }
        return true;
    }

    bool ensureScratchTexture(ID3D11Texture2D* source,
                              const D3D11_TEXTURE2D_DESC& sourceDescription,
                              std::string& error) {
        if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            sourceDescription.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
            error = "D3D11 queue source must be RGB32 B8G8R8A8/B8G8R8X8";
            return false;
        }
        if (sourceDescription.Width != static_cast<UINT>(frameWidth) ||
            sourceDescription.Height != static_cast<UINT>(frameHeight) ||
            sourceDescription.SampleDesc.Count != 1) {
            error = "D3D11 queue source dimensions/sample count changed";
            return false;
        }
        if (scratchTexture && scratchFormat == sourceDescription.Format) return true;

        scratchSrv.Reset();
        scratchTexture.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(frameWidth);
        description.Height = static_cast<UINT>(frameHeight);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = sourceDescription.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(
            &description, nullptr, &scratchTexture);
        if (FAILED(hr)) {
            error = "CreateTexture2D(queue scratch) failed: " + hrText(hr);
            return false;
        }
        hr = device->CreateShaderResourceView(
            scratchTexture.Get(), nullptr, &scratchSrv);
        if (FAILED(hr)) {
            error = "CreateShaderResourceView(queue scratch) failed: " + hrText(hr);
            return false;
        }
        scratchFormat = sourceDescription.Format;
        return true;
    }

    void unbindCompute() {
        ID3D11ShaderResourceView* nullSrvs[2]{nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetShaderResources(0, 2, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    void unbindFusedCompute() {
        ID3D11ShaderResourceView* nullSrvs[3]{nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUavs[2]{nullptr, nullptr};
        context->CSSetShaderResources(0, 3, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    void clearStatistics() {
        if (!context || !statisticsUav) return;
        const UINT zero[4]{0, 0, 0, 0};
        context->ClearUnorderedAccessViewUint(statisticsUav.Get(), zero);
    }

    bool updateParameters(std::string& error) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(
            parameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            error = "Map(queue shader parameters) failed: " + hrText(hr);
            return false;
        }
        const ShaderParameters parameters{
            static_cast<std::uint32_t>(geometry.cnnSamples.size()),
            static_cast<std::uint32_t>(geometry.legacyPoints.size()),
            static_cast<std::uint32_t>(frames),
            static_cast<std::uint32_t>(darkPixelLimit)
        };
        std::memcpy(mapped.pData, &parameters, sizeof(parameters));
        context->Unmap(parameterBuffer.Get(), 0);
        return true;
    }

    bool updateFusedParameters(std::uint32_t boardSampleCount,
                               std::string& error) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(
            fusedParameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            error = "Map(fused shader parameters) failed: " + hrText(hr);
            return false;
        }
        const FusedShaderParameters parameters{
            boardSampleCount,
            static_cast<std::uint32_t>(geometry.cnnSamples.size()),
            static_cast<std::uint32_t>(frames), 0
        };
        std::memcpy(mapped.pData, &parameters, sizeof(parameters));
        context->Unmap(fusedParameterBuffer.Get(), 0);
        return true;
    }

    bool initialized = false;
    bool directDecoderTexture = false;
    bool cudaMapped = false;
    int frameWidth = 0;
    int frameHeight = 0;
    int darkPixelLimit = 40;
    int cudaDevice = -1;
    std::size_t frames = 0;
    OnnxQueueModel* model = nullptr;
    Geometry geometry;

    bool adaptiveEnabled = false;
    bool pipelineEnabled = false;
    int adaptiveRefreshFrames = 12;
    int adaptiveColorCountDelta = 1;
    double adaptiveRgbDelta = 3.0;
    double adaptiveDarkRatioDelta = 0.005;
    std::array<bool, SlotsPerFrame> adaptivePreviousStatisticsValid{};
    std::array<bool, SlotsPerFrame> adaptivePreviousPredictionsValid{};
    std::array<int, SlotsPerFrame> adaptiveFramesSinceInference{};
    D3D11QueueFrameStatistics adaptivePreviousStatistics{};
    std::array<QueueCnnPrediction, SlotsPerFrame> adaptivePreviousPredictions{};
    std::future<PipelineResult> pipelineFuture;
    bool pipelineFutureActive = false;

    HMODULE cudaModule = nullptr;
    HMODULE compilerModule = nullptr;
    D3DCompileFunction d3dCompile = nullptr;
    CudaD3D11GetDevice cudaD3D11GetDevice = nullptr;
    CudaSetDevice cudaSetDevice = nullptr;
    CudaRegisterResource cudaRegisterResource = nullptr;
    CudaUnregisterResource cudaUnregisterResource = nullptr;
    CudaMapResources cudaMapResources = nullptr;
    CudaUnmapResources cudaUnmapResources = nullptr;
    CudaGetMappedPointer cudaGetMappedPointer = nullptr;
    CudaGetErrorString cudaGetErrorString = nullptr;
    CudaMalloc cudaMalloc = nullptr;
    CudaFree cudaFree = nullptr;
    CudaMemcpy cudaMemcpy = nullptr;
    CudaGraphicsResource tensorCudaResource = nullptr;
    void* adaptiveCompactTensor = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> scratchTexture;
    ComPtr<ID3D11ShaderResourceView> scratchSrv;
    DXGI_FORMAT scratchFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Buffer> cnnSampleBuffer;
    ComPtr<ID3D11ShaderResourceView> cnnSampleSrv;
    ComPtr<ID3D11Buffer> legacyPointBuffer;
    ComPtr<ID3D11ShaderResourceView> legacyPointSrv;
    ComPtr<ID3D11Buffer> cropRectBuffer;
    ComPtr<ID3D11ShaderResourceView> cropRectSrv;
    ComPtr<ID3D11Buffer> tensorBuffer;
    ComPtr<ID3D11UnorderedAccessView> tensorUav;
    ComPtr<ID3D11Buffer> statisticsBuffer;
    ComPtr<ID3D11UnorderedAccessView> statisticsUav;
    ComPtr<ID3D11Buffer> statisticsStaging;
    ComPtr<ID3D11Buffer> parameterBuffer;
    ComPtr<ID3D11Buffer> fusedParameterBuffer;
    ComPtr<ID3D11ComputeShader> resizeShader;
    ComPtr<ID3D11ComputeShader> fusedResizeShader;
    ComPtr<ID3D11ComputeShader> legacyShader;
    ComPtr<ID3D11ComputeShader> darkShader;
};

D3D11QueuePreprocessor::D3D11QueuePreprocessor() = default;
D3D11QueuePreprocessor::~D3D11QueuePreprocessor() = default;

bool D3D11QueuePreprocessor::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    int frameWidth,
    int frameHeight,
    const Settings& settings,
    OnnxQueueModel* model,
    std::string& error) {
    error.clear();
    impl_.reset();
    if (!device || !context || !model || !model->ready() ||
        frameWidth <= 0 || frameHeight <= 0) {
        error = "D3D11 queue preprocessing received invalid initialization inputs";
        return false;
    }
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "D3D11 queue preprocessing requires the reader's immediate context";
        return false;
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device) {
        error = "D3D11 queue context does not belong to the supplied device";
        return false;
    }

    auto candidate = std::make_unique<Impl>();
    candidate->device = device;
    candidate->context = context;
    candidate->frameWidth = frameWidth;
    candidate->frameHeight = frameHeight;
    candidate->darkPixelLimit = std::clamp(settings.queueCnnDarkPixelLimit, 0, 255);
    candidate->model = model;
    candidate->directDecoderTexture = directDecoderTextureEnabled();
    candidate->geometry = buildGeometry(frameWidth, frameHeight, settings);
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE")) {
        candidate->adaptiveEnabled = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_QUEUE_SAFE_PIPELINE")) {
        candidate->pipelineEnabled = std::string(configured) != "0";
    }
    // Adaptive prediction reuse requires the prior batch's per-slot outputs
    // while selecting the next batch. Keep the two experiments independent.
    if (candidate->adaptiveEnabled) candidate->pipelineEnabled = false;
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE_REFRESH_FRAMES")) {
        candidate->adaptiveRefreshFrames = std::clamp(std::atoi(configured), 1, 600);
    }
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE_COLOR_COUNT_DELTA")) {
        candidate->adaptiveColorCountDelta = std::clamp(std::atoi(configured), 0, 1000000);
    }
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE_RGB_DELTA")) {
        candidate->adaptiveRgbDelta = std::clamp(std::atof(configured), 0.0, 255.0);
    }
    if (const char* configured = std::getenv("TR_QUEUE_ADAPTIVE_DARK_RATIO_DELTA")) {
        candidate->adaptiveDarkRatioDelta = std::clamp(std::atof(configured), 0.0, 1.0);
    }

    if (!candidate->loadCompiler(error) || !candidate->loadRuntime(error)) {
        return false;
    }
    if (!compileShader(candidate->d3dCompile, device, ResizeShaderSource,
                       candidate->resizeShader, error) ||
        !compileShader(candidate->d3dCompile, device, FusedResizeShaderSource,
                       candidate->fusedResizeShader, error) ||
        !compileShader(candidate->d3dCompile, device, LegacyShaderSource,
                       candidate->legacyShader, error) ||
        !compileShader(candidate->d3dCompile, device, DarkShaderSource,
                       candidate->darkShader, error)) {
        return false;
    }
    if (!createImmutableStructuredBuffer(
            device, candidate->geometry.cnnSamples.data(),
            candidate->geometry.cnnSamples.size(), candidate->cnnSampleBuffer,
            candidate->cnnSampleSrv, error) ||
        !createImmutableStructuredBuffer(
            device, candidate->geometry.legacyPoints.data(),
            candidate->geometry.legacyPoints.size(), candidate->legacyPointBuffer,
            candidate->legacyPointSrv, error) ||
        !createImmutableStructuredBuffer(
            device, candidate->geometry.cropRects.data(),
            candidate->geometry.cropRects.size(), candidate->cropRectBuffer,
            candidate->cropRectSrv, error)) {
        return false;
    }
    ComPtr<ID3D11Buffer> unusedTensorStaging;
    if (!createWritableStructuredBuffer(
            device, MaximumFrames * SlotsPerFrame * FloatsPerCrop, sizeof(float),
            false, candidate->tensorBuffer, candidate->tensorUav,
            unusedTensorStaging, error)) {
        return false;
    }
    if (!createWritableStructuredBuffer(
            device, MaximumFrames * SlotsPerFrame * StatisticFields,
            sizeof(std::uint32_t), true, candidate->statisticsBuffer,
            candidate->statisticsUav, candidate->statisticsStaging, error)) {
        return false;
    }

    D3D11_BUFFER_DESC parameterDescription{};
    parameterDescription.ByteWidth = sizeof(ShaderParameters);
    parameterDescription.Usage = D3D11_USAGE_DYNAMIC;
    parameterDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    parameterDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = device->CreateBuffer(
        &parameterDescription, nullptr, &candidate->parameterBuffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(queue shader parameters) failed: " + hrText(hr);
        return false;
    }
    hr = device->CreateBuffer(
        &parameterDescription, nullptr, &candidate->fusedParameterBuffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(fused shader parameters) failed: " + hrText(hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        error = "ID3D11Device is not an IDXGIDevice: " + hrText(hr);
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) {
        error = "IDXGIDevice::GetAdapter failed: " + hrText(hr);
        return false;
    }
    int cudaStatus = candidate->cudaD3D11GetDevice(
        &candidate->cudaDevice, adapter.Get());
    if (cudaStatus != 0) {
        error = candidate->cudaError("cudaD3D11GetDevice", cudaStatus);
        return false;
    }
    cudaStatus = candidate->cudaSetDevice(candidate->cudaDevice);
    if (cudaStatus != 0) {
        error = candidate->cudaError("cudaSetDevice", cudaStatus);
        return false;
    }
    cudaStatus = candidate->cudaRegisterResource(
        &candidate->tensorCudaResource, candidate->tensorBuffer.Get(), 0);
    if (cudaStatus != 0) {
        error = candidate->cudaError(
            "cudaGraphicsD3D11RegisterResource", cudaStatus);
        return false;
    }
    if (candidate->adaptiveEnabled || candidate->pipelineEnabled) {
        const std::size_t compactBytes =
            D3D11QueuePreprocessor::MaximumFrames * SlotsPerFrame *
            FloatsPerCrop * sizeof(float);
        cudaStatus = candidate->cudaMalloc(
            &candidate->adaptiveCompactTensor, compactBytes);
        if (cudaStatus != 0) {
            error = candidate->cudaError("cudaMalloc(adaptive queue tensor)", cudaStatus);
            return false;
        }
    }

    candidate->clearStatistics();
    candidate->initialized = true;
    impl_ = std::move(candidate);
    return true;
}

bool D3D11QueuePreprocessor::append(
    ID3D11Texture2D* texture,
    UINT sourceSubresource,
    std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || !texture) {
        error = "D3D11 queue preprocessor is not initialized";
        return false;
    }
    if (impl_->frames >= MaximumFrames) {
        error = "D3D11 queue preprocessor reached its configured frame capacity";
        return false;
    }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice.Get() != impl_->device.Get()) {
        error = "D3D11 queue source texture belongs to a different device";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const UINT subresourceCount = description.MipLevels * description.ArraySize;
    if (sourceSubresource >= subresourceCount ||
        sourceSubresource % std::max(1u, description.MipLevels) != 0) {
        error = "D3D11 queue source subresource is invalid or is not mip zero";
        return false;
    }
    if (!impl_->updateParameters(error)) {
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> directSourceSrv;
    ID3D11ShaderResourceView* sourceSrv = nullptr;
    const bool canBindDirectly = impl_->directDecoderTexture &&
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        description.ArraySize == 1 && sourceSubresource == 0;
    if (canBindDirectly) {
        const HRESULT hr = impl_->device->CreateShaderResourceView(
            texture, nullptr, &directSourceSrv);
        if (FAILED(hr)) {
            error = "CreateShaderResourceView(direct queue decoder texture) failed: " +
                hrText(hr);
            return false;
        }
        sourceSrv = directSourceSrv.Get();
    } else {
        if (!impl_->ensureScratchTexture(texture, description, error)) return false;
        impl_->context->CopySubresourceRegion(
            impl_->scratchTexture.Get(), 0, 0, 0, 0,
            texture, sourceSubresource, nullptr);
        sourceSrv = impl_->scratchSrv.Get();
    }
    ID3D11Buffer* constants[]{impl_->parameterBuffer.Get()};
    impl_->context->CSSetConstantBuffers(0, 1, constants);

    ID3D11ShaderResourceView* resizeSrvs[]{
        sourceSrv, impl_->cnnSampleSrv.Get()
    };
    ID3D11UnorderedAccessView* tensorUavs[]{impl_->tensorUav.Get()};
    impl_->context->CSSetShader(impl_->resizeShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, resizeSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, tensorUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.cnnSamples.size() + 255) / 256), 1, 1);
    impl_->unbindCompute();

    ID3D11ShaderResourceView* legacySrvs[]{
        sourceSrv, impl_->legacyPointSrv.Get()
    };
    ID3D11UnorderedAccessView* statisticUavs[]{impl_->statisticsUav.Get()};
    impl_->context->CSSetShader(impl_->legacyShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, legacySrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, statisticUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.legacyPoints.size() + 255) / 256), 1, 1);
    impl_->unbindCompute();

    ID3D11ShaderResourceView* darkSrvs[]{
        sourceSrv, impl_->cropRectSrv.Get()
    };
    impl_->context->CSSetShader(impl_->darkShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, darkSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, statisticUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.maxCropWidth + 7) / 8),
        static_cast<UINT>((impl_->geometry.maxCropHeight + 7) / 8),
        SlotsPerFrame);
    impl_->unbindCompute();

    const HRESULT removed = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = "D3D11 device was removed during queue preprocessing: " +
            hrText(removed);
        return false;
    }
    ++impl_->frames;
    return true;
}

bool D3D11QueuePreprocessor::appendFused(
    ID3D11Texture2D* texture,
    UINT sourceSubresource,
    D3D11BoardPreprocessor& board,
    std::int64_t sourceTime100ns,
    std::uint64_t deliveryOrder,
    D3D11BoardFrameHandle& handle,
    std::string& error) {
    error.clear();
    handle = {};
    if (!impl_ || !impl_->initialized || !texture) {
        error = "D3D11 queue preprocessor is not initialized";
        return false;
    }
    if (impl_->frames >= MaximumFrames) {
        error = "D3D11 queue preprocessor reached its configured frame capacity";
        return false;
    }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice.Get() != impl_->device.Get()) {
        error = "D3D11 fused source texture belongs to a different device";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const bool canBindDirectly = impl_->directDecoderTexture &&
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        description.Width == static_cast<UINT>(impl_->frameWidth) &&
        description.Height == static_cast<UINT>(impl_->frameHeight) &&
        description.SampleDesc.Count == 1 && description.ArraySize == 1 &&
        sourceSubresource == 0;
    if (!canBindDirectly) {
        error = "D3D11 decoder texture cannot be bound to the fused board/queue shader";
        return false;
    }

    D3D11BoardFusedTarget boardTarget;
    if (!board.prepareFusedRetain(
            texture, sourceSubresource, sourceTime100ns, deliveryOrder,
            boardTarget, error) ||
        !impl_->updateParameters(error) ||
        !impl_->updateFusedParameters(boardTarget.sampleCount, error)) {
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> sourceSrv;
    HRESULT hr = impl_->device->CreateShaderResourceView(texture, nullptr, &sourceSrv);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView(fused decoder texture) failed: " + hrText(hr);
        return false;
    }

    ID3D11Buffer* fusedConstants[]{impl_->fusedParameterBuffer.Get()};
    ID3D11ShaderResourceView* fusedSrvs[]{
        sourceSrv.Get(), boardTarget.sampleSrv, impl_->cnnSampleSrv.Get()
    };
    ID3D11UnorderedAccessView* fusedUavs[]{
        boardTarget.boardUav, impl_->tensorUav.Get()
    };
    impl_->context->CSSetConstantBuffers(0, 1, fusedConstants);
    impl_->context->CSSetShader(impl_->fusedResizeShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 3, fusedSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 2, fusedUavs, nullptr);
    const std::size_t workItems = std::max<std::size_t>(
        boardTarget.sampleCount, impl_->geometry.cnnSamples.size());
    impl_->context->Dispatch(static_cast<UINT>((workItems + 255) / 256), 1, 1);
    impl_->unbindFusedCompute();

    ID3D11Buffer* queueConstants[]{impl_->parameterBuffer.Get()};
    impl_->context->CSSetConstantBuffers(0, 1, queueConstants);
    ID3D11ShaderResourceView* legacySrvs[]{
        sourceSrv.Get(), impl_->legacyPointSrv.Get()
    };
    ID3D11UnorderedAccessView* statisticUavs[]{impl_->statisticsUav.Get()};
    impl_->context->CSSetShader(impl_->legacyShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, legacySrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, statisticUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.legacyPoints.size() + 255) / 256), 1, 1);
    impl_->unbindCompute();

    ID3D11ShaderResourceView* darkSrvs[]{
        sourceSrv.Get(), impl_->cropRectSrv.Get()
    };
    impl_->context->CSSetShader(impl_->darkShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, darkSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, statisticUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.maxCropWidth + 7) / 8),
        static_cast<UINT>((impl_->geometry.maxCropHeight + 7) / 8),
        SlotsPerFrame);
    impl_->unbindCompute();

    if (!board.finishFusedRetain(boardTarget, handle, error)) return false;
    const HRESULT removed = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = "D3D11 device was removed during fused preprocessing: " + hrText(removed);
        handle = {};
        return false;
    }
    ++impl_->frames;
    return true;
}

bool D3D11QueuePreprocessor::flush(
    D3D11QueueBatchResult& result,
    std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized) {
        error = "D3D11 queue preprocessor is not initialized";
        return false;
    }
    if (impl_->frames == 0) {
        result = {};
        return true;
    }

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
            error = "CUDA queue pipeline threw an exception: " +
                std::string(exception.what());
            return false;
        } catch (...) {
            impl_->pipelineFutureActive = false;
            error = "CUDA queue pipeline threw an unknown exception";
            return false;
        }
    }

    D3D11QueueBatchResult candidate;
    candidate.frames.resize(impl_->frames);
    impl_->context->CopyResource(
        impl_->statisticsStaging.Get(), impl_->statisticsBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mappedStatistics{};
    HRESULT hr = impl_->context->Map(
        impl_->statisticsStaging.Get(), 0, D3D11_MAP_READ, 0,
        &mappedStatistics);
    if (FAILED(hr)) {
        error = "Map(queue statistics staging) failed: " + hrText(hr);
        return false;
    }
    const auto* statistics =
        static_cast<const std::uint32_t*>(mappedStatistics.pData);
    for (std::size_t frame = 0; frame < impl_->frames; ++frame) {
        for (std::size_t slot = 0; slot < SlotsPerFrame; ++slot) {
            const std::size_t base =
                (frame * SlotsPerFrame + slot) * StatisticFields;
            auto& destination = candidate.frames[frame].slots[slot];
            destination.sumR = statistics[base + SumR];
            destination.sumG = statistics[base + SumG];
            destination.sumB = statistics[base + SumB];
            destination.colorCount = statistics[base + ColorCount];
            destination.darkCount = statistics[base + DarkCount];
            const GpuRect& rect = impl_->geometry.cropRects[slot];
            destination.cropPixelCount = static_cast<std::uint32_t>(
                std::max(0, rect.width) * std::max(0, rect.height));
        }
    }
    impl_->context->Unmap(impl_->statisticsStaging.Get(), 0);

    candidate.adaptiveEnabled = impl_->adaptiveEnabled;
    const std::size_t activeSlots = impl_->frames * SlotsPerFrame;
    std::vector<bool> inferSlot(activeSlots, true);
    if (impl_->adaptiveEnabled) {
        const auto statisticsSimilar = [&](const D3D11QueueSlotStatistics& a,
                                           const D3D11QueueSlotStatistics& b) {
            const auto countDifference = static_cast<std::uint64_t>(
                std::llabs(static_cast<long long>(a.colorCount) -
                           static_cast<long long>(b.colorCount)));
            if (countDifference >
                static_cast<std::uint64_t>(impl_->adaptiveColorCountDelta)) {
                return false;
            }
            const double aDark = a.cropPixelCount
                ? static_cast<double>(a.darkCount) / a.cropPixelCount : 0.0;
            const double bDark = b.cropPixelCount
                ? static_cast<double>(b.darkCount) / b.cropPixelCount : 0.0;
            if (std::abs(aDark - bDark) > impl_->adaptiveDarkRatioDelta) return false;
            if ((a.colorCount == 0) != (b.colorCount == 0)) return false;
            if (a.colorCount != 0 && b.colorCount != 0) {
                const auto averageDifference = [&](std::uint64_t aSum,
                                                   std::uint64_t bSum) {
                    return std::abs(
                        static_cast<double>(aSum) / a.colorCount -
                        static_cast<double>(bSum) / b.colorCount);
                };
                if (averageDifference(a.sumR, b.sumR) > impl_->adaptiveRgbDelta ||
                    averageDifference(a.sumG, b.sumG) > impl_->adaptiveRgbDelta ||
                    averageDifference(a.sumB, b.sumB) > impl_->adaptiveRgbDelta) {
                    return false;
                }
            }
            return true;
        };
        auto planningPredictionValid = impl_->adaptivePreviousPredictionsValid;
        for (std::size_t frame = 0; frame < impl_->frames; ++frame) {
            for (std::size_t slot = 0; slot < SlotsPerFrame; ++slot) {
                const std::size_t index = frame * SlotsPerFrame + slot;
                const bool refreshDue = impl_->adaptiveFramesSinceInference[slot] >=
                    impl_->adaptiveRefreshFrames;
                const bool similar = impl_->adaptivePreviousStatisticsValid[slot] &&
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

    int cudaStatus = impl_->cudaSetDevice(impl_->cudaDevice);
    if (cudaStatus != 0) {
        error = impl_->cudaError("cudaSetDevice", cudaStatus);
        return false;
    }
    Impl::CudaGraphicsResource resource = impl_->tensorCudaResource;
    cudaStatus = impl_->cudaMapResources(1, &resource, nullptr);
    if (cudaStatus != 0) {
        error = impl_->cudaError("cudaGraphicsMapResources", cudaStatus);
        return false;
    }
    impl_->cudaMapped = true;

    void* mappedTensor = nullptr;
    std::size_t mappedBytes = 0;
    cudaStatus = impl_->cudaGetMappedPointer(
        &mappedTensor, &mappedBytes, impl_->tensorCudaResource);
    if (cudaStatus != 0) {
        error = impl_->cudaError(
            "cudaGraphicsResourceGetMappedPointer", cudaStatus);
    } else {
        const std::size_t batch = impl_->frames * SlotsPerFrame;
        const std::size_t requiredBytes =
            batch * FloatsPerCrop * sizeof(float);
        if (!mappedTensor || mappedBytes < requiredBytes) {
            error = "CUDA mapped queue tensor is smaller than the active batch";
        } else {
            try {
                auto* deviceTensor = static_cast<float*>(mappedTensor);
                if (impl_->pipelineEnabled) {
                    constexpr int CudaMemcpyDeviceToDevice = 3;
                    const int copyStatus = impl_->cudaMemcpy(
                        impl_->adaptiveCompactTensor, deviceTensor, requiredBytes,
                        CudaMemcpyDeviceToDevice);
                    if (copyStatus != 0) {
                        error = impl_->cudaError(
                            "cudaMemcpy(queue pipeline staging)", copyStatus);
                    }
                } else {
                    candidate.predictions.resize(batch);
                    std::vector<std::size_t> selectedSlots;
                    selectedSlots.reserve(batch);
                    for (std::size_t frame = 0; frame < impl_->frames; ++frame) {
                        bool frameInferred = false;
                        for (std::size_t slot = 0; slot < SlotsPerFrame; ++slot) {
                            const std::size_t index = frame * SlotsPerFrame + slot;
                            if (inferSlot[index]) {
                                selectedSlots.push_back(index);
                                frameInferred = true;
                                ++candidate.inferredSlots;
                            } else {
                                ++candidate.reusedSlots;
                            }
                        }
                        if (frameInferred) ++candidate.inferredFrames;
                        else ++candidate.reusedFrames;
                    }
                    if (impl_->adaptiveEnabled && !selectedSlots.empty()) {
                        constexpr int CudaMemcpyDeviceToDevice = 3;
                        std::size_t compactSlot = 0;
                        for (std::size_t selected = 0;
                             selected < selectedSlots.size() && error.empty();) {
                            const std::size_t runStart = selectedSlots[selected];
                            std::size_t runSlots = 1;
                            while (selected + runSlots < selectedSlots.size() &&
                                   selectedSlots[selected + runSlots] ==
                                       runStart + runSlots) {
                                ++runSlots;
                            }
                            const int copyStatus = impl_->cudaMemcpy(
                                static_cast<float*>(impl_->adaptiveCompactTensor) +
                                    compactSlot * FloatsPerCrop,
                                deviceTensor + runStart * FloatsPerCrop,
                                runSlots * FloatsPerCrop * sizeof(float),
                                CudaMemcpyDeviceToDevice);
                            if (copyStatus != 0) {
                                error = impl_->cudaError(
                                    "cudaMemcpy(adaptive queue compaction)", copyStatus);
                                break;
                            }
                            compactSlot += runSlots;
                            selected += runSlots;
                        }
                        if (error.empty()) {
                            const std::size_t selectedBatch = selectedSlots.size();
                            auto selectedPredictions = inferCudaDevice(
                                *impl_->model,
                                static_cast<float*>(impl_->adaptiveCompactTensor),
                                selectedBatch, error, 0);
                            if (error.empty() &&
                                selectedPredictions.size() != selectedBatch) {
                                error = "CUDA queue inference returned an unexpected compacted batch size";
                            }
                            if (error.empty()) {
                                for (std::size_t selected = 0;
                                     selected < selectedSlots.size(); ++selected) {
                                    candidate.predictions[selectedSlots[selected]] =
                                        selectedPredictions[selected];
                                }
                                candidate.inferenceRuns = 1;
                            }
                        }
                    } else if (!impl_->adaptiveEnabled) {
                        candidate.predictions = inferCudaDevice(
                            *impl_->model, deviceTensor, batch, error, 0);
                        if (error.empty() && candidate.predictions.size() != batch) {
                            error = "CUDA queue inference returned an unexpected batch size";
                        }
                    }
                    if (error.empty() && impl_->adaptiveEnabled) {
                        for (std::size_t frame = 0; frame < impl_->frames; ++frame) {
                            for (std::size_t slot = 0; slot < SlotsPerFrame; ++slot) {
                                const std::size_t index = frame * SlotsPerFrame + slot;
                                if (inferSlot[index]) {
                                    impl_->adaptivePreviousPredictions[slot] =
                                        candidate.predictions[index];
                                    impl_->adaptivePreviousPredictionsValid[slot] = true;
                                } else if (impl_->adaptivePreviousPredictionsValid[slot]) {
                                    candidate.predictions[index] =
                                        impl_->adaptivePreviousPredictions[slot];
                                } else {
                                    error = "Adaptive queue inference has no prior slot prediction";
                                    break;
                                }
                            }
                            if (!error.empty()) break;
                        }
                    }
                    if (error.empty() && !impl_->adaptiveEnabled) {
                        candidate.inferredFrames = impl_->frames;
                        candidate.inferredSlots = batch;
                        candidate.inferenceRuns = impl_->frames ? 1 : 0;
                    }
                }
            } catch (const std::exception& exception) {
                error = "CUDA queue inference threw an exception: " +
                    std::string(exception.what());
            } catch (...) {
                error = "CUDA queue inference threw an unknown exception";
            }
        }
    }

    resource = impl_->tensorCudaResource;
    const int unmapStatus = impl_->cudaUnmapResources(1, &resource, nullptr);
    impl_->cudaMapped = false;
    if (unmapStatus != 0 && error.empty()) {
        error = impl_->cudaError("cudaGraphicsUnmapResources", unmapStatus);
    }
    if (!error.empty()) return false;

    if (impl_->pipelineEnabled) {
        auto* pipelineImpl = impl_.get();
        auto submitted = std::move(candidate);
        const std::size_t submittedBatch = impl_->frames * SlotsPerFrame;
        impl_->pipelineFuture = std::async(
            std::launch::async,
            [pipelineImpl, submitted = std::move(submitted), submittedBatch]() mutable {
                Impl::PipelineResult completed;
                completed.batch = std::move(submitted);
                const auto started = std::chrono::steady_clock::now();
                const int deviceStatus =
                    pipelineImpl->cudaSetDevice(pipelineImpl->cudaDevice);
                if (deviceStatus != 0) {
                    completed.error = pipelineImpl->cudaError(
                        "cudaSetDevice(queue pipeline)", deviceStatus);
                } else {
                    completed.batch.predictions = inferCudaDevice(
                        *pipelineImpl->model,
                        static_cast<float*>(pipelineImpl->adaptiveCompactTensor),
                        submittedBatch, completed.error, 0);
                    if (completed.error.empty() &&
                        completed.batch.predictions.size() != submittedBatch) {
                        completed.error =
                            "CUDA queue pipeline returned an unexpected batch size";
                    }
                }
                completed.batch.inferredFrames = completed.batch.frames.size();
                completed.batch.inferredSlots = submittedBatch;
                completed.batch.inferenceRuns = submittedBatch ? 1 : 0;
                completed.batch.backgroundInferenceSeconds =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                return completed;
            });
        impl_->pipelineFutureActive = true;
        result = std::move(completedPipelineBatch);
        impl_->frames = 0;
        impl_->clearStatistics();
        return true;
    }

    result = std::move(candidate);
    impl_->frames = 0;
    impl_->clearStatistics();
    return true;
}

bool D3D11QueuePreprocessor::pipelineEnabled() const {
    return impl_ && impl_->pipelineEnabled;
}

bool D3D11QueuePreprocessor::finishPipeline(
    D3D11QueueBatchResult& result,
    std::string& error) {
    error.clear();
    result = {};
    if (!impl_ || !impl_->initialized) {
        error = "D3D11 queue preprocessor is not initialized";
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
        error = "CUDA queue pipeline threw an exception: " +
            std::string(exception.what());
    } catch (...) {
        impl_->pipelineFutureActive = false;
        error = "CUDA queue pipeline threw an unknown exception";
    }
    return false;
}

void D3D11QueuePreprocessor::reset() {
    if (!impl_) return;
    impl_->frames = 0;
    impl_->adaptivePreviousStatisticsValid.fill(false);
    impl_->adaptivePreviousPredictionsValid.fill(false);
    impl_->adaptiveFramesSinceInference.fill(0);
    impl_->clearStatistics();
}

bool D3D11QueuePreprocessor::ready() const {
    return impl_ && impl_->initialized;
}

std::size_t D3D11QueuePreprocessor::frameCount() const {
    return impl_ ? impl_->frames : 0;
}

} // namespace tr

#endif // Windows ultra GPU build
