#pragma once

#include "common.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT)

#include "onnx_model.hpp"

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace tr {

// Opaque, generation-checked reference into the bounded GPU board ring. The
// caller retains these in Source Reader delivery order and performs the same
// first-delivered-frame-at/after-request selection as the CPU implementation.
struct D3D11BoardFrameHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    std::int64_t sourceTime100ns = 0;
    std::uint64_t deliveryOrder = 0;

    explicit operator bool() const { return generation != 0; }
};

struct D3D11BoardInference {
    BoardProbabilities probabilities{};
    VisibleBoard classic{};
    std::int64_t sourceTime100ns = 0;
    std::uint64_t deliveryOrder = 0;
    bool player1 = true;
};

// Internal hand-off used by the next-generation fused frame shader.  The
// resources stay owned by D3D11BoardPreprocessor; callers may only bind them
// for the single immediate-context dispatch between prepare/finish.
struct D3D11BoardFusedTarget {
    ID3D11ShaderResourceView* sampleSrv = nullptr;
    ID3D11UnorderedAccessView* boardUav = nullptr;
    std::uint32_t sampleCount = 0;
    std::uint32_t slot = 0;
    std::int64_t sourceTime100ns = 0;
    std::uint64_t deliveryOrder = 0;
};

// Correctness-first board stream for the post-20-second CNN path:
//   MF D3D11 RGB32 texture -> quantized P1/P2 BoardImage ring (D3D11)
//   -> requested 200x3x32x32 tensor (D3D11)
//   -> CUDA I/O binding (OnnxBoardModel)
// Only 200x9 probabilities plus 200 classic colour sums are returned to CPU.
// All methods are synchronous with respect to the immediate context and must
// be called from one thread.
class D3D11BoardPreprocessor {
public:
    // A 1.mov slot is about 2.02 MiB. This cap avoids a single unbounded GPU
    // allocation while retaining roughly five seconds at 38.33 fps.
    // 384 MiB keeps the accepted Maximum build at roughly 192 frames.  The
    // separate front-GPU profile may explicitly request 512 MiB so long
    // phases (2.mov reaches one near 212.73 s) stay exact without a complete
    // second-pass fallback.
    static constexpr std::size_t MaximumFrames = 256;

    D3D11BoardPreprocessor();
    ~D3D11BoardPreprocessor();
    D3D11BoardPreprocessor(const D3D11BoardPreprocessor&) = delete;
    D3D11BoardPreprocessor& operator=(const D3D11BoardPreprocessor&) = delete;

    // memoryBudgetBytes controls the number of frame slots, capped by
    // MaximumFrames. model is non-owning and must outlive this object.
    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    int frameWidth,
                    int frameHeight,
                    const Settings& settings,
                    OnnxBoardModel* model,
                    std::size_t memoryBudgetBytes,
                    std::string& error);

    // Retains both players from one decoder texture without CPU Map/readback.
    // Reusing a ring position invalidates its previous handle by generation.
    bool retain(ID3D11Texture2D* texture,
                UINT sourceSubresource,
                std::int64_t sourceTime100ns,
                std::uint64_t deliveryOrder,
                D3D11BoardFrameHandle& handle,
                std::string& error);

    bool prepareFusedRetain(ID3D11Texture2D* texture,
                            UINT sourceSubresource,
                            std::int64_t sourceTime100ns,
                            std::uint64_t deliveryOrder,
                            D3D11BoardFusedTarget& target,
                            std::string& error);
    bool finishFusedRetain(const D3D11BoardFusedTarget& target,
                           D3D11BoardFrameHandle& handle,
                           std::string& error);

    // Generates one 200-cell tensor for the selected player and invokes CUDA
    // ONNX directly. Keeping one board per call is intentional: the current
    // model is faster as batch=200 than as large multi-board batches.
    bool infer(const D3D11BoardFrameHandle& handle,
               bool player1,
               D3D11BoardInference& result,
               std::string& error);

    bool valid(const D3D11BoardFrameHandle& handle) const;
    void reset();
    bool ready() const;
    std::size_t capacity() const;
    std::size_t bytesPerFrame() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr

#endif // Windows ultra GPU build
