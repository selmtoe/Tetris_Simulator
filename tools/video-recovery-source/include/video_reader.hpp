#pragma once

#include "common.hpp"

#ifndef __EMSCRIPTEN__
#include <windows.h>
#include <d3d11.h>
#endif

#include <memory>
#include <functional>

namespace tr {

class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    bool open(const std::filesystem::path& path, std::string& error,
              bool advancedVideoProcessing = false,
              bool forceLegacyProcessing = false,
              bool preferNvdec = false);
    // Restrict NVDEC's NV12-to-BGRA conversion to the exact board/queue
    // rectangles consumed downstream. Other backends ignore this hint.
    void setGpuConversionRegions(const std::vector<PixelRegion>& regions);
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
    void useBrowserMfConversion(bool enabled);
#endif
    bool seek(double seconds, std::string& error);
    bool read(Frame& frame, bool& endOfStream, std::string& error);
    // Decode the next sample but only transfer/lock its pixels when the
    // timestamp predicate accepts it. The timestamp and dimensions are
    // still returned for rejected samples. This is especially important for
    // D3D11 video: it avoids a synchronous full-frame GPU readback for the
    // many decoded frames that contain no requested board sample.
    bool readFiltered(Frame& frame, bool& endOfStream, std::string& error,
                      const std::function<bool(std::int64_t)>& needPixels);
    bool readFilteredRegion(Frame& frame, bool& endOfStream, std::string& error,
                            const PixelRegion& region,
                            const std::function<bool(std::int64_t)>& needPixels);
    // CPU RGB32 compatibility path: retain full-frame addressing but copy
    // only several disjoint source rectangles from one locked MF buffer.
    // Bytes outside the rectangles are unspecified and must not be sampled.
    bool readSparseRegions(Frame& frame, bool& endOfStream, std::string& error,
                           const std::vector<PixelRegion>& regions);
#ifndef __EMSCRIPTEN__
    // Deliver the next decoded D3D11 texture synchronously while the owning
    // Media Foundation sample is alive. The consumer must enqueue/copy all
    // texture work before returning and must preserve callback order.
    using D3D11SampleConsumer = std::function<bool(
        ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, UINT,
        std::int64_t, std::string&)>;
    bool readD3D11Sample(std::int64_t& timestamp100ns, bool& endOfStream,
                         std::string& error, const D3D11SampleConsumer& consumer);
    using CudaNv12SampleConsumer = std::function<bool(
        const void*, std::size_t, int, int, bool,
        std::int64_t, std::string&)>;
    bool readCudaNv12Sample(std::int64_t& timestamp100ns, bool& endOfStream,
                            std::string& error,
                            const CudaNv12SampleConsumer& consumer,
                            void* consumerStream = nullptr);
#endif
    void close();

    int width() const { return width_; }
    int height() const { return height_; }
    double durationSeconds() const { return durationSeconds_; }
    double frameRate() const { return frameRate_; }
    bool usesD3D11() const;
    bool usesNvdecCuda() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int width_ = 0;
    int height_ = 0;
    double durationSeconds_ = 0;
    double frameRate_ = 0;
};

} // namespace tr
