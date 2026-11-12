#include "engine/ui/load_screen.hpp"

#include "core/flex.hpp"
#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string_view>

namespace crusader {

namespace {

constexpr int kLeft = 111, kTop = 66;
constexpr int kQuarterW = 208, kQuarterH = 174;
constexpr int kSlotX = 183, kSlotY = 109, kSlotStep = 27;
constexpr std::size_t kNameEntry = 9;
constexpr std::size_t kNameSize = 32;

}  // namespace

// @orig 1058:000D
LoadScreen::Names LoadScreen::read_names(const std::filesystem::path& savegame_dir) {
    Names names;
    for (int i = 0; i < kNames; ++i) {
        const auto path = savegame_dir / ("SAVEGAME." + std::to_string(i + 1));
        if (!std::filesystem::exists(path)) continue;
        try {
            const auto flex = FlexArchive::open(path);
            if (flex.count() <= kNameEntry) continue;
            const auto e = flex.entry(kNameEntry);
            const std::size_t n = std::min(e.size(), kNameSize);
            std::string name(e.begin(), e.begin() + n);
            names[i] = name.substr(0, name.find('\0'));
        } catch (const std::exception&) {
            // unreadable: empty name, as for a missing file
        }
    }
    return names;
}

// @orig 1008:0088
LoadScreen::LoadScreen(ShapeCache& gumps, const FontSet& fonts, const Names& names, int& selected, Mode mode)
    : gumps_(&gumps), fonts_(&fonts), names_(&names), selected_(&selected), mode_(mode) {
    *selected_ = std::clamp(*selected_, 0, visible_slots() - 1);
    const Shape* s = gumps_->get(kSlotShape);
    const int w = s && !s->frames().empty() ? s->frames()[0].width : 1;
    const int h = s && !s->frames().empty() ? s->frames()[0].height : 1;
    for (int i = 0; i < kSlots; ++i) {
        const int y = kSlotY + kSlotStep * i;
        slots_[i] = {kSlotX, y, kSlotX + w - 1, y + h - 1};
    }
}

// @orig 1008:05A6 1008:0C9A
void LoadScreen::draw(Surface8& screen) const {
    screen.clear(0);
    for (int q = 0; q < 4; ++q) {
        const auto shape = static_cast<std::uint16_t>(64 + 5 * static_cast<int>(mode_) + q);
        if (const Shape* s = gumps_->get(shape); s && !s->frames().empty()) {
            s->frames()[0].draw(screen, kLeft + (q & 1) * kQuarterW, kTop + (q >> 1) * kQuarterH);
        }
    }
    const Shape* slot = gumps_->get(kSlotShape);
    for (int i = 0; i < visible_slots(); ++i) {
        const Rect& r = slots_[i];
        if (slot && !slot->frames().empty()) slot->frames()[0].draw(screen, r.x0, r.y0);
        if (i == *selected_) outline(screen, r.x0, r.y0, r.x1, r.y1, kSelectedColour);
        const bool edited = editing_ && i == *selected_;
        TextWriter(*fonts_, kFont).print(screen, r.x0 + 4, r.y0 + 12, edited ? name_ + '_' : (*names_)[i]);
    }
}

// @orig 1008:0312
void LoadScreen::key(std::uint16_t code) {
    if (result_) return;
    if (editing_) {
        edit_key(code);
        return;
    }
    const int sel = *selected_;
    switch (code) {
    case gump_key::kEscape:
        result_ = 0;
        break;
    case gump_key::kUp:
    case '8':
        if (sel != 0) *selected_ = sel - 1;
        break;
    case gump_key::kDown:
    case '2':
        if (sel < visible_slots() - 1) *selected_ = sel + 1;
        break;
    case gump_key::kEnter:
        choose(sel);
        break;
    default:
        break;
    }
}

// @orig 1008:0AEA 1008:0B63
void LoadScreen::mouse_down(int x, int y) {
    if (result_ || editing_) return;
    for (int i = 0; i < visible_slots(); ++i) {
        if (slots_[i].contains(x, y)) {
            *selected_ = i;
            choose(i);
            return;
        }
    }
}

// @orig 1008:0B63
void LoadScreen::choose(int i) {
    if (mode_ == Mode::kLoad) {
        if (!(*names_)[i].empty()) result_ = 1;
        return;
    }
    editing_ = true;
    name_ = auto_name_ ? auto_name_() : (*names_)[i];
}

// @orig none: port addition (save names without a keyboard)
std::string LoadScreen::dated_name(const std::tm& local) {
    if (local.tm_year + 1900 < 2020) return tr("SAUVEGARDE", "SAVED GAME");
    const int a = us_version() ? local.tm_mon + 1 : local.tm_mday;
    const int b = us_version() ? local.tm_mday : local.tm_mon + 1;
    char text[32];
    std::snprintf(text, sizeof text, "%02d/%02d/%04d %02d:%02d", a, b, local.tm_year + 1900, local.tm_hour,
                  local.tm_min);
    return text;
}

// @orig 1008:089C 1008:041A
void LoadScreen::edit_key(std::uint16_t code) {
    // Escape, Enter and Backspace are told by the code's low byte.
    const auto c = static_cast<std::uint8_t>(code);
    if (c == 0x1B) {
        editing_ = false;
        return;
    }
    if (c == 0x0D) {
        if (!name_.empty()) {
            editing_ = false;
            result_ = 1;
        }
        return;
    }
    if (c == 0x08) {
        if (!name_.empty()) name_.pop_back();
        return;
    }
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    // The US executable no longer takes < : and ; (13D8:09F3).
    const std::string_view punctuation = us_version() ? " .?!,/\"'" : "< :;.?!,/\"'";
    if (!(alnum || punctuation.find(static_cast<char>(c)) != std::string_view::npos)) return;
    if (name_.size() >= kMaxNameLength || code >= 0x100) return;
    name_ += static_cast<char>(c >= 'a' && c <= 'z' ? c - 0x20 : c);
    const Rect& r = slots_[*selected_];
    if (TextWriter(*fonts_, kFont).measure(name_ + '_').width > (r.x1 - r.x0 + 1) - 4) name_.pop_back();
}


}  // namespace crusader
