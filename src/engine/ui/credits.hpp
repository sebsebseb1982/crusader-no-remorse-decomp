#pragma once
// The credits (WORLD_13f0_03f4, segment 13F0), from the main menu's "credits"
// button.
//
// Data: STATIC/CRED.DAT is an 8-bit 640 x 480 BMP read row by row from the
// bottom (segment 1010), STATIC/CRED.PAL its palette, STATIC/CREDITS.DAT the
// script. Fonts 16 (titles) and 17 (names).
//
// The original runs a loop of its own, one pass per vertical retrace (60 Hz
// in 640 x 480): text step (WORLD_13f0_041d), retrace, palette step (0753).
// It stops at the end of the script or when a key is pressed (DS:322A).
//
// Text step, when no palette effect runs: if the page must be cleared, the
// background is put back; then, once per 60 Hz timer tick, a wait counter
// (DS:62B0) counts down; at 0 it is set to 64 and the script is read until
// '*' or '&':
//   '!' u16   y of the next line
//   '@' text  title in font 16, '$' text: name in font 17, both centred
//             ((640 - width) >> 1) at y; y += height + 4
//   '^'       fade the first 128 colours in
//   '#'       end
//   '*'       stop here (so each '*' waits 64 ticks)
//   '&'       stop here; y = 20; fade the first 128 colours out; the page
//             is cleared once the fade is over and the wait counter is 0
// After a '*' or '&', if the page was cleared and no effect runs, the colours
// fade in. Any other byte is skipped.
//
// Palette effects (WORLD_13f0_0776): colours 0..127 of CRED.PAL written as
// max(c - level, 0), one write per retrace. Fade out: level 0, 4, ..., 60, 63;
// fade in: level 63, 59, ..., 3, 0. The rest of the DAC is left alone.
//
// Not reproduced: the music started for the credits (STDINT_1200_0262 0x11),
// and the end video played when the credits ran to the end (DS:08F0).

#include "core/data_files.hpp"
#include "core/font.hpp"
#include "core/palette.hpp"
#include "core/surface.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace crusader {

struct CreditsData {
    Surface8 background;
    Palette palette;
    std::vector<std::uint8_t> script;

    static CreditsData load(const DataFiles& files);
};

// segment 1010: 8-bit BMP (14-byte file header, 40-byte info header, 1024-byte
// colour table), rows stored bottom-up, `width` bytes each.
Surface8 read_bmp8(const std::vector<std::uint8_t>& file);
// Bmp_palette (1010:01EB): the colour table's B, G, R, x entries as 6-bit
// components (each >> 2).
Palette read_bmp_palette(const std::vector<std::uint8_t>& file);

class Credits {
public:
    static constexpr int kTitleFont = 16;
    static constexpr int kNameFont = 17;
    static constexpr int kWait = 64;

    Credits(const CreditsData& data, const FontSet& fonts);

    // One pass of the loop: text step with the 60 Hz timer value, then palette step.
    void frame(std::uint32_t timer_ticks);
    void key_pressed() { stopped_ = true; }

    bool finished() const { return ended_ || stopped_; }
    bool reached_end() const { return ended_; }
    const Surface8& screen() const { return screen_; }
    const Palette& palette() const { return dac_; }

private:
    void text_step(std::uint32_t timer_ticks);
    void palette_step();
    void start_effect(bool fade_in);
    void line(int font, std::size_t& pos, int& y);

    const CreditsData* data_;
    const FontSet* fonts_;
    Surface8 screen_;
    Palette dac_;
    std::size_t pos_ = 0;
    int y_ = 0x50;              // DS:62CE
    int wait_ = 0;              // DS:62B0
    bool clear_page_ = false;   // DS:62BB
    bool ended_ = false;        // DS:62B4
    bool stopped_ = false;
    std::int64_t last_tick_ = -1;
    bool effect_ = false;       // DS:62C0
    bool fade_in_ = false;
    int level_ = 0;             // DS:62C3
};

}  // namespace crusader
