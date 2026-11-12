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

#include "core/surface.hpp"
#include "engine/ui/credits.hpp"
#include "engine/ui/difficulty_screen.hpp"
#include "engine/ui/load_screen.hpp"
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

    // Set once the menu has closed: kQuit, kBack, kLoad, kSave or kNewGame.
    std::optional<int> result() const { return result_; }
    int load_slot() const { return load_slot_; }  // DS:0C94, for kLoad and kSave
    const std::string& save_name() const { return save_name_; }  // for kSave
    int new_game_difficulty() const { return new_game_difficulty_; }  // DS:7E19, for kNewGame
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
    const Rect& button(int i) const { return buttons_[i]; }

private:
    void select(int i) { *selected_ = i; }
    void activate(int i);

    void after_sub_screen();
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
    bool no_game_;
    std::array<Rect, kButtons> buttons_{};
    int captured_ = -1;  // button holding the mouse
    std::optional<int> result_;
};

}  // namespace crusader
