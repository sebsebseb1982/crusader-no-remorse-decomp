#pragma once
// Port addition (@orig none): reads the two archives the releases ship (zip
// for Windows, tar.gz for Linux), for the self-update (core/self_update.hpp).
// No library: DEFLATE (RFC 1951), gzip (RFC 1952), the zip central directory
// and the tar headers (ustar, GNU long names, pax paths) are read here.
//
// Everything is bounded (kMaxEntries, kMaxUnpacked, an entry never larger than
// what its header tells): a broken or hostile archive gives an error, never
// a crash nor a flood of memory.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace crusader {

inline constexpr std::size_t kMaxArchiveEntries = 4096;
inline constexpr std::size_t kMaxUnpacked = 256u << 20;  // bytes, all entries together

// The stream `data` holds, raw DEFLATE; none when it is broken or would give
// more than `max_out` bytes. `consumed`, when given, tells the bytes read.
std::optional<std::vector<std::uint8_t>> inflate_raw(const std::uint8_t* data, std::size_t size,
                                                     std::size_t max_out, std::size_t* consumed = nullptr);
// A gzip member (one file): its header, its DEFLATE stream, its CRC-32 and
// length checked.
std::optional<std::vector<std::uint8_t>> gunzip(const std::uint8_t* data, std::size_t size, std::size_t max_out);

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

struct ArchiveEntry {
    std::string path;  // '/' separated, as stored
    bool directory = false;
    bool executable = false;  // the tar's mode; a zip tells nothing: decided by the caller
    std::vector<std::uint8_t> data;
};

enum class ArchiveFormat { kZip, kTarGz };

struct ArchiveResult {
    bool ok = false;
    std::string error;  // for the log, English
};

// Calls `each` for every file and directory of the archive, in order; it
// returns false to stop (the result is then an error "stopped").
ArchiveResult read_archive(const std::vector<std::uint8_t>& bytes, ArchiveFormat format,
                           const std::function<bool(ArchiveEntry&)>& each);

}  // namespace crusader
