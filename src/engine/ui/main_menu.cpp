#include "engine/ui/main_menu.hpp"

#include "core/language.hpp"
#include "engine/ui/gump_keys.hpp"

#include <cctype>

namespace crusader {

namespace {

struct Point {
    int x, y;
};
constexpr std::array<Point, MainMenu::kButtons> kButtonCorners = {{
    {46, 170}, {46, 221}, {46, 271}, {447, 178}, {469, 271}, {517, 318},
}};
// The US executable places "credits" and "quit" for its English shapes.
constexpr std::array<Point, MainMenu::kButtons> kButtonCornersUs = {{
    {46, 170}, {46, 221}, {46, 271}, {447, 178}, {490, 271}, {551, 318},
}};
constexpr int kVersionFont = 7, kVersionColour = 0x75;
constexpr int kVersionY0 = 438, kVersionLineHeight = 16;
constexpr std::array<Point, 4> kBackgroundCorners = {{{1, 120}, {320, 120}, {1, 240}, {320, 240}}};

}  // namespace

// @orig 13D0:0000
MainMenu::MainMenu(const Resources& res, Shared& shared, bool no_game)
    : res_(res),
      gumps_(res.gumps),
      shared_(&shared),
      selected_(&shared.selected),
      no_game_(no_game) {
    for (int i = 0; i < kButtons; ++i) {
        const Shape* s = gumps_->get(static_cast<std::uint16_t>(kFirstButton + i));
        const int w = s && !s->frames().empty() ? s->frames()[0].width : 1;
        const int h = s && !s->frames().empty() ? s->frames()[0].height : 1;
        const Point p = (us_version() ? kButtonCornersUs : kButtonCorners)[i];
        buttons_[i] = {p.x, p.y, p.x + w - 1, p.y + h - 1};
    }
}

// @orig none: version lines of the port. The font has its own colours, so it
// is drawn on a scratch surface and copied in one colour.
void MainMenu::draw_version(Surface8& screen, int y, const std::string& text) const {
    if (text.empty() || !res_.fonts) return;
    // The font has capitals only, and no hyphen ("0.1.0-dev" reads "0.1.0 DEV").
    std::string upper = text;
    for (char& c : upper) c = c == '-' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    TextWriter writer(*res_.fonts, kVersionFont);
    constexpr int kPad = 4, kAbove = 32, kHeight = 48;
    const int width = writer.measure(upper).width;
    Surface8 scratch(width + 2 * kPad, kHeight);
    writer.print(scratch, kPad, kAbove, upper);
    const int x = (screen.width - width) / 2;
    for (int sy = 0; sy < kHeight; ++sy) {
        for (int sx = 0; sx < scratch.width; ++sx) {
            const int dx = x - kPad + sx, dy = y - kAbove + sy;
            if (scratch.row(sy)[sx] && dx >= 0 && dy >= 0 && dx < screen.width && dy < screen.height) {
                screen.row(dy)[dx] = kVersionColour;
            }
        }
    }
}

// @orig 13D0:04EE 13D0:2C6B
void MainMenu::draw(Surface8& screen) const {
    if (credits_) {
        screen.pixels = credits_->screen().pixels;
        return;
    }
    if (load_) {
        load_->draw(screen);
        return;
    }
    if (options_) {
        options_->draw(screen);
        return;
    }
    if (difficulty_) {
        difficulty_->draw(screen);
        return;
    }
    screen.clear(0);
    for (int q = 0; q < 4; ++q) {
        if (const Shape* s = gumps_->get(static_cast<std::uint16_t>(kFirstBackground + q)); s && !s->frames().empty()) {
            s->frames()[0].draw(screen, kBackgroundCorners[q].x, kBackgroundCorners[q].y);
        }
    }
    for (int i = 0; i < kButtons; ++i) {
        const Shape* s = gumps_->get(static_cast<std::uint16_t>(kFirstButton + i));
        const std::size_t frame = i == *selected_ ? 1 : 0;
        if (s && frame < s->frames().size()) s->frames()[frame].draw(screen, buttons_[i].x0, buttons_[i].y0);
    }
    draw_version(screen, kVersionY0, res_.port_version);
    draw_version(screen, kVersionY0 + kVersionLineHeight, res_.data_version);
}

// @orig 13D0:06CD
void MainMenu::key(std::uint16_t code) {
    if (result_) return;
    if (credits_) {  // any key stops them (DS:322A)
        credits_->key_pressed();
        return;
    }
    if (load_) {
        load_->key(code);
        after_sub_screen();
        return;
    }
    if (options_) {
        options_->key(code);
        after_sub_screen();
        return;
    }
    if (difficulty_) {
        difficulty_->key(code);
        after_sub_screen();
        return;
    }
    const int sel = *selected_;
    switch (code) {
    case gump_key::kUp:
    case '8':
        if (sel != 0) select(sel - 1);
        break;
    case gump_key::kDown:
    case '2':
        if (sel < 5) select(sel + 1);
        break;
    case gump_key::kLeft:
    case '4':
        if (sel >= 3) select(sel - 3);
        break;
    case gump_key::kRight:
    case '6':
        if (sel < 3) select(sel + 3);
        break;
    case gump_key::kEnter:
        activate(sel);
        break;
    case gump_key::kEscape:
        activate(kBack);
        break;
    default:
        break;
    }
}

// @orig 1310:001D
void MainMenu::mouse_down(int x, int y) {
    if (credits_) return;  // the mouse driver is shut down
    if (load_) {
        load_->mouse_down(x, y);
        after_sub_screen();
        return;
    }
    if (options_) {
        options_->mouse_down(x, y);
        after_sub_screen();
        return;
    }
    if (difficulty_) {
        difficulty_->mouse_down(x, y);
        return;
    }
    if (result_ || captured_ >= 0) return;
    for (int i = 0; i < kButtons; ++i) {
        if (buttons_[i].contains(x, y)) {
            captured_ = i;
            select(i);
            return;
        }
    }
}

// @orig 1310:0151
void MainMenu::mouse_move(int x, int y) {
    if (options_) {
        options_->mouse_move(x, y);
        return;
    }
    if (difficulty_) {
        difficulty_->mouse_move(x, y);
        return;
    }
    if (load_ || credits_) return;
    // Inside: selected again; outside: "deselected", which leaves the
    // selected button drawn selected (IRGUMP_13d0_2d2e).
    if (captured_ >= 0 && buttons_[captured_].contains(x, y)) select(captured_);
}

// @orig 1310:001D
void MainMenu::mouse_up(int x, int y) {
    if (options_) {
        options_->mouse_up(x, y);
        after_sub_screen();
        return;
    }
    if (difficulty_) {
        difficulty_->mouse_up(x, y);
        after_sub_screen();
        return;
    }
    if (load_ || credits_ || captured_ < 0) return;
    const int i = captured_;
    captured_ = -1;
    if (buttons_[i].contains(x, y)) activate(i);
}

// @orig 13D0:082E
void MainMenu::activate(int i) {
    switch (i) {
    case kQuit:
        result_ = i;
        break;
    case kBack:
        if (!no_game_) result_ = i;
        break;
    case kLoad:
        load_ = std::make_unique<LoadScreen>(*gumps_, *res_.fonts, *res_.save_names, shared_->selected_slot);
        break;
    case kSave:
        if (!no_game_) {
            load_ = std::make_unique<LoadScreen>(*gumps_, *res_.fonts, *res_.save_names, shared_->selected_slot,
                                                 LoadScreen::Mode::kSave);
            load_->set_auto_name(res_.auto_save_name);
        }
        break;
    case kCredits:
        credits_ = std::make_unique<Credits>(*res_.credits, *res_.fonts);
        break;
    case kOptions:
        options_ = std::make_unique<OptionsScreen>(*gumps_, *res_.config, res_.audio, shared_->options_focus, res_.fonts);
        break;
    case kNewGame:
        difficulty_ = std::make_unique<DifficultyScreen>(*gumps_, shared_->difficulty);
        break;
    default:
        break;
    }
}

}  // namespace crusader

namespace crusader {

void MainMenu::update(std::uint32_t ticks) {
    if (options_) options_->update(ticks);
}

// @orig 13D0:082E
void MainMenu::after_sub_screen() {
    if (difficulty_ && difficulty_->result()) {
        if (*difficulty_->result() != 0) {
            new_game_difficulty_ = *difficulty_->result();
            result_ = kNewGame;
        }
        difficulty_.reset();
    }
    if (options_ && options_->closed()) {
        options_.reset();
        if (res_.save_config) res_.save_config();
    }
    if (!load_ || !load_->result()) return;
    if (*load_->result() == 1) {
        load_slot_ = load_->slot();
        save_name_ = load_->name();
        result_ = load_->mode() == LoadScreen::Mode::kSave ? kSave : kLoad;
    }

    load_.reset();
}

}  // namespace crusader
