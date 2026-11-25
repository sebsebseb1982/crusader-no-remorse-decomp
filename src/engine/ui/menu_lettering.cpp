#include "engine/ui/menu_lettering.hpp"

#include <algorithm>
#include <array>

namespace crusader {

namespace {

struct Letter {
    char c;
    std::array<const char*, MenuLettering::kLetterHeight> rows;  // '#' face, '+' edge, '.' backdrop
};

// clang-format off
const Letter kLetters[] = {
    {'A', {"..++###########++",
          "..++###########++",
          "####++.......##++",
          "##++.........##++",
          "##++.........##++",
          "##++..++#######++",
          "##++..++#######++",
          "##++.........##++",
          "##++.........##++",
          "##++.........##++"}},
    {'C', {"..++#########++",
          "..++#########++",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "..++#########++",
          "..++#########++"}},
    {'D', {"++##########++..",
          "++##########++..",
          "##++........##++",
          "##++........##++",
          "##++........##++",
          "##++........##++",
          "##++........##++",
          "##++........##++",
          "##++..++####++..",
          "##++..++####++.."}},
    {'E', {"..+++########++",
          "..+++########++",
          "++###..........",
          "++###..........",
          "++###########++",
          "++###########++",
          "++###..........",
          "++###..........",
          "..+++########++",
          "..+++########++"}},
    {'I', {"##++",
          "##++",
          "##++",
          "##++",
          "##++",
          "##++",
          "##++",
          "##++",
          "##++",
          "##++"}},
    {'L', {"##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++...........",
          "##++++.........",
          "####++.........",
          "..++#########++",
          "..++#########++"}},
    {'M', {"++##++........++##++",
          "++###++......++###++",
          "##+###++....++###+##",
          "##++###++..++###++##",
          "##++.###++++###.++##",
          "##++..###++###..++##",
          "##++...######...++##",
          "##++............++##",
          "##++............++##",
          "##++............++##"}},
    {'N', {"++##++......##++",
          "++###++.....##++",
          "##+###++....##++",
          "##++###++...##++",
          "##++.###++..##++",
          "##++..###++.##++",
          "##++...###++##++",
          "##++....###+##++",
          "##++.....###++..",
          "##++......##++.."}},
    {'O', {"..++###########++..",
          "..++###########++..",
          "##++...........++##",
          "##++...........++##",
          "##++...........++##",
          "##++...........++##",
          "##++...........++##",
          "##++...........++##",
          "....++#######++....",
          "....++#######++...."}},
    {'R', {"############+++..",
          "#############++..",
          "##++........###++",
          "##++.........##++",
          "##++.........##++",
          "##++.++######++..",
          "##++.++######++..",
          "##++.........##++",
          "##++.........##++",
          "##++.........##++"}},
    {'S', {"++##########++",
          "++##########++",
          "##............",
          "##............",
          "++##########++",
          "++##########++",
          "............##",
          "............##",
          "++##########++",
          "++##########++"}},
    {'T', {"++###########++",
          "++###########++",
          "......##++.....",
          "......##++.....",
          "......##++.....",
          "......##++.....",
          "......##++.....",
          "......##++.....",
          "......##++.....",
          "......##++....."}},
    {'U', {"##++.......##++",
          "##++.......##++",
          "##++.......##++",
          "##++.......##++",
          "##++.......##++",
          "##++.......##++",
          "##++++.....##++",
          "####++.....##++",
          "..++#########++",
          "..++#########++"}},
};
// clang-format on

const Letter* find(char c) {
    for (const Letter& l : kLetters) {
        if (l.c == c) return &l;
    }
    return nullptr;
}

int letter_width(char c) {
    const Letter* l = find(c);
    return l ? static_cast<int>(std::char_traits<char>::length(l->rows[0])) : MenuLettering::kSpaceWidth;
}

// The buttons' backdrop: a dither of near-black colours.
std::uint8_t backdrop(int x, int y) {
    constexpr std::uint8_t kDark[] = {52, 3, 4, 52, 3, 57};
    return kDark[static_cast<unsigned>(x * 7 + y * 13 + (x * y) % 5) % 6];
}

}  // namespace

MenuLettering::MenuLettering(std::vector<std::string> lines) : lines_(std::move(lines)) {
    for (const std::string& line : lines_) width_ = std::max(width_, line_width(line));
    width_ += 2;
    height_ = lines_.empty() ? 2 : kLineStep * (static_cast<int>(lines_.size()) - 1) + kLetterHeight + 2;
}

int MenuLettering::line_width(const std::string& line) {
    int w = 0;
    for (char c : line) w += letter_width(c);
    return w + kLetterGap * std::max(0, static_cast<int>(line.size()) - 1);
}

void MenuLettering::draw(Surface8& screen, int x0, int y0, bool selected) const {
    auto plot = [&](int x, int y, std::uint8_t colour) {
        if (x >= 0 && y >= 0 && x < screen.width && y < screen.height) screen.row(y)[x] = colour;
    };
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) plot(x0 + x, y0 + y, backdrop(x, y));
    }
    const std::uint8_t face = selected ? kFaceSelected : kFace, edge = selected ? kEdgeSelected : kEdge;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        int x = x0 + 1 + (width_ - 2 - line_width(lines_[i])) / 2;
        const int y = y0 + 1 + kLineStep * static_cast<int>(i);
        for (char c : lines_[i]) {
            if (const Letter* l = find(c)) {
                for (int r = 0; r < kLetterHeight; ++r) {
                    for (int k = 0; l->rows[r][k]; ++k) {
                        if (l->rows[r][k] == '#') plot(x + k, y + r, face);
                        else if (l->rows[r][k] == '+') plot(x + k, y + r, edge);
                    }
                }
            }
            x += letter_width(c) + kLetterGap;
        }
    }
}

}  // namespace crusader
