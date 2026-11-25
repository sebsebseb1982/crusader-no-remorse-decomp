#pragma once
// The main menu (IRGUMP_13d0_0000, segment 13D0), shown at the start of a game
// and by Escape during play (WORLD_13e8_10c3).
//
// Background (IRGUMP_13d0_04ee): the screen is cleared, then GUMPS.FLX shapes
// 54..57 are drawn at (1, 120), (320, 120), (1, 240), (320, 240), the corner of
// a 638 x 240 area centred on the 640 x 480 screen. Palette: STATIC/MISC.PAL.
//
// Six buttons (IRGUMP_13d0_2ba4), shape 58 + i, frame 1 when selected, 0
// otherwise, top-left corner at:
//   0 (46, 170)  new game        3 (447, 178)  options
//   1 (46, 221)  load game       4 (469, 271)  credits
//   2 (46, 271)  save game       5 (517, 318)  quit
// The selected button (DS:5BE8, 0 at start-up) outlives the menu.
//
// Keys (IRGUMP_13d0_06cd, gump key codes): up / '8' select the previous
// button, down / '2' the next (not past 0 or 5), left / '4' go back 3 from the
// right column, right / '6' forward 3 from the left column; Enter activates
// the selected button, Escape acts as button 6 (back to the game).
//
// Mouse (button base class, SEG1310): a button press inside a button selects
// it and captures the mouse; moving keeps it selected (a selected button is
// never drawn unselected); releasing inside it activates it. Moving without
// pressing selects nothing.
//
// Activation (IRGUMP_13d0_082e): 5 (quit) and 6 (back) close the menu, and
// the menu returns that number to its caller. While no game is running
// (DS:5BEE, set for the menu shown at start-up) 2 (save) and 6 do nothing.
// 1 (load) opens the load screen (load_screen.hpp): if a slot is chosen, the
// menu closes with 1 and load_slot() tells which; if cancelled, the menu is
// back. 4 (credits) runs the credits (credits.hpp), then the menu is back. 3
// opens the options screen (options_screen.hpp); once it closes, the
// configuration is saved (Resources::save_config) and the menu is back. 0
// (new game) opens the difficulty screen (difficulty_screen.hpp): a level
// chosen closes the menu with 0 and new_game_difficulty() tells which;
// cancelled, the menu is back. 2 (save) opens the load screen in save mode:
// a name given closes the menu with 2, load_slot() and save_name() tell
// which slot and name; cancelled, the menu is back.
//
// Port addition (@orig none): two small lines centred at the bottom of the
// menu, in the dim colour of the options buttons: the version of the port
// (from the build) and the version of the game data detected.
//
// Port addition (@orig none): from 24 to 31 December, a Christmas hat on the
// Silencer, drawn over the background (christmas_hat.hpp).
//
// Port addition (@orig none): with Resources::no_limit, a seventh button,
// "CRUSADER / NO LIMIT" (button 7), in the right column between "options"
// and "credits", drawn in the buttons' lettering (menu_lettering.hpp). Down
// from "options" and up from "credits" go through it, right from "load game"
// goes to it and left from it to "load game"; the other keys keep the
// original's moves. Like "new game", it opens the difficulty screen: a level
// chosen closes the menu with 7 and new_game_difficulty() tells which. The
// caller then starts a level of the procedural generator, Crusader: No
// Limit (docs/GENERATEUR.md). Without no_limit, the menu is the original's.
//
// Port addition (@orig none): with Resources::online (news_screen.hpp,
// GitHub issue #36):
//   - while the player has not said whether the game may go online, the
//     menu opens with the UpdatesQuestion over it, which gets every event
//     until answered (MenuOnline::answer);
//   - when a newer release of the port is known, a green line above the
//     version lines, "NEW VERSION x.y.z AVAILABLE": a press on it opens the
//     release's page (MenuOnline::open_release), or, when the game can install
//     that version itself and no game is running, the UpdateDialog
//     (MenuOnline::can_install, core/self_update.hpp);
//   - when there are news, a "NEWS" button (button 8) in the black band
//     above the background, on the right, with a red disc and the count of
//     the news not read yet on its corner when there are some. Up from
//     "options" goes to it, down from it back to "options"; the other keys
//     do nothing there but Enter (opens it) and Escape. It opens the
//     NewsScreen; once closed, the news are marked read
//     (MenuOnline::mark_news_read).

#include "core/surface.hpp"
#include "engine/ui/credits.hpp"
#include "engine/ui/difficulty_screen.hpp"
#include "engine/ui/load_screen.hpp"
#include "engine/ui/menu_lettering.hpp"
#include "engine/ui/news_screen.hpp"
#include "engine/ui/options_screen.hpp"
#include "engine/world/render.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace crusader {

class MainMenu {
public:
    static constexpr int kButtons = 6;
    static constexpr std::uint16_t kFirstBackground = 54;
    static constexpr std::uint16_t kFirstButton = 58;
    static constexpr int kNewGame = 0, kLoad = 1, kSave = 2, kOptions = 3, kCredits = 4, kQuit = 5, kBack = 6;
    static constexpr int kNoLimit = 7;  // port addition
    static constexpr int kNews = 8;     // port addition
    // The "CRUSADER / NO LIMIT" button: centred on x kNoLimitCentreX (between
    // the centres of "options" and "credits"), its top at kNoLimitTop.
    static constexpr int kNoLimitCentreX = 537, kNoLimitTop = 222;

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // State kept by the caller between menus: DS:5BE8 (selected button),
    // DS:0098 (selected save slot), DS:5BEA (options line) and DS:5BEC
    // (difficulty level); `no_game` is DS:5BEE.
    struct Shared {
        int selected = 0;
        int selected_slot = 0;
        int options_focus = 0;
        int difficulty = 3;
    };
    struct Resources {
        ShapeCache* gumps;
        const FontSet* fonts;
        const LoadScreen::Names* save_names;
        const CreditsData* credits;
        GameConfig* config;
        Audio* audio;                      // may be null
        std::function<void()> save_config;  // config_save, when the options close
        // The two lines at the bottom of the menu (port build, game data
        // detected); an empty line is not drawn.
        std::string port_version;
        std::string data_version;
        // Port addition: the save screen's proposed name (LoadScreen::set_auto_name),
        // empty for the original's.
        std::function<std::string()> auto_save_name;
        // Port addition: the Christmas hat on the background.
        bool christmas_hat = false;
        // Port addition: the "CRUSADER / NO LIMIT" button.
        bool no_limit = false;
        // Port addition: the online question, update notice and news; null
        // for none of them.
        MenuOnline* online = nullptr;
    };
    MainMenu(const Resources& res, Shared& shared, bool no_game);

    void draw(Surface8& screen) const;

    // Gump key code (see gump_keys.hpp).
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    void mouse_move(int x, int y);
    void mouse_up(int x, int y);
    // Timer ticks, at every pass of the modal loop.
    void update(std::uint32_t ticks);
    // The controller screen of the options waits for a button, and gets it.
    bool waiting_for_pad() const { return options_ && options_->waiting_for_pad(); }
    void pad_button(int button) {
        if (options_) options_->pad_button(button);
    }

    // Set once the menu has closed: kQuit, kBack, kLoad, kSave, kNewGame or
    // kNoLimit.
    std::optional<int> result() const { return result_; }
    int load_slot() const { return load_slot_; }  // DS:0C94, for kLoad and kSave
    const std::string& save_name() const { return save_name_; }  // for kSave
    int new_game_difficulty() const { return new_game_difficulty_; }  // DS:7E19, for kNewGame and kNoLimit
    const DifficultyScreen* difficulty_screen() const { return difficulty_.get(); }
    const LoadScreen* load_screen() const { return load_.get(); }
    const OptionsScreen* options() const { return options_.get(); }
    // The credits, while they run; the caller feeds frame() at 60 Hz and
    // calls end_credits() once finished().
    Credits* credits() { return credits_.get(); }
    // Game::I_playCredits (game.hpp): the credits started from outside, as
    // button 4 does.
    void show_credits() { activate(kCredits); }
    void end_credits() { credits_.reset(); }
    int selected() const { return *selected_; }
    // kNewGame..kQuit, and kNoLimit when the menu has it.
    const Rect& button(int i) const {
        return i == kNoLimit ? no_limit_button_ : i == kNews ? news_button_ : buttons_[i];
    }
    bool has_no_limit() const { return res_.no_limit; }
    // Port addition: the news button is shown (there are news).
    bool has_news() const { return res_.online && !res_.online->news().empty(); }
    const UpdatesQuestion* updates_question() const { return question_.get(); }
    const NewsScreen* news_screen() const { return news_.get(); }
    const UpdateDialog* update_dialog() const { return update_.get(); }
    // The "new version" line, empty when it is not shown.
    Rect update_line() const;

private:
    void select(int i) { *selected_ = i; }
    void activate(int i);

    void after_sub_screen();
    // The button under (x, y), -1 for none.
    int button_at(int x, int y) const;
    void draw_version(Surface8& screen, int y, const std::string& text) const;

    Resources res_;
    ShapeCache* gumps_;
    Shared* shared_;
    int* selected_;
    std::unique_ptr<LoadScreen> load_;
    std::unique_ptr<Credits> credits_;
    std::unique_ptr<OptionsScreen> options_;
    std::unique_ptr<DifficultyScreen> difficulty_;
    int load_slot_ = -1;
    std::string save_name_;

    int new_game_difficulty_ = 0;
    bool difficulty_for_no_limit_ = false;  // the difficulty screen opened by kNoLimit
    MenuLettering no_limit_label_{{"CRUSADER", "NO LIMIT"}};
    Rect no_limit_button_{};
    // Port addition: the online part.
    void draw_online(Surface8& screen) const;
    // A key for the news button or its neighbours; true when used.
    bool news_key(std::uint16_t code);
    std::unique_ptr<UpdatesQuestion> question_;
    std::unique_ptr<NewsScreen> news_;
    std::unique_ptr<UpdateDialog> update_;
    Rect news_button_{};
    bool no_game_;
    std::array<Rect, kButtons> buttons_{};
    int captured_ = -1;  // button holding the mouse
    std::optional<int> result_;
};

}  // namespace crusader
