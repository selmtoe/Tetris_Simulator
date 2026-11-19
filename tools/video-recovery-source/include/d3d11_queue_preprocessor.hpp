#pragma once

#include "common.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT)

#include "queue_onnx_model.hpp"

#include <d3d11.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tr {

class D3D11BoardPreprocessor;
struct D3D11BoardFrameHandle;

// Post-legacy queue preprocessing performed entirely on the D3D11 device used
// by Media Foundation. Slot order is fixed and matches the existing CPU batch:
// p1 HOLD/NEXT1..5 followed by p2 HOLD/NEXT1..5.
struct D3D11QueueSlotStatistics {
    // Legacy colour sampling includes a source pixel when any channel is > 50.
    // These sums/count reproduce VisionAnalyzer::observeQueue without retaining
    // the source frame on the CPU.
    std::uint64_t sumR = 0;
    std::uint64_t sumG = 0;
    std::uint64_t sumB = 0;
    std::uint32_t colorCount = 0;

    // CNN dark-ratio inputs use the complete mapped crop rectangle.
    std::uint32_t darkCount = 0;
    std::uint32_t cropPixelCount = 0;
};

struct D3D11QueueFrameStatistics {
    std::array<D3D11QueueSlotStatistics, 12> slots{};
};

struct D3D11QueueBatchResult {
    // Flat order is frame-major, then the fixed 12-slot order documented above.
    std::vector<QueueCnnPrediction> predictions;
    std::vector<D3D11QueueFrameStatistics> frames;
    // Diagnostics for the optional ROI-statistics adaptive inference probe.
    // Counts are per delivered video frame, not per queue slot.
    bool adaptiveEnabled = false;
    std::size_t inferredFrames = 0;
    std::size_t reusedFrames = 0;
    std::size_t inferredSlots = 0;
    std::size_t reusedSlots = 0;
    std::size_t inferenceRuns = 0;
    double backgroundInferenceSeconds = 0;
};

class D3D11QueuePreprocessor {
public:
    static constexpr std::size_t SlotsPerFrame = 12;
    // The historical GPU profile flushed at eight frames.  Keep a larger
    // allocation available so separately named experiments can measure wider
    // ONNX batches without changing delivery order or tensor layout.
    static constexpr std::size_t MaximumFrames = 32;

    D3D11QueuePreprocessor();
    ~D3D11QueuePreprocessor();
    D3D11QueuePreprocessor(const D3D11QueuePreprocessor&) = delete;
    D3D11QueuePreprocessor& operator=(const D3D11QueuePreprocessor&) = delete;

    // device/context must be the exact pair owned by VideoReader. model is
    // non-owning and must outlive this object. All methods are single-threaded;
    // call append synchronously from ReadSample delivery order.
    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    int frameWidth,
                    int frameHeight,
                    const Settings& settings,
                    OnnxQueueModel* model,
                    std::string& error);

    // Queues one decoder texture/subresource into the next frame-major batch
    // position. No CPU texture or tensor readback occurs.
    bool append(ID3D11Texture2D* texture,
                UINT sourceSubresource,
                std::string& error);

    // Produces the queue tensor and retained board image from one decoder SRV
    // and one compute dispatch. This is available only to the separately
    // versioned next-generation profile and falls back at the recovery layer.
    bool appendFused(ID3D11Texture2D* texture,
                     UINT sourceSubresource,
                     D3D11BoardPreprocessor& board,
                     std::int64_t sourceTime100ns,
                     std::uint64_t deliveryOrder,
                     D3D11BoardFrameHandle& handle,
                     std::string& error);

    // Maps the accumulated FP32 UAV through CUDA-D3D11 interop and invokes the
    // planned OnnxQueueModel::inferCudaDevice(float*, batch, error). Only CNN
    // predictions and the small legacy/dark statistics buffer reach the CPU.
    // A successful flush resets the batch; a failed flush leaves it available
    // for diagnostics or an explicit reset.
    bool flush(D3D11QueueBatchResult& result, std::string& error);

    // The safe pipeline copies the active D3D tensor into independent CUDA
    // memory before a worker calls ORT. flush then returns the preceding batch;
    // finishPipeline returns the final submitted batch.
    bool pipelineEnabled() const;
    bool finishPipeline(D3D11QueueBatchResult& result, std::string& error);

    void reset();
    bool ready() const;
    std::size_t frameCount() const;
    bool full() const { return frameCount() >= MaximumFrames; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr

#endif // Windows ultra GPU build
