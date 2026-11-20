#include "core/png_writer.hpp"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace crusader {

namespace {

// @orig none: PNG export for the tools
std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// @orig none: PNG export for the tools
void put_u32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// @orig none: PNG export for the tools
void put_chunk(std::vector<std::uint8_t>& out, std::string_view type, const std::vector<std::uint8_t>& body) {
    put_u32be(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), body.begin(), body.end());
    put_u32be(out, crc32(out.data() + start, out.size() - start));
}

// zlib stream made of uncompressed deflate blocks.
// @orig none: PNG export for the tools
std::vector<std::uint8_t> zlib_store(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(raw.size() - pos, 65535);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put_u32be(z, b << 16 | a);
    return z;
}

}  // namespace

// @orig none: PNG export for the tools
void write_png(const std::filesystem::path& path, const Surface8& s, const Palette& palette,
               std::optional<std::uint8_t> transparent) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    std::vector<std::uint8_t> ihdr;
    put_u32be(ihdr, static_cast<std::uint32_t>(s.width));
    put_u32be(ihdr, static_cast<std::uint32_t>(s.height));
    ihdr.insert(ihdr.end(), {8, 3, 0, 0, 0});  // 8-bit, indexed, deflate, no filter, no interlace
    put_chunk(png, "IHDR", ihdr);

    std::vector<std::uint8_t> plte;
    for (const Rgb& c : palette.colors) {
        plte.insert(plte.end(), {c.r, c.g, c.b});
    }
    put_chunk(png, "PLTE", plte);

    if (transparent) {
        std::vector<std::uint8_t> trns(std::size_t{*transparent} + 1, 0xFF);
        trns.back() = 0;
        put_chunk(png, "tRNS", trns);
    }

    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(s.width + 1) * s.height);
    for (int y = 0; y < s.height; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), s.row(y), s.row(y) + s.width);
    }
    put_chunk(png, "IDAT", zlib_store(raw));
    put_chunk(png, "IEND", {});

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

// @orig none: PNG export for the tools
void write_png_rgb(const std::filesystem::path& path, int width, int height, const std::uint8_t* rgb) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put_u32be(ihdr, static_cast<std::uint32_t>(width));
    put_u32be(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, no filter, no interlace
    put_chunk(png, "IHDR", ihdr);
    std::vector<std::uint8_t> raw;
    const std::size_t stride = static_cast<std::size_t>(width) * 3;
    raw.reserve((stride + 1) * height);
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgb + y * stride, rgb + (y + 1) * stride);
    }
    put_chunk(png, "IDAT", zlib_store(raw));
    put_chunk(png, "IEND", {});
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

}  // namespace crusader
