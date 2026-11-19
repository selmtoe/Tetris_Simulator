#pragma once
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
#include "recovery.hpp"
namespace tr {
std::vector<PixelRegion> browserNameRegions(int width, int height, const Settings& settings);
void browserRecognizeNames(const std::filesystem::path& input, double seconds,
                          const Settings& settings, RecoveryOutput& result);
}
#endif
