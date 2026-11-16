#pragma once
// The difficulty screen (IRGUMP_13d0_25a2, segment 13D0), opened by the main
// menu's "new game" button. Choosing a level starts a new game.
//
// Palette STATIC/DIFF.PAL; mouse cursor 0x26, the mouse put at (320, 400).
// Background (13D0:2B16): the screen cleared, GUMPS.FLX shape 0x49 frame 0 at
// (139, 76) and frame 1 at (331, 76); the US shape has a single frame, centred
// at y 76. Four buttons (13D0:2D5E), level 1..4,
// shape 0x49 + level: frame 0 at the button's corner and frame 1 170 pixels
// to its right. A button's rectangle is 321 pixels wide (x 159..479, centred
// on the screen) and as high as the shape, from y 120, 179, 238, 297. The
// selected level (DS:5BEC, 3 at start-up, kept between screens) is outlined
// in colour 0x44.
//
// Keys (13D0:29B3): up / '8' the level above (not before 1), down / '2' the
// one below (not after 4), Enter chooses the selected level, Escape cancels.
// Mouse (button base class, SEG1310): a press inside a button selects it and
// captures the mouse; releasing inside chooses it (13D0:2975).
//
// Choosing sets the difficulty (DS:7E19) and DS:2172 = (difficulty != 1), the
// BA-40's ammo type (dtable::set_difficulty, applied by Game::new_game); the
// screen closes with the level, or 0.

#include "core/surface.hpp"
#include "engine/world/render.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace crusader {

class DifficultyScreen {
public:
    static constexpr int kLevels = 4;
    static constexpr std::uint16_t kBackground = 0x49;
    static constexpr std::uint16_t kCursor = 0x26;
    static constexpr int kMouseX = 320, kMouseY = 400;
    static constexpr std::uint8_t kOutlineColour = 0x44;

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // `selected` is DS:5BEC (1..4).
    DifficultyScreen(ShapeCache& gumps, int& selected);

    void draw(Surface8& screen) const;
    // Gump key code (see gump_keys.hpp).
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    void mouse_move(int x, int y);
    void mouse_up(int x, int y);

    // Set once closed: the level chosen, or 0 when cancelled.
    std::optional<int> result() const { return result_; }
    int selected() const { return *selected_; }
    const Rect& button(int level) const { return buttons_[level - 1]; }

private:
    void draw_shape(Surface8& screen, std::uint16_t shape, int frame, int x, int y) const;

    ShapeCache* gumps_;
    int* selected_;
    std::array<Rect, kLevels> buttons_{};
    int captured_ = 0;  // level whose button holds the mouse, 0 for none
    std::optional<int> result_;
};

}  // namespace crusader
