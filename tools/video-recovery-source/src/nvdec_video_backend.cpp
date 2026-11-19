#include "nvdec_video_backend.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include <cuda.h>
#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "NvDecoder/NvDecoder.h"
#include "NvCodecUtils.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

simplelogger::Logger* logger =
    simplelogger::LoggerFactory::CreateConsoleLogger();

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
                                   cudaStream_t stream);

namespace tr {
namespace {

std::string hrText(HRESULT hr) {
    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

std::string ffText(int status) {
    char value[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, value, sizeof(value));
    return value;
}

bool ffCheck(int status, const char* operation, std::string& error) {
    if (status >= 0) return true;
    error = std::string(operation) + " failed: " + ffText(status);
    return false;
}

std::string cudaText(cudaError_t status) {
    const char* value = cudaGetErrorString(status);
    return value ? value : ("CUDA error " + std::to_string(status));
}

cudaVideoCodec nvCodec(AVCodecID codec) {
    if (codec == AV_CODEC_ID_H264) return cudaVideoCodec_H264;
    if (codec == AV_CODEC_ID_HEVC) return cudaVideoCodec_HEVC;
    if (codec == AV_CODEC_ID_AV1) return cudaVideoCodec_AV1;
    // YouTube serves VP9 for most non-AV1 streams; the GPU decodes it natively,
    // so it must never require a transcode.
    if (codec == AV_CODEC_ID_VP9) return cudaVideoCodec_VP9;
    if (codec == AV_CODEC_ID_VP8) return cudaVideoCodec_VP8;
    return cudaVideoCodec_NumCodecs;
}

class Demuxer {
public:
    ~Demuxer() { close(); }

    bool open(const std::filesystem::path& path, std::string& error) {
        close();
        const std::string utf8 = path.u8string();
        if (!ffCheck(avformat_open_input(&format_, utf8.c_str(), nullptr, nullptr),
                     "avformat_open_input", error) ||
            !ffCheck(avformat_find_stream_info(format_, nullptr),
                     "avformat_find_stream_info", error)) {
            close();
            return false;
        }
        videoStream_ = av_find_best_stream(
            format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (!ffCheck(videoStream_, "av_find_best_stream(video)", error)) {
            close();
            return false;
        }
        stream_ = format_->streams[videoStream_];
        // Frame times are reported relative to the first presented frame, never
        // to the container's absolute PTS. A fragmented MP4 remux drops the
        // source edit list, so the identical bitstream starts one frame later
        // than in an indexed MP4 (0.033 s at 30 fps). Without this
        // normalisation every derived frame index shifts by one, the board
        // sampler reads the neighbouring frame, and the repair ladder fires on
        // matches that are actually clean.
        //
        // The origin is kept in the stream's own time base, not in 100 ns
        // units. AVStream::start_time is documented in stream time base, so
        // converting it to 100 ns and subtracting that from an already
        // converted packet timestamp rescales twice and leaves a residue. At
        // 1/15360 a one-frame origin of 512 ticks converted as if it were
        // AV_TIME_BASE became 5120 (0.512 ms) instead of 333333 (33.333 ms):
        // every fragmented frame kept a 32.8 ms offset, which is 0.9846 of a
        // frame, so board requests that sit just after a frame boundary
        // resolved to the previous frame. Subtracting in ticks and converting
        // once makes the identical bitstream report identical integer
        // timestamps from both containers.
        timeOriginTicks_ = 0;
        firstPacketSeen_ = false;
        // Diagnostic escape hatch for A/B runs; production always normalises.
        const char* normalizeEnv = std::getenv("TR_TIME_ORIGIN_NORMALIZE");
        normalizeTimeOrigin_ =
            !(normalizeEnv && std::string(normalizeEnv) == "0");
        if (normalizeTimeOrigin_ &&
            stream_->start_time != AV_NOPTS_VALUE && stream_->start_time > 0) {
            timeOriginTicks_ = stream_->start_time;
        }
        packet_ = av_packet_alloc();
        filtered_ = av_packet_alloc();
        if (!packet_ || !filtered_) {
            error = "FFmpeg packet allocation failed";
            close();
            return false;
        }
        const char* filterName = nullptr;
        if (codec() == AV_CODEC_ID_H264) filterName = "h264_mp4toannexb";
        if (codec() == AV_CODEC_ID_HEVC) filterName = "hevc_mp4toannexb";
        if (filterName) {
            const AVBitStreamFilter* bitstreamFilter = av_bsf_get_by_name(filterName);
            if (!bitstreamFilter ||
                !ffCheck(av_bsf_alloc(bitstreamFilter, &filter_),
                         "av_bsf_alloc", error) ||
                !ffCheck(avcodec_parameters_copy(filter_->par_in, stream_->codecpar),
                         "avcodec_parameters_copy", error)) {
                close();
                return false;
            }
            filter_->time_base_in = stream_->time_base;
            if (!ffCheck(av_bsf_init(filter_), "av_bsf_init", error)) {
                close();
                return false;
            }
        }
        return true;
    }

    void close() {
        av_packet_free(&filtered_);
        av_packet_free(&packet_);
        av_bsf_free(&filter_);
        avformat_close_input(&format_);
        stream_ = nullptr;
        videoStream_ = -1;
    }

    bool seek(double seconds, std::string& error) {
        if (!format_ || !stream_) {
            error = "NVDEC demuxer is not open";
            return false;
        }
        // Callers express seek targets on the normalised timeline, so add the
        // container origin back. The origin is already in the stream's time
        // base, so a zero origin reproduces the previous arithmetic exactly: the
        // seek rounding must not move a single tick, because AVSEEK_FLAG_BACKWARD
        // could then land on a different keyframe and change which frame the
        // board sampler reads.
        const std::int64_t originTicks = normalizeTimeOrigin_ ? timeOriginTicks_ : 0;
        const std::int64_t target = originTicks + av_rescale_q(
            static_cast<std::int64_t>(std::max(0.0, seconds) * AV_TIME_BASE),
            AVRational{1, AV_TIME_BASE}, stream_->time_base);
        // v78: land on an IDR. MP4 marks every I-frame with a recovery point as a
        // keyframe, but NVDEC cannot start on one (no IDR) and silently drops
        // frames until the next IDR, 2-5 s later. YouTube streams mix both kinds
        // (10ell5 226.8 s is a non-IDR keyframe, 231.2 s the next IDR), so a match
        // whose start fell after such a keyframe lost its first seconds: the
        // queue-move search saw one frame (found=0) and the first 5-6 pieces were
        // never tracked (10ell5 1/94, XoaWL 25, XTveY 132). When the landing
        // keyframe is not an IDR, step back to the keyframe before it. A landing
        // that already is an IDR re-seeks with the same target, so ordinary files
        // decode exactly as before. TR_SEEK_IDR=0 restores the old behaviour.
        std::int64_t seekTarget = target;
        const char* idrFlag = std::getenv("TR_SEEK_IDR");
        const bool requireIdr = codec() == AV_CODEC_ID_H264 &&
            !(idrFlag && std::string(idrFlag) == "0");
        for (int attempt = 0; requireIdr && attempt < 8; ++attempt) {
            if (!seekRaw(seekTarget, error)) return false;
            std::int64_t landed = AV_NOPTS_VALUE;
            bool idr = true;
            if (!probeLanding(landed, idr, error)) return false;
            if (std::getenv("TR_SEEK_DEBUG")) {
                std::cerr << "seek_debug target=" << target << " try=" << seekTarget
                          << " landed=" << landed << " idr=" << (idr ? 1 : 0)
                          << " tb=" << stream_->time_base.num << '/' << stream_->time_base.den
                          << '\n' << std::flush;
            }
            if (idr || landed == AV_NOPTS_VALUE || landed <= 0) break;
            // A target just before the non-IDR keyframe lands on it again (the
            // fragmented-MP4 index snaps to the fragment that holds a keyframe),
            // so step back 1, 2, 4 ... s from that keyframe until an IDR is hit.
            const std::int64_t step = av_rescale_q(
                static_cast<std::int64_t>(AV_TIME_BASE) << attempt,
                AVRational{1, AV_TIME_BASE}, stream_->time_base);
            const std::int64_t earlier = std::min(seekTarget, landed) - step;
            if (earlier <= 0) {
                seekTarget = 0;
                break;
            }
            seekTarget = earlier;
        }
        return seekRaw(seekTarget, error);
    }

    bool seekRaw(std::int64_t ticks, std::string& error) {
        if (!ffCheck(av_seek_frame(format_, videoStream_, ticks, AVSEEK_FLAG_BACKWARD),
                     "av_seek_frame", error)) return false;
        avformat_flush(format_);
        if (filter_) av_bsf_flush(filter_);
        av_packet_unref(packet_);
        av_packet_unref(filtered_);
        return true;
    }

    // Reads the first video packet after a seek (raw, before the Annex B filter)
    // and reports its timestamp and whether it carries an H.264 IDR slice.
    bool probeLanding(std::int64_t& timestamp, bool& idr, std::string& error) {
        timestamp = AV_NOPTS_VALUE;
        idr = true;
        for (int packets = 0; packets < 4096; ++packets) {
            av_packet_unref(packet_);
            const int status = av_read_frame(format_, packet_);
            if (status == AVERROR_EOF) return true;
            if (!ffCheck(status, "av_read_frame", error)) return false;
            if (packet_->stream_index != videoStream_) continue;
            timestamp = packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts;
            idr = containsIdr(packet_->data, packet_->size);
            av_packet_unref(packet_);
            return true;
        }
        return true;
    }

    bool containsIdr(const std::uint8_t* data, int size) const {
        if (!data || size <= 0) return true;
        const AVCodecParameters* par = stream_->codecpar;
        const bool avcc = par->extradata && par->extradata_size >= 7 && par->extradata[0] == 1;
        if (avcc) {
            const int lengthSize = (par->extradata[4] & 3) + 1;
            int pos = 0;
            while (pos + lengthSize <= size) {
                std::uint32_t length = 0;
                for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[pos + k];
                pos += lengthSize;
                if (length == 0 || pos + static_cast<int>(length) > size) break;
                if ((data[pos] & 0x1f) == 5) return true;
                pos += static_cast<int>(length);
            }
            return false;
        }
        // Annex B: scan start codes.
        bool sawSlice = false;
        for (int i = 0; i + 3 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                const int type = data[i + 3] & 0x1f;
                if (type == 5) return true;
                if (type == 1) sawSlice = true;
                i += 2;
            }
        }
        return !sawSlice;
    }

    bool next(const std::uint8_t*& data, int& size,
              std::int64_t& timestamp100ns, bool& eof,
              std::string& error) {
        eof = false;
        av_packet_unref(filtered_);
        for (;;) {
            av_packet_unref(packet_);
            const int status = av_read_frame(format_, packet_);
            if (status == AVERROR_EOF) {
                eof = true;
                data = nullptr;
                size = 0;
                timestamp100ns = 0;
                return true;
            }
            if (!ffCheck(status, "av_read_frame", error)) return false;
            if (packet_->stream_index != videoStream_) continue;
            AVPacket* output = packet_;
            if (filter_) {
                if (!ffCheck(av_bsf_send_packet(filter_, packet_),
                             "av_bsf_send_packet", error)) return false;
                const int receive = av_bsf_receive_packet(filter_, filtered_);
                if (receive == AVERROR(EAGAIN)) continue;
                if (!ffCheck(receive, "av_bsf_receive_packet", error)) return false;
                output = filtered_;
            }
            const std::int64_t timestamp = output->pts != AV_NOPTS_VALUE
                ? output->pts : output->dts;
            if (!firstPacketSeen_) {
                firstPacketSeen_ = true;
                // Container metadata can over-state the origin; the first packet
                // that actually comes out of the demuxer wins when it is earlier.
                if (normalizeTimeOrigin_ && timestamp != AV_NOPTS_VALUE &&
                    timestamp < timeOriginTicks_) {
                    timeOriginTicks_ = timestamp;
                }
            }
            if (timestamp == AV_NOPTS_VALUE) {
                timestamp100ns = 0;
            } else {
                // Subtract in the stream's time base and convert exactly once.
                // Converting first and subtracting a separately converted origin
                // does not round-trip: (a - b) * s != a * s - b * s after rounding,
                // so two containers carrying the same frames could disagree by a
                // tick or two even when the origin itself was correct.
                const std::int64_t originTicks =
                    normalizeTimeOrigin_ ? timeOriginTicks_ : 0;
                const std::int64_t relativeTicks = timestamp - originTicks;
                timestamp100ns = av_rescale_q(
                    relativeTicks > 0 ? relativeTicks : 0,
                    stream_->time_base, AVRational{1, 10000000});
            }
            data = output->data;
            size = output->size;
            return true;
        }
    }

    AVCodecID codec() const {
        return stream_ ? stream_->codecpar->codec_id : AV_CODEC_ID_NONE;
    }
    const std::uint8_t* sequenceHeader() const {
        return stream_ ? stream_->codecpar->extradata : nullptr;
    }
    int sequenceHeaderSize() const {
        return stream_ ? stream_->codecpar->extradata_size : 0;
    }
    int width() const { return stream_ ? stream_->codecpar->width : 0; }
    int height() const { return stream_ ? stream_->codecpar->height : 0; }
    double durationSeconds() const {
        return format_ && format_->duration != AV_NOPTS_VALUE
            ? static_cast<double>(format_->duration) / AV_TIME_BASE : 0.0;
    }
    double frameRate() const {
        if (!stream_) return 0;
        const AVRational rate = av_guess_frame_rate(format_, stream_, nullptr);
        return rate.den ? static_cast<double>(rate.num) / rate.den : 0.0;
    }

private:
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVPacket* filtered_ = nullptr;
    AVBSFContext* filter_ = nullptr;
    int videoStream_ = -1;
    // Absolute PTS of the first presented frame, in the stream's own time base.
    // Every reported frame time is this value subtracted, so an indexed MP4 and
    // a fragmented remux of the same bitstream produce identical frame indices.
    std::int64_t timeOriginTicks_ = 0;
    bool firstPacketSeen_ = false;
    bool normalizeTimeOrigin_ = true;
};

bool createDevice(ComPtr<IDXGIAdapter1>& adapter,
                  ComPtr<ID3D11Device>& device,
                  ComPtr<ID3D11DeviceContext>& context,
                  std::string& error) {
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = "NVDEC CreateDXGIFactory1 failed: " + hrText(hr);
        return false;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        int cudaDevice = -1;
        if (cudaD3D11GetDevice(&cudaDevice, candidate.Get()) == cudaSuccess) {
            adapter = std::move(candidate);
            break;
        }
    }
    if (!adapter) {
        error = "No CUDA-compatible high-performance D3D11 adapter was found";
        return false;
    }
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           flags, levels, static_cast<UINT>(std::size(levels)),
                           D3D11_SDK_VERSION, &device, &actual, &context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               flags, levels + 1, 1, D3D11_SDK_VERSION,
                               &device, &actual, &context);
    }
    if (FAILED(hr)) {
        error = "NVDEC D3D11CreateDevice failed: " + hrText(hr);
        return false;
    }
    return true;
}

} // namespace

struct NvdecVideoBackend::Impl {
    ~Impl() { close(); }

    cudaStream_t lockedConsumerStream = nullptr;
    bool lockedStreamsMixed = false;

    void retainProcessedFrame(std::uint8_t* frame, void* consumerStream) {
        const auto stream = reinterpret_cast<cudaStream_t>(consumerStream);
        if (processedLockedFrames.empty()) {
            lockedConsumerStream = stream;
            lockedStreamsMixed = false;
        } else if (stream != lockedConsumerStream) {
            lockedStreamsMixed = true;
        }
        processedLockedFrames.push_back(frame);
    }

    bool reclaimLockedFrames(bool force, std::string& error, bool streamAlive = false) {
        if (processedLockedFrames.empty() ||
            (!force && processedLockedFrames.size() < asyncLockBatch)) {
            return true;
        }
        // Each reader owns its decode buffers. Waiting for another match's ORT
        // work is unnecessary when every consumer used the advertised stream.
        // Close/seek retain the device fence: the consumer may already have
        // destroyed its stream. Unknown/mixed streams retain the old fence too.
        const char* configured = std::getenv("TR_NVDEC_STREAM_RECLAIM");
        const bool local = streamAlive && lockedConsumerStream && !lockedStreamsMixed &&
            (!configured || std::string(configured) != "0");
        const cudaError_t status = local
            ? cudaStreamSynchronize(lockedConsumerStream) : cudaDeviceSynchronize();
        if (status != cudaSuccess) {
            error = "CUDA synchronize(NVDEC locked frames) failed: " +
                cudaText(status);
            return false;
        }
        decoder->UnlockFrame(processedLockedFrames.data(),
                             static_cast<int>(processedLockedFrames.size()));
        processedLockedFrames.clear();
        return true;
    }

    bool createDecoder(std::string& error) {
        try {
            const std::uint8_t* sequenceHeader = demux.sequenceHeader();
            int sequenceHeaderSize = demux.sequenceHeaderSize();
            // ISO BMFF stores AV1 config OBUs after a four-byte av1C record.
            // NVDEC expects only the config OBUs in pExtVideoInfo.
            if (demux.codec() == AV_CODEC_ID_AV1 && sequenceHeader &&
                sequenceHeaderSize >= 4 && (sequenceHeader[0] & 0x80)) {
                sequenceHeader += 4;
                sequenceHeaderSize -= 4;
            }
            decoder = std::make_unique<NvDecoder>(
                cudaContext, demux.width(), demux.height(), true,
                nvCodec(demux.codec()), nullptr, false, true,
                nullptr, nullptr, 0, 0, sequenceHeader, sequenceHeaderSize);
            return true;
        } catch (const std::exception& exception) {
            error = "NVDEC decoder initialization failed: " +
                std::string(exception.what());
            return false;
        }
    }

    void close() {
        std::string ignored;
        if (decoder) {
            reclaimLockedFrames(true, ignored);
            if (pendingLocked && pendingIndex < pendingFrames.size()) {
                decoder->UnlockFrame(
                    pendingFrames.data() + pendingIndex,
                    static_cast<int>(pendingFrames.size() - pendingIndex));
            }
        }
        if (graphicsResource) {
            cudaGraphicsUnregisterResource(graphicsResource);
            graphicsResource = nullptr;
        }
        decoder.reset();
        outputTexture.Reset();
        d3dContext.Reset();
        d3dDevice.Reset();
        adapter.Reset();
        demux.close();
        pendingFrames.clear();
        pendingTimestamps.clear();
        processedLockedFrames.clear();
        pendingIndex = 0;
        pendingLocked = false;
        eofSubmitted = false;
        mappedEofDrained = false;
        if (primaryRetained) {
            cuDevicePrimaryCtxRelease(cudaDevice);
            primaryRetained = false;
        }
        cudaContext = nullptr;
    }

    bool fillPending(bool lockFrames, std::string& error) {
        pendingFrames.clear();
        pendingTimestamps.clear();
        pendingIndex = 0;
        pendingLocked = lockFrames;
        for (;;) {
            const std::uint8_t* bytes = nullptr;
            int byteCount = 0;
            std::int64_t packetTimestamp = 0;
            bool eof = false;
            if (!eofSubmitted &&
                !demux.next(bytes, byteCount, packetTimestamp, eof, error)) {
                return false;
            }
            if (eof) eofSubmitted = true;
            std::uint8_t** frames = nullptr;
            std::int64_t* timestamps = nullptr;
            int returned = 0;
            const std::uint32_t packetFlags = 0;
            try {
                if (lockFrames) {
                    decoder->DecodeLockFrame(eofSubmitted ? nullptr : bytes,
                                             eofSubmitted ? 0 : byteCount,
                                             &frames, &returned, packetFlags, &timestamps,
                                             packetTimestamp);
                } else {
                    decoder->Decode(eofSubmitted ? nullptr : bytes,
                                    eofSubmitted ? 0 : byteCount,
                                    &frames, &returned, packetFlags, &timestamps,
                                    packetTimestamp);
                }
            } catch (const std::exception& exception) {
                error = "NVDEC decode failed: " + std::string(exception.what());
                if (bytes && byteCount > 0) {
                    std::ostringstream packetInfo;
                    packetInfo << " [packet_bytes=" << byteCount << " prefix=";
                    packetInfo << std::hex;
                    const int prefixSize = std::min(byteCount, 16);
                    for (int index = 0; index < prefixSize; ++index) {
                        if (index) packetInfo << '-';
                        packetInfo.width(2);
                        packetInfo.fill('0');
                        packetInfo << static_cast<unsigned int>(bytes[index]);
                    }
                    packetInfo << ']';
                    error += packetInfo.str();
                }
                return false;
            }
            for (int index = 0; index < returned; ++index) {
                pendingFrames.push_back(frames[index]);
                pendingTimestamps.push_back(timestamps ? timestamps[index] : 0);
            }
            if (!pendingFrames.empty() || eofSubmitted) return true;
        }
    }

    struct MappedDelivery {
        Impl* owner = nullptr;
        const NvdecVideoBackend::CudaConsumer* consumer = nullptr;
        std::string* error = nullptr;
        CUstream processingStream = nullptr;
        std::int64_t lastTimestamp100ns = 0;
        int delivered = 0;
        bool consumerSucceeded = true;
    };

    static bool consumeMappedFrame(
        void* opaque, CUdeviceptr source, unsigned int pitch,
        int width, int height, int surfaceHeight,
        int matrix, bool fullRange, std::int64_t timestamp100ns,
        CUstream /*decodeStream*/) {
        auto& delivery = *static_cast<MappedDelivery*>(opaque);
        delivery.lastTimestamp100ns = timestamp100ns;
        ++delivery.delivered;
        if (!delivery.owner || delivery.owner->decoder->GetBitDepth() != 8) {
            *delivery.error =
                "NVDEC mapped CUDA preprocessing requires 8-bit NV12 input";
            delivery.consumerSucceeded = false;
        } else if (width != delivery.owner->width ||
                   height > delivery.owner->height ||
                   surfaceHeight < height) {
            *delivery.error =
                "NVDEC mapped frame geometry changed during analysis";
            delivery.consumerSucceeded = false;
        } else if (delivery.consumer && *delivery.consumer) {
            delivery.consumerSucceeded = (*delivery.consumer)(
                reinterpret_cast<const void*>(source),
                static_cast<std::size_t>(pitch), surfaceHeight,
                matrix, fullRange, timestamp100ns, *delivery.error);
        }

        // NVDEC VPP and the direct preprocessor use this same stream.  Waiting
        // only for it keeps unrelated ORT/queue work concurrent while still
        // guaranteeing that the mapped decode surface is no longer read.
        const CUresult synchronized = delivery.processingStream
            ? cuStreamSynchronize(delivery.processingStream)
            : cuCtxSynchronize();
        if (synchronized != CUDA_SUCCESS) {
            *delivery.error = delivery.processingStream
                ? "cuStreamSynchronize(mapped NVDEC surface) failed"
                : "cuCtxSynchronize(mapped NVDEC surface) failed";
            delivery.consumerSucceeded = false;
            return false;
        }
        return delivery.consumerSucceeded;
    }

    bool fillMapped(const NvdecVideoBackend::CudaConsumer& consumer,
                    CUstream processingStream,
                    std::int64_t& timestamp100ns,
                    bool& endOfStream,
                    std::string& error) {
        timestamp100ns = 0;
        endOfStream = false;
        if (mappedEofDrained) {
            endOfStream = true;
            return true;
        }
        for (;;) {
            const std::uint8_t* bytes = nullptr;
            int byteCount = 0;
            std::int64_t packetTimestamp = 0;
            bool eof = false;
            if (!eofSubmitted &&
                !demux.next(bytes, byteCount, packetTimestamp, eof, error)) {
                return false;
            }
            if (eof) eofSubmitted = true;
            MappedDelivery delivery{
                this, &consumer, &error, processingStream
            };
            int returned = 0;
            try {
                decoder->DecodeMapped(
                    eofSubmitted ? nullptr : bytes,
                    eofSubmitted ? 0 : byteCount,
                    &Impl::consumeMappedFrame, &delivery, &returned,
                    0, packetTimestamp, processingStream);
            } catch (const std::exception& exception) {
                if (error.empty()) {
                    error = "NVDEC mapped decode failed: " +
                        std::string(exception.what());
                }
                return false;
            }
            if (eofSubmitted) mappedEofDrained = true;
            if (!delivery.consumerSucceeded) return false;
            if (delivery.delivered != returned) {
                error = "NVDEC mapped callback count was inconsistent";
                return false;
            }
            if (delivery.delivered > 0) {
                timestamp100ns = delivery.lastTimestamp100ns;
                return true;
            }
            if (eofSubmitted) {
                endOfStream = true;
                return true;
            }
        }
    }

    bool convert(std::uint8_t* source, std::string& error) {
        cudaError_t status = cudaGraphicsMapResources(1, &graphicsResource, nullptr);
        if (status != cudaSuccess) {
            error = "cudaGraphicsMapResources(NVDEC output) failed: " + cudaText(status);
            return false;
        }
        cudaArray_t destination = nullptr;
        status = cudaGraphicsSubResourceGetMappedArray(
            &destination, graphicsResource, 0, 0);
        if (status == cudaSuccess) {
            const CUVIDEOFORMAT format = decoder->GetVideoFormatInfo();
            status = static_cast<cudaError_t>(trNv12ToBgraRegions(
                source, static_cast<std::size_t>(decoder->GetDeviceFramePitch()),
                decoder->GetWidth(), decoder->GetHeight(), textureHeight,
                format.video_signal_description.matrix_coefficients,
                format.video_signal_description.video_full_range_flag,
                packedConversionRegions.empty() ? nullptr :
                    packedConversionRegions.data(),
                static_cast<int>(conversionRegions.size()),
                destination, nullptr));
        }
        const cudaError_t unmap = cudaGraphicsUnmapResources(
            1, &graphicsResource, nullptr);
        if (status == cudaSuccess) status = unmap;
        if (status != cudaSuccess) {
            error = "NVDEC NV12-to-BGRA conversion failed: " + cudaText(status);
            return false;
        }
        return true;
    }

    bool nextFrame(std::uint8_t*& frame,
                   std::int64_t& timestamp100ns,
                   bool& endOfStream,
                   bool lockFrames,
                   std::string& error) {
        frame = nullptr;
        timestamp100ns = 0;
        endOfStream = false;
        if (pendingIndex >= pendingFrames.size() &&
            !fillPending(lockFrames, error)) {
            return false;
        }
        if (pendingIndex >= pendingFrames.size()) {
            endOfStream = true;
            return true;
        }
        frame = pendingFrames[pendingIndex];
        timestamp100ns = pendingTimestamps[pendingIndex];
        ++pendingIndex;
        if (decoder->GetBitDepth() != 8) {
            error = "NVDEC CUDA preprocessing currently requires 8-bit NV12 input";
            return false;
        }
        return true;
    }

    Demuxer demux;
    std::unique_ptr<NvDecoder> decoder;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<ID3D11Texture2D> outputTexture;
    cudaGraphicsResource* graphicsResource = nullptr;
    CUdevice cudaDevice = 0;
    CUcontext cudaContext = nullptr;
    bool primaryRetained = false;
    bool eofSubmitted = false;
    std::vector<std::uint8_t*> pendingFrames;
    std::vector<std::int64_t> pendingTimestamps;
    std::vector<std::uint8_t*> processedLockedFrames;
    std::vector<PixelRegion> conversionRegions;
    std::vector<int> packedConversionRegions;
    std::size_t pendingIndex = 0;
    std::size_t asyncLockBatch = 8;
    bool pendingLocked = false;
    bool mappedZeroCopy = false;
    bool mappedEofDrained = false;
    int width = 0;
    int height = 0;
    int textureHeight = 0;
    double duration = 0;
    double rate = 0;
};

NvdecVideoBackend::NvdecVideoBackend() = default;
NvdecVideoBackend::~NvdecVideoBackend() { close(); }

bool NvdecVideoBackend::open(const std::filesystem::path& path,
                             std::string& error) {
    close();
    auto candidate = std::make_unique<Impl>();
    if (!candidate->demux.open(path, error)) return false;
    const AVCodecID sourceCodec = candidate->demux.codec();
    if (sourceCodec != AV_CODEC_ID_H264 &&
        sourceCodec != AV_CODEC_ID_HEVC &&
        sourceCodec != AV_CODEC_ID_AV1 &&
        sourceCodec != AV_CODEC_ID_VP9 &&
        sourceCodec != AV_CODEC_ID_VP8) {
        // Name the codec so the failure is actionable instead of a guess.
        const AVCodecDescriptor* descriptor = avcodec_descriptor_get(sourceCodec);
        error = "NVDEC backend supports only H.264/HEVC/AV1/VP9/VP8 video";
        if (descriptor && descriptor->name) {
            error += " (this file is ";
            error += descriptor->name;
            error += ")";
        }
        return false;
    }
    if (!createDevice(candidate->adapter, candidate->d3dDevice,
                      candidate->d3dContext, error)) return false;
    int runtimeDevice = -1;
    cudaError_t runtimeStatus = cudaD3D11GetDevice(
        &runtimeDevice, candidate->adapter.Get());
    if (runtimeStatus != cudaSuccess ||
        (runtimeStatus = cudaSetDevice(runtimeDevice)) != cudaSuccess) {
        error = "CUDA could not select the NVDEC D3D11 adapter: " +
            cudaText(runtimeStatus);
        return false;
    }
    candidate->cudaDevice = runtimeDevice;
    CUresult driverStatus = cuDevicePrimaryCtxRetain(
        &candidate->cudaContext, candidate->cudaDevice);
    if (driverStatus != CUDA_SUCCESS ||
        cuCtxSetCurrent(candidate->cudaContext) != CUDA_SUCCESS) {
        error = "CUDA primary context initialization failed for NVDEC";
        return false;
    }
    candidate->primaryRetained = true;
    if (!candidate->createDecoder(error)) return false;

    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = static_cast<UINT>(candidate->demux.width());
    const int outputHeight = (candidate->demux.height() + 15) & ~15;
    texture.Height = static_cast<UINT>(outputHeight);
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = candidate->d3dDevice->CreateTexture2D(
        &texture, nullptr, &candidate->outputTexture);
    if (FAILED(hr)) {
        error = "CreateTexture2D(NVDEC BGRA output) failed: " + hrText(hr);
        return false;
    }
    runtimeStatus = cudaGraphicsD3D11RegisterResource(
        &candidate->graphicsResource, candidate->outputTexture.Get(),
        cudaGraphicsRegisterFlagsSurfaceLoadStore);
    if (runtimeStatus != cudaSuccess) {
        error = "cudaGraphicsD3D11RegisterResource(NVDEC output) failed: " +
            cudaText(runtimeStatus);
        return false;
    }
    candidate->width = candidate->demux.width();
    candidate->height = candidate->demux.height();
    candidate->textureHeight = outputHeight;
    candidate->duration = candidate->demux.durationSeconds();
    candidate->rate = candidate->demux.frameRate();
    if (const char* configured = std::getenv("TR_NVDEC_ASYNC_BATCH")) {
        candidate->asyncLockBatch = static_cast<std::size_t>(
            std::clamp(std::atoi(configured), 1, 64));
    }
    if (const char* configured = std::getenv("TR_NVDEC_MAPPED_ZERO_COPY")) {
        candidate->mappedZeroCopy = std::string(configured) != "0";
    }
    if (std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "nvdec_async_lock_batch="
                  << candidate->asyncLockBatch
                  << " nvdec_mapped_zero_copy="
                  << (candidate->mappedZeroCopy ? 1 : 0) << '\n';
    }
    impl_ = std::move(candidate);
    return true;
}

bool NvdecVideoBackend::seek(double seconds, std::string& error) {
    if (!impl_) {
        error = "NVDEC backend is not open";
        return false;
    }
    if (!impl_->demux.seek(seconds, error)) return false;
    if (!impl_->reclaimLockedFrames(true, error)) return false;
    if (impl_->pendingLocked && impl_->pendingIndex < impl_->pendingFrames.size()) {
        impl_->decoder->UnlockFrame(
            impl_->pendingFrames.data() + impl_->pendingIndex,
            static_cast<int>(impl_->pendingFrames.size() - impl_->pendingIndex));
    }
    impl_->decoder.reset();
    impl_->pendingFrames.clear();
    impl_->pendingTimestamps.clear();
    impl_->pendingIndex = 0;
    impl_->pendingLocked = false;
    impl_->eofSubmitted = false;
    impl_->mappedEofDrained = false;
    return impl_->createDecoder(error);
}

void NvdecVideoBackend::setConversionRegions(
    const std::vector<PixelRegion>& regions) {
    if (!impl_) return;
    impl_->conversionRegions.clear();
    impl_->packedConversionRegions.clear();
    std::uint64_t pixels = 0;
    for (const PixelRegion& source : regions) {
        const int x0 = std::clamp(source.x, 0, impl_->width);
        const int y0 = std::clamp(source.y, 0, impl_->height);
        const int x1 = std::clamp(source.x + source.width, 0, impl_->width);
        const int y1 = std::clamp(source.y + source.height, 0, impl_->height);
        if (x1 <= x0 || y1 <= y0) continue;
        const PixelRegion region{x0, y0, x1 - x0, y1 - y0};
        impl_->conversionRegions.push_back(region);
        impl_->packedConversionRegions.insert(
            impl_->packedConversionRegions.end(),
            {region.x, region.y, region.width, region.height});
        pixels += static_cast<std::uint64_t>(x1 - x0) * (y1 - y0);
    }
    if (std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "nvdec_cuda_regions=" << impl_->conversionRegions.size()
                  << " pixels=" << pixels << '\n';
    }
}

bool NvdecVideoBackend::readD3D11Sample(
    std::int64_t& timestamp100ns,
    bool& endOfStream,
    std::string& error,
    const Consumer& consumer) {
    timestamp100ns = 0;
    endOfStream = false;
    if (!impl_) {
        error = "NVDEC backend is not open";
        return false;
    }
    std::uint8_t* frame = nullptr;
    if (!impl_->nextFrame(frame, timestamp100ns, endOfStream, false, error)) return false;
    if (endOfStream) return true;
    if (!impl_->convert(frame, error)) return false;
    if (!consumer) return true;
    return consumer(impl_->d3dDevice.Get(), impl_->d3dContext.Get(),
                    impl_->outputTexture.Get(), 0, timestamp100ns, error);
}

bool NvdecVideoBackend::readCudaSample(
    std::int64_t& timestamp100ns,
    bool& endOfStream,
    std::string& error,
    const CudaConsumer& consumer,
    void* consumerStream) {
    timestamp100ns = 0;
    endOfStream = false;
    if (!impl_) {
        error = "NVDEC backend is not open";
        return false;
    }
    if (impl_->mappedZeroCopy) {
        return impl_->fillMapped(
            consumer, reinterpret_cast<CUstream>(consumerStream),
            timestamp100ns, endOfStream, error);
    }
    bool asyncFrames = false;
    if (const char* configured = std::getenv("TR_NVDEC_ASYNC_FRAMES")) {
        asyncFrames = std::string(configured) != "0";
    }
    std::uint8_t* frame = nullptr;
    if (!impl_->nextFrame(
            frame, timestamp100ns, endOfStream, asyncFrames, error)) return false;
    if (endOfStream) return impl_->reclaimLockedFrames(true, error, true);
    if (!consumer) {
        if (asyncFrames) {
            impl_->retainProcessedFrame(frame, consumerStream);
            return impl_->reclaimLockedFrames(false, error, true);
        }
        return true;
    }
    const CUVIDEOFORMAT format = impl_->decoder->GetVideoFormatInfo();
    const bool consumed = consumer(
        frame, static_cast<std::size_t>(impl_->decoder->GetDeviceFramePitch()),
        impl_->decoder->GetHeight(),
        format.video_signal_description.matrix_coefficients,
        format.video_signal_description.video_full_range_flag != 0,
        timestamp100ns, error);
    if (asyncFrames) {
        impl_->retainProcessedFrame(frame, consumerStream);
        if (!impl_->reclaimLockedFrames(!consumed, error, true)) return false;
    }
    return consumed;
}

void NvdecVideoBackend::close() { impl_.reset(); }
int NvdecVideoBackend::width() const { return impl_ ? impl_->width : 0; }
int NvdecVideoBackend::height() const { return impl_ ? impl_->height : 0; }
double NvdecVideoBackend::durationSeconds() const { return impl_ ? impl_->duration : 0; }
double NvdecVideoBackend::frameRate() const { return impl_ ? impl_->rate : 0; }

} // namespace tr

#endif
