#pragma once
// Presents an 8-bit framebuffer through SDL. Replaces the original VESA mode
// setup (SUPERVGA), banked VRAM blits (VPORT asm blitters) and DAC writes
// (palette_write_hw): the game draws into a Surface8, this converts it with the
// current palette and scales it to the window.

#include "core/palette.hpp"
#include "core/surface.hpp"

#include <array>
#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace crusader {

class SdlPresenter {
public:
    SdlPresenter(const std::string& title, int width, int height, int scale, bool fullscreen = false);
    ~SdlPresenter();
    SdlPresenter(const SdlPresenter&) = delete;
    SdlPresenter& operator=(const SdlPresenter&) = delete;

    void set_title(const std::string& title);
    // Brightens (> 1) or darkens (< 1) the picture; 1 is the original palette.
    void set_gamma(double gamma);
    // Scales the frames to the window with bilinear filtering (soft edges, as
    // DOSBox's "output=opengl") instead of nearest pixels (its "openglnb").
    void set_smooth(bool smooth);
    void present(const Surface8& frame, const Palette& palette);
    void maximize();
    // Desktop fullscreen on / off (Alt+Enter).
    void toggle_fullscreen();
    // Size of the window's drawing area in real pixels.
    void output_size(int* w, int* h) const;
    // Changes the size of the frames given to present (the window is unchanged).
    void resize(int width, int height);
    // Puts the system pointer on a point of the game screen (mouse_warp).
    void warp_mouse(int x, int y);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    std::array<std::uint8_t, 256> gamma_table_;
    bool smooth_ = false;
    int width_;
    int height_;
};

}  // namespace crusader
