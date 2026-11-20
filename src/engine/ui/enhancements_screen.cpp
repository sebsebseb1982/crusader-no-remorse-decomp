#include "engine/ui/enhancements_screen.hpp"

#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>
#include <utility>

namespace crusader {

namespace {

// At most 9 rows a page.
constexpr int kPanelX0 = 100, kPanelY0 = 34, kPanelX1 = 539, kPanelY1 = 465;
constexpr int kTitleY = 66, kIconSize = 14, kIconGap = 8;
constexpr int kRowX0 = 130, kRowX1 = 509, kRowY0 = 90, kRowStep = 26, kRowHeight = 24;
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

namespace {

using Entry = EnhancementsScreen::Entry;
using Page = EnhancementsScreen::Page;

constexpr Entry kMainEntries[] = {Entry::Controls, Entry::Display, Entry::Effects,
                                  Entry::Game,     Entry::Cheats,  Entry::Back};
constexpr Entry kControlsEntries[] = {Entry::Pad,        Entry::Directional, Entry::Aim,
                                      Entry::Draw,       Entry::PreciseAim,  Entry::Run,
                                      Entry::Vibration,  Entry::VibrationIntensity, Entry::Back};
constexpr Entry kDisplayEntries[] = {Entry::Native,   Entry::Zoom,  Entry::Smooth,     Entry::Follow,
                                     Entry::Lighting, Entry::Smoke, Entry::Flashlight, Entry::Back};
constexpr Entry kEffectsEntries[] = {Entry::Impact, Entry::Blood, Entry::Casings, Entry::Back};
constexpr Entry kGameEntries[] = {Entry::Autosave, Entry::FixBugs, Entry::Language, Entry::Back};
constexpr Entry kCheatsEntries[] = {Entry::RemainsLoot, Entry::Back};

// The name of a row that opens something (a page, the controller screen,
// "retour"), shown in its middle.
// @orig none: port-only enhancements window
std::string button_label(Entry entry) {
    switch (entry) {
    case Entry::Controls: return tr("CONTROLS");
    case Entry::Display: return tr("DISPLAY");
    case Entry::Effects: return tr("EFFECTS");
    case Entry::Game: return tr("GAME");
    case Entry::Cheats: return tr("CHEATS");
    case Entry::Pad: return tr("CONTROLLER");
    case Entry::Back: return tr("BACK");
    default: return {};
    }
}

// The page a row of the first page opens.
// @orig none: port-only enhancements window
Page page_of(Entry entry) {
    switch (entry) {
    case Entry::Controls: return Page::Controls;
    case Entry::Display: return Page::Display;
    case Entry::Effects: return Page::Effects;
    case Entry::Game: return Page::Game;
    case Entry::Cheats: return Page::Cheats;
    default: return Page::Main;
    }
}

}  // namespace

EnhancementsScreen::EnhancementsScreen(const FontSet& fonts, GameConfig& config, int& pad_focus,
                                       std::function<void()> vibration_preview)
    : fonts_(&fonts), config_(&config), pad_focus_(&pad_focus), vibration_preview_(std::move(vibration_preview)) {}

// @orig none: port-only enhancements window
std::span<const EnhancementsScreen::Entry> EnhancementsScreen::entries() const {
    switch (page_) {
    case Page::Controls: return kControlsEntries;
    case Page::Display: return kDisplayEntries;
    case Page::Effects: return kEffectsEntries;
    case Page::Game: return kGameEntries;
    case Page::Cheats: return kCheatsEntries;
    default: return kMainEntries;
    }
}

// @orig none: port-only enhancements window
int EnhancementsScreen::row_of(Entry entry) const {
    const auto list = entries();
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i] == entry) return static_cast<int>(i);
    return -1;
}

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
    // The frame ends below the last row (kPanelY1 at the most).
    const int panel_y1 = std::min(kPanelY1, row_rect(static_cast<int>(entries().size()) - 1).y1 + 22);
    draw_rounded_box(screen, kPanelX0, kPanelY0, kPanelX1, panel_y1, -1, kFrameColour);
    draw_rounded_box(screen, kPanelX0 + 3, kPanelY0 + 3, kPanelX1 - 3, panel_y1 - 3, -1, kFrameColour);

    const std::string title = page_ == Page::Main ? tr("ENHANCEMENTS")
                              : page_ == Page::Controls ? button_label(Entry::Controls)
                              : page_ == Page::Display  ? button_label(Entry::Display)
                              : page_ == Page::Effects  ? button_label(Entry::Effects)
                              : page_ == Page::Game     ? button_label(Entry::Game)
                                                        : button_label(Entry::Cheats);
    const int title_width = TextWriter(*fonts_, kFont).measure(title).width;
    const int x0 = (screen.width - (kIconSize + kIconGap + title_width)) / 2;
    draw_plus_icon(screen, x0, kTitleY - 12, kIconSize, kGreen);
    print_tinted(screen, *fonts_, x0 + kIconSize + kIconGap, kTitleY, title, kGreen);

    const auto list = entries();
    for (int row = 0; row < static_cast<int>(list.size()); ++row) {
        const Entry entry = list[static_cast<std::size_t>(row)];
        const Rect r = row_rect(row);
        const bool focused = row == focus_;
        const int tint = focused ? kGreen : kDimGreen;
        draw_rounded_box(screen, r.x0, r.y0, r.x1, r.y1, -1, static_cast<std::uint8_t>(tint));
        const TextWriter writer(*fonts_, kFont);
        const std::string label = button_label(entry);
        if (!label.empty()) {
            const int width = writer.measure(label).width;
            print_tinted(screen, *fonts_, r.x0 + (r.x1 - r.x0 + 1 - width) / 2, r.y0 + kTextDrop, label, tint);
            continue;
        }
        // An option: its name on the left, its state on the right.
        std::string name;
        bool on = false;
        std::string value;
        switch (entry) {
        case Entry::Directional:
            name = tr("DIRECTIONAL MOVE");
            on = config_->directional_move;
            break;
        case Entry::Aim:
            name = tr("DIRECTIONAL AIM");
            on = config_->directional_aim;
            break;
        case Entry::Draw:
            name = tr("DRAW ON AIM");
            on = config_->draw_on_aim;
            break;
        case Entry::PreciseAim:
            name = tr("PRECISE AIM");
            on = config_->precise_aim;
            break;
        case Entry::Run:
            name = tr("TOGGLE RUN");
            on = config_->toggle_run;
            break;
        case Entry::Vibration:
            name = tr("VIBRATION");
            on = config_->vibration;
            break;
        case Entry::VibrationIntensity:
            name = tr("INTENSITY");
            value = std::to_string(config_->vibration_intensity) + "%";
            break;
        case Entry::Native:
            name = tr("NATIVE RESOLUTION");
            on = config_->native_resolution;
            break;
        case Entry::Zoom: {
            name = tr("ZOOM");
            const int pct = config_->view_zoom_pct;
            if (!pct) value = tr("AUTO");
            else value = "x" + std::to_string(pct / 100) + (pct % 100 == 50 ? ".5" : "");
            break;
        }
        case Entry::Smooth:
            name = tr("SMOOTHING");
            on = config_->smooth_scaling;
            break;
        case Entry::Follow:
            name = tr("CAMERA FOLLOW");
            on = config_->follow_player;
            break;
        case Entry::Lighting:
            name = tr("3D LIGHTING");
            on = config_->lighting;
            break;
        case Entry::Smoke:
            name = tr("VOLUMETRIC SMOKE");
            on = config_->volumetric_smoke;
            break;
        case Entry::Flashlight:
            name = tr("FLASHLIGHT");
            on = config_->flashlight;
            break;
        case Entry::Impact:
            name = tr("IMPACT MARKS");
            on = config_->impact_marks;
            break;
        case Entry::Blood:
            name = tr("BLOOD");
            if (config_->blood_marks && config_->gore_mode) value = tr("GORE");
            else on = config_->blood_marks;
            break;
        case Entry::Casings:
            name = tr("CASINGS");
            on = config_->casings;
            break;
        case Entry::Autosave:
            name = tr("AUTOSAVE");
            on = config_->autosave;
            break;
        case Entry::FixBugs:
            name = tr("ORIGINAL BUG FIXES");
            on = config_->fix_original_bugs;
            break;
        case Entry::Language:
            name = tr("LANGUAGE");
            value = selected_language().id.empty() ? tr("AUTO") : selected_language().name;
            break;
        case Entry::RemainsLoot:
            name = tr("LOOT FROM REMAINS");
            on = config_->remains_loot;
            break;
        default: break;
        }
        if (value.empty()) value = on ? tr("ON") : tr("OFF");
        print_tinted(screen, *fonts_, r.x0 + kRowPad, r.y0 + kTextDrop, name, tint);
        print_tinted(screen, *fonts_, r.x1 + 1 - kRowPad - writer.measure(value).width, r.y0 + kTextDrop, value, tint);
    }
    // The language's data files (pictures, scripts, subtitles...) are read at
    // start-up: under the window, a note when the one picked differs.
    if (page_ == Page::Game && language_needs_restart()) {
        const std::string note = tr("RESTART TO LOAD ITS GAME FILES");
        const int width = TextWriter(*fonts_, kFont).measure(note).width;
        print_tinted(screen, *fonts_, (screen.width - width) / 2, panel_y1 + 30, note, kDimGreen);
    }
}

// @orig none: port-only enhancements window
void EnhancementsScreen::open(Page page) {
    page_ = page;
    focus_ = 0;
}

// From a page, the first one with that page's row focused; from the first
// one, closed.
// @orig none: port-only enhancements window
void EnhancementsScreen::back() {
    if (page_ == Page::Main) {
        closed_ = true;
        return;
    }
    const Entry from = page_ == Page::Controls ? Entry::Controls
                       : page_ == Page::Display ? Entry::Display
                       : page_ == Page::Effects ? Entry::Effects
                       : page_ == Page::Game    ? Entry::Game
                                                : Entry::Cheats;
    page_ = Page::Main;
    focus_ = std::max(row_of(from), 0);
}

// @orig none: port-only enhancements window
void EnhancementsScreen::activate(int row) {
    const auto list = entries();
    if (row < 0 || row >= static_cast<int>(list.size())) return;
    focus_ = row;
    switch (list[static_cast<std::size_t>(row)]) {
    case Entry::Controls:
    case Entry::Display:
    case Entry::Effects:
    case Entry::Game:
    case Entry::Cheats: open(page_of(list[static_cast<std::size_t>(row)])); break;
    case Entry::Pad: pad_ = std::make_unique<GamepadScreen>(*fonts_, *config_, *pad_focus_); break;
    case Entry::Directional: config_->directional_move = !config_->directional_move; break;
    case Entry::Aim: config_->directional_aim = !config_->directional_aim; break;
    case Entry::Draw: config_->draw_on_aim = !config_->draw_on_aim; break;
    case Entry::PreciseAim: config_->precise_aim = !config_->precise_aim; break;
    case Entry::Run: config_->toggle_run = !config_->toggle_run; break;
    case Entry::Vibration: config_->vibration = !config_->vibration; break;
    case Entry::VibrationIntensity:
        config_->vibration_intensity = (config_->vibration_intensity + 10) % 110;
        if (vibration_preview_) vibration_preview_();
        break;
    case Entry::Native: config_->native_resolution = !config_->native_resolution; break;
    case Entry::Zoom:
        config_->view_zoom_pct = GameConfig::next_view_zoom(config_->view_zoom_pct, config_->view_zoom_limit_pct);
        break;
    case Entry::Smooth: config_->smooth_scaling = !config_->smooth_scaling; break;
    case Entry::Follow: config_->follow_player = !config_->follow_player; break;
    case Entry::Lighting: config_->lighting = !config_->lighting; break;
    case Entry::Smoke: config_->volumetric_smoke = !config_->volumetric_smoke; break;
    case Entry::Flashlight: config_->flashlight = !config_->flashlight; break;
    case Entry::Impact: config_->impact_marks = !config_->impact_marks; break;
    case Entry::Blood:
        // Off, on, gore, off.
        if (!config_->blood_marks) {
            config_->blood_marks = true;
            config_->gore_mode = false;
        } else if (!config_->gore_mode) {
            config_->gore_mode = true;
        } else {
            config_->blood_marks = false;
            config_->gore_mode = false;
        }
        break;
    case Entry::Casings: config_->casings = !config_->casings; break;
    case Entry::Autosave: config_->autosave = !config_->autosave; break;
    case Entry::RemainsLoot: config_->remains_loot = !config_->remains_loot; break;
    case Entry::FixBugs: config_->fix_original_bugs = !config_->fix_original_bugs; break;
    case Entry::Language: {
        // The next language offered, back to the data's after the last.
        const auto choices = language_choices();
        std::size_t i = 0;
        while (i < choices.size() && choices[i].id != selected_language().id) ++i;
        config_->language = choices[i + 1 < choices.size() ? i + 1 : 0].id;
        select_language(config_->language);
        break;
    }
    case Entry::Back: back(); break;
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
    const bool on_intensity = row_of(Entry::VibrationIntensity) == focus_;
    switch (code) {
    case gump_key::kEscape: back(); break;
    case gump_key::kEnter: activate(focus_); break;
    case gump_key::kLeft:
    case '4':
        if (on_intensity && config_->vibration_intensity > 0) {
            config_->vibration_intensity -= 10;
            if (vibration_preview_) vibration_preview_();
        }
        break;
    case gump_key::kRight:
    case '6':
        if (on_intensity && config_->vibration_intensity < 100) {
            config_->vibration_intensity += 10;
            if (vibration_preview_) vibration_preview_();
        }
        break;
    case gump_key::kUp:
    case '8': focus_ = std::max(focus_ - 1, 0); break;
    case gump_key::kDown:
    case '2': focus_ = std::min(focus_ + 1, static_cast<int>(entries().size()) - 1); break;
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
    for (int row = 0; row < static_cast<int>(entries().size()); ++row) {
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
