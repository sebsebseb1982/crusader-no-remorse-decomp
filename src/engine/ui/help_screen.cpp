#include "engine/ui/help_screen.hpp"

#include "core/flex.hpp"
#include "core/language.hpp"
#include "engine/ui/credits.hpp"
#include "engine/ui/gump_keys.hpp"

#include <algorithm>
#include <exception>
#include <string>

namespace crusader {

namespace {

constexpr std::uint16_t kNextShape = 0x51, kQuitShape = 0x52;
// x of "next" and "quit": 13E8:38E5 (13F0:38D8 in the US executable).
constexpr int kNextX = 410, kQuitX = 516, kNextXUs = 493, kQuitXUs = 549, kButtonY = 449;
constexpr int kNextId = 0;
constexpr int kSounds[] = {0xB8, 0x62, 0x67, 0xA, 0xB};

}  // namespace

// @orig 13E8:3BA5
HelpScreen::Pictures HelpScreen::Pictures::load(const DataFiles& files) {
    Pictures p;
    for (int i = 0; i < 3; ++i) {
        try {
            const auto file = read_file(files.find("STATIC/HELP" + std::to_string(i + 1) + ".DAT"));
            p.pages[i] = Page{read_bmp8(file), read_bmp_palette(file)};
        } catch (const std::exception&) {
            // missing or unreadable: the page stays black
        }
    }
    return p;
}

HelpScreen::HelpScreen(ShapeCache& gumps, const Pictures& pictures, std::function<int(int)> random,
                       std::function<void(int)> play_sfx)
    : pictures_(&pictures), random_(std::move(random)), play_sfx_(std::move(play_sfx)) {
    buttons_.buttons.emplace_back(gumps, kNextShape, kNextId, us_version() ? kNextXUs : kNextX, kButtonY);
    buttons_.buttons.emplace_back(gumps, kQuitShape, 1, us_version() ? kQuitXUs : kQuitX, kButtonY);
    show();
}

// @orig 13E8:3BA5
bool HelpScreen::show() {
    if (page_ == -1) {
        page_ = 0;
        return true;
    }
    if (page_ == 2) {
        closed_ = true;
        return false;
    }
    if (page_ == 0) picture_ = 0;
    else if (page_ == 1) picture_ = 1;
    else if (page_ == kHiddenPage) picture_ = 2;
    return true;
}

const Palette* HelpScreen::palette() const {
    static const Palette black{};
    if (picture_ < 0 || !pictures_->pages[picture_]) return &black;
    return &pictures_->pages[picture_]->palette;
}

// @orig 13E8:3BA5
void HelpScreen::draw(Surface8& screen) const {
    screen.clear(0);
    if (picture_ >= 0 && pictures_->pages[picture_]) {
        const Surface8& pic = pictures_->pages[picture_]->picture;
        const int w = std::min(pic.width, screen.width);
        for (int y = 0; y < pic.height && y < screen.height; ++y) std::copy_n(pic.row(y), w, screen.row(y));
    }
    buttons_.draw(screen);
}

// @orig 13E8:3D99
void HelpScreen::key(std::uint16_t code) {
    if (closed_) return;
    if (page_ == kHiddenPage) page_ = 0;
    switch (code) {
    case gump_key::kEscape:
    case 'x': case 'X':
        closed_ = true;
        return;
    case gump_key::kLeft:
    case gump_key::kUp:
        --page_;
        show();
        sequence_ = 0;
        return;
    case gump_key::kRight:
    case gump_key::kDown:
    case 'n': case 'N':
        ++page_;
        if (show()) sequence_ = 0;
        return;
    case 'e': case 'E':
        sequence_ = 1;
        return;
    case '-':
        if (sequence_ != 0) {
            if (++sequence_ == 4) {
                page_ = kHiddenPage;
                show();
            }
            return;
        }
        break;
    default:
        break;
    }
    sequence_ = 0;
}

// @orig 13E8:3D53
void HelpScreen::mouse_up(int x, int y) {
    if (closed_) return;
    const int id = buttons_.mouse_up(x, y);
    if (id < 0) return;
    // 13E8:3D53.
    if (id == kNextId) {
        ++page_;
        show();
    } else {
        closed_ = true;
    }
}

// @orig 13E8:3EC8
void HelpScreen::update(std::uint32_t ticks) {
    if (!last_tick_) last_tick_ = ticks;
    for (; *last_tick_ < ticks; ++*last_tick_) {
        if (page_ == kHiddenPage && random_(8000) == 1) play_sfx_(kSounds[random_(5)]);
    }
}

}  // namespace crusader
