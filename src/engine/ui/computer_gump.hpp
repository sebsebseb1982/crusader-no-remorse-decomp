#pragma once
// The computer screen (BARK.C, segment 13A0; ComputerGump in ScummVM), opened
// by the intrinsic ComputerGump::I_readComputer (0x0FE, 13A0:0005) with a
// text: World_enter_menu_mode, the gump run by Gump_run_modal, then
// World_leave_menu_mode. The monitors' scripts (MONITNS / MONITEW, through
// TEXTFILE) and the kill counter (VMAIL) use it.
//
// Text (13A0:03C8): a copy of the string, wrapped at 20 columns: '^' is a
// line break; walking the text, a count of the characters since the last
// break (code 1 not counted) and the last space seen; at a character with 20
// or more counted and a space seen, that space becomes '^' and the count
// restarts at (index - space). On screen code 1 and '*' take no column.
//
// Layout: GUMPS.FLX shape 0x1E, frame 0 (the top of the monitor) at (x, y)
// and frame 1 at (x, y + 160); the gump, frame 0's width by both heights,
// centred on the screen (13A0:0034). Characters in font 6 on a grid of 8 x 9
// pixels from (x + 41, y + 38): 20 columns, a new page when the 14th row
// (row 13) is reached.
//
// Fields: text +0x34, characters shown +0x38, page start +0x3C, column and
// row +0x46 / +0x48, page full +0x3B, skip +0x4A; DS:55AA asks for a full
// redraw.
//
// Each pass of the modal loop (vtable +4, 13A0:08D2):
//   - text ended: only a redraw;
//   - skip set: the layout is redone (without drawing) until the page is
//     full or the text ends, then skip is cleared and a full redraw asked;
//   - a redraw (13A0:02D1), which shows one character more: the layout of
//     the page (13A0:0487) from the page start to the characters shown, and
//     when the page is not full and the text not ended, one more character
//     and the cursor (glyph 0x5B) where it goes; a full page shows "-SUITE-"
//     (DS:55AB; "<MORE>" in the US version) at the start of its row 13;
//   - unless the page is full: after a '*', a pause (delay 4800, 1470:0692);
//     otherwise sound 0x33 restarted (unless the character is '\n') and one
//     tick of wait (SystemTimer_busy_wait).
// Keys (vtable +0x18, 13A0:0A2A): a full page goes to the next one (any key);
// otherwise Escape or Enter close the gump once the text has ended, and
// Escape skips to the end of the page before that. A mouse press (vtable +8,
// 13A0:0887) closes once the text has ended, otherwise skips; on a full page
// it does nothing more. The mouse cursor is hidden while it is open.
//
// The pause after '*': 1470:0692 is the RTL's delay(ms), timed with the PIT
// counter as if it wrapped every 65536 counts (DS:77CC = 1193 counts per
// millisecond). The game reprograms the PIT (mode 3, 11D8:2A70) to its
// fastest timed process, 60 Hz: the counter then wraps every 19886 / 2
// counts, so the pause lasts about 4800 * 9943 / 65536 = 728 ms. The port
// waits 44 ticks.
//
// Not reproduced:
//   - the partial redraws: the original draws only the new character, on a
//     cell first filled with colour 0 (the cursor's size at a '^'), and the
//     whole page only on DS:55AA; the port draws the page at every frame (the
//     monitor's screen is black, so the fills do not show);
//   - the joystick (DS:6396): the gump waits for its buttons to be released,
//     and a button sends Escape;
//   - the pause after '*' blocks the original's loop, events included; the
//     port only delays the next character;
//   - DS:0636 (animations off) and DS:0907 cleared while it is open; on
//     closing the view is redrawn (DS:2C62 +0x2C, +0) and the mouse clicks
//     forgotten (DS:8A80..8A84).

#include "core/font.hpp"
#include "core/language.hpp"
#include "core/surface.hpp"
#include "engine/ui/modal_gump.hpp"
#include "engine/world/render.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace crusader {

class ComputerGump : public ModalGump {
public:
    static constexpr int kFont = 6;
    static constexpr std::uint16_t kShape = 0x1E;
    static constexpr int kBottomY = 160;
    static constexpr int kTextX = 41;
    static constexpr int kTextY = 38;
    static constexpr int kCharWidth = 8;
    static constexpr int kRowHeight = 9;
    static constexpr int kColumns = 20;
    static constexpr int kRows = 13;
    static constexpr unsigned char kCursor = 0x5B;
    static constexpr int kSound = 0x33;
    static constexpr std::uint32_t kStarPause = 44;  // ticks, see above
    static const char* more() { return tr("<MORE>"); }  // DS:55AB (US: DS:557F)

    // `play_sfx` restarts a sound (dsfx_stop_if, then AudioProcess_I_playSFX).
    ComputerGump(ShapeCache& gumps, const FontSet& fonts, const std::string& text, std::function<void(int)> play_sfx,
                 int screen_width = 640, int screen_height = 480);

    // The text as wrapped (13A0:03C8).
    static std::string wrap(const std::string& text);

    void draw(Surface8& screen) const override;
    void key(std::uint16_t code) override;
    void mouse_down(int x, int y) override;
    void update(std::uint32_t ticks) override;
    bool closed() const override { return closed_; }
    bool over_view() const override { return true; }

    const std::string& text() const { return text_; }
    int x() const { return x_; }
    int y() const { return y_; }
    int shown() const { return shown_; }
    int page_start() const { return page_start_; }
    bool page_full() const { return page_full_; }
    bool ended() const { return at(shown_) == 0; }

private:
    char at(int i) const { return i < static_cast<int>(text_.size()) ? text_[static_cast<std::size_t>(i)] : 0; }
    void layout();  // 13A0:0487 without the drawing
    void run();     // 13A0:08D2

    ShapeCache* gumps_;
    const FontSet* fonts_;
    std::function<void(int)> play_sfx_;
    std::string text_;
    int x_ = 0;
    int y_ = 0;
    int shown_ = 0;
    int page_start_ = 0;
    int column_ = 0;
    int row_ = 0;
    bool page_full_ = false;
    bool skip_ = false;
    bool cursor_ = false;  // drawn by the last layout, at (column_, row_)
    bool closed_ = false;
    std::uint32_t next_run_ = 0;
};

}  // namespace crusader
