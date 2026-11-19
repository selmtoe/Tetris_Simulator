#pragma once
// Opt-in GPU memory settings for every ONNX Runtime CUDA session (v60).
// Nothing changes unless one of these variables is set:
//   TR_CUDA_ARENA_SAME=1        arena grows by the requested size, not power-of-two
//   TR_CUDA_CUDNN_SEARCH=X      cudnn_conv_algo_search: EXHAUSTIVE (ORT default) | HEURISTIC | DEFAULT
//   TR_CUDA_CUDNN_MAX_WS=0      cudnn_conv_use_max_workspace=0 (smaller cuDNN workspace)
//   TR_CUDA_MEM_LIMIT_MB=N      gpu_mem_limit per session arena
// cuDNN search/workspace can change the selected convolution algorithm, so those two
// must be validated for identical traces before use.
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace tr {
inline std::unordered_map<std::string, std::string> cudaProviderOptionsWithEnv(
    std::unordered_map<std::string, std::string> options) {
    const auto env = [](const char* name) -> std::string {
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    };
    if (env("TR_CUDA_ARENA_SAME") == "1") options["arena_extend_strategy"] = "kSameAsRequested";
    const std::string search = env("TR_CUDA_CUDNN_SEARCH");
    if (search == "EXHAUSTIVE" || search == "HEURISTIC" || search == "DEFAULT")
        options["cudnn_conv_algo_search"] = search;
    if (env("TR_CUDA_CUDNN_MAX_WS") == "0") options["cudnn_conv_use_max_workspace"] = "0";
    const std::string limit = env("TR_CUDA_MEM_LIMIT_MB");
    if (!limit.empty()) {
        const long long mb = std::atoll(limit.c_str());
        if (mb > 0) options["gpu_mem_limit"] = std::to_string(mb * 1048576LL);
    }
    return options;
}
}  // namespace tr
