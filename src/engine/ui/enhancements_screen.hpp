#pragma once
// The enhancements window (port addition, the original has nothing like it),
// opened from the options screen's "ameliorations" button (a "+" icon and its
// name). It gathers what the port adds to the original options, on four
// pages listed by its first one:
// - "controles": the controller screen (gamepad_screen.hpp), the
//   "mode directionnel" toggle (GameConfig::directional_move): the left stick
//   walks the NPC where it points instead of the tank controls, "visee
//   directionnelle" (GameConfig::directional_aim): the right stick puts the
//   crosshair where it points, "degaine en visant" (GameConfig::draw_on_aim):
//   pushing that stick draws the weapon, "visee precise"
//   (GameConfig::precise_aim): shots only lock on a target they can reach,
//   "course permanente" (GameConfig::toggle_run), and controller vibration
//   (GameConfig::vibration / vibration_intensity);
// - "affichage": "resolution native" (GameConfig::native_resolution): the
//   view fills the screen, "zoom" (GameConfig::view_zoom): auto, then pixels
//   of 1 to 4 screen pixels, "lissage" (GameConfig::smooth_scaling):
//   bilinear filtering, and "suivi camera" (GameConfig::follow_player): the
//   controlled NPC stays at the centre of the screen;
// - "effets": "impacts" (GameConfig::impact_marks): shots mark the scenery,
//   and "sang" (GameConfig::blood_marks, gore_mode): off, the wounded
//   bleed on it, or "gore": much more;
// - "jeu": "sauvegarde auto" (GameConfig::autosave): the game saves itself at
//   the key moments of a mission, and "correctifs original"
//   (GameConfig::fix_original_bugs): bugs of the original game's scripts are
//   corrected.
//
// Look: the screen cleared, a small red framed window (colour 0x57) in the
// middle, the title (the page's name on a page) with the "+" icon in green,
// and one row per entry, "retour" last, outlined as the controller screen's
// rows. Keys: up / '8' and down / '2' change row, Enter opens or toggles,
// Escape (or "retour") goes back from a page to the first one, with the
// page's row focused, and closes the window from the first one. Left / right
// adjust vibration intensity. Mouse: pressing a row focuses and activates it.
// While the controller screen is open it gets every event.

#include "core/font.hpp"
#include "core/game_config.hpp"
#include "core/surface.hpp"
#include "engine/ui/gamepad_screen.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace crusader {

// A frame with its corners cut like the original buttons', filled with `fill`
// (a palette index) unless it is negative.
void draw_rounded_box(Surface8& screen, int x0, int y0, int x1, int y1, int fill, std::uint8_t colour);

// A "+" in a square frame of `size` pixels, top-left at (x, y).
void draw_plus_icon(Surface8& screen, int x, int y, int size, std::uint8_t colour);

class EnhancementsScreen {
public:
    enum class Page { Main, Controls, Display, Effects, Game };
    enum class Entry {
        // The first page: one row per page.
        Controls, Display, Effects, Game,
        // Controls.
        Pad, Directional, Aim, Draw, PreciseAim, Run, Vibration, VibrationIntensity,
        // Display.
        Native, Zoom, Smooth, Follow,
        // Effects.
        Impact, Blood,
        // Game.
        Autosave, FixBugs,
        // Last on every page.
        Back,
    };
    static constexpr int kFont = 14;

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // `pad_focus` is the controller screen's row, kept by the caller.
    EnhancementsScreen(const FontSet& fonts, GameConfig& config, int& pad_focus,
                       std::function<void()> vibration_preview = {});

    void draw(Surface8& screen) const;
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    void update(std::uint32_t ticks);
    bool waiting() const { return pad_ && pad_->waiting(); }
    void pad_button(int button) {
        if (pad_) pad_->pad_button(button);
    }

    bool closed() const { return closed_; }
    Page page() const { return page_; }
    int focus() const { return focus_; }
    // The rows of the page shown, top to bottom.
    std::span<const Entry> entries() const;
    // The row of `entry` on the page shown, -1 when it is not there.
    int row_of(Entry entry) const;
    const GamepadScreen* gamepad() const { return pad_.get(); }
    Rect row_rect(int row) const;

private:
    void activate(int row);
    void open(Page page);
    void back();

    const FontSet* fonts_;
    GameConfig* config_;
    int* pad_focus_;
    std::function<void()> vibration_preview_;
    Page page_ = Page::Main;
    int focus_ = 0;
    bool closed_ = false;
    std::unique_ptr<GamepadScreen> pad_;
};

}  // namespace crusader
