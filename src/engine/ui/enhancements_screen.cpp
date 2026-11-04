#include "engine/ui/enhancements_screen.hpp"

#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>

namespace crusader {

namespace {

constexpr int kPanelX0 = 100, kPanelY0 = 84, kPanelX1 = 539, kPanelY1 = 426;
constexpr int kTitleY = 120, kIconSize = 14, kIconGap = 8;
constexpr int kRowX0 = 130, kRowX1 = 509, kRowY0 = 158, kRowStep = 30, kRowHeight = 24;
constexpr int kTextDrop = 17, kRowPad = 18;  // glyphs hang from their baseline
constexpr std::uint8_t kFrameColour = 0x57, kGreen = 0x0C, kDimGreen = 0x75;

// Text in one colour: drawn on a scratch surface, copied in `tint`.
// @orig none: screen of the port's enhancements
void print_tinted(Surface8& screen, const FontSet& fonts, int x, int y, const std::string& text, int tint) {
    TextWriter writer(fonts, EnhancementsScreen::kFont);
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

}  // namespace

// @orig none: port-only enhancements window
void draw_rounded_box(Surface8& screen, int x0, int y0, int x1, int y1, int fill, std::uint8_t colour) {
    // Corners cut over three pixels, like the frames of the original buttons.
    constexpr int kInset[3] = {3, 2, 1};
    for (int y = y0; y <= y1; ++y) {
        const int d = std::min(y - y0, y1 - y);
        const int in = d < 3 ? kInset[d] : 0;
        if (fill >= 0) fill_rect(screen, x0 + in, y, x1 - in, y, static_cast<std::uint8_t>(fill));
        if (d == 0) {
            hline(screen, x0 + in, y, x1 - in, colour);
        } else {
            outline(screen, x0 + in, y, x0 + in, y, colour);
            outline(screen, x1 - in, y, x1 - in, y, colour);
        }
    }
}

// @orig none: port-only enhancements window
void draw_plus_icon(Surface8& screen, int x, int y, int size, std::uint8_t colour) {
    outline(screen, x, y, x + size - 1, y + size - 1, colour);
    const int mid = x + size / 2, midy = y + size / 2;
    const int arm = size / 2 - 2;
    fill_rect(screen, mid - arm, midy - 1, mid + arm, midy, colour);
    fill_rect(screen, mid - 1, midy - arm, mid, midy + arm, colour);
}

EnhancementsScreen::EnhancementsScreen(const FontSet& fonts, GameConfig& config, int& pad_focus)
    : fonts_(&fonts), config_(&config), pad_focus_(&pad_focus) {}

EnhancementsScreen::Rect EnhancementsScreen::row_rect(int row) const {
    const int y = kRowY0 + kRowStep * row;
    return {kRowX0, y, kRowX1, y + kRowHeight - 1};
}

// @orig none: port-only enhancements window
void EnhancementsScreen::draw(Surface8& screen) const {
    if (pad_) {
        pad_->draw(screen);
        return;
    }
    screen.clear(0);
    draw_rounded_box(screen, kPanelX0, kPanelY0, kPanelX1, kPanelY1, -1, kFrameColour);
    draw_rounded_box(screen, kPanelX0 + 3, kPanelY0 + 3, kPanelX1 - 3, kPanelY1 - 3, -1, kFrameColour);

    const std::string title = tr("AMELIORATIONS", "ENHANCEMENTS");
    const int title_width = TextWriter(*fonts_, kFont).measure(title).width;
    const int x0 = (screen.width - (kIconSize + kIconGap + title_width)) / 2;
    draw_plus_icon(screen, x0, kTitleY - 12, kIconSize, kGreen);
    print_tinted(screen, *fonts_, x0 + kIconSize + kIconGap, kTitleY, title, kGreen);

    for (int row = 0; row < kRows; ++row) {
        const Rect r = row_rect(row);
        const bool focused = row == focus_;
        const int tint = focused ? kGreen : kDimGreen;
        draw_rounded_box(screen, r.x0, r.y0, r.x1, r.y1, -1, static_cast<std::uint8_t>(tint));
        const TextWriter writer(*fonts_, kFont);
        if (row == kFollowRow || row == kDirectionalRow || row == kAimRow || row == kNativeRow || row == kZoomRow ||
            row == kAutosaveRow || row == kRunRow) {
            // The name on the left, its state on the right.
            std::string name;
            bool on = false;
            std::string value;
            if (row == kFollowRow) {
                name = tr("SUIVI CAMERA", "CAMERA FOLLOW");
                on = config_->follow_player;
            } else if (row == kDirectionalRow) {
                name = tr("MODE DIRECTIONNEL", "DIRECTIONAL MOVE");
                on = config_->directional_move;
            } else if (row == kZoomRow) {
                name = tr("ZOOM", "ZOOM");
                value = config_->view_zoom ? "x" + std::to_string(config_->view_zoom) : tr("AUTO", "AUTO");
            } else if (row == kAutosaveRow) {
                name = tr("SAUVEGARDE AUTO", "AUTOSAVE");
                on = config_->autosave;
            } else if (row == kRunRow) {
                name = tr("COURSE PERMANENTE", "TOGGLE RUN");
                on = config_->toggle_run;
            } else if (row == kNativeRow) {
                name = tr("RESOLUTION NATIVE", "NATIVE RESOLUTION");
                on = config_->native_resolution;
            } else {
                name = tr("VISEE DIRECTIONNELLE", "DIRECTIONAL AIM");
                on = config_->directional_aim;
            }
            if (row != kZoomRow) value = on ? tr("MARCHE", "ON") : tr("ARRET", "OFF");
            print_tinted(screen, *fonts_, r.x0 + kRowPad, r.y0 + kTextDrop, name, tint);
            print_tinted(screen, *fonts_, r.x1 + 1 - kRowPad - writer.measure(value).width, r.y0 + kTextDrop, value,
                         tint);
            continue;
        }
        const std::string label = row == kPadRow ? tr("MANETTE", "CONTROLLER") : tr("RETOUR", "BACK");
        const int width = writer.measure(label).width;
        print_tinted(screen, *fonts_, r.x0 + (r.x1 - r.x0 + 1 - width) / 2, r.y0 + kTextDrop, label, tint);
    }
}

// @orig none: port-only enhancements window
void EnhancementsScreen::activate(int row) {
    focus_ = row;
    if (row == kPadRow) {
        pad_ = std::make_unique<GamepadScreen>(*fonts_, *config_, *pad_focus_);
    } else if (row == kFollowRow) {
        config_->follow_player = !config_->follow_player;
    } else if (row == kDirectionalRow) {
        config_->directional_move = !config_->directional_move;
    } else if (row == kZoomRow) {
        config_->view_zoom = (config_->view_zoom + 1) % (std::clamp(config_->view_zoom_limit, 1, 4) + 1);
    } else if (row == kAutosaveRow) {
        config_->autosave = !config_->autosave;
    } else if (row == kRunRow) {
        config_->toggle_run = !config_->toggle_run;
    } else if (row == kNativeRow) {
        config_->native_resolution = !config_->native_resolution;
    } else if (row == kAimRow) {
        config_->directional_aim = !config_->directional_aim;
    } else {
        closed_ = true;
    }
}

// @orig none: port-only enhancements window
void EnhancementsScreen::key(std::uint16_t code) {
    if (pad_) {
        pad_->key(code);
        if (pad_->closed()) pad_.reset();
        return;
    }
    if (closed_) return;
    switch (code) {
    case gump_key::kEscape: closed_ = true; break;
    case gump_key::kEnter: activate(focus_); break;
    case gump_key::kUp:
    case '8': focus_ = std::max(focus_ - 1, 0); break;
    case gump_key::kDown:
    case '2': focus_ = std::min(focus_ + 1, kRows - 1); break;
    default: break;
    }
}

// @orig none: port-only enhancements window
void EnhancementsScreen::mouse_down(int x, int y) {
    if (pad_) {
        pad_->mouse_down(x, y);
        if (pad_->closed()) pad_.reset();
        return;
    }
    if (closed_) return;
    for (int row = 0; row < kRows; ++row) {
        if (row_rect(row).contains(x, y)) {
            activate(row);
            return;
        }
    }
}

void EnhancementsScreen::update(std::uint32_t ticks) {
    if (pad_) pad_->update(ticks);
}

}  // namespace crusader
