#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "d3d11_board_preprocessor.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT)

#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tr {
namespace {

constexpr int Players = 2;
constexpr int CellsPerBoard = BoardWidth * VisibleRows;
constexpr int CellSize = 32;
constexpr int ResizedWidth = BoardWidth * CellSize;
constexpr int ResizedHeight = VisibleRows * CellSize;
constexpr int ValuesPerCell = 3 * CellSize * CellSize;
constexpr int TensorValues = CellsPerBoard * ValuesPerCell;
constexpr int ClassicFields = 3;

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

struct GpuBoardSample {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    double fx = 0;
    double fy = 0;
};
static_assert(sizeof(GpuBoardSample) == 32);

struct GpuResizeSample {
    std::uint32_t offset00 = 0;
    std::uint32_t offset10 = 0;
    std::uint32_t offset01 = 0;
    std::uint32_t offset11 = 0;
    double fx = 0;
    double fy = 0;
};
static_assert(sizeof(GpuResizeSample) == 32);

struct GpuClassicRect {
    std::uint32_t boardBase = 0;
    std::uint32_t boardWidth = 0;
    std::uint32_t x0 = 0;
    std::uint32_t y0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y1 = 0;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
};
static_assert(sizeof(GpuClassicRect) == 32);

struct Geometry {
    std::array<int, Players> boardWidths{};
    std::array<int, Players> boardHeights{};
    std::array<std::uint32_t, Players> boardBases{};
    std::array<std::vector<GpuResizeSample>, Players> resizeSamples;
    std::vector<GpuBoardSample> boardSamples;
    std::vector<GpuBoardSample> globalBoardSamples;
    std::vector<GpuClassicRect> classicRects;
    std::array<std::uint32_t, Players * CellsPerBoard> classicCounts{};
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::size_t boardPixelCount = 0;
};

Geometry buildGeometry(int frameWidth, int frameHeight, const Settings& settings) {
    Geometry geometry;
    const ContentMapping mapping = contentMapping(frameWidth, frameHeight);
    const std::array<const PlayerLayout*, Players> layouts{
        &settings.p1, &settings.p2
    };

    int minimumX = frameWidth;
    int minimumY = frameHeight;
    int maximumX = -1;
    int maximumY = -1;
    std::uint32_t boardBase = 0;
    for (int player = 0; player < Players; ++player) {
        const LayoutRect& layout = layouts[player]->board;
        const int width = std::max(10, static_cast<int>(std::floor(layout.w)));
        const int height = std::max(20, static_cast<int>(std::floor(layout.h)));
        geometry.boardWidths[player] = width;
        geometry.boardHeights[player] = height;
        geometry.boardBases[player] = boardBase;

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
                const int x1 = std::min(frameWidth - 1, x0 + 1);
                const int y1 = std::min(frameHeight - 1, y0 + 1);
                geometry.boardSamples.push_back({
                    x0, y0, x1, y1, clampedX - x0, clampedY - y0
                });
                minimumX = std::min(minimumX, x0);
                minimumY = std::min(minimumY, y0);
                maximumX = std::max(maximumX, x1);
                maximumY = std::max(maximumY, y1);
            }
        }
        boardBase += static_cast<std::uint32_t>(width * height);
    }
    geometry.boardPixelCount = boardBase;
    if (maximumX < minimumX || maximumY < minimumY) return {};
    geometry.sourceX = minimumX;
    geometry.sourceY = minimumY;
    geometry.sourceWidth = maximumX - minimumX + 1;
    geometry.sourceHeight = maximumY - minimumY + 1;
    geometry.globalBoardSamples = geometry.boardSamples;
    for (GpuBoardSample& sample : geometry.boardSamples) {
        sample.x0 -= minimumX;
        sample.x1 -= minimumX;
        sample.y0 -= minimumY;
        sample.y1 -= minimumY;
    }

    geometry.classicRects.reserve(Players * CellsPerBoard);
    for (int player = 0; player < Players; ++player) {
        const int width = geometry.boardWidths[player];
        const int height = geometry.boardHeights[player];
        const std::uint32_t base = geometry.boardBases[player];
        auto& samples = geometry.resizeSamples[player];
        samples.reserve(ResizedWidth * ResizedHeight);

        std::array<int, ResizedWidth> x0{};
        std::array<int, ResizedWidth> x1{};
        std::array<double, ResizedWidth> fx{};
        std::array<int, ResizedHeight> y0{};
        std::array<int, ResizedHeight> y1{};
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

const char* BoardImageShaderSource = R"HLSL(
struct BoardSample {
    int x0; int y0; int x1; int y1;
    double fx; double fy;
};
Texture2D<float4> sourceTexture : register(t0);
StructuredBuffer<BoardSample> samples : register(t1);
RWStructuredBuffer<uint> boardPixels : register(u0);
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
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    uint count, stride;
    samples.GetDimensions(count, stride);
    if (index >= count) return;
    BoardSample s = samples[index];
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
)HLSL";

const char* TensorShaderSource = R"HLSL(
struct ResizeSample {
    uint offset00; uint offset10; uint offset01; uint offset11;
    double fx; double fy;
};
StructuredBuffer<uint> boardPixels : register(t0);
StructuredBuffer<ResizeSample> samples : register(t1);
RWStructuredBuffer<float> tensor : register(u0);
uint channel(uint packed, uint shift) { return (packed >> shift) & 255; }
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
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    uint count, stride;
    samples.GetDimensions(count, stride);
    if (index >= count) return;
    ResizeSample s = samples[index];
    uint p00=boardPixels[s.offset00], p10=boardPixels[s.offset10];
    uint p01=boardPixels[s.offset01], p11=boardPixels[s.offset11];
    uint y=index/320, x=index%320;
    uint cell=(y/32)*10+x/32;
    uint pixel=(y%32)*32+x%32;
    uint base=cell*3072+pixel;
    uint r=canvasByte(blend(channel(p00,0),channel(p10,0),channel(p01,0),
                            channel(p11,0),s.fx,s.fy));
    uint g=canvasByte(blend(channel(p00,8),channel(p10,8),channel(p01,8),
                            channel(p11,8),s.fx,s.fy));
    uint b=canvasByte(blend(channel(p00,16),channel(p10,16),channel(p01,16),
                            channel(p11,16),s.fx,s.fy));
    tensor[base]=r/255.0f;
    tensor[base+1024]=g/255.0f;
    tensor[base+2048]=b/255.0f;
}
)HLSL";

const char* ClassicShaderSource = R"HLSL(
struct ClassicRect {
    uint boardBase; uint boardWidth; uint x0; uint y0;
    uint x1; uint y1; uint unused0; uint unused1;
};
StructuredBuffer<uint> boardPixels : register(t0);
StructuredBuffer<ClassicRect> rects : register(t1);
RWStructuredBuffer<uint> sums : register(u0);
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint cell=id.x;
    uint count, stride;
    rects.GetDimensions(count, stride);
    if (cell>=count) return;
    ClassicRect rect=rects[cell];
    uint sumR=0,sumG=0,sumB=0;
    for(uint y=rect.y0;y<rect.y1;++y) {
        for(uint x=rect.x0;x<rect.x1;++x) {
            uint p=boardPixels[rect.boardBase+y*rect.boardWidth+x];
            sumR+=p&255; sumG+=(p>>8)&255; sumB+=(p>>16)&255;
        }
    }
    sums[cell*3]=sumR; sums[cell*3+1]=sumG; sums[cell*3+2]=sumB;
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
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compiler(
        source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &code, &errors);
    if (FAILED(hr)) {
        error = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                          errors->GetBufferSize())
            : "D3DCompile(board) failed: " + hrText(hr);
        return false;
    }
    const HRESULT create = device->CreateComputeShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
    if (FAILED(create)) {
        error = "CreateComputeShader(board) failed: " + hrText(create);
        return false;
    }
    return true;
}

template <typename T>
bool createImmutableStructuredBuffer(ID3D11Device* device,
                                     const std::vector<T>& values,
                                     ComPtr<ID3D11Buffer>& buffer,
                                     ComPtr<ID3D11ShaderResourceView>& srv,
                                     std::string& error) {
    if (values.empty() ||
        values.size() > std::numeric_limits<UINT>::max() / sizeof(T)) {
        error = "D3D11 board geometry buffer has an invalid size";
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(values.size() * sizeof(T));
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = sizeof(T);
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = values.data();
    HRESULT hr = device->CreateBuffer(&description, &initial, &buffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(board geometry) failed: " + hrText(hr);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = static_cast<UINT>(values.size());
    hr = device->CreateShaderResourceView(buffer.Get(), &view, &srv);
    if (FAILED(hr)) {
        error = "CreateShaderResourceView(board geometry) failed: " + hrText(hr);
        return false;
    }
    return true;
}

bool createWritableStructuredBuffer(ID3D11Device* device,
                                    UINT count,
                                    UINT stride,
                                    bool shaderReadable,
                                    ComPtr<ID3D11Buffer>& buffer,
                                    ComPtr<ID3D11UnorderedAccessView>& uav,
                                    ComPtr<ID3D11ShaderResourceView>& srv,
                                    std::string& error) {
    if (count == 0 || count > std::numeric_limits<UINT>::max() / stride) {
        error = "D3D11 board output buffer has an invalid size";
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = count * stride;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS |
        (shaderReadable ? D3D11_BIND_SHADER_RESOURCE : 0);
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    HRESULT hr = device->CreateBuffer(&description, nullptr, &buffer);
    if (FAILED(hr)) {
        error = "CreateBuffer(board output) failed: " + hrText(hr);
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC unordered{};
    unordered.Format = DXGI_FORMAT_UNKNOWN;
    unordered.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    unordered.Buffer.NumElements = count;
    hr = device->CreateUnorderedAccessView(buffer.Get(), &unordered, &uav);
    if (FAILED(hr)) {
        error = "CreateUnorderedAccessView(board output) failed: " + hrText(hr);
        return false;
    }
    if (shaderReadable) {
        D3D11_SHADER_RESOURCE_VIEW_DESC resource{};
        resource.Format = DXGI_FORMAT_UNKNOWN;
        resource.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        resource.Buffer.NumElements = count;
        hr = device->CreateShaderResourceView(buffer.Get(), &resource, &srv);
        if (FAILED(hr)) {
            error = "CreateShaderResourceView(board output) failed: " + hrText(hr);
            return false;
        }
    }
    return true;
}

struct Rgb {
    double r = 0;
    double g = 0;
    double b = 0;
};

Cell classicCell(std::uint32_t sumR,
                 std::uint32_t sumG,
                 std::uint32_t sumB,
                 std::uint32_t count) {
    if (count == 0) return Cell::Empty;
    const Rgb input{
        static_cast<double>(sumR) / count,
        static_cast<double>(sumG) / count,
        static_cast<double>(sumB) / count
    };
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
    for (const Rgb& candidate : palette[0].second) {
        if (distance(candidate) < 6000) return Cell::Empty;
    }
    for (const Rgb& candidate : palette[1].second) {
        if (distance(candidate) < 10000) return Cell::Garbage;
    }
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

} // namespace

struct D3D11BoardPreprocessor::Impl {
    using CudaGraphicsResource = void*;
    using CudaStream = void*;
    using CudaD3D11GetDevice = int (WINAPI*)(int*, IDXGIAdapter*);
    using CudaSetDevice = int (WINAPI*)(int);
    using CudaRegisterResource = int (WINAPI*)(
        CudaGraphicsResource*, ID3D11Resource*, unsigned int);
    using CudaUnregisterResource = int (WINAPI*)(CudaGraphicsResource);
    using CudaMapResources = int (WINAPI*)(int, CudaGraphicsResource*, CudaStream);
    using CudaUnmapResources = int (WINAPI*)(int, CudaGraphicsResource*, CudaStream);
    using CudaGetMappedPointer = int (WINAPI*)(
        void**, std::size_t*, CudaGraphicsResource);
    using CudaGetErrorString = const char* (WINAPI*)(int);

    struct Slot {
        ComPtr<ID3D11Buffer> boardBuffer;
        ComPtr<ID3D11UnorderedAccessView> boardUav;
        ComPtr<ID3D11ShaderResourceView> boardSrv;
        ComPtr<ID3D11Buffer> classicBuffer;
        ComPtr<ID3D11UnorderedAccessView> classicUav;
        std::uint32_t generation = 0;
        std::int64_t sourceTime100ns = 0;
        std::uint64_t deliveryOrder = 0;
    };

    ~Impl() {
        if (cudaMapped && cudaUnmapResources && tensorCudaResource) {
            CudaGraphicsResource resource = tensorCudaResource;
            cudaUnmapResources(1, &resource, nullptr);
        }
        if (tensorCudaResource && cudaUnregisterResource) {
            cudaUnregisterResource(tensorCudaResource);
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
            loadCudaFunction(cudaGetErrorString, "cudaGetErrorString", error);
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

    bool ensureScratchTexture(const D3D11_TEXTURE2D_DESC& source,
                              std::string& error) {
        if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            source.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
            error = "D3D11 board source must be RGB32 B8G8R8A8/B8G8R8X8";
            return false;
        }
        if (source.Width != static_cast<UINT>(frameWidth) ||
            source.Height != static_cast<UINT>(frameHeight) ||
            source.SampleDesc.Count != 1) {
            error = "D3D11 board source dimensions/sample count changed";
            return false;
        }
        if (scratchTexture && scratchFormat == source.Format) return true;
        scratchSrv.Reset();
        scratchTexture.Reset();
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(geometry.sourceWidth);
        description.Height = static_cast<UINT>(geometry.sourceHeight);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = source.Format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(
            &description, nullptr, &scratchTexture);
        if (FAILED(hr)) {
            error = "CreateTexture2D(board union scratch) failed: " + hrText(hr);
            return false;
        }
        hr = device->CreateShaderResourceView(
            scratchTexture.Get(), nullptr, &scratchSrv);
        if (FAILED(hr)) {
            error = "CreateShaderResourceView(board union scratch) failed: " + hrText(hr);
            return false;
        }
        scratchFormat = source.Format;
        return true;
    }

    void unbindCompute() {
        ID3D11ShaderResourceView* nullSrvs[2]{nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetShaderResources(0, 2, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    bool initialized = false;
    bool directDecoderTexture = false;
    bool cudaMapped = false;
    int frameWidth = 0;
    int frameHeight = 0;
    int cudaDevice = -1;
    std::size_t nextSlot = 0;
    std::uint32_t nextGeneration = 1;
    std::size_t frameBytes = 0;
    OnnxBoardModel* model = nullptr;
    Geometry geometry;
    std::vector<Slot> slots;

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
    CudaGraphicsResource tensorCudaResource = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> scratchTexture;
    ComPtr<ID3D11ShaderResourceView> scratchSrv;
    DXGI_FORMAT scratchFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Buffer> boardSampleBuffer;
    ComPtr<ID3D11ShaderResourceView> boardSampleSrv;
    ComPtr<ID3D11Buffer> globalBoardSampleBuffer;
    ComPtr<ID3D11ShaderResourceView> globalBoardSampleSrv;
    std::array<ComPtr<ID3D11Buffer>, Players> resizeSampleBuffers;
    std::array<ComPtr<ID3D11ShaderResourceView>, Players> resizeSampleSrvs;
    ComPtr<ID3D11Buffer> classicRectBuffer;
    ComPtr<ID3D11ShaderResourceView> classicRectSrv;
    ComPtr<ID3D11Buffer> tensorBuffer;
    ComPtr<ID3D11UnorderedAccessView> tensorUav;
    ComPtr<ID3D11Buffer> classicStaging;
    ComPtr<ID3D11ComputeShader> boardShader;
    ComPtr<ID3D11ComputeShader> tensorShader;
    ComPtr<ID3D11ComputeShader> classicShader;
};

D3D11BoardPreprocessor::D3D11BoardPreprocessor() = default;
D3D11BoardPreprocessor::~D3D11BoardPreprocessor() = default;

bool D3D11BoardPreprocessor::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    int frameWidth,
    int frameHeight,
    const Settings& settings,
    OnnxBoardModel* model,
    std::size_t memoryBudgetBytes,
    std::string& error) {
    error.clear();
    impl_.reset();
    if (!device || !context || !model || !model->cudaReady() ||
        frameWidth <= 0 || frameHeight <= 0) {
        error = "D3D11 board preprocessing received invalid initialization inputs";
        return false;
    }
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        error = "D3D11 board preprocessing requires the reader immediate context";
        return false;
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (contextDevice.Get() != device) {
        error = "D3D11 board context does not belong to the supplied device";
        return false;
    }

    auto candidate = std::make_unique<Impl>();
    candidate->device = device;
    candidate->context = context;
    candidate->frameWidth = frameWidth;
    candidate->frameHeight = frameHeight;
    candidate->model = model;
    candidate->directDecoderTexture = directDecoderTextureEnabled();
    candidate->geometry = buildGeometry(frameWidth, frameHeight, settings);
    if (candidate->geometry.boardPixelCount == 0 ||
        candidate->geometry.sourceWidth <= 0 || candidate->geometry.sourceHeight <= 0) {
        error = "D3D11 board geometry is empty";
        return false;
    }
    candidate->frameBytes =
        candidate->geometry.boardPixelCount * sizeof(std::uint32_t) +
        Players * CellsPerBoard * ClassicFields * sizeof(std::uint32_t);
    const std::size_t slotCount = std::min(
        MaximumFrames, memoryBudgetBytes / candidate->frameBytes);
    if (slotCount == 0) {
        error = "D3D11 board ring memory budget is smaller than one frame";
        return false;
    }

    D3D11_FEATURE_DATA_DOUBLES doubleSupport{};
    HRESULT hr = device->CheckFeatureSupport(
        D3D11_FEATURE_DOUBLES, &doubleSupport, sizeof(doubleSupport));
    if (FAILED(hr) || !doubleSupport.DoublePrecisionFloatShaderOps) {
        error = "D3D11 FP64 shader operations required for legacy bilinear matching are unavailable";
        return false;
    }
    if (!candidate->loadCompiler(error) || !candidate->loadRuntime(error)) return false;
    if (!compileShader(candidate->d3dCompile, device, BoardImageShaderSource,
                       candidate->boardShader, error) ||
        !compileShader(candidate->d3dCompile, device, TensorShaderSource,
                       candidate->tensorShader, error) ||
        !compileShader(candidate->d3dCompile, device, ClassicShaderSource,
                       candidate->classicShader, error)) {
        return false;
    }
    if (!createImmutableStructuredBuffer(
            device, candidate->geometry.boardSamples,
            candidate->boardSampleBuffer, candidate->boardSampleSrv, error) ||
        !createImmutableStructuredBuffer(
            device, candidate->geometry.globalBoardSamples,
            candidate->globalBoardSampleBuffer, candidate->globalBoardSampleSrv, error) ||
        !createImmutableStructuredBuffer(
            device, candidate->geometry.classicRects,
            candidate->classicRectBuffer, candidate->classicRectSrv, error)) {
        return false;
    }
    for (int player = 0; player < Players; ++player) {
        if (!createImmutableStructuredBuffer(
                device, candidate->geometry.resizeSamples[player],
                candidate->resizeSampleBuffers[player],
                candidate->resizeSampleSrvs[player], error)) {
            return false;
        }
    }
    ComPtr<ID3D11ShaderResourceView> unusedSrv;
    if (!createWritableStructuredBuffer(
            device, TensorValues, sizeof(float), false,
            candidate->tensorBuffer, candidate->tensorUav, unusedSrv, error)) {
        return false;
    }
    D3D11_BUFFER_DESC stagingDescription{};
    stagingDescription.ByteWidth = CellsPerBoard * ClassicFields * sizeof(std::uint32_t);
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateBuffer(
        &stagingDescription, nullptr, &candidate->classicStaging);
    if (FAILED(hr)) {
        error = "CreateBuffer(board classic staging) failed: " + hrText(hr);
        return false;
    }

    candidate->slots.resize(slotCount);
    for (Impl::Slot& slot : candidate->slots) {
        if (!createWritableStructuredBuffer(
                device,
                static_cast<UINT>(candidate->geometry.boardPixelCount),
                sizeof(std::uint32_t), true,
                slot.boardBuffer, slot.boardUav, slot.boardSrv, error)) {
            return false;
        }
        ComPtr<ID3D11ShaderResourceView> unusedClassicSrv;
        if (!createWritableStructuredBuffer(
                device, Players * CellsPerBoard * ClassicFields,
                sizeof(std::uint32_t), false,
                slot.classicBuffer, slot.classicUav, unusedClassicSrv, error)) {
            return false;
        }
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
        error = "IDXGIDevice::GetAdapter(board) failed: " + hrText(hr);
        return false;
    }
    int cudaStatus = candidate->cudaD3D11GetDevice(
        &candidate->cudaDevice, adapter.Get());
    if (cudaStatus != 0) {
        error = candidate->cudaError("cudaD3D11GetDevice(board)", cudaStatus);
        return false;
    }
    cudaStatus = candidate->cudaSetDevice(candidate->cudaDevice);
    if (cudaStatus != 0) {
        error = candidate->cudaError("cudaSetDevice(board)", cudaStatus);
        return false;
    }
    cudaStatus = candidate->cudaRegisterResource(
        &candidate->tensorCudaResource, candidate->tensorBuffer.Get(), 0);
    if (cudaStatus != 0) {
        error = candidate->cudaError(
            "cudaGraphicsD3D11RegisterResource(board)", cudaStatus);
        return false;
    }

    candidate->initialized = true;
    impl_ = std::move(candidate);
    return true;
}

bool D3D11BoardPreprocessor::retain(
    ID3D11Texture2D* texture,
    UINT sourceSubresource,
    std::int64_t sourceTime100ns,
    std::uint64_t deliveryOrder,
    D3D11BoardFrameHandle& handle,
    std::string& error) {
    error.clear();
    handle = {};
    if (!impl_ || !impl_->initialized || !texture) {
        error = "D3D11 board preprocessor is not initialized";
        return false;
    }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice.Get() != impl_->device.Get()) {
        error = "D3D11 board source texture belongs to a different device";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const UINT subresourceCount = description.MipLevels * description.ArraySize;
    if (sourceSubresource >= subresourceCount ||
        sourceSubresource % std::max(1u, description.MipLevels) != 0) {
        error = "D3D11 board source subresource is invalid or is not mip zero";
        return false;
    }
    const std::size_t slotIndex = impl_->nextSlot;
    Impl::Slot& slot = impl_->slots[slotIndex];
    ComPtr<ID3D11ShaderResourceView> directSourceSrv;
    ID3D11ShaderResourceView* sourceSrv = nullptr;
    ID3D11ShaderResourceView* sampleSrv = nullptr;
    const bool canBindDirectly = impl_->directDecoderTexture &&
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
        description.ArraySize == 1 && sourceSubresource == 0;
    if (canBindDirectly) {
        const HRESULT hr = impl_->device->CreateShaderResourceView(
            texture, nullptr, &directSourceSrv);
        if (FAILED(hr)) {
            error = "CreateShaderResourceView(direct board decoder texture) failed: " +
                hrText(hr);
            return false;
        }
        sourceSrv = directSourceSrv.Get();
        sampleSrv = impl_->globalBoardSampleSrv.Get();
    } else {
        if (!impl_->ensureScratchTexture(description, error)) return false;
        D3D11_BOX sourceBox{
            static_cast<UINT>(impl_->geometry.sourceX),
            static_cast<UINT>(impl_->geometry.sourceY), 0,
            static_cast<UINT>(impl_->geometry.sourceX + impl_->geometry.sourceWidth),
            static_cast<UINT>(impl_->geometry.sourceY + impl_->geometry.sourceHeight), 1
        };
        impl_->context->CopySubresourceRegion(
            impl_->scratchTexture.Get(), 0, 0, 0, 0,
            texture, sourceSubresource, &sourceBox);
        sourceSrv = impl_->scratchSrv.Get();
        sampleSrv = impl_->boardSampleSrv.Get();
    }

    ID3D11ShaderResourceView* boardSrvs[]{
        sourceSrv, sampleSrv
    };
    ID3D11UnorderedAccessView* boardUavs[]{slot.boardUav.Get()};
    impl_->context->CSSetShader(impl_->boardShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, boardSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, boardUavs, nullptr);
    impl_->context->Dispatch(static_cast<UINT>(
        (impl_->geometry.boardSamples.size() + 255) / 256), 1, 1);
    impl_->unbindCompute();

    ID3D11ShaderResourceView* classicSrvs[]{
        slot.boardSrv.Get(), impl_->classicRectSrv.Get()
    };
    ID3D11UnorderedAccessView* classicUavs[]{slot.classicUav.Get()};
    impl_->context->CSSetShader(impl_->classicShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, classicSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, classicUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.classicRects.size() + 63) / 64), 1, 1);
    impl_->unbindCompute();

    std::uint32_t generation = impl_->nextGeneration++;
    if (impl_->nextGeneration == 0) impl_->nextGeneration = 1;
    if (generation == 0) generation = impl_->nextGeneration++;
    slot.generation = generation;
    slot.sourceTime100ns = sourceTime100ns;
    slot.deliveryOrder = deliveryOrder;
    handle = {
        static_cast<std::uint32_t>(slotIndex), generation,
        sourceTime100ns, deliveryOrder
    };
    impl_->nextSlot = (slotIndex + 1) % impl_->slots.size();

    const HRESULT removed = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = "D3D11 device was removed during board retain: " + hrText(removed);
        handle = {};
        return false;
    }
    return true;
}

bool D3D11BoardPreprocessor::prepareFusedRetain(
    ID3D11Texture2D* texture,
    UINT sourceSubresource,
    std::int64_t sourceTime100ns,
    std::uint64_t deliveryOrder,
    D3D11BoardFusedTarget& target,
    std::string& error) {
    error.clear();
    target = {};
    if (!impl_ || !impl_->initialized || !texture) {
        error = "D3D11 board preprocessor is not initialized";
        return false;
    }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    if (textureDevice.Get() != impl_->device.Get()) {
        error = "D3D11 fused board source texture belongs to a different device";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!impl_->directDecoderTexture ||
        (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 ||
        description.Width != static_cast<UINT>(impl_->frameWidth) ||
        description.Height != static_cast<UINT>(impl_->frameHeight) ||
        description.SampleDesc.Count != 1 || description.ArraySize != 1 ||
        sourceSubresource != 0) {
        error = "D3D11 decoder texture cannot be bound to the fused board/queue shader";
        return false;
    }
    const std::size_t slotIndex = impl_->nextSlot;
    Impl::Slot& slot = impl_->slots[slotIndex];
    target.sampleSrv = impl_->globalBoardSampleSrv.Get();
    target.boardUav = slot.boardUav.Get();
    target.sampleCount = static_cast<std::uint32_t>(
        impl_->geometry.globalBoardSamples.size());
    target.slot = static_cast<std::uint32_t>(slotIndex);
    target.sourceTime100ns = sourceTime100ns;
    target.deliveryOrder = deliveryOrder;
    return true;
}

bool D3D11BoardPreprocessor::finishFusedRetain(
    const D3D11BoardFusedTarget& target,
    D3D11BoardFrameHandle& handle,
    std::string& error) {
    error.clear();
    handle = {};
    if (!impl_ || !impl_->initialized || target.slot != impl_->nextSlot ||
        target.slot >= impl_->slots.size() || !target.sampleSrv ||
        !target.boardUav || target.sampleCount != impl_->geometry.boardSamples.size()) {
        error = "D3D11 fused board target is stale or invalid";
        return false;
    }
    Impl::Slot& slot = impl_->slots[target.slot];
    if (target.sampleSrv != impl_->globalBoardSampleSrv.Get() ||
        target.boardUav != slot.boardUav.Get()) {
        error = "D3D11 fused board target resources do not match the active slot";
        return false;
    }
    ID3D11ShaderResourceView* classicSrvs[]{
        slot.boardSrv.Get(), impl_->classicRectSrv.Get()
    };
    ID3D11UnorderedAccessView* classicUavs[]{slot.classicUav.Get()};
    impl_->context->CSSetShader(impl_->classicShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, classicSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, classicUavs, nullptr);
    impl_->context->Dispatch(
        static_cast<UINT>((impl_->geometry.classicRects.size() + 63) / 64), 1, 1);
    impl_->unbindCompute();

    std::uint32_t generation = impl_->nextGeneration++;
    if (impl_->nextGeneration == 0) impl_->nextGeneration = 1;
    if (generation == 0) generation = impl_->nextGeneration++;
    slot.generation = generation;
    slot.sourceTime100ns = target.sourceTime100ns;
    slot.deliveryOrder = target.deliveryOrder;
    handle = {target.slot, generation, target.sourceTime100ns, target.deliveryOrder};
    impl_->nextSlot = (static_cast<std::size_t>(target.slot) + 1) % impl_->slots.size();

    const HRESULT removed = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        error = "D3D11 device was removed during fused board retain: " + hrText(removed);
        handle = {};
        return false;
    }
    return true;
}

bool D3D11BoardPreprocessor::infer(
    const D3D11BoardFrameHandle& handle,
    bool player1,
    D3D11BoardInference& result,
    std::string& error) {
    error.clear();
    result = {};
    if (!impl_ || !impl_->initialized || !valid(handle)) {
        error = "D3D11 board ring handle is stale or invalid";
        return false;
    }
    const int player = player1 ? 0 : 1;
    Impl::Slot& slot = impl_->slots[handle.slot];
    ID3D11ShaderResourceView* tensorSrvs[]{
        slot.boardSrv.Get(), impl_->resizeSampleSrvs[player].Get()
    };
    ID3D11UnorderedAccessView* tensorUavs[]{impl_->tensorUav.Get()};
    impl_->context->CSSetShader(impl_->tensorShader.Get(), nullptr, 0);
    impl_->context->CSSetShaderResources(0, 2, tensorSrvs);
    impl_->context->CSSetUnorderedAccessViews(0, 1, tensorUavs, nullptr);
    impl_->context->Dispatch(static_cast<UINT>(
        (impl_->geometry.resizeSamples[player].size() + 255) / 256), 1, 1);
    impl_->unbindCompute();

    int cudaStatus = impl_->cudaSetDevice(impl_->cudaDevice);
    if (cudaStatus != 0) {
        error = impl_->cudaError("cudaSetDevice(board inference)", cudaStatus);
        return false;
    }
    Impl::CudaGraphicsResource resource = impl_->tensorCudaResource;
    cudaStatus = impl_->cudaMapResources(1, &resource, nullptr);
    if (cudaStatus != 0) {
        error = impl_->cudaError("cudaGraphicsMapResources(board)", cudaStatus);
        return false;
    }
    impl_->cudaMapped = true;
    void* mappedTensor = nullptr;
    std::size_t mappedBytes = 0;
    cudaStatus = impl_->cudaGetMappedPointer(
        &mappedTensor, &mappedBytes, impl_->tensorCudaResource);
    std::vector<BoardProbabilities> probabilities;
    if (cudaStatus != 0) {
        error = impl_->cudaError(
            "cudaGraphicsResourceGetMappedPointer(board)", cudaStatus);
    } else if (!mappedTensor || mappedBytes < TensorValues * sizeof(float)) {
        error = "CUDA mapped board tensor is smaller than 200x3x32x32";
    } else {
        try {
            probabilities = impl_->model->inferCudaDevice(
                static_cast<float*>(mappedTensor), 1, error);
            if (error.empty() && probabilities.size() != 1) {
                error = "CUDA board inference returned an unexpected board count";
            }
        } catch (const std::exception& exception) {
            error = "CUDA board inference threw an exception: " +
                std::string(exception.what());
        } catch (...) {
            error = "CUDA board inference threw an unknown exception";
        }
    }
    resource = impl_->tensorCudaResource;
    const int unmapStatus = impl_->cudaUnmapResources(1, &resource, nullptr);
    impl_->cudaMapped = false;
    if (unmapStatus != 0 && error.empty()) {
        error = impl_->cudaError("cudaGraphicsUnmapResources(board)", unmapStatus);
    }
    if (!error.empty()) return false;

    const UINT classicBytes = CellsPerBoard * ClassicFields * sizeof(std::uint32_t);
    const UINT classicOffset = static_cast<UINT>(player) * classicBytes;
    D3D11_BOX classicBox{classicOffset, 0, 0, classicOffset + classicBytes, 1, 1};
    impl_->context->CopySubresourceRegion(
        impl_->classicStaging.Get(), 0, 0, 0, 0,
        slot.classicBuffer.Get(), 0, &classicBox);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = impl_->context->Map(
        impl_->classicStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        error = "Map(board classic statistics) failed: " + hrText(hr);
        return false;
    }
    const auto* sums = static_cast<const std::uint32_t*>(mapped.pData);
    VisibleBoard classic{};
    for (int cell = 0; cell < CellsPerBoard; ++cell) {
        const std::size_t base = static_cast<std::size_t>(cell) * ClassicFields;
        classic[cell] = classicCell(
            sums[base], sums[base + 1], sums[base + 2],
            impl_->geometry.classicCounts[static_cast<std::size_t>(
                player * CellsPerBoard + cell)]);
    }
    impl_->context->Unmap(impl_->classicStaging.Get(), 0);

    result.probabilities = probabilities.front();
    result.classic = classic;
    result.sourceTime100ns = slot.sourceTime100ns;
    result.deliveryOrder = slot.deliveryOrder;
    result.player1 = player1;
    return true;
}

bool D3D11BoardPreprocessor::valid(
    const D3D11BoardFrameHandle& handle) const {
    return impl_ && impl_->initialized && handle.generation != 0 &&
        handle.slot < impl_->slots.size() &&
        impl_->slots[handle.slot].generation == handle.generation;
}

void D3D11BoardPreprocessor::reset() {
    if (!impl_) return;
    for (Impl::Slot& slot : impl_->slots) slot.generation = 0;
    impl_->nextSlot = 0;
}

bool D3D11BoardPreprocessor::ready() const {
    return impl_ && impl_->initialized;
}

std::size_t D3D11BoardPreprocessor::capacity() const {
    return impl_ ? impl_->slots.size() : 0;
}

std::size_t D3D11BoardPreprocessor::bytesPerFrame() const {
    return impl_ ? impl_->frameBytes : 0;
}

} // namespace tr

#endif // Windows ultra GPU build
