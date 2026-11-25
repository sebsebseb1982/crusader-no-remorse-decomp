#pragma once
// Port addition (@orig none): SHA-256 (FIPS 180-4), to check the archive of an
// update against the release's SHA256SUMS (core/self_update.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace crusader {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    // The digest as 64 lower-case hexadecimal digits; the object is spent.
    std::string finish_hex();

private:
    void block(const std::uint8_t* chunk);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;  // bytes
};

std::string sha256_hex(const void* data, std::size_t size);

}  // namespace crusader
