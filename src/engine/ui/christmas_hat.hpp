#pragma once
// Port addition (@orig none): a Christmas hat on the Silencer of the main
// menu, from 24 to 31 December, the dates of No Regret's Christmas music
// (STDINT 1210:0000), until Ctrl+M turns the Christmas mode off for the
// session (Music::allow_christmas). The original No Remorse has no Christmas
// mode; No Regret announces Ctrl+M at start-up as the way to "disable Xmas
// mode".
//
// Each hat is drawn once over the menu's background (GUMPS.FLX 54..57,
// MISC.PAL), right after it: the background is the same in the French,
// English and German No Remorse data, so the hat's palette indices carry its
// shading, its fur, the edges blended with the background and its shadow on
// the helmet. The No Remorse one follows the background's scanlines (rows
// y % 4 = 1, 2 darker).

#include "core/surface.hpp"

#include <ctime>

namespace crusader {

// 24 to 31 December.
bool is_christmas(const std::tm& date);

// The hat of No Remorse's menu, or of No Regret's (its background is
// another picture, the top row at y = 75).
void draw_christmas_hat(Surface8& screen, bool regret);

}  // namespace crusader
