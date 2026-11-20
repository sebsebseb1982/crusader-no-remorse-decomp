#include "engine/ui/gamepad_screen.hpp"

#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>

namespace crusader {

namespace {

constexpr int kPanelX0 = 20, kPanelY0 = 10, kPanelX1 = 619, kPanelY1 = 462;
constexpr int kTitleY = 34;
constexpr int kRowX0 = 50, kRowX1 = 589, kRowY0 = 44, kRowStep = 15, kRowHeight = 14;
constexpr int kValueX0 = 340;
constexpr int kLabelX = 58, kTextDrop = 12;  // glyphs hang from their baseline
constexpr int kHintY = 456;
constexpr int kHintFont = 7;
constexpr std::uint8_t kFrameColour = 0x57, kGreen = 0x0C, kDimGreen = 0x75;
constexpr std::uint16_t kDelete = 0x153;

}  // namespace

GamepadScreen::GamepadScreen(const FontSet& fonts, GameConfig& config, int& focus)
    : fonts_(&fonts), config_(&config), focus_(&focus) {
    *focus_ = std::clamp(*focus_, 0, kRows - 1);
}

GamepadScreen::Rect GamepadScreen::row_rect(int row) const {
    const int y = kRowY0 + kRowStep * row;
    return {kRowX0, y, kRowX1, y + kRowHeight - 1};
}

GamepadScreen::Rect GamepadScreen::value_rect(int row) const {
    const Rect r = row_rect(row);
    return {kValueX0, r.y0, r.x1, r.y1};
}

// Text in one colour: the font is drawn on a scratch surface, then its
// pixels are copied in `tint`. A negative tint keeps the font's own colours.
// @orig none: port-only gamepad screen

void GamepadScreen::print(Surface8& screen, int x, int y, const std::string& text, int tint) const {
    TextWriter writer(*fonts_, kFont);
    if (tint < 0) {
        writer.print(screen, x, y, text);
        return;
    }
    constexpr int kPad = 4, kAbove = 32, kHeight = 48;
    const int width = writer.measure(text).width + 2 * kPad;
    Surface8 scratch(width, kHeight);
    writer.print(scratch, kPad, kAbove, text);
    for (int sy = 0; sy < kHeight; ++sy) {
        for (int sx = 0; sx < width; ++sx) {
            const int dx = x - kPad + sx, dy = y - kAbove + sy;
            if (scratch.row(sy)[sx] && dx >= 0 && dy >= 0 && dx < screen.width && dy < screen.height) {
                screen.row(dy)[dx] = static_cast<std::uint8_t>(tint);
            }
        }
    }
}

// @orig none: port-only gamepad screen

void GamepadScreen::draw(Surface8& screen) const {
    screen.clear(0);
    outline(screen, kPanelX0, kPanelY0, kPanelX1, kPanelY1, kFrameColour);
    outline(screen, kPanelX0 + 2, kPanelY0 + 2, kPanelX1 - 2, kPanelY1 - 2, kFrameColour);
    const std::string title = tr("CONTROLLER");
    const int title_width = TextWriter(*fonts_, kFont).measure(title).width;
    print(screen, (screen.width - title_width) / 2, kTitleY, title, kGreen);

    const PadSettings& pad = config_->pad;
    for (int row = 0; row < kRows; ++row) {
        const Rect r = row_rect(row);
        const bool focused = row == *focus_;
        outline(screen, r.x0, r.y0, r.x1, r.y1, focused ? kGreen : kDimGreen);
        const int y = r.y0 + kTextDrop;
        const int tint = focused ? kGreen : kDimGreen;

        std::string label, value;
        if (row < kActionRows) {
            const int action = action_of(row);
            label = tr(kPadActions[action].label);
            const int button = pad.button_of(action);
            value = waiting_ && focused ? tr("PRESS A KEY...")
                    : button < 0           ? tr("NONE")
                                           : tr(kPadButtonLabels[button]);
        } else if (row == kAimRow) {
            label = tr("AIM SPEED");
            value = "< " + std::to_string(pad.aim) + " / " + std::to_string(PadSettings::kAimLevels) + " >";
        } else if (row == kDeadZoneRow) {
            label = tr("DEAD ZONE");
            value = "< " + std::to_string(pad.dead_zone + 1) + " / " + std::to_string(PadSettings::kDeadZoneLevels) + " >";
        } else {
            label = row == kDefaultRow ? tr("DEFAULTS") : tr("BACK");
        }
        print(screen, kLabelX, y, label, tint);
        if (!value.empty()) print(screen, kValueX0 + 8, y, value, waiting_ && focused ? kGreen : -1);
    }

    // A one-line help under the rows, in the dim green of the labels.
    const std::string hint = waiting_ ? tr("PRESS A BUTTON OR PUSH A STICK   ESC: CANCEL")
                                      : tr("CLICK: ASSIGN A BUTTON   DEL: CLEAR");
    TextWriter hint_writer(*fonts_, kHintFont);
    constexpr int kPad = 4, kAbove = 32, kHeight = 48;
    const int width = hint_writer.measure(hint).width;
    Surface8 scratch(width + 2 * kPad, kHeight);
    hint_writer.print(scratch, kPad, kAbove, hint);
    const int x0 = (screen.width - width) / 2;
    for (int sy = 0; sy < kHeight; ++sy) {
        for (int sx = 0; sx < scratch.width; ++sx) {
            const int dx = x0 - kPad + sx, dy = kHintY - kAbove + sy;
            if (scratch.row(sy)[sx] && dx >= 0 && dy >= 0 && dx < screen.width && dy < screen.height) {
                screen.row(dy)[dx] = kDimGreen;
            }
        }
    }
}

void GamepadScreen::change(int row, int delta) {
    PadSettings& p = config_->pad;
    if (row == kAimRow) p.aim = std::clamp(p.aim + delta, 1, PadSettings::kAimLevels);
    else if (row == kDeadZoneRow) p.dead_zone = std::clamp(p.dead_zone + delta, 0, PadSettings::kDeadZoneLevels - 1);
}

// @orig none: port-only gamepad screen

void GamepadScreen::activate(int row) {
    if (row < kActionRows) {
        waiting_ = true;
        wait_start_ = now_;
    } else if (row == kDefaultRow) {
        config_->pad = PadSettings{};
    } else if (row == kBackRow) {
        closed_ = true;
    } else {
        change(row, 1);
    }
}

void GamepadScreen::pad_button(int button) {
    if (!waiting_ || button < 0 || button >= kPadButtonCount) return;
    waiting_ = false;
    config_->pad.assign(button, action_of(*focus_));
}

void GamepadScreen::update(std::uint32_t ticks) {
    now_ = ticks;
    if (waiting_ && now_ - wait_start_ >= kWaitTicks) waiting_ = false;
}

// @orig none: port-only gamepad screen

void GamepadScreen::key(std::uint16_t code) {
    if (closed_) return;
    if (waiting_) {  // only Escape: the buttons come through pad_button()
        if (code == gump_key::kEscape) waiting_ = false;
        return;
    }
    int& focus = *focus_;
    switch (code) {
    case gump_key::kEscape: closed_ = true; break;
    case gump_key::kEnter: activate(focus); break;
    case kDelete:
        if (focus < kActionRows) config_->pad.unbind(action_of(focus));
        break;
    case gump_key::kUp:
    case '8': focus = std::max(focus - 1, 0); break;
    case gump_key::kDown:
    case '2': focus = std::min(focus + 1, kRows - 1); break;
    case gump_key::kLeft:
    case '4': change(focus, -1); break;
    case gump_key::kRight:
    case '6': change(focus, 1); break;
    default: break;
    }
}

// @orig none: port-only gamepad screen

void GamepadScreen::mouse_down(int x, int y) {
    if (closed_) return;
    waiting_ = false;  // any click cancels the wait
    for (int row = 0; row < kRows; ++row) {
        if (!row_rect(row).contains(x, y)) continue;
        *focus_ = row;
        if (row == kAimRow || row == kDeadZoneRow) {
            if (value_rect(row).contains(x, y)) {
                const Rect v = value_rect(row);
                change(row, x < (v.x0 + v.x1) / 2 ? -1 : 1);
            }
        } else if (row >= kDefaultRow) {
            activate(row);
        } else if (value_rect(row).contains(x, y)) {
            activate(row);
        }
        return;
    }
}

}  // namespace crusader
