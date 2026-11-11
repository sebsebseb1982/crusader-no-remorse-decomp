#pragma once
// The enhancements window (port addition, the original has nothing like it),
// opened from the options screen's "ameliorations" button (a "+" icon and its
// name). It gathers what the port adds to the original options: the
// controller screen (gamepad_screen.hpp), opened by its first row, and the
// "camera suit joueur" on / off toggle (GameConfig::follow_player), which keeps
// the controlled NPC at the centre of the screen, and the "deplacement
// directionnel" toggle (GameConfig::directional_move): the controller's left
// stick walks the NPC where it points instead of the tank controls, and the
// "visee directionnelle" toggle (GameConfig::directional_aim): the right stick
// puts the crosshair where it points, and the "degaine en visant" toggle
// (GameConfig::draw_on_aim): pushing that stick draws the weapon, and the
// "resolution native" toggle
// (GameConfig::native_resolution): the view fills the screen, and its "zoom"
// row (GameConfig::view_zoom): auto, then pixels of 1 to 4 screen pixels, the
// "lissage" toggle (GameConfig::smooth_scaling): bilinear filtering, and
// the "sauvegarde auto" toggle (GameConfig::autosave): the game saves itself
// at the key moments of a mission, "course toggle" (GameConfig::toggle_run),
// and controller vibration (GameConfig::vibration / vibration_intensity).
//
// Look: the screen cleared, a small red framed window (colour 0x57) in the
// middle, the title with the "+" icon in green, and one row per entry
// ("manette", "retour") outlined as the controller screen's rows. Keys: up /
// '8' and down / '2' change row, Enter opens or closes, Escape closes. Left /
// right adjust vibration intensity. Mouse: pressing a row focuses and
// activates it. While the controller screen is open it gets every event.

#include "core/font.hpp"
#include "core/game_config.hpp"
#include "core/surface.hpp"
#include "engine/ui/gamepad_screen.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace crusader {

// A frame with its corners cut like the original buttons', filled with `fill`
// (a palette index) unless it is negative.
void draw_rounded_box(Surface8& screen, int x0, int y0, int x1, int y1, int fill, std::uint8_t colour);

// A "+" in a square frame of `size` pixels, top-left at (x, y).
void draw_plus_icon(Surface8& screen, int x, int y, int size, std::uint8_t colour);

class EnhancementsScreen {
public:
    static constexpr int kPadRow = 0, kFollowRow = 1, kDirectionalRow = 2, kAimRow = 3, kDrawRow = 4,
                         kNativeRow = 5, kZoomRow = 6, kSmoothRow = 7, kAutosaveRow = 8, kRunRow = 9,
                         kVibrationRow = 10, kVibrationIntensityRow = 11, kBackRow = 12, kRows = 13;
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
    int focus() const { return focus_; }
    const GamepadScreen* gamepad() const { return pad_.get(); }
    Rect row_rect(int row) const;

private:
    void activate(int row);

    const FontSet* fonts_;
    GameConfig* config_;
    int* pad_focus_;
    std::function<void()> vibration_preview_;
    int focus_ = 0;
    bool closed_ = false;
    std::unique_ptr<GamepadScreen> pad_;
};

}  // namespace crusader
