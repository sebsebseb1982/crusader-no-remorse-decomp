#include "engine/ui/keypad_gump.hpp"

#include "engine/ui/gump_keys.hpp"

#include <algorithm>

namespace crusader {

namespace {

// @orig none: a frame of GUMPS.FLX, null when missing
const ShapeFrame* gump_frame(ShapeCache& gumps, std::uint16_t shape, int frame) {
    const Shape* s = gumps.get(shape);
    return s && frame >= 0 && static_cast<std::size_t>(frame) < s->frames().size()
               ? &s->frames()[static_cast<std::size_t>(frame)]
               : nullptr;
}

constexpr std::uint16_t kBackspaceKey = 0x08;

}  // namespace

const std::array<KeypadGump::Rect, KeypadGump::kButtons> KeypadGump::kButtonRects = {{
    {12, 25, 38, 49}, {39, 25, 65, 49}, {66, 25, 91, 49},
    {12, 50, 38, 73}, {39, 50, 65, 73}, {66, 50, 91, 73},
    {12, 74, 38, 97}, {39, 74, 65, 97}, {66, 74, 91, 97},
    {12, 98, 38, 123}, {39, 98, 65, 123}, {66, 98, 91, 123},
}};

// @orig 13C8:0133 13C8:0000
KeypadGump::KeypadGump(ShapeCache& gumps, std::int16_t code, std::function<void(int)> play_sfx, int screen_width,
                       int screen_height)
    : gumps_(&gumps), play_sfx_(std::move(play_sfx)), code_(code) {
    // GumpShape_rect of the frame, centred on the screen.
    const ShapeFrame* f = gump_frame(gumps, kShape, 0);
    x_ = (screen_width - (f ? f->width : 0)) >> 1;
    y_ = (screen_height - (f ? f->height : 0)) >> 1;
}

// @orig 13C8:08F5 13C8:00C4
void KeypadGump::draw(Surface8& screen) const {
    if (const ShapeFrame* f = gump_frame(*gumps_, kShape, 0)) f->draw(screen, x_, y_);
    int x = x_ + kDigitsX;
    for (int i = 0; i < count_; ++i) {
        const int digit = digits_[static_cast<std::size_t>(i)];
        if (const ShapeFrame* f = gump_frame(*gumps_, kDigitShape, digit == 0 ? 9 : digit - 1)) {
            f->draw(screen, x, y_ + kDigitsY);
            x += f->width + kDigitSpacing;
        } else {
            x += kDigitSpacing;
        }
    }
    // The children, after the gump.
    for (int i = 0; i < kButtons; ++i) {
        const Rect& r = kButtonRects[static_cast<std::size_t>(i)];
        const int frame = pressed_[static_cast<std::size_t>(i)] ||
                                  (selection_active_ && selected_ == i)
                              ? i + kPressedFrame
                              : i;
        if (const ShapeFrame* f = gump_frame(*gumps_, kButtonShape, frame)) f->draw(screen, x_ + r.x0, y_ + r.y0);
    }
}

void KeypadGump::add_digit(int digit) {
    if (count_ < kMaxDigits) digits_[static_cast<std::size_t>(count_++)] = digit;
}

// @orig 13C8:0543
void KeypadGump::button(int index) {
    if (index == kBackspace) {
        if (count_ > 0) --count_;
    } else if (index == kEnter) {
        compute_value();
        check();
        finish(static_cast<std::int16_t>(value_));
    } else {
        add_digit(index == kZero ? 0 : index + 1);
    }
}

// @orig none: keypad selection moved by the gamepad, port addition
void KeypadGump::move_selection(int dx, int dy) {
    int column = selected_ % 3;
    int row = selected_ / 3;
    column = std::clamp(column + dx, 0, 2);
    row = std::clamp(row + dy, 0, 3);
    selected_ = row * 3 + column;
    selection_active_ = true;
}

// @orig 13C8:089A
void KeypadGump::compute_value() {
    value_ = 0;
    for (int i = 0; i < count_; ++i) value_ = value_ * 10 + digits_[static_cast<std::size_t>(i)];
}

// @orig 13C8:09DB
void KeypadGump::check() {
    if (code_ == 0) return;
    if (value_ == kCheatCode) {
        matched_ = 1;
        value_ = code_;
    } else {
        value_ %= 10000;  // the RTL's LMOD (1000:339D)
        matched_ = value_ == code_ ? 1 : 0;
    }
    if (play_sfx_) play_sfx_(matched_ ? kSoundRight : kSoundWrong);
}

// The value goes to Gump_run_modal in a gump event (kind 0x10), then the
// gump closes (vtable +0x3C, Gump_close).
// @orig 1368:1297 1368:12EE
void KeypadGump::finish(int result) {
    result_ = result;
    closed_ = true;
}

// @orig 13C8:0646
void KeypadGump::key(std::uint16_t code) {
    // The shifted digits of a US keyboard.
    switch (code) {
    case ')': code = '0'; break;
    case '!': code = '1'; break;
    case '@': code = '2'; break;
    case '#': code = '3'; break;
    case '$': code = '4'; break;
    case '%': code = '5'; break;
    case '^': code = '6'; break;
    case '&': code = '7'; break;
    case '*': code = '8'; break;
    case '(': code = '9'; break;
    default: break;
    }
    if (code == gump_key::kUp) {
        move_selection(0, -1);
    } else if (code == gump_key::kDown) {
        move_selection(0, 1);
    } else if (code == gump_key::kLeft) {
        move_selection(-1, 0);
    } else if (code == gump_key::kRight) {
        move_selection(1, 0);
    } else if (code >= '0' && code <= '9') {
        selection_active_ = false;
        if (count_ < kMaxDigits) {
            add_digit(code & 0x0F);
            if (play_sfx_) play_sfx_(kSoundButton);
        }
    } else if (code == kBackspaceKey) {
        selection_active_ = false;
        if (count_ > 0) --count_;
        if (play_sfx_) play_sfx_(kSoundBackspace);
    } else if (code == gump_key::kEscape) {
        value_ = 0;
        finish(-1);
    } else if (code == gump_key::kEnter) {
        if (selection_active_) {
            button(selected_);
            if (play_sfx_) play_sfx_(selected_ == kBackspace ? kSoundBackspace : kSoundButton);
        } else if (count_ > 0) {
            compute_value();
            check();
            finish(static_cast<std::int16_t>(value_));
        }
    }
}

// A button released after being pressed sounds.
// @orig 13C8:0086
void KeypadGump::release(int index) {
    if (!pressed_[static_cast<std::size_t>(index)]) return;
    pressed_[static_cast<std::size_t>(index)] = false;
    if (play_sfx_) play_sfx_(index == kBackspace ? kSoundBackspace : kSoundButton);
}

// @orig 1310:001D 13C8:006D
void KeypadGump::mouse_down(int x, int y) {
    if (closed_ || captured_ >= 0) return;
    selection_active_ = false;
    for (int i = 0; i < kButtons; ++i) {
        if (kButtonRects[static_cast<std::size_t>(i)].contains(x - x_, y - y_)) {
            captured_ = i;
            pressed_[static_cast<std::size_t>(i)] = true;
            return;
        }
    }
}

// @orig 1310:0151
void KeypadGump::mouse_move(int x, int y) {
    if (captured_ < 0) return;
    const bool inside = kButtonRects[static_cast<std::size_t>(captured_)].contains(x - x_, y - y_);
    if (inside) pressed_[static_cast<std::size_t>(captured_)] = true;
    else release(captured_);
}

// @orig 1310:0254 1310:001D
void KeypadGump::mouse_up(int x, int y) {
    if (captured_ < 0) return;
    const int index = captured_;
    captured_ = -1;
    if (kButtonRects[static_cast<std::size_t>(index)].contains(x - x_, y - y_)) button(index);
    release(index);
}

}  // namespace crusader
