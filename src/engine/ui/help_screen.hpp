#pragma once
// The help screen (WORLD.C 13E8:38E5), opened by F1 or '?' in play.
//
// A full-screen window gump that draws nothing itself: each page is an 8-bit
// 640 x 480 BMP drawn on the cleared screen with its own palette (segment
// 1010), STATIC/HELP1.DAT for page 0, HELP2.DAT for page 1, HELP3.DAT for
// page 4. Two shape buttons (shape_dialog.hpp) over it: "next" shape 0x51 at
// (410, 449), id 0, and "quit" 0x52 at (516, 449), id 1 (x 493 and 549 in
// the US executable). Cursor frame 0x27.
//
// Showing the page +0x49 (13E8:3BA5): -1 becomes 0 and nothing is drawn
// again; 2 closes the screen; otherwise the screen is cleared and the page's
// picture drawn, a page without one (3 and above, other than 4) drawing the
// last picture loaded again. The palette faders are stopped (type 0x206)
// and the picture's palette written.
//
// Keys (13E8:3D99, every key reaches it), page 4 being first turned into 0
// at each key:
//   Escape, x, X                   close
//   left, up                       page - 1
//   right, down, n, N              page + 1 (so "next" from page 1 closes)
//   e, E                           start the hidden page sequence (+0x47 = 1)
//   '-' once started               +0x47 + 1; at 4, page 4 (HELP3.DAT)
//   any other key                  the sequence is dropped
// Turning a page also drops the sequence. The buttons (13E8:3D53): "next"
// page + 1, "quit" closes.
//
// On page 4, at each pass of the modal loop, one chance in 8000
// (random_range(8000) == 1) to play one of the sounds 0xB8, 0x62, 0x67, 0xA,
// 0xB (random_range(5)). The port draws once per 60 Hz tick.
//
// Not reproduced: the keyboard and mouse clicks flushed after a page turn
// (keyboard_flush, DS:8A80..8A84).

#include "core/data_files.hpp"
#include "core/palette.hpp"
#include "engine/ui/modal_gump.hpp"
#include "engine/ui/shape_dialog.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace crusader {

class HelpScreen : public ModalGump {
public:
    static constexpr std::uint16_t kCursor = 0x27;
    static constexpr int kHiddenPage = 4;

    struct Page {
        Surface8 picture;
        Palette palette;
    };
    // The three pictures (STATIC/HELP1.DAT .. HELP3.DAT), read once.
    struct Pictures {
        std::array<std::optional<Page>, 3> pages;
        static Pictures load(const DataFiles& files);
    };

    // `random(n)` draws 0 .. n - 1 (random_range); `play_sfx` plays a sound.
    HelpScreen(ShapeCache& gumps, const Pictures& pictures, std::function<int(int)> random,
               std::function<void(int)> play_sfx);

    void draw(Surface8& screen) const override;
    void key(std::uint16_t code) override;
    void mouse_down(int x, int y) override { buttons_.mouse_down(x, y); }
    void mouse_move(int x, int y) override { buttons_.mouse_move(x, y); }
    void mouse_up(int x, int y) override;
    void update(std::uint32_t ticks) override;
    bool closed() const override { return closed_; }

    int page() const { return page_; }
    // The palette of the picture shown (written when the page is shown).
    const Palette* palette() const override;
    const std::vector<ShapeButton>& buttons() const { return buttons_.buttons; }

private:
    bool show();  // 13E8:3BA5

    const Pictures* pictures_;
    std::function<int(int)> random_;
    std::function<void(int)> play_sfx_;
    ShapeButtons buttons_;
    int page_ = 0;      // +0x49
    int sequence_ = 0;  // +0x47
    int picture_ = -1;  // the picture loaded
    std::optional<std::uint32_t> last_tick_;
    bool closed_ = false;
};

}  // namespace crusader
