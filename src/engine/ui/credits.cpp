#include "engine/ui/credits.hpp"

#include "core/bytes.hpp"
#include "core/flex.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace crusader {

namespace {
constexpr int kEffectFirst = 0, kEffectCount = 0x80, kEffectStep = 4;
constexpr int kPageTop = 20;
}  // namespace

// @orig 1010:0098 1010:0167
Surface8 read_bmp8(const std::vector<std::uint8_t>& file) {
    const std::span<const std::uint8_t> d(file);
    const std::uint32_t pixels = read_u32(d, 10);
    const int width = read_i32(d, 18);
    const int height = read_i32(d, 22);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || read_u16(d, 28) != 8) {
        throw std::runtime_error("not an 8-bit BMP");
    }
    check_range(d, pixels, static_cast<std::size_t>(width) * height);
    Surface8 s(width, height);
    for (int i = 0; i < height; ++i) {
        const auto row = d.subspan(pixels + static_cast<std::size_t>(i) * width, width);
        std::copy(row.begin(), row.end(), s.row(height - 1 - i));
    }
    return s;
}

// @orig 1010:01EB
Palette read_bmp_palette(const std::vector<std::uint8_t>& file) {
    const std::span<const std::uint8_t> d(file);
    constexpr std::size_t kTable = 14 + 40;
    check_range(d, kTable, 256 * 4);
    std::array<std::uint8_t, 768> vga{};
    for (std::size_t i = 0; i < 256; ++i) {
        vga[i * 3] = static_cast<std::uint8_t>(d[kTable + i * 4 + 2] >> 2);
        vga[i * 3 + 1] = static_cast<std::uint8_t>(d[kTable + i * 4 + 1] >> 2);
        vga[i * 3 + 2] = static_cast<std::uint8_t>(d[kTable + i * 4] >> 2);
    }
    return Palette::from_vga(vga);
}

// @orig 13F0:003A
CreditsData CreditsData::load(const DataFiles& files) {
    CreditsData c;
    c.background = read_bmp8(read_file(files.find("STATIC/CRED.DAT")));
    c.palette = Palette::load(files.find("STATIC/CRED.PAL"));
    c.script = read_file(files.find("STATIC/CREDITS.DAT"));
    return c;
}

Credits::Credits(const CreditsData& data, const FontSet& fonts)
    : data_(&data), fonts_(&fonts), screen_(data.background), dac_(data.palette) {}

void Credits::frame(std::uint32_t timer_ticks) {
    if (finished()) return;
    text_step(timer_ticks);
    palette_step();
}

// @orig 13F0:0776
void Credits::start_effect(bool fade_in) {
    // WORLD_13f0_0776 then an immediate palette step.
    effect_ = true;
    fade_in_ = fade_in;
    level_ = fade_in ? 0x3F : 0;
    palette_step();
}

// @orig 13F0:0753
void Credits::palette_step() {
    if (!effect_) return;
    if (fade_in_ && level_ < 0) level_ = 0;
    if (!fade_in_ && level_ > 0x3E) level_ = 0x3F;
    auto vga = dac_.to_vga();
    const auto src = data_->palette.to_vga();
    for (int i = kEffectFirst * 3; i < (kEffectFirst + kEffectCount) * 3; ++i) {
        vga[i] = static_cast<std::uint8_t>(std::max(0, src[i] - level_));
    }
    dac_ = Palette::from_vga(vga);
    if (fade_in_) {
        if (level_ == 0) effect_ = false;
        else level_ -= kEffectStep;
    } else {
        if (level_ == 0x3F) effect_ = false;
        else level_ += kEffectStep;
    }
}

// @orig 13F0:041D
void Credits::line(int font, std::size_t& pos, int& y) {
    const auto& s = data_->script;
    std::size_t end = pos;
    while (end < s.size() && s[end] != 0) ++end;
    const std::string text(s.begin() + pos, s.begin() + end);
    TextWriter t(*fonts_, font);
    const auto size = t.measure(text);
    t.print(screen_, (640 - size.width) >> 1, y, text);
    y += size.height + 4;
    pos = end + 1;
}

// @orig 13F0:041D
void Credits::text_step(std::uint32_t timer_ticks) {
    if (effect_) return;
    if (clear_page_) screen_ = data_->background;
    if (static_cast<std::int64_t>(timer_ticks) == last_tick_) return;
    last_tick_ = timer_ticks;
    if (wait_ != 0) {
        --wait_;
        return;
    }
    wait_ = kWait;
    const auto& s = data_->script;
    int y = y_;
    while (pos_ < s.size() && s[pos_] != '*' && s[pos_] != '&') {
        switch (s[pos_]) {
        case '$':
            ++pos_;
            line(kNameFont, pos_, y);
            break;
        case '@':
            ++pos_;
            line(kTitleFont, pos_, y);
            break;
        case '!':
            y = static_cast<std::int16_t>(read_u16(s, pos_ + 1));
            pos_ += 3;
            break;
        case '#':
            ended_ = true;
            return;
        case '^':
            start_effect(true);
            ++pos_;
            break;
        default:
            ++pos_;
            break;
        }
    }
    if (pos_ >= s.size()) {  // no '#': the original would read past the end
        ended_ = true;
        return;
    }
    if (s[pos_] == '&') {
        y = kPageTop;
        start_effect(false);
        clear_page_ = true;
        wait_ = 0;
    }
    if (clear_page_ && !effect_) {
        start_effect(true);
        clear_page_ = false;
    }
    ++pos_;
    y_ = y;
}

}  // namespace crusader
