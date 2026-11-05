#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace lcs {

// A PNG file as RGBA8 (FFmpeg's PNG decoder). Empty on any failure.
[[nodiscard]] std::vector<std::uint8_t> decode_png_rgba(const std::filesystem::path &path, std::uint32_t &width,
                                                        std::uint32_t &height);

}  // namespace lcs
