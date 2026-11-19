#pragma once

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include "common.hpp"
#include "d3d11_board_preprocessor.hpp"
#include "d3d11_queue_preprocessor.hpp"
#include "onnx_model.hpp"
#include "queue_onnx_model.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tr {

struct NvdecCudaBoardRequest {
    D3D11BoardFrameHandle handle;
    bool player1 = false;
};

// Direct next-generation front end:
//   NVDEC NV12 device frame -> CUDA board ring + CUDA queue tensor/statistics
//   -> ONNX Runtime CUDA I/O binding.
// No BGRA full/ROI texture and no CUDA-D3D graphics interop are involved.
class NvdecCudaPreprocessor {
public:
    static constexpr std::size_t MaximumFrames = 256;
    static constexpr std::size_t MaximumQueueFrames = 32;

    NvdecCudaPreprocessor();
    ~NvdecCudaPreprocessor();
    NvdecCudaPreprocessor(const NvdecCudaPreprocessor&) = delete;
    NvdecCudaPreprocessor& operator=(const NvdecCudaPreprocessor&) = delete;

    bool initialize(int frameWidth, int frameHeight,
                    const Settings& settings,
                    OnnxBoardModel* boardModel,
                    OnnxQueueModel* queueModel,
                    std::size_t memoryBudgetBytes,
                    std::string& error);

    // Processes the source synchronously so NvDecoder may safely recycle its
    // device frame after this function returns. Queue work is skipped on
    // frames that are not scan boundaries.
    bool processFrame(const void* nv12, std::size_t pitch, int sourceHeight,
                      int matrix, bool fullRange,
                      std::int64_t sourceTime100ns,
                      std::uint64_t deliveryOrder,
                      bool retainBoard,
                      bool appendQueue,
                      D3D11BoardFrameHandle& handle,
                      std::string& error);

    bool inferBoard(const D3D11BoardFrameHandle& handle, bool player1,
                    D3D11BoardInference& result, std::string& error);
    // Preserves request order exactly. Each returned item corresponds to the
    // handle/player pair at the same index, while all cells are submitted to
    // ONNX Runtime in one dynamic batch.
    bool inferBoards(const std::vector<NvdecCudaBoardRequest>& requests,
                     std::vector<D3D11BoardInference>& results,
                     std::string& error);
    bool flushQueue(D3D11QueueBatchResult& result, std::string& error);
    bool pipelineEnabled() const;
    bool finishQueuePipeline(D3D11QueueBatchResult& result,
                             std::string& error);

    bool valid(const D3D11BoardFrameHandle& handle) const;
    // One-pass range analysis support. Copy one source rectangle of the
    // retained raw NV12 board crop of `player1` to host memory as BGRA. The
    // bytes are produced by the same NV12-to-BGRA arithmetic VideoReader uses,
    // so the established CPU board analyzer can run on frames that were
    // decoded by the scene scan instead of a second range decode. Requires the
    // raw NV12 board ring.
    bool readBoardBgra(const D3D11BoardFrameHandle& handle, bool player1,
                       int x, int y, int width, int height,
                       std::vector<std::uint8_t>& destination,
                       std::string& error);
    // Same conversion applied directly to a decoded NV12 surface instead of a
    // ring slot. Used by the bounded repair so it reuses the scan's own decoder
    // and never creates a second D3D11 device.
    bool convertNv12ToBgra(const void* nv12, std::size_t pitch,
                           int sourceHeight, int matrix, bool fullRange,
                           int x, int y, int width, int height,
                           std::vector<std::uint8_t>& destination,
                           std::string& error);
    bool ready() const;
    // True when the ring retains the raw NV12 board crop, which is what makes
    // readBoardBgra() possible.
    bool rawBoardRingEnabled() const;
    std::size_t capacity() const;
    std::size_t bytesPerFrame() const;
    std::size_t boardBatchCapacity() const;
    std::size_t queueFrameCount() const;
    // Opaque CUDA stream used by every NV12 preprocessing command.  The
    // NVDEC backend uses it only to order mapped-surface VPP and lifetime.
    void* cudaStreamHandle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr

#endif
