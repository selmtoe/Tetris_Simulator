#include "video_reader.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace tr {

struct VideoReader::Impl {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> outputType;
    DWORD videoStream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    LONG stride = 0;
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

} // namespace

VideoReader::VideoReader() = default;
VideoReader::~VideoReader() { close(); }

bool VideoReader::open(const std::filesystem::path& path, std::string& error) {
    close();
    impl_ = std::make_unique<Impl>();

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 4);
    if (FAILED(hr)) { error = "MFCreateAttributes failed: " + hrText(hr); return false; }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
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
    if (FAILED(hr)) { error = "RGB32形式への変換を設定できませんでした: " + hrText(hr); close(); return false; }
    UINT32 packedStride = 0;
    if (SUCCEEDED(rgbType->GetUINT32(MF_MT_DEFAULT_STRIDE, &packedStride))) {
        impl_->stride = static_cast<LONG>(packedStride);
    } else {
        impl_->stride = static_cast<LONG>(width_ * 4);
    }
    impl_->outputType = rgbType;
    return true;
}

bool VideoReader::read(Frame& frame, bool& endOfStream, std::string& error) {
    endOfStream = false;
    frame = {};
    if (!impl_ || !impl_->reader) { error = "VideoReader is not open"; return false; }

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    HRESULT hr = impl_->reader->ReadSample(impl_->videoStream, 0, &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) { error = "映像フレームの読み込みに失敗しました: " + hrText(hr); return false; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { endOfStream = true; return true; }
    if (!sample) return true;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) { error = "映像バッファの取得に失敗しました: " + hrText(hr); return false; }

    BYTE* bytes = nullptr;
    DWORD maxLength = 0, currentLength = 0;
    hr = buffer->Lock(&bytes, &maxLength, &currentLength);
    if (FAILED(hr)) { error = "映像バッファのロックに失敗しました: " + hrText(hr); return false; }

    const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4;
    const std::size_t sourceStride = static_cast<std::size_t>(std::abs(impl_->stride));
    const std::size_t needed = sourceStride * static_cast<std::size_t>(height_);
    if (currentLength < needed) {
        buffer->Unlock();
        error = "映像バッファのサイズが不正です";
        return false;
    }
    frame.width = width_;
    frame.height = height_;
    frame.time100ns = timestamp;
    frame.bgra.resize(needed);
    // Respect the Media Foundation stride and its sign. This prevents vertically flipped
    // boards when a decoder exposes a bottom-up RGB32 DIB.
    for (int y = 0; y < height_; ++y) {
        const int sourceY = impl_->stride >= 0 ? y : (height_ - 1 - y);
        const BYTE* source = bytes + static_cast<std::size_t>(sourceY) * sourceStride;
        std::copy(source, source + rowBytes, frame.bgra.begin() + static_cast<std::size_t>(y) * rowBytes);
    }
    buffer->Unlock();
    return true;
}

void VideoReader::close() {
    if (impl_ && impl_->reader) impl_->reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    impl_.reset();
    width_ = height_ = 0;
    durationSeconds_ = frameRate_ = 0;
}

} // namespace tr
