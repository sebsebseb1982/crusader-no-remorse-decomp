#pragma once
// Usecode process stack: a byte array growing downwards, addressed by 16-bit
// offsets (bp/sp are offsets into it), little-endian values.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace crusader {

class UCStack {
public:
    static constexpr std::size_t kSize = 0x1000;

    std::uint16_t sp() const { return sp_; }
    void set_sp(std::uint32_t sp) {
        if (sp > kSize) throw std::runtime_error("usecode stack: sp out of range");
        sp_ = static_cast<std::uint16_t>(sp);
    }
    // Positive: drops bytes (like popping); negative: reserves bytes.
    void add_sp(int delta) { set_sp(static_cast<std::uint32_t>(static_cast<int>(sp_) + delta)); }
    std::size_t used() const { return kSize - sp_; }

    void push(std::span<const std::uint8_t> bytes) {
        add_sp(-static_cast<int>(bytes.size()));
        std::memcpy(data_.data() + sp_, bytes.data(), bytes.size());
    }
    void push_zero(std::size_t n) {
        add_sp(-static_cast<int>(n));
        std::memset(data_.data() + sp_, 0, n);
    }
    void push1(std::uint8_t v) { push(std::span(&v, 1)); }
    void push2(std::uint16_t v) { write(static_cast<std::uint16_t>(sp_ - 2), v, 2), add_sp(-2); }
    void push4(std::uint32_t v) { write(static_cast<std::uint16_t>(sp_ - 4), v, 4), add_sp(-4); }

    std::uint16_t pop2() { const auto v = read(sp_, 2); add_sp(2); return static_cast<std::uint16_t>(v); }
    std::uint32_t pop4() { const auto v = read(sp_, 4); add_sp(4); return v; }

    // @orig none: stack access helper
    std::uint32_t read(std::uint32_t offset, std::size_t n) const {
        check(offset, n);
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= std::uint32_t{data_[offset + i]} << (8 * i);
        return v;
    }
    void write(std::uint32_t offset, std::uint32_t v, std::size_t n) {
        check(offset, n);
        for (std::size_t i = 0; i < n; ++i) data_[offset + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    std::span<std::uint8_t> bytes(std::uint32_t offset, std::size_t n) {
        check(offset, n);
        return std::span(data_).subspan(offset, n);
    }
    std::span<const std::uint8_t> top(std::size_t n) const {
        check(sp_, n);
        return std::span(data_).subspan(sp_, n);
    }

private:
    static void check(std::uint32_t offset, std::size_t n) {
        if (offset > kSize || n > kSize - offset) throw std::runtime_error("usecode stack: access out of range");
    }

    std::array<std::uint8_t, kSize> data_{};
    std::uint16_t sp_ = kSize;
};

}  // namespace crusader
