#pragma once
// The running game, without platform code: data, world, process kernel and
// usecode machine (what CRUSYS.C sets up before entering the main loop).
//
// The original main loop (KERNEL.C 31CF, called from the entry point at
// 1048:0079 with a period of 0) runs kernel slices back to back; time comes
// from the 60 Hz system timer and the timed processes, and the display is a
// process too. Here the platform layer calls update() with the real elapsed
// time, then render(), once per presented frame.

#include "core/anim_dat.hpp"
#include "core/game_config.hpp"
#include "core/dtable.hpp"
#include "core/font.hpp"
#include "core/xform.hpp"
#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/surface.hpp"
#include "core/trig.hpp"
#include "core/typeflags.hpp"
#include "engine/audio/audio.hpp"
#include "engine/input/keyboard.hpp"
#include "engine/input/mouse_queue.hpp"
#include "engine/kernel.hpp"
#include "engine/ui/help_screen.hpp"
#include "engine/ui/main_menu.hpp"
#include "engine/ui/message_gump.hpp"
#include "engine/ui/modal_gump.hpp"
#include "engine/ui/movie_player.hpp"
#include "engine/ui/mouse_cursor.hpp"
#include "engine/ui/pickup_area.hpp"
#include "engine/ui/status_bar.hpp"
#include "engine/usecode/uc_machine.hpp"
#include "engine/usecode/usecode_file.hpp"
#include "engine/world/avatar_mover.hpp"
#include "engine/world/combat_control.hpp"
#include "core/weapon_overlay.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"
#include "engine/world/world_mouse.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace crusader {

class Game {
public:
    static constexpr int kScreenWidth = 640;
    static constexpr int kScreenHeight = 480;
    static constexpr int kQuickSave = 13;  // SAVEGAME.13, slot 12 (F4 / F5)
    static constexpr int kAutoSave = 12;   // SAVEGAME.12, port addition (F6): the autosave

    // `saves`: where the saved games are read and written, data/SAVEGAME
    // when empty (the tests use a copy of the original saves).
    explicit Game(const std::filesystem::path& data, const std::filesystem::path& saves = {});
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Loads SAVEGAME/SAVEGAME.<slot>, puts the camera on the player and shows
    // the status bar (end of GAMEPAL_1028_0000).
    void load_save(int slot);
    // Same, from any file (the tests keep saves under names telling their bug).
    void load_save_file(const std::filesystem::path& path);

    // Game_save (CRUSYS_1058_0115): writes SAVEGAME/SAVEGAME.<slot> under
    // the name of that slot (save_names(), entry 9). Only the entries the
    // port holds are written: 9, 90, 91, the item table and its lists (0,
    // 1), the NPC table (10), the player block (20), the tracked items (50
    // to 52), the usecode globals (60) and the music (70, 71). The sprite
    // manager (30 to 33), the YAMM memory (40, 41) and the kernel's processes
    // (100 and up) are not, so the original cannot load these saves yet.
    void save_game(int slot);
    // CRUSYS_1058_0528: the name of save slot `slot` (1-based), at most 31
    // characters; the save screen sets it before saving.
    void set_save_name(int slot, const std::string& name);
    const LoadScreen::Names& save_names() const { return save_names_; }
    // The end of the save screen (FUN_1008_041a): the name, the disk space
    // check, then save_game.
    void save_named(int slot, const std::string& name);
    // save_game, a failure shown in the message box; false then.
    bool save_or_report(int slot);

    // Port addition (GameConfig::autosave): saves the game in slot kAutoSave
    // and shows the floppy disk for a moment. Does nothing when switched off,
    // in a menu, a screen or a movie, or when the player is not the controlled
    // NPC. It happens when a keycard is picked up, a door opens with one or
    // with a code, a chest opens with a code (the keypad matched), a mission
    // starts and a movie ends; the last two wait until the player may act
    // again (request_autosave).
    void autosave();
    void request_autosave();
    // The floppy disk is on screen.
    bool autosave_icon_shown() const { return autosave_icon_us_ > 0; }

    // Port only: where the saved games are read and written (data/SAVEGAME
    // by default); the slot names are read again from there.
    void set_save_dir(const std::filesystem::path& dir);




    // A new game (GAMEPAL_1028_0000, menu result 0): everything is cleared,
    // the usecode globals come from USECODE/UNKDS.DAT (FLAG_1410_0051), the
    // player is made again (NPC_10f0_000c / 385c: item 1, shape 1, 150 hit
    // points; difficulty 3 unless 1..4), with the datalink (0x4D4, through
    // MainActor::I_addItemCru) and a container (0x598) in his inventory.
    // Then a teleport to egg 0x1E of map 1 (SHAPHAND_1098_011f) loads the
    // first mission when the kernel runs.
    void new_game(int difficulty);

    // Advances the clock by `elapsed_us` and runs one kernel slice. While the
    // menu is open, only the processes that run at any level do.
    void update(std::uint32_t elapsed_us);

    // Draws the view (the original's screen origin, see World::set_camera),
    // then the status bar, or the menu; then the mouse cursor if shown.
    // `screen` is 640 x 480, or larger for the native resolution: the view then
    // shows more around the camera, and the menus, screens, films and cursor
    // stay 640 x 480 in the middle (see ui_x / ui_y for the mouse).
    void render(Surface8& screen, RenderStats* stats = nullptr);
    // Where the 640 x 480 area starts in the last rendered screen; the mouse
    // coordinates the game takes are relative to it.
    int ui_x() const { return ui_x_; }
    int ui_y() const { return ui_y_; }

    // Moves the camera by (dx, dy) usecode units at its current height.
    void move_camera(std::int32_t dx, std::int32_t dy);

    // A key went down or up (PC set-1 scancode, E0 prefix, BIOS shift state),
    // as the keyboard interrupt handler sees it.
    void key(std::uint8_t scancode, bool extended, bool pressed, std::uint16_t flags, std::uint8_t ascii = 0);
    const Keyboard& keyboard() const { return keyboard_; }

    // Mouse driver events, in screen coordinates (MouseCtl_on_driver_event);
    // button 1 is the left one, 2 the right one. In play they go through the
    // mouse queue (mouse_queue.hpp) to the game view (world_mouse.hpp);
    // the menus and screens take the left button at once.
    void mouse_move(int x, int y);
    void mouse_button(bool pressed, int x, int y) { mouse_button(1, pressed, x, y); }
    void mouse_button(int button, bool pressed, int x, int y);
    const MouseCursor& cursor() const { return cursor_; }
    // Controller screen (options): it waits for a button press to bind, which
    // the platform gives with pad_button() instead of sending its key.
    bool waiting_for_pad() const;
    void pad_button(int button);
    // In play (no menu, screen, message box or movie), the game view puts the
    // mouse back at (320, 200) after each move: only relative motion counts.
    // Directional aiming: the right stick's direction on the screen (degrees,
    // counter-clockwise from east) while it is pushed, applied by update().
    void set_aim_stick(bool active, double screen_deg) {
        aim_stick_ = active;
        aim_stick_deg_ = screen_deg;
    }
    bool mouse_in_play() const { return !modal() && !movie_; }
    // True in play while the controlled NPC has its weapon out. The controller
    // then gives the original's combat-stance moves on the left stick.
    bool weapon_out() const;
    const WorldMouse& world_mouse() const { return *world_mouse_; }

    // In-game menu (Escape, WORLD_13e8_10c3). Closing it with "quit" sets
    // quit_requested(); with a slot chosen in "load", that game is loaded at
    // the next update (CRUSYS_1058_02f1).
    void open_menu();
    const MainMenu* menu() const { return menu_.get(); }

    // Game::I_playCredits (intrinsic 0x095, 13E8:003F), the end of the game:
    // at the next update the music stops, the credits run, then the movies
    // of the start-up (T01, T02) and the start-up menu, with no game to go
    // back to (see play_end_credits in game.cpp).
    void play_credits() { end_credits_pending_ = true; }

    // Normal launch (Game_start, 1028:0000), with no game given on the
    // command line: the movies T01 and T02, then the start-up menu with no
    // game running ("back" and "save" do nothing), where "new game" and
    // "load" lead into a game.
    void start_up();

    // The engine's message box (message_gump.hpp, the WorldHooks::show_message
    // of the world): the game freezes as for the menu until a key, or a click
    // on "Ok", closes it. Nothing when one is already open.
    void show_message(const std::string& text);
    const MessageGump* message() const { return message_.get(); }

    // A screen opened by a function key of the game view (World_on_key):
    // help, options, load screen, the questions of F4 / F5 / Alt+X and the
    // notice after a quick load; or the computer screen of
    // ComputerGump::I_readComputer and the keypad of KeypadGump::I_showKeypad.
    // The game is frozen while it is open.
    const ModalGump* screen() const { return screen_.get(); }

    // A cutscene (the movie intrinsics, see movie_player.hpp): FLICS/<name>,
    // nothing when it does not exist. The kernel pass ends at once; until the
    // movie is over, update() plays it instead of running the kernel, and
    // render() shows it.
    void play_movie(const std::string& name, MovieKind kind);
    const MoviePlayer* movie() const { return movie_.get(); }

    // ComputerGump::I_readComputer (computer_gump.hpp): the text on the
    // computer screen, in the menu mode, until it is closed. The calling
    // process goes on afterwards. Nothing when a screen is already open.
    void read_computer(const std::string& text);
    // KeypadGump::I_showKeypad (keypad_gump.hpp): the keypad for `code`, in
    // the menu mode, until it is closed; then process `pid` gets its result
    // (-1 cancelled, else the value entered) as the call's result. Nothing
    // (result 0) when a screen is already open.
    void show_keypad(std::int16_t code, Pid pid);
    // WeaselGump::I_showWeaselGump (weasel_gump.hpp): Weasel's shop for
    // `level`, in the menu mode, until it is closed; its movies are played
    // as cutscenes. Nothing when a screen is already open.
    void show_weasel(int level);
    bool quit_requested() const { return quit_; }

    World& world() { return *world_; }
    Kernel& kernel() { return *kernel_; }
    UCMachine& machine() { return *machine_; }
    // Palette on screen (the DAC): GAMEPAL.PAL in play, MISC.PAL in the menu,
    // changed by the palette fades.
    const Palette& palette() const { return world_->state.screen_palette; }
    const AnimDat& animations() const { return anims_; }
    const FontSet& fonts() const { return fonts_; }
    const StatusBar& status_bar() const { return status_bar_; }
    const CombatControl& combat() const { return *combat_; }
    // The sound system, null without SOUND.FLX.
    Audio* audio() { return audio_.get(); }
    // Without an audio device pulling the mixer (tests, runs without a
    // window), update() mixes the time that passed itself, so that sounds
    // end; with `capture`, the samples are kept.
    void set_audio_device(bool attached) { audio_device_ = attached; }
    void capture_sound(bool on) { capture_ = on; }
    const std::vector<std::int16_t>& captured_sound() const { return captured_; }
    // CRUSADER.CFG, read from the data directory at start-up. Closing the
    // options screen writes it to `path` (config_save); nothing is written
    // until a path is given.
    const GameConfig& config() const { return config_; }
    GameConfig& config() { return config_; }
    // Replaces it, the sound settings applied (tests).
    void set_config(GameConfig config);
    // Port addition: the biggest pixel size the window allows (the enhancements window stops there).
    void set_view_zoom_limit(int limit) { config_.view_zoom_limit = limit; }
    void set_config_file(std::filesystem::path path) { config_file_ = std::move(path); }
    // The two lines at the bottom of the main menu: the port's version (from
    // the build; empty hides the line) and the version of the game data
    // detected in CRUSADER.EXE ("CRUSADER:  No Remorse Vx.yy"), with its language.
    void set_port_version(std::string version) { port_version_ = std::move(version); }
    const std::string& port_version() const { return port_version_; }
    // "" when CRUSADER.EXE is missing or has no version string.
    const std::string& data_version() const { return data_version_; }
    // "FR" or "US", "" when not recognised.
    const std::string& data_language() const { return data_language_; }
    // Frames that fired a weapon (each calls Item::I_fireWeapon).
    int shots_fired() const { return shots_fired_; }

private:
    void fire_frame(ObjId actor, int muzzle_a, int muzzle_b, int muzzle_z);
    void reset_world();
    // The usecode globals (0x42 bytes in this version), the rest cleared.
    void set_globals(std::span<const std::uint8_t> bytes);
    void on_key(const GameKeyEvent& e);
    void close_menu();
    void close_message();
    // World_on_key's function keys and Alt+X: true when `code` opened a screen.
    bool function_key(std::uint16_t code);
    // Runs `screen` as Gump_run_modal does; `closed` gets it once closed.
    void open_screen(std::unique_ptr<ModalGump> screen, std::function<void(const ModalGump&)> closed);
    void close_screen();
    // A question of the game view (13F8:0000) with its sound; `yes` runs when
    // it is answered 1, the menu mode being left otherwise.
    void ask(std::uint16_t shape, int sfx, std::function<void()> yes);
    // The notice (13F8:0383), the menu mode left when it closes.
    void notify(std::uint16_t shape);
    // World_enter_menu_mode / World_leave_menu_mode (13E8:0E7D / 0EF9).
    void enter_menu_mode();
    void leave_menu_mode();
    // The menu, a message box or a screen holds the game.
    bool modal() const { return menu_ || message_ || screen_; }
    void dispatch_mouse();
    // Palette and cursor of the menu's difficulty screen when it opens or closes.
    void follow_menu_screen();
    void apply_status_bar_mouse_range();
    void run_credits(std::uint32_t ticks_before);
    void play_end_credits();
    void open_menu(bool no_game);
    void start_movie(const std::string& name, MovieKind kind, Surface8 screen);
    void play_weasel_movie(const std::string& name);
    void next_start_up_movie();
    // Without an audio device, mixes the time that passed (see set_audio_device).
    void mix_elapsed(std::uint32_t elapsed_us);
    // The game process's pass (1038:01B1) and the animation of the scenery
    // it asks for (DS:2C52), unless a modal screen pauses the game.
    void animate_scenery();
    // The game view and the status bar, without the cursor.
    void render_view(Surface8& screen, RenderStats* stats = nullptr);
    template <typename Draw>
    void draw_window(Surface8& screen, int x0, int y0, Draw&& draw);
    Surface8 window_;  // 640 x 480 scratch for draw_window
    int ui_x_ = 0, ui_y_ = 0;
    void update_movie(std::uint32_t elapsed_us);
    void end_movie();
    void save_config();
    void draw_autosave_icon(Surface8& screen);


    // Data, kept open for the whole run.
    FlexArchive shapes_flex_;
    FlexArchive globs_;
    FlexArchive fixed_;
    std::vector<ShapeInfo> typeflags_;
    TrigTables trig_;
    std::vector<std::uint8_t> damage_;
    AnimDat anims_;
    bool aim_stick_ = false;
    double aim_stick_deg_ = 0;
    WeaponOverlay weapon_overlay_;
    Palette palette_;
    UsecodeFile usecode_;
    ShapeCache shapes_;
    FlexArchive gumps_flex_;
    ShapeCache gumps_;
    FontSet fonts_;
    XformTable xform_;
    dtable::NpcTypes npc_types_;
    StatusBar status_bar_;
    PickupArea pickup_area_;
    Palette menu_palette_;
    Palette difficulty_palette_;
    MouseCursor cursor_;
    std::filesystem::path data_;
    std::string port_version_;
    std::string data_version_;  // "1.08", "1.23", "1.01"
    std::string data_language_;
    std::span<const std::uint16_t> intrinsic_numbering_;  // of the usecode (intrinsic_numbering.hpp)
    std::filesystem::path save_dir_;  // data_/SAVEGAME unless set_save_dir
    GameConfig config_;
    std::filesystem::path config_file_;
    std::unique_ptr<Audio> audio_;  // kept across loads, as the sound driver
    bool audio_device_ = false;
    bool credits_music_ = false;
    bool capture_ = false;
    std::vector<std::int16_t> captured_;
    std::uint64_t mixed_us_ = 0, mixed_samples_ = 0;

    // Rebuilt by each load, in this order (the machine refers to the kernel,
    // the intrinsics to the world).
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<UCMachine> machine_;
    std::unique_ptr<World> world_;

    Keyboard keyboard_;
    std::unique_ptr<AvatarMover> mover_;
    TimedProcess* key_daemon_ = nullptr;  // owned by the kernel
    CombatControl* combat_ = nullptr;     // owned by the kernel
    MouseQueue mouse_queue_;
    std::uint16_t mouse_buttons_ = 0;     // driver state: bit 0 left, bit 1 right
    std::unique_ptr<WorldMouse> world_mouse_;
    int shots_fired_ = 0;

    std::unique_ptr<MoviePlayer> movie_;
    // Player field +0xC2: the movie resumes the colour cycling when it ends
    // (DS:5FFE clear when it started).
    bool movie_resumes_cycle_ = true;
    // A movie of Weasel's shop: the game palette and the colour cycling come
    // back after it (13E0:0CD8).
    bool weasel_movie_ = false;
    bool weasel_intro_played_ = false;  // DS:5ECF
    // The game process's pass count (type 0x245, +0x40 / +0x44) and its value
    // at the last animation pass of the scenery (DS:2D3B), item_anim.hpp.
    std::uint32_t game_passes_ = 0;
    std::uint32_t animated_at_ = 0;
    // Sounds of the last movies: the mixer may still read the last one for a
    // buffer after it is stopped.
    std::deque<std::vector<std::int8_t>> movie_sounds_;

    std::unique_ptr<MainMenu> menu_;
    Process* menu_process_ = nullptr;  // modal placeholder, owned by the kernel
    MainMenu::Shared menu_shared_;     // DS:5BE8, DS:0098
    LoadScreen::Names save_names_;     // DS:7964
    CreditsData credits_data_;
    HelpScreen::Pictures help_pictures_;
    bool autosave_pending_ = false;    // request_autosave, until the player may act
    std::uint32_t autosave_due_ = 0;   // the kernel tick from which it can run
    bool run_toggled_ = false;         // GameConfig::toggle_run: running is switched on
    std::uint32_t autosave_icon_us_ = 0;  // what is left of the floppy disk's display
    std::vector<std::uint8_t> floppy_;    // its palette indices (0xFF: transparent), built when first drawn
    int pending_load_ = 0;             // save to load at the next update (1-based)
    int pending_new_game_ = 0;         // difficulty of a new game to start at the next update
    bool end_credits_pending_ = false; // Game::I_playCredits: its process has not run yet
    bool end_credits_ = false;         // the credits running are the end of the game's
    std::vector<std::string> next_movies_;  // Game_start: movies to play after the current one
    std::unique_ptr<MessageGump> message_;
    Process* message_process_ = nullptr;  // modal placeholder, owned by the kernel
    std::unique_ptr<ModalGump> screen_;
    std::function<void(const ModalGump&)> screen_closed_;
    Process* screen_process_ = nullptr;   // modal placeholder, owned by the kernel
    std::uint16_t pending_notice_ = 0;    // notice to show after the pending load
    bool sprites_suspended_ = false;      // DS:2D8F: the screen sprites are not drawn
    bool difficulty_shown_ = false;    // the menu shows the difficulty screen
    bool quit_ = false;
    bool status_bar_shown_ = false;    // last state seen, for the mouse range
};

}  // namespace crusader
