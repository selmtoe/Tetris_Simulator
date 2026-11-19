#include "video_reader.hpp"
#if defined(TR_NVDEC_DEFAULT)
#include "nvdec_video_backend.hpp"
#endif

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace tr {

struct VideoReader::Impl {
    struct SparseStagingSlot {
        ComPtr<ID3D11Texture2D> texture;
        int width = 0;
        int height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> outputType;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    ComPtr<ID3D11Texture2D> stagingTexture;
    int stagingWidth = 0;
    int stagingHeight = 0;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    std::vector<SparseStagingSlot> sparseStaging;
    DWORD videoStream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    LONG stride = 0;
    bool d3d11 = false;
    bool reportedCpuBuffer = false;
    bool reportedD3D11Texture = false;
    std::wstring adapterName;
#if defined(TR_NVDEC_DEFAULT)
    std::unique_ptr<NvdecVideoBackend> nvdec;
#endif
};

namespace {

std::string hrText(HRESULT hr) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

bool getUInt64(IMFMediaType* type, REFGUID key, std::uint64_t& value) {
    UINT64 raw = 0;
    if (FAILED(type->GetUINT64(key, &raw))) return false;
    value = raw;
    return true;
}

bool advancedVideoProcessingEnabled(bool requested) {
    bool enabled = requested;
    // Keep the processing path independently switchable for A/B benchmarks.
    // A value of 0 forces the legacy software processor; any other non-empty
    // value enables the advanced Source Reader video processor.
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA(
        "TR_MF_ADVANCED_PROCESSING", value, static_cast<DWORD>(std::size(value)));
    if (length > 0 && length < std::size(value)) enabled = std::atoi(value) != 0;
    return enabled;
}

int gpuVideoMode() {
#ifdef TR_D3D11_VIDEO_DEFAULT
    int mode = TR_D3D11_VIDEO_DEFAULT;
#else
    int mode = 0;
#endif
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA(
        "TR_GPU_VIDEO", value, static_cast<DWORD>(std::size(value)));
    if (length > 0 && length < std::size(value)) mode = std::clamp(std::atoi(value), 0, 3);
    return mode;
}

bool gpuDiagnosticsEnabled() {
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA(
        "TR_GPU_DIAGNOSTICS", value, static_cast<DWORD>(std::size(value)));
    return length > 0 && length < std::size(value) && std::atoi(value) != 0;
}

template <typename ReaderImpl>
bool createHighPerformanceD3D11Device(ReaderImpl& impl, std::string& error) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = "CreateDXGIFactory1 failed: " + hrText(hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> selected;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6))) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            hr = factory6->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
            selected = std::move(candidate);
            impl.adapterName = description.Description;
            break;
        }
    }
    if (!selected) {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            hr = factory->EnumAdapters1(index, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
            selected = std::move(candidate);
            impl.adapterName = description.Description;
            break;
        }
    }
    if (!selected) {
        error = "No hardware DXGI adapter is available";
        return false;
    }

    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actualLevel{};
    hr = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                           levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                           &impl.d3dDevice, &actualLevel, &impl.d3dContext);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               levels + 1, 1, D3D11_SDK_VERSION,
                               &impl.d3dDevice, &actualLevel, &impl.d3dContext);
    }
    if (FAILED(hr)) {
        error = "D3D11CreateDevice failed: " + hrText(hr);
        return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(impl.d3dDevice.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    UINT resetToken = 0;
    hr = MFCreateDXGIDeviceManager(&resetToken, &impl.dxgiManager);
    if (FAILED(hr)) {
        error = "MFCreateDXGIDeviceManager failed: " + hrText(hr);
        return false;
    }
    hr = impl.dxgiManager->ResetDevice(impl.d3dDevice.Get(), resetToken);
    if (FAILED(hr)) {
        error = "IMFDXGIDeviceManager::ResetDevice failed: " + hrText(hr);
        return false;
    }
    return true;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, size)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::string videoSubtypeName(REFGUID subtype) {
    if (subtype == MFVideoFormat_H264) return "H264";
    if (subtype == MFVideoFormat_HEVC) return "HEVC";
    if (subtype == MFVideoFormat_MJPG) return "MJPG";
    if (subtype == MFVideoFormat_RGB32) return "RGB32";
    wchar_t value[64]{};
    if (StringFromGUID2(subtype, value, static_cast<int>(std::size(value))) > 0) {
        return utf8(value);
    }
    return "unknown";
}

} // namespace

VideoReader::VideoReader() = default;
VideoReader::~VideoReader() { close(); }

bool VideoReader::usesD3D11() const {
    return impl_ && impl_->d3d11;
}

bool VideoReader::usesNvdecCuda() const {
#if defined(TR_NVDEC_DEFAULT)
    return impl_ && impl_->nvdec != nullptr;
#else
    return false;
#endif
}

bool VideoReader::open(const std::filesystem::path& path, std::string& error,
                       bool advancedVideoProcessing,
                       bool forceLegacyProcessing,
                       bool preferNvdec) {
    close();
    impl_ = std::make_unique<Impl>();

#if defined(TR_NVDEC_DEFAULT)
    if (preferNvdec && !forceLegacyProcessing) {
        impl_->nvdec = std::make_unique<NvdecVideoBackend>();
        if (!impl_->nvdec->open(path, error)) {
            close();
            return false;
        }
        width_ = impl_->nvdec->width();
        height_ = impl_->nvdec->height();
        durationSeconds_ = impl_->nvdec->durationSeconds();
        frameRate_ = impl_->nvdec->frameRate();
        impl_->d3d11 = true;
        if (gpuDiagnosticsEnabled()) {
            std::cerr << "video_backend=nvdec-cuda-d3d11"
                      << " native_codec=HEVC"
                      << " output_fps=" << frameRate_ << '\n';
        }
        return true;
    }
#else
    (void)preferNvdec;
#endif

    const bool advanced = forceLegacyProcessing
        ? false : advancedVideoProcessingEnabled(advancedVideoProcessing);
    const int requestedGpuMode = forceLegacyProcessing ? 0 : gpuVideoMode();
    const bool requestD3D11 = requestedGpuMode >= 2 ||
                              (requestedGpuMode == 1 && advancedVideoProcessing);
    const bool d3d11WithLegacyConverter = requestedGpuMode == 3 && !advancedVideoProcessing;

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 10);
    if (FAILED(hr)) { error = "MFCreateAttributes failed: " + hrText(hr); return false; }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (requestD3D11) {
        if (!createHighPerformanceD3D11Device(*impl_, error)) {
            close();
            return false;
        }
        attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, impl_->dxgiManager.Get());
        attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
        attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        if (d3d11WithLegacyConverter) {
            // Keep the established software YUV->RGB32 converter and its
            // frame delivery semantics, while making a D3D11 manager
            // available to the HEVC decoder for DXVA acceleration.
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        } else {
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        impl_->d3d11 = true;
    } else if (advanced) {
        // The Windows 8+ advanced processor may select XVP or another
        // registered video processor.  It supports a wider range of colour
        // conversion paths and can use hardware for some conversions.
        attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    } else {
        // Preserve the established YUV -> RGB32 software conversion path.
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);

    hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &impl_->reader);
    if (FAILED(hr)) { error = "動画を開けませんでした: " + hrText(hr); close(); return false; }

    ComPtr<IMFMediaType> nativeType;
    hr = impl_->reader->GetNativeMediaType(impl_->videoStream, 0, &nativeType);
    if (FAILED(hr)) { error = "動画の映像ストリームを取得できませんでした: " + hrText(hr); close(); return false; }

    std::uint64_t packedSize = 0;
    if (!getUInt64(nativeType.Get(), MF_MT_FRAME_SIZE, packedSize)) {
        error = "動画サイズを取得できませんでした"; close(); return false;
    }
    width_ = static_cast<int>(packedSize >> 32);
    height_ = static_cast<int>(packedSize & 0xffffffffu);

    std::uint64_t packedRate = 0;
    if (getUInt64(nativeType.Get(), MF_MT_FRAME_RATE, packedRate)) {
        const UINT32 numerator = static_cast<UINT32>(packedRate >> 32);
        const UINT32 denominator = static_cast<UINT32>(packedRate & 0xffffffffu);
        if (denominator) frameRate_ = static_cast<double>(numerator) / denominator;
    }

    PROPVARIANT duration{};
    PropVariantInit(&duration);
    if (SUCCEEDED(impl_->reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                           MF_PD_DURATION, &duration)) && duration.vt == VT_UI8) {
        durationSeconds_ = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&duration);

    ComPtr<IMFMediaType> rgbType;
    hr = MFCreateMediaType(&rgbType);
    if (FAILED(hr)) { error = "MFCreateMediaType failed: " + hrText(hr); close(); return false; }
    rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    rgbType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    rgbType->SetUINT64(MF_MT_FRAME_SIZE, packedSize);
    hr = impl_->reader->SetCurrentMediaType(impl_->videoStream, nullptr, rgbType.Get());
    if (FAILED(hr)) {
        const std::string mediaFoundationError =
            "RGB32形式への変換を設定できませんでした: " + hrText(hr);
#if defined(TR_NVDEC_DEFAULT)
        // Windows Media Foundation commonly has no AV1 RGB32 transform even
        // when the NVIDIA GPU can decode that same stream. Compatibility and
        // verification passes request the MF path explicitly, so fall back to
        // the native NVDEC backend instead of failing those later passes.
        close();
        impl_ = std::make_unique<Impl>();
        impl_->nvdec = std::make_unique<NvdecVideoBackend>();
        std::string nvdecError;
        if (!impl_->nvdec->open(path, nvdecError)) {
            error = mediaFoundationError + "; NVDEC fallback failed: " + nvdecError;
            close();
            return false;
        }
        width_ = impl_->nvdec->width();
        height_ = impl_->nvdec->height();
        durationSeconds_ = impl_->nvdec->durationSeconds();
        frameRate_ = impl_->nvdec->frameRate();
        impl_->d3d11 = true;
        if (gpuDiagnosticsEnabled()) {
            std::cerr << "video_backend=nvdec-fallback-after-mf-rgb32"
                      << " output_fps=" << frameRate_ << '\n';
        }
        return true;
#else
        error = mediaFoundationError;
        close();
        return false;
#endif
    }
    ComPtr<IMFMediaType> actualOutputType;
    impl_->reader->GetCurrentMediaType(impl_->videoStream, &actualOutputType);
    UINT32 packedStride = 0;
    if (SUCCEEDED(rgbType->GetUINT32(MF_MT_DEFAULT_STRIDE, &packedStride))) {
        impl_->stride = static_cast<LONG>(packedStride);
    } else {
        impl_->stride = static_cast<LONG>(width_ * 4);
    }
    impl_->outputType = rgbType;
    if (gpuDiagnosticsEnabled()) {
        GUID nativeSubtype{};
        const bool hasNativeSubtype = SUCCEEDED(
            nativeType->GetGUID(MF_MT_SUBTYPE, &nativeSubtype));
        std::cerr << "video_backend="
                  << (impl_->d3d11 ? (d3d11WithLegacyConverter ? "d3d11-legacy-converter" : "d3d11")
                                   : (advanced ? "mf-advanced" : "mf-legacy"));
        if (impl_->d3d11) std::cerr << " adapter=\"" << utf8(impl_->adapterName) << '"';
        if (hasNativeSubtype) std::cerr << " native_codec=" << videoSubtypeName(nativeSubtype);
        std::uint64_t actualRate = 0;
        if (actualOutputType && getUInt64(actualOutputType.Get(), MF_MT_FRAME_RATE, actualRate)) {
            const UINT32 numerator = static_cast<UINT32>(actualRate >> 32);
            const UINT32 denominator = static_cast<UINT32>(actualRate & 0xffffffffu);
            if (denominator) {
                std::cerr << " output_fps=" << static_cast<double>(numerator) / denominator;
            }
        }
        std::cerr << '\n';
    }
    return true;
}

void VideoReader::setGpuConversionRegions(const std::vector<PixelRegion>& regions) {
#if defined(TR_NVDEC_DEFAULT)
    if (impl_ && impl_->nvdec) impl_->nvdec->setConversionRegions(regions);
#else
    (void)regions;
#endif
}

bool VideoReader::seek(double seconds, std::string& error) {
#if defined(TR_NVDEC_DEFAULT)
    if (impl_ && impl_->nvdec) return impl_->nvdec->seek(seconds, error);
#endif
    if (!impl_ || !impl_->reader) {
        error = "VideoReader is not open";
        return false;
    }
    PROPVARIANT position{};
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, seconds) * 10000000.0);
    // Media Foundation's media-time format is GUID_NULL.
    const HRESULT hr = impl_->reader->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    if (FAILED(hr)) {
        error = "Video seek failed: " + hrText(hr);
        return false;
    }
    return true;
}

bool VideoReader::read(Frame& frame, bool& endOfStream, std::string& error) {
    return readFiltered(frame, endOfStream, error, [](std::int64_t) { return true; });
}

bool VideoReader::readFiltered(Frame& frame, bool& endOfStream, std::string& error,
                               const std::function<bool(std::int64_t)>& needPixels) {
    return readFilteredRegion(frame, endOfStream, error,
                              {0, 0, width_, height_}, needPixels);
}

bool VideoReader::readFilteredRegion(Frame& frame, bool& endOfStream, std::string& error,
                                     const PixelRegion& requestedRegion,
                                     const std::function<bool(std::int64_t)>& needPixels) {
    endOfStream = false;
    // Every output byte is overwritten below.  Clearing the vector keeps its
    // capacity so sequential decoding does not allocate a full RGB frame for
    // every decoded sample.
    frame.width = 0;
    frame.height = 0;
    frame.sourceX = 0;
    frame.sourceY = 0;
    frame.time100ns = 0;
    frame.bgra.clear();
    const int regionX0 = std::clamp(requestedRegion.x, 0, width_);
    const int regionY0 = std::clamp(requestedRegion.y, 0, height_);
    const int regionX1 = std::clamp(requestedRegion.x + requestedRegion.width, 0, width_);
    const int regionY1 = std::clamp(requestedRegion.y + requestedRegion.height, 0, height_);
    if (regionX1 <= regionX0 || regionY1 <= regionY0) {
        error = "Video readback region is empty";
        return false;
    }
    const int regionWidth = regionX1 - regionX0;
    const int regionHeight = regionY1 - regionY0;
    frame.width = regionWidth;
    frame.height = regionHeight;
    frame.sourceX = regionX0;
    frame.sourceY = regionY0;
    if (!impl_ || !impl_->reader) {
#if defined(TR_NVDEC_DEFAULT)
        if (impl_ && impl_->nvdec) {
            std::int64_t timestamp = 0;
            return impl_->nvdec->readD3D11Sample(
                timestamp, endOfStream, error,
                [&](ID3D11Device* device, ID3D11DeviceContext* context,
                    ID3D11Texture2D* texture, UINT sourceSubresource,
                    std::int64_t sampleTimestamp, std::string& callbackError) {
                    frame.time100ns = sampleTimestamp;
                    if (needPixels && !needPixels(sampleTimestamp)) return true;
                    D3D11_TEXTURE2D_DESC sourceDescription{};
                    texture->GetDesc(&sourceDescription);
                    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                        sourceDescription.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
                        callbackError = "NVDEC RGB32 texture has an unexpected DXGI format: " +
                            std::to_string(static_cast<unsigned>(sourceDescription.Format));
                        return false;
                    }
                    if (!impl_->stagingTexture ||
                        impl_->stagingWidth != regionWidth ||
                        impl_->stagingHeight != regionHeight ||
                        impl_->stagingFormat != sourceDescription.Format) {
                        D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
                        stagingDescription.Width = static_cast<UINT>(regionWidth);
                        stagingDescription.Height = static_cast<UINT>(regionHeight);
                        stagingDescription.MipLevels = 1;
                        stagingDescription.ArraySize = 1;
                        stagingDescription.Usage = D3D11_USAGE_STAGING;
                        stagingDescription.BindFlags = 0;
                        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        stagingDescription.MiscFlags = 0;
                        const HRESULT createStatus = device->CreateTexture2D(
                            &stagingDescription, nullptr, &impl_->stagingTexture);
                        if (FAILED(createStatus)) {
                            callbackError = "NVDEC readback texture could not be created: " +
                                hrText(createStatus);
                            return false;
                        }
                        impl_->stagingWidth = regionWidth;
                        impl_->stagingHeight = regionHeight;
                        impl_->stagingFormat = sourceDescription.Format;
                    }
                    D3D11_BOX sourceBox{};
                    sourceBox.left = static_cast<UINT>(regionX0);
                    sourceBox.top = static_cast<UINT>(regionY0);
                    sourceBox.front = 0;
                    sourceBox.right = static_cast<UINT>(regionX1);
                    sourceBox.bottom = static_cast<UINT>(regionY1);
                    sourceBox.back = 1;
                    context->CopySubresourceRegion(
                        impl_->stagingTexture.Get(), 0, 0, 0, 0,
                        texture, sourceSubresource, &sourceBox);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    const HRESULT mapStatus = context->Map(
                        impl_->stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                    if (FAILED(mapStatus)) {
                        callbackError = "NVDEC video readback failed: " + hrText(mapStatus);
                        return false;
                    }
                    const std::size_t rowBytes =
                        static_cast<std::size_t>(regionWidth) * 4;
                    frame.bgra.resize(
                        rowBytes * static_cast<std::size_t>(regionHeight));
                    for (int y = 0; y < regionHeight; ++y) {
                        const auto* source = static_cast<const BYTE*>(mapped.pData) +
                            static_cast<std::size_t>(y) * mapped.RowPitch;
                        std::memcpy(
                            frame.bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                            source, rowBytes);
                    }
                    context->Unmap(impl_->stagingTexture.Get(), 0);
                    return true;
                });
        }
#endif
        error = "VideoReader is not open"; return false;
    }

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = impl_->reader->ReadSample(impl_->videoStream, 0, &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) { error = "映像フレームの読み込みに失敗しました: " + hrText(hr); return false; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { endOfStream = true; return true; }
    if (!sample) return true;

    frame.time100ns = timestamp;
    if (needPixels && !needPixels(timestamp)) return true;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) { error = "映像バッファの取得に失敗しました: " + hrText(hr); return false; }

    if (impl_->d3d11) {
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
            ComPtr<ID3D11Texture2D> texture;
            hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
            if (FAILED(hr)) {
                error = "GPU映像テクスチャを取得できませんでした: " + hrText(hr);
                return false;
            }
            UINT sourceSubresource = 0;
            dxgiBuffer->GetSubresourceIndex(&sourceSubresource);
            D3D11_TEXTURE2D_DESC sourceDescription{};
            texture->GetDesc(&sourceDescription);
            if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                sourceDescription.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
                error = "GPU RGB32 texture has an unexpected DXGI format: " +
                        std::to_string(static_cast<unsigned>(sourceDescription.Format));
                return false;
            }
            if (!impl_->stagingTexture || impl_->stagingWidth != regionWidth ||
                impl_->stagingHeight != regionHeight ||
                impl_->stagingFormat != sourceDescription.Format) {
                D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
                stagingDescription.Width = static_cast<UINT>(regionWidth);
                stagingDescription.Height = static_cast<UINT>(regionHeight);
                stagingDescription.MipLevels = 1;
                stagingDescription.ArraySize = 1;
                stagingDescription.Usage = D3D11_USAGE_STAGING;
                stagingDescription.BindFlags = 0;
                stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                stagingDescription.MiscFlags = 0;
                hr = impl_->d3dDevice->CreateTexture2D(&stagingDescription, nullptr,
                                                       &impl_->stagingTexture);
                if (FAILED(hr)) {
                    error = "GPU readback texture could not be created: " + hrText(hr);
                    return false;
                }
                impl_->stagingWidth = regionWidth;
                impl_->stagingHeight = regionHeight;
                impl_->stagingFormat = sourceDescription.Format;
            }
            D3D11_BOX sourceBox{};
            sourceBox.left = static_cast<UINT>(regionX0);
            sourceBox.top = static_cast<UINT>(regionY0);
            sourceBox.front = 0;
            sourceBox.right = static_cast<UINT>(regionX1);
            sourceBox.bottom = static_cast<UINT>(regionY1);
            sourceBox.back = 1;
            impl_->d3dContext->CopySubresourceRegion(impl_->stagingTexture.Get(), 0, 0, 0, 0,
                                                      texture.Get(), sourceSubresource, &sourceBox);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = impl_->d3dContext->Map(impl_->stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr)) {
                error = "GPU video readback failed: " + hrText(hr);
                return false;
            }
            const std::size_t rowBytes = static_cast<std::size_t>(regionWidth) * 4;
            frame.time100ns = timestamp;
            frame.bgra.resize(rowBytes * static_cast<std::size_t>(regionHeight));
            for (int y = 0; y < regionHeight; ++y) {
                const auto* source = static_cast<const BYTE*>(mapped.pData) +
                                     static_cast<std::size_t>(y) * mapped.RowPitch;
                std::memcpy(frame.bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                            source, rowBytes);
            }
            impl_->d3dContext->Unmap(impl_->stagingTexture.Get(), 0);
            return true;
        }
        if (gpuDiagnosticsEnabled() && !impl_->reportedCpuBuffer) {
            std::cerr << "video_backend_warning=d3d11_source_returned_cpu_buffer\n";
            impl_->reportedCpuBuffer = true;
        }
    }

    BYTE* bytes = nullptr;
    DWORD maxLength = 0, currentLength = 0;
    hr = buffer->Lock(&bytes, &maxLength, &currentLength);
    if (FAILED(hr)) { error = "映像バッファのロックに失敗しました: " + hrText(hr); return false; }

    const std::size_t rowBytes = static_cast<std::size_t>(regionWidth) * 4;
    const std::size_t sourceStride = static_cast<std::size_t>(std::abs(impl_->stride));
    const std::size_t needed = sourceStride * static_cast<std::size_t>(height_);
    if (currentLength < needed) {
        buffer->Unlock();
        error = "映像バッファのサイズが不正です";
        return false;
    }
    frame.time100ns = timestamp;
    frame.bgra.resize(rowBytes * static_cast<std::size_t>(regionHeight));
    // Respect the Media Foundation stride and its sign. This prevents vertically flipped
    // boards when a decoder exposes a bottom-up RGB32 DIB.
    for (int y = 0; y < regionHeight; ++y) {
        const int fullSourceY = regionY0 + y;
        const int sourceY = impl_->stride >= 0 ? fullSourceY : (height_ - 1 - fullSourceY);
        const BYTE* source = bytes + static_cast<std::size_t>(sourceY) * sourceStride +
                             static_cast<std::size_t>(regionX0) * 4;
        std::copy(source, source + rowBytes,
                  frame.bgra.begin() + static_cast<std::size_t>(y) * rowBytes);
    }
    buffer->Unlock();
    return true;
}

bool VideoReader::readSparseRegions(Frame& frame, bool& endOfStream, std::string& error,
                                    const std::vector<PixelRegion>& regions) {
    endOfStream = false;
    frame.width = width_;
    frame.height = height_;
    frame.sourceX = 0;
    frame.sourceY = 0;
    frame.time100ns = 0;
    if (!impl_ || !impl_->reader) { error = "VideoReader is not open"; return false; }
    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = impl_->reader->ReadSample(
        impl_->videoStream, 0, &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) {
        error = "映像フレームの読み込みに失敗しました: " + hrText(hr);
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { endOfStream = true; return true; }
    if (!sample) {
        frame.width = 0;
        frame.height = 0;
        frame.bgra.clear();
        return true;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) {
        error = "映像バッファの取得に失敗しました: " + hrText(hr);
        return false;
    }
    const std::size_t fullBytes = static_cast<std::size_t>(width_) * height_ * 4;
    if (frame.bgra.size() != fullBytes) frame.bgra.resize(fullBytes);

    if (impl_->d3d11) {
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
            ComPtr<ID3D11Texture2D> texture;
            hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
            if (FAILED(hr)) {
                error = "GPU映像テクスチャを取得できませんでした: " + hrText(hr);
                return false;
            }
            UINT sourceSubresource = 0;
            dxgiBuffer->GetSubresourceIndex(&sourceSubresource);
            D3D11_TEXTURE2D_DESC sourceDescription{};
            texture->GetDesc(&sourceDescription);
            if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                sourceDescription.Format != DXGI_FORMAT_B8G8R8X8_UNORM) {
                error = "GPU RGB32 texture has an unexpected DXGI format: " +
                        std::to_string(static_cast<unsigned>(sourceDescription.Format));
                return false;
            }

            if (impl_->sparseStaging.size() < regions.size()) {
                impl_->sparseStaging.resize(regions.size());
            }
            struct PendingRegion {
                int x0 = 0;
                int y0 = 0;
                int width = 0;
                int height = 0;
                std::size_t slot = 0;
            };
            std::vector<PendingRegion> pending;
            pending.reserve(regions.size());
            for (std::size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
                const PixelRegion& requested = regions[regionIndex];
                const int x0 = std::clamp(requested.x, 0, width_);
                const int y0 = std::clamp(requested.y, 0, height_);
                const int x1 = std::clamp(requested.x + requested.width, 0, width_);
                const int y1 = std::clamp(requested.y + requested.height, 0, height_);
                if (x1 <= x0 || y1 <= y0) continue;
                const int regionWidth = x1 - x0;
                const int regionHeight = y1 - y0;
                auto& slot = impl_->sparseStaging[regionIndex];
                if (!slot.texture || slot.width != regionWidth || slot.height != regionHeight ||
                    slot.format != sourceDescription.Format) {
                    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
                    stagingDescription.Width = static_cast<UINT>(regionWidth);
                    stagingDescription.Height = static_cast<UINT>(regionHeight);
                    stagingDescription.MipLevels = 1;
                    stagingDescription.ArraySize = 1;
                    stagingDescription.Usage = D3D11_USAGE_STAGING;
                    stagingDescription.BindFlags = 0;
                    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDescription.MiscFlags = 0;
                    slot.texture.Reset();
                    hr = impl_->d3dDevice->CreateTexture2D(
                        &stagingDescription, nullptr, &slot.texture);
                    if (FAILED(hr)) {
                        error = "GPU sparse readback texture could not be created: " + hrText(hr);
                        return false;
                    }
                    slot.width = regionWidth;
                    slot.height = regionHeight;
                    slot.format = sourceDescription.Format;
                }
                D3D11_BOX sourceBox{};
                sourceBox.left = static_cast<UINT>(x0);
                sourceBox.top = static_cast<UINT>(y0);
                sourceBox.front = 0;
                sourceBox.right = static_cast<UINT>(x1);
                sourceBox.bottom = static_cast<UINT>(y1);
                sourceBox.back = 1;
                impl_->d3dContext->CopySubresourceRegion(
                    slot.texture.Get(), 0, 0, 0, 0,
                    texture.Get(), sourceSubresource, &sourceBox);
                pending.push_back({x0, y0, regionWidth, regionHeight, regionIndex});
            }
            // Submit every GPU copy before the first blocking Map so the
            // command stream can overlap independent region transfers.
            for (const PendingRegion& region : pending) {
                auto& slot = impl_->sparseStaging[region.slot];
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = impl_->d3dContext->Map(slot.texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
                if (FAILED(hr)) {
                    error = "GPU sparse video readback failed: " + hrText(hr);
                    return false;
                }
                const std::size_t rowBytes = static_cast<std::size_t>(region.width) * 4;
                for (int y = 0; y < region.height; ++y) {
                    const auto* source = static_cast<const BYTE*>(mapped.pData) +
                                         static_cast<std::size_t>(y) * mapped.RowPitch;
                    auto* destination = frame.bgra.data() +
                        (static_cast<std::size_t>(region.y0 + y) * width_ + region.x0) * 4;
                    std::memcpy(destination, source, rowBytes);
                }
                impl_->d3dContext->Unmap(slot.texture.Get(), 0);
            }
            frame.time100ns = timestamp;
            return true;
        }
        if (gpuDiagnosticsEnabled() && !impl_->reportedCpuBuffer) {
            std::cerr << "video_backend_warning=d3d11_source_returned_cpu_buffer\n";
            impl_->reportedCpuBuffer = true;
        }
    }

    BYTE* bytes = nullptr;
    DWORD maxLength = 0, currentLength = 0;
    hr = buffer->Lock(&bytes, &maxLength, &currentLength);
    if (FAILED(hr)) {
        error = "映像バッファのロックに失敗しました: " + hrText(hr);
        return false;
    }
    const std::size_t sourceStride = static_cast<std::size_t>(std::abs(impl_->stride));
    const std::size_t needed = sourceStride * static_cast<std::size_t>(height_);
    if (currentLength < needed) {
        buffer->Unlock();
        error = "映像バッファのサイズが不正です";
        return false;
    }

    for (const PixelRegion& requested : regions) {
        const int x0 = std::clamp(requested.x, 0, width_);
        const int y0 = std::clamp(requested.y, 0, height_);
        const int x1 = std::clamp(requested.x + requested.width, 0, width_);
        const int y1 = std::clamp(requested.y + requested.height, 0, height_);
        if (x1 <= x0 || y1 <= y0) continue;
        const std::size_t rowBytes = static_cast<std::size_t>(x1 - x0) * 4;
        for (int y = y0; y < y1; ++y) {
            const int sourceY = impl_->stride >= 0 ? y : (height_ - 1 - y);
            const BYTE* source = bytes + static_cast<std::size_t>(sourceY) * sourceStride +
                                 static_cast<std::size_t>(x0) * 4;
            std::memcpy(frame.bgra.data() +
                            (static_cast<std::size_t>(y) * width_ + x0) * 4,
                        source, rowBytes);
        }
    }
    buffer->Unlock();
    frame.time100ns = timestamp;
    return true;
}

bool VideoReader::readD3D11Sample(std::int64_t& timestamp100ns, bool& endOfStream,
                                  std::string& error,
                                  const D3D11SampleConsumer& consumer) {
    timestamp100ns = 0;
    endOfStream = false;
#if defined(TR_NVDEC_DEFAULT)
    if (impl_ && impl_->nvdec) {
        return impl_->nvdec->readD3D11Sample(
            timestamp100ns, endOfStream, error, consumer);
    }
#endif
    if (!impl_ || !impl_->reader) {
        error = "VideoReader is not open";
        return false;
    }
    if (!impl_->d3d11 || !impl_->d3dDevice || !impl_->d3dContext) {
        error = "D3D11 sample delivery requires the D3D11 video path";
        return false;
    }

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = impl_->reader->ReadSample(
        impl_->videoStream, 0, &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) {
        error = "映像フレームの読み込みに失敗しました: " + hrText(hr);
        return false;
    }
    timestamp100ns = timestamp;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        endOfStream = true;
        return true;
    }
    if (!sample) return true;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) {
        error = "映像バッファの取得に失敗しました: " + hrText(hr);
        return false;
    }
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer.As(&dxgiBuffer))) {
        error = "D3D11 video sample unexpectedly returned a CPU buffer";
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        error = "GPU映像テクスチャを取得できませんでした: " + hrText(hr);
        return false;
    }
    UINT sourceSubresource = 0;
    hr = dxgiBuffer->GetSubresourceIndex(&sourceSubresource);
    if (FAILED(hr)) {
        error = "GPU video subresource index could not be read: " + hrText(hr);
        return false;
    }
    if (gpuDiagnosticsEnabled() && !impl_->reportedD3D11Texture) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        std::cerr << "d3d11_input_texture"
                  << " width=" << description.Width
                  << " height=" << description.Height
                  << " format=" << static_cast<unsigned>(description.Format)
                  << " mip_levels=" << description.MipLevels
                  << " array_size=" << description.ArraySize
                  << " usage=" << static_cast<unsigned>(description.Usage)
                  << " bind_flags=0x" << std::hex << description.BindFlags
                  << " cpu_access=0x" << description.CPUAccessFlags
                  << " misc=0x" << description.MiscFlags << std::dec
                  << " subresource=" << sourceSubresource << '\n';
        impl_->reportedD3D11Texture = true;
    }
    if (!consumer) return true;
    return consumer(impl_->d3dDevice.Get(), impl_->d3dContext.Get(), texture.Get(),
                    sourceSubresource, timestamp, error);
}

bool VideoReader::readCudaNv12Sample(
    std::int64_t& timestamp100ns, bool& endOfStream,
    std::string& error, const CudaNv12SampleConsumer& consumer,
    void* consumerStream) {
#if defined(TR_NVDEC_DEFAULT)
    if (impl_ && impl_->nvdec) {
        return impl_->nvdec->readCudaSample(
            timestamp100ns, endOfStream, error, consumer, consumerStream);
    }
#else
    (void)timestamp100ns;
    (void)endOfStream;
    (void)consumer;
    (void)consumerStream;
#endif
    error = "CUDA NV12 sample delivery requires the NVDEC backend";
    return false;
}

void VideoReader::close() {
#if defined(TR_NVDEC_DEFAULT)
    if (impl_ && impl_->nvdec) impl_->nvdec->close();
#endif
    if (impl_ && impl_->reader) impl_->reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    impl_.reset();
    width_ = height_ = 0;
    durationSeconds_ = frameRate_ = 0;
}

} // namespace tr
