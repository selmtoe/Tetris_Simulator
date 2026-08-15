#pragma once

#include <filesystem>
#include <string>

namespace tr {

std::string wasmSha256File(const std::filesystem::path& path);

} // namespace tr
