#include "engine/ui/main_menu.hpp"

#include "core/language.hpp"
#include "engine/ui/christmas_hat.hpp"
#include "engine/ui/enhancements_screen.hpp"
#include "engine/ui/gump_keys.hpp"

#include <cctype>
#include <cstdio>

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
// The German one "options", "credits" and "quit" for its German shapes
// (13D0:02BD, 030E, 035F).
constexpr std::array<Point, MainMenu::kButtons> kButtonCornersDe = {{
    {46, 170}, {46, 221}, {46, 271}, {423, 178}, {456, 271}, {551, 318},
}};
constexpr int kVersionFont = 7, kVersionColour = 0x75;
constexpr int kVersionY0 = 438, kVersionLineHeight = 16;
constexpr std::array<Point, 4> kBackgroundCorners = {{{1, 120}, {320, 120}, {1, 240}, {320, 240}}};
// Port addition: the online part (font of the enhancements window).
constexpr int kOnlineFont = 14, kOnlineTextDrop = 17;
constexpr int kNewsButtonX1 = 597, kNewsButtonY0 = 44, kNewsButtonHeight = 24, kNewsButtonPad = 18;
constexpr int kBadgeRadius = 10;
constexpr int kUpdateLineY = 406;  // pen position

// "UPDATE TO x.y.z", translated: an invitation rather than a notice.
std::string update_text(const std::string& version) {
    char text[160];
    std::snprintf(text, sizeof text, tr("UPDATE TO %s"), version.c_str());
    return text;
}

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
        const Point p = (us_version() ? kButtonCornersUs : german_version() ? kButtonCornersDe : kButtonCorners)[i];
        buttons_[i] = {p.x, p.y, p.x + w - 1, p.y + h - 1};
    }
    const int x = kNoLimitCentreX - no_limit_label_.width() / 2;
    no_limit_button_ = {x, kNoLimitTop, x + no_limit_label_.width() - 1, kNoLimitTop + no_limit_label_.height() - 1};
    if (res_.online && res_.fonts && res_.online->ask()) question_ = std::make_unique<UpdatesQuestion>(*res_.fonts);
    if (*selected_ == kNews && !has_news()) select(kOptions);
}

// @orig none: port's online news. Measured at each call: the language can
// change while the menu is open (enhancements window).
MainMenu::Rect MainMenu::news_button() const {
    if (!res_.fonts) return {};
    const int width = TextWriter(*res_.fonts, kOnlineFont).measure(tr("NEWS")).width + 2 * kNewsButtonPad;
    return {kNewsButtonX1 - width + 1, kNewsButtonY0, kNewsButtonX1, kNewsButtonY0 + kNewsButtonHeight - 1};
}

// @orig none: the buttons' hit test of 1310:001D, with the port's button
int MainMenu::button_at(int x, int y) const {
    for (int i = 0; i < kButtons; ++i) {
        if (buttons_[i].contains(x, y)) return i;
    }
    if (res_.no_limit && no_limit_button_.contains(x, y)) return kNoLimit;
    if (has_news() && news_button().contains(x, y)) return kNews;
    return -1;
}

// @orig none: port's online news
MainMenu::Rect MainMenu::update_line() const {
    if (!res_.online || !res_.fonts || question_) return {-1, -1, -2, -2};
    const std::string version = res_.online->new_version();
    if (version.empty()) return {-1, -1, -2, -2};
    const int width = TextWriter(*res_.fonts, kOnlineFont).measure(update_text(version)).width;
    const int x0 = (640 - width) / 2;
    return {x0, kUpdateLineY - kOnlineTextDrop, x0 + width - 1, kUpdateLineY + 6};
}

// @orig none: port's online news
void MainMenu::draw_online(Surface8& screen) const {
    using namespace menu_colour;
    if (const Rect line = update_line(); line.x1 >= line.x0) {
        print_tinted(screen, *res_.fonts, kOnlineFont, line.x0, kUpdateLineY,
                     update_text(res_.online->new_version()), kGreen);
    }
    if (!has_news()) return;
    const Rect r = news_button();
    const std::uint8_t tint = *selected_ == kNews ? kGreen : kDimGreen;
    draw_rounded_box(screen, r.x0, r.y0, r.x1, r.y1, -1, tint);
    print_tinted(screen, *res_.fonts, kOnlineFont, r.x0 + kNewsButtonPad, r.y0 + kOnlineTextDrop, tr("NEWS"), tint);
    // The news not read yet: their count on a red disc, on the top right
    // corner.
    const int unread = count_unread(res_.online->news(), res_.online->news_read());
    if (unread <= 0) return;
    const int cx = r.x1 - 2, cy = r.y0 + 1;
    for (int dy = -kBadgeRadius; dy <= kBadgeRadius; ++dy) {
        for (int dx = -kBadgeRadius; dx <= kBadgeRadius; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (dx * dx + dy * dy <= kBadgeRadius * kBadgeRadius + kBadgeRadius / 2 && x >= 0 && y >= 0 &&
                x < screen.width && y < screen.height) {
                screen.row(y)[x] = kRed;
            }
        }
    }
    const std::string count = unread > 9 ? "9+" : std::to_string(unread);
    const int width = TextWriter(*res_.fonts, kOnlineFont).measure(count).width;
    print_tinted(screen, *res_.fonts, kOnlineFont, cx - width / 2, cy + 6, count, kWhite);
}

// @orig none: port's online news; the keys of the news button
bool MainMenu::news_key(std::uint16_t code) {
    if (!has_news()) return false;
    const int sel = *selected_;
    const bool up = code == gump_key::kUp || code == '8';
    const bool down = code == gump_key::kDown || code == '2';
    if (sel == kOptions && up) {
        select(kNews);
        return true;
    }
    if (sel != kNews) return false;
    if (down) select(kOptions);
    else if (code == gump_key::kEnter) activate(kNews);
    else if (code == gump_key::kEscape) activate(kBack);
    return true;
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
    if (news_) {  // port addition
        news_->draw(screen);
        return;
    }
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
    if (res_.christmas_hat) draw_christmas_hat(screen, false);
    for (int i = 0; i < kButtons; ++i) {
        const Shape* s = gumps_->get(static_cast<std::uint16_t>(kFirstButton + i));
        const std::size_t frame = i == *selected_ ? 1 : 0;
        if (s && frame < s->frames().size()) s->frames()[frame].draw(screen, buttons_[i].x0, buttons_[i].y0);
    }
    if (res_.no_limit) {
        no_limit_label_.draw(screen, no_limit_button_.x0, no_limit_button_.y0, *selected_ == kNoLimit);
    }
    draw_version(screen, kVersionY0, res_.port_version);
    draw_version(screen, kVersionY0 + kVersionLineHeight, res_.data_version);
    if (res_.online) draw_online(screen);  // port addition
    if (question_) question_->draw(screen);
    if (update_) update_->draw(screen);
}

// @orig 13D0:06CD
void MainMenu::key(std::uint16_t code) {
    if (result_) return;
    // Port addition: the online question, the news.
    if (question_) {
        question_->key(code);
        if (const auto answer = question_->result()) {
            question_.reset();
            res_.online->answer(*answer);
        }
        return;
    }
    if (update_) {
        update_->key(code);
        if (update_->closed()) update_.reset();
        return;
    }
    if (news_) {
        news_->key(code);
        if (news_->closed()) {
            news_.reset();
            res_.online->mark_news_read();
        }
        return;
    }
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
    if (news_key(code)) return;
    const int sel = *selected_;
    // Port addition: the moves to and from "CRUSADER / NO LIMIT", in the
    // right column between "options" (3) and "credits" (4), beside "load
    // game" (1).
    if (res_.no_limit) {
        int to = -1;
        switch (code) {
        case gump_key::kUp:
        case '8':
            to = sel == kNoLimit ? kOptions : sel == kCredits ? kNoLimit : -1;
            break;
        case gump_key::kDown:
        case '2':
            to = sel == kNoLimit ? kCredits : sel == kOptions ? kNoLimit : -1;
            break;
        case gump_key::kLeft:
        case '4':
            to = sel == kNoLimit ? kLoad : -1;
            break;
        case gump_key::kRight:
        case '6':
            to = sel == kLoad || sel == kNoLimit ? kNoLimit : -1;  // nothing right of it
            break;
        default:
            break;
        }
        if (to >= 0) {
            select(to);
            return;
        }
    }
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
    // Port addition: the online question, the news, the update line.
    if (question_) {
        question_->mouse_down(x, y);
        if (const auto answer = question_->result()) {
            question_.reset();
            res_.online->answer(*answer);
        }
        return;
    }
    if (update_) {
        update_->mouse_down(x, y);
        if (update_->closed()) update_.reset();
        return;
    }
    if (news_) {
        news_->mouse_down(x, y);
        if (news_->closed()) {
            news_.reset();
            res_.online->mark_news_read();
        }
        return;
    }
    if (!load_ && !options_ && !difficulty_ && !result_ && update_line().contains(x, y)) {
        // Installed by the game itself when it can, and no game is running
        // (the program restarts); else the release's page.
        if (no_game_ && res_.online->can_install()) {
            update_ = std::make_unique<UpdateDialog>(*res_.fonts, *res_.online,
                                                     update_text(res_.online->new_version()));
        } else {
            res_.online->open_release();
        }
        return;
    }
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
    if (const int i = button_at(x, y); i >= 0) {
        captured_ = i;
        select(i);
    }
}

// @orig 1310:0151
void MainMenu::mouse_move(int x, int y) {
    if (question_ || news_ || update_) return;  // port addition
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
    if (captured_ >= 0 && button(captured_).contains(x, y)) select(captured_);
}

// @orig 1310:001D
void MainMenu::mouse_up(int x, int y) {
    if (question_ || news_ || update_) return;  // port addition
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
    if (button(i).contains(x, y)) activate(i);
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
    case kNoLimit:  // port addition
        difficulty_ = std::make_unique<DifficultyScreen>(*gumps_, shared_->difficulty);
        difficulty_for_no_limit_ = i == kNoLimit;
        break;
    case kNews:  // port addition
        if (has_news()) news_ = std::make_unique<NewsScreen>(*res_.fonts, res_.online->news(), res_.online->news_read());
        break;
    default:
        break;
    }
}

}  // namespace crusader

namespace crusader {

void MainMenu::update(std::uint32_t ticks) {
    if (update_) {  // port addition: follows the install
        update_->update();
        if (update_->closed()) update_.reset();
    }
    if (options_) options_->update(ticks);
}

// @orig 13D0:082E
void MainMenu::after_sub_screen() {
    if (difficulty_ && difficulty_->result()) {
        if (*difficulty_->result() != 0) {
            new_game_difficulty_ = *difficulty_->result();
            result_ = difficulty_for_no_limit_ ? kNoLimit : kNewGame;
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
