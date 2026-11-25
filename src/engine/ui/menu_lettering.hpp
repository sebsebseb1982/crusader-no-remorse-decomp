#pragma once
// Port addition (@orig none): words drawn in the lettering of the main
// menu's buttons (GUMPS.FLX shapes 58..63), for the buttons the port adds.
//
// The buttons' letters are pictures, not a font: square letters 10 pixels
// high with strokes 2 pixels thick, in two greens (palette indices of
// STATIC/MISC.PAL): the face 28 and its rounded edges 127 when the button is
// not selected, 12 and 28 when it is, on a dark backdrop (indices 52, 3, 4,
// 57) one pixel wider than the letters. The letters here are copied from
// the French buttons (C from CHARGER, A, D, S, U from SAUVEGARDER...); L, M
// and T, which no button has, are drawn after them. Letters of a line are 2
// pixels apart, lines 16 pixels apart (top to top), as on the buttons.

#include "core/surface.hpp"

#include <string>
#include <vector>

namespace crusader {

class MenuLettering {
public:
    static constexpr int kLetterHeight = 10;
    static constexpr int kLineStep = 16;
    static constexpr int kLetterGap = 2;
    static constexpr int kSpaceWidth = 8;
    static constexpr std::uint8_t kFace = 28, kEdge = 127, kFaceSelected = 12, kEdgeSelected = 28;

    // The lines, centred on each other. Letters: A C D E I L M N O R S T U
    // and the space; any other character is drawn as a space.
    explicit MenuLettering(std::vector<std::string> lines);

    // The whole picture, backdrop included.
    int width() const { return width_; }
    int height() const { return height_; }

    // Top-left corner of the backdrop at (x, y).
    void draw(Surface8& screen, int x, int y, bool selected) const;

    static int line_width(const std::string& line);

private:
    std::vector<std::string> lines_;
    int width_ = 0, height_ = 0;
};

}  // namespace crusader
