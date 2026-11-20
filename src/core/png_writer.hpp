#pragma once
// Minimal indexed-colour PNG writer (stored deflate blocks, no zlib needed).
// Used by tools and tests to inspect decoded graphics.

#include "core/palette.hpp"
#include "core/surface.hpp"

#include <filesystem>
#include <optional>

namespace crusader {

// Writes `surface` as an 8-bit indexed PNG. `transparent` marks one palette
// index as fully transparent (tRNS chunk).
void write_png(const std::filesystem::path& path, const Surface8& surface, const Palette& palette,
               std::optional<std::uint8_t> transparent = std::nullopt);

// Writes `rgb` (width * height * 3 bytes, rows top to bottom) as an 8-bit RGB PNG.
void write_png_rgb(const std::filesystem::path& path, int width, int height, const std::uint8_t* rgb);

}  // namespace crusader
