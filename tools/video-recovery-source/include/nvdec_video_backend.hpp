#pragma once

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_NVDEC_DEFAULT)

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common.hpp"

namespace tr {

class NvdecVideoBackend {
public:
    using Consumer = std::function<bool(
        ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, UINT,
        std::int64_t, std::string&)>;
    using CudaConsumer = std::function<bool(
        const void*, std::size_t, int, int, bool,
        std::int64_t, std::string&)>;

    NvdecVideoBackend();
    ~NvdecVideoBackend();
    NvdecVideoBackend(const NvdecVideoBackend&) = delete;
    NvdecVideoBackend& operator=(const NvdecVideoBackend&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    void setConversionRegions(const std::vector<PixelRegion>& regions);
    bool seek(double seconds, std::string& error);
    bool readD3D11Sample(std::int64_t& timestamp100ns, bool& endOfStream,
                         std::string& error, const Consumer& consumer);
    bool readCudaSample(std::int64_t& timestamp100ns, bool& endOfStream,
                        std::string& error, const CudaConsumer& consumer,
                        void* consumerStream = nullptr);
    void close();

    int width() const;
    int height() const;
    double durationSeconds() const;
    double frameRate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr

#endif
