#pragma once
// The keypad of the doors with a code (IRGUMP, segment 13C8; KeypadGump in
// ScummVM), opened by the intrinsic KeypadGump::I_showKeypad (0x0C5,
// 13C8:00FE) with the code to enter: World_enter_menu_mode, the gump run by
// Gump_run_modal, then World_leave_menu_mode; its result goes back to the
// script. The KEYPAD class (use event) reads the code from the VALBOX with
// the same QLo, and opens the door when the result is that code; -1 means
// cancelled.
//
// Layout (13C8:0133): GUMPS.FLX shape 10, frame 0, centred on the screen.
// Twelve buttons (13C8:0000), children of the gump, shape 11 with frame i
// (i + 12 while pressed, 13C8:006D), each on a rectangle of the gump
// (inclusive): columns 12..38, 39..65, 66..91 and rows 25..49, 50..73,
// 74..97, 98..123, numbered from 0 left to right then top to bottom:
// 0..8 the digits 1..9, 9 backspace, 10 the digit 0, 11 enter. The digits
// entered (8 at most, +0x44) are drawn (13C8:08F5) with shape 12, frame
// digit - 1 (9 for 0), from (x + 25, y + 12), each the frame's width + 1
// further.
//
// Fields: shape +0x34, frame +0x36, digits entered +0x39, value +0x3B
// (32 bits), code +0x3F, matched +0x43 (0xFF until checked), digits +0x44;
// DS:0636 (animations off) set while it is open, restored by the dtor.
//
// Buttons (1310:0254, Button_on_mouse 1310:001D then the release): a press
// inside captures the mouse and presses the button; a drag (1310:0151)
// presses it again inside and releases it outside; the release ends the
// capture, notifies the gump (13C8:0543) when inside, then releases the
// button. A button released after being pressed plays sound 0x3A
// (backspace) or 0x3B (13C8:0086).
//
// The gump (13C8:0543 for its buttons, 13C8:0646 for the keys):
//   - a digit (button or key '0'..'9') is added while fewer than 8 are
//     entered; the key also plays 0x3B, even with 8 digits shown but only
//     when added;
//   - backspace removes the last digit; the key also plays 0x3A;
//   - enter (the button always, the key only with a digit entered): the
//     value (13C8:089A) is checked (13C8:09DB), the result is the value,
//     the gump closes;
//   - Escape: the result is -1, the gump closes.
//   - arrows move a selection over the buttons; Enter activates it. The
//     selection is inactive until an arrow is pressed, preserving Enter as
//     the submit key for codes typed on the keyboard.
//   The shifted digits of a US keyboard count as the digits: ! @ # $ % ^ &
//   * ( ) for 1..9, 0.
// The check, only with a code (non-zero): the value 74697689 (0x0473CBD9) is
// a cheat code, taken as the code; otherwise the value modulo 10000. Sound
// 0x32 when it matches the code, 0x31 otherwise.
//
// Not reproduced:
//   - the result goes through a gump event of kind 0x10 whose 16-bit value
//     Gump_run_modal returns; the port keeps the low 16 bits of the value;
//   - DS:0636 and the dtor's flush of the keyboard and the mouse clicks
//     (DS:8A80..8A84), done by the game when the gump closes;
//   - a double click counts as a press (event kind 4, DS:47D2 bit 2).

#include "core/surface.hpp"
#include "engine/ui/modal_gump.hpp"
#include "engine/world/render.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace crusader {

class KeypadGump : public ModalGump {
public:
    static constexpr std::uint16_t kShape = 10;
    static constexpr std::uint16_t kButtonShape = 11;
    static constexpr std::uint16_t kDigitShape = 12;
    static constexpr int kButtons = 12;
    static constexpr int kPressedFrame = 12;  // added to a button's frame
    static constexpr int kBackspace = 9;
    static constexpr int kZero = 10;
    static constexpr int kEnter = 11;
    static constexpr int kMaxDigits = 8;
    static constexpr int kDigitsX = 25;
    static constexpr int kDigitsY = 12;
    static constexpr int kDigitSpacing = 1;
    static constexpr std::int32_t kCheatCode = 74697689;
    static constexpr int kSoundBackspace = 0x3A;
    static constexpr int kSoundButton = 0x3B;
    static constexpr int kSoundWrong = 0x31;
    static constexpr int kSoundRight = 0x32;

    struct Rect {
        int x0, y0, x1, y1;
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };
    // The buttons' rectangles in the gump (13C8:0133).
    static const std::array<Rect, kButtons> kButtonRects;

    // `code`: the code to enter; `play_sfx` plays a sound (AudioProcess_I_playSFX).
    KeypadGump(ShapeCache& gumps, std::int16_t code, std::function<void(int)> play_sfx, int screen_width = 640,
               int screen_height = 480);

    void draw(Surface8& screen) const override;
    void key(std::uint16_t code) override;
    void mouse_down(int x, int y) override;
    void mouse_move(int x, int y) override;
    void mouse_up(int x, int y) override;
    bool closed() const override { return closed_; }
    // The usecode's result: -1 when cancelled, else the low 16 bits of the
    // value entered (the code itself when it matched).
    int result() const override { return result_; }
    bool over_view() const override { return true; }

    int x() const { return x_; }
    int y() const { return y_; }
    std::int16_t code() const { return code_; }
    int digit_count() const { return count_; }
    std::int32_t value() const { return value_; }
    // 1 matched, 0 not, -1 not checked (+0x43).
    int matched() const { return matched_; }
    bool pressed(int button) const { return pressed_[static_cast<std::size_t>(button)]; }

private:
    void button(int index);      // 13C8:0543
    void move_selection(int dx, int dy);
    void add_digit(int digit);
    void compute_value();        // 13C8:089A
    void check();                // 13C8:09DB
    void finish(int result);
    void release(int index);     // 13C8:0086

    ShapeCache* gumps_;
    std::function<void(int)> play_sfx_;
    std::int16_t code_;
    int x_ = 0;
    int y_ = 0;
    std::array<int, kMaxDigits> digits_{};
    int count_ = 0;
    std::int32_t value_ = 0;
    int matched_ = -1;
    std::array<bool, kButtons> pressed_{};
    int captured_ = -1;  // the button holding the mouse (DS:47D2)
    int selected_ = 0;
    bool selection_active_ = false;
    int result_ = 0;
    bool closed_ = false;
};

}  // namespace crusader
