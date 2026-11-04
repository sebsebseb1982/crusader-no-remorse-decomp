#pragma once
// CRUSADER.CFG, "key=value" lines: read at start-up (1020:0000) and written
// back by config_save (1020:0C7F) when the options screen closes.
//
// Only the keys of the options screen, the subtitles and "noweasel" are
// decoded here; the others (sound card, CD letter, mouse buttons...) are kept
// as they are. Defaults are the data segment's initial values, used when a
// key is missing.
//
// The original rewrites the whole file from its globals, with lines ending in
// "\n\r"; the port replaces the lines of the keys it knows, keeps the other
// lines and their order, and ends lines with "\r\n" like the installer.

#include "core/pad_config.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crusader {

class GameConfig {
public:
    int sound_volume = 0x40;   // "soundvolume" (DS:464B), 0..0x40
    int music_volume = 0x20;   // "musicvolume" (DS:464D)
    bool filter = true;        // "filter" (DS:464F): the mixer's oversampling
    int animation = 0;         // "animation" (DS:0636): 0 shows the animations
    int frameskip = 0;         // "frameskip" (DS:2887): 0, or 3 for one frame in two
    bool limit_blasts = false; // "limitblasts" (DS:2888)
    int mouse_speed = 2;       // "mousespeed" (DS:6001), 0..3
    bool big_video = false;    // "bigvideo" (DS:68CE: mode 0x13 / 8 when big, else 0x101 / 0)
    bool subtitles = false;    // "subtitles" (DS:0179): the cutscenes' subtitles
    bool no_weasel = false;    // "noweasel" (DS:0178): no movies in Weasel's shop
    // Port addition, written only when set: "followplayer", the camera keeps
    // the controlled NPC at the centre of the screen.
    bool follow_player = false;
    // Port addition, written only when set: "directionalmove", the controller's
    // left stick walks the NPC in the direction it points (the keypad's screen
    // directions) instead of the original's tank controls.
    bool directional_move = false;
    // Port addition, written only when set: "directionalaim", the right stick
    // puts the crosshair in the direction it points.
    bool directional_aim = false;
    // Port addition, written when switched off: "drawonaim", pushing the
    // directional aim stick draws the weapon when it is holstered, without the
    // fire key first.
    bool draw_on_aim = true;
    // Port addition, written when switched off: "nativeresolution", the view
    // takes the whole screen (more scenery around the camera) instead of the
    // original 640 x 480.
    bool native_resolution = true;
    // Port addition, written when switched off: "autosave", the game saves
    // itself (slot Game::kAutoSave) at the key moments of a mission.
    bool autosave = true;
    // Port addition, written only when set: "togglerun", the Shift key switches
    // running on and off at each press instead of running while it is held.
    bool toggle_run = false;
    // Port addition, written when not 0: "viewzoom", the largest pixel size
    // (whole multiple) of the native view; 0 takes the biggest that fits the
    // window, 1 gives the finest picture.
    int view_zoom = 0;
    // Not saved: the biggest pixel size the window allows (set by the platform
    // layer), where the zoom choices stop.
    int view_zoom_limit = 4;
    // Port addition, written only when changed: "padmap" (comma-separated
    // action numbers, see PadButton), "padaim" and "paddeadzone".
    PadSettings pad;

    // A missing or unreadable file gives the defaults.
    static GameConfig load(const std::filesystem::path& path);
    static GameConfig parse(std::string_view text);

    std::string text() const;
    // Throws std::runtime_error when the file cannot be written.
    void save(const std::filesystem::path& path) const;

private:
    void set(std::string_view key, const std::string& value);
    void erase(std::string_view key);

    std::vector<std::string> lines_;  // as read, without line ends
};

}  // namespace crusader
