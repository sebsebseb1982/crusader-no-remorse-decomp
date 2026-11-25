#pragma once
// The controller screen (port addition, the original has no gamepad), opened
// from the options screen's "manette" button. It sets PadSettings
// (core/pad_config.hpp), saved in CRUSADER.CFG when the options close.
//
// Look of the load screen: the screen cleared, a red frame (colour 0x57, as
// the options panel), the labels in font 14 tinted green (0x75, 0x0C when
// focused), the values in font 14 as it is (red), a green outline on the
// focused row (0x0C, as the options screen). Two columns, rows 18 pixels apart:
//   left   the actions of the game (TIR, COURIR, ROULADE GAUCHE, ARME SUIVANTE...), rows 0..16,
//          then "vitesse de visee" and "zone morte" (the right stick), then
//          "par defaut" and "retour"
//   right  the controller button each action is on ("A", "LB"..., or "AUCUN"),
//          "< 3 / 5 >" on the two stick rows
//
// Binding: pressing an action's button (a click, Enter) makes the screen wait
// ("APPUYEZ SUR UN BOUTON"): the next controller button pressed is bound to
// the action (the platform layer hands it over with pad_button(), instead of
// sending its key). The button's previous action is unbound, and so is the
// button the action was on. Escape, any click, or 10 seconds without a button
// cancel. Delete unbinds the focused action.
//
// Keys, as the options screen: up / '8' and down / '2' change row; left / '4'
// and right / '6' change the stick rows; Enter binds, resets on "par defaut"
// and closes on "retour"; Escape closes. Mouse: pressing a row focuses it, and
// binds on an action row; on the stick rows the "<" half of the value text goes
// back and the ">" half forward. The controller drives the screen through
// the arrows, A (Enter) and B (Escape) while it is not waiting.

#include "core/font.hpp"
#include "core/game_config.hpp"
#include "core/surface.hpp"

#include <cstdint>
#include <string>

namespace crusader {

class GamepadScreen {
public:
    static constexpr int kActionRows = kPadActionCount - 1;  // every action but "none"
    static constexpr int kAimRow = kActionRows, kDeadZoneRow = kActionRows + 1;
    static constexpr int kDefaultRow = kActionRows + 2, kBackRow = kActionRows + 3;
    static constexpr int kRows = kActionRows + 4;
    static constexpr int kFont = 14;
    static constexpr std::uint32_t kWaitTicks = 600;  // 10 seconds at 60 Hz

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // `focus` is the row, kept by the caller between openings.
    GamepadScreen(const FontSet& fonts, GameConfig& config, int& focus);

    void draw(Surface8& screen) const;
    // Gump key code (see gump_keys.hpp).
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    // Timer ticks: the wait for a button times out.
    void update(std::uint32_t ticks);
    // A controller button (PadButton) pressed while waiting().
    void pad_button(int button);

    bool closed() const { return closed_; }
    int focus() const { return *focus_; }
    bool waiting() const { return waiting_; }
    // The action of an action row (PadActionId).
    static int action_of(int row) { return row + 1; }
    Rect row_rect(int row) const;
    Rect value_rect(int row) const;

private:
    void change(int row, int delta);  // the stick rows; delta: -1, +1
    std::string stick_value(int row) const;  // "< 3 / 5 >"
    void activate(int row);           // Enter, or a press on a row
    void print(Surface8& screen, int x, int y, const std::string& text, int tint) const;

    const FontSet* fonts_;
    GameConfig* config_;
    int* focus_;
    bool closed_ = false;
    bool waiting_ = false;
    std::uint32_t now_ = 0;
    std::uint32_t wait_start_ = 0;
};

}  // namespace crusader
