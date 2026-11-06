#pragma once
// The options screen (IRGUMP_13d0_0ece, segment 13D0), opened by the main
// menu's "options" button. Closing it writes CRUSADER.CFG (config_save,
// 1020:0C7F, from its destructor 13D0:1759).
//
// Layout: the screen is cleared; GUMPS.FLX shapes 44..47 at (1, 120),
// (320, 120), (1, 240), (320, 240), as the main menu; the labels "volume son"
// (shape 41) at (468, 146) and "volume musique" (42) at (459, 196). Palette:
// STATIC/MISC.PAL. The widgets, each with its number (byte +10 of the gump)
// and top-left corner:
//
//   two-choice pairs (13D0:0CE7), frame 0 when chosen, 1 otherwise:
//     animations      0x50 on  (35, 163)  shape 31   0x51 off   (165, 163)  32
//     one frame in 2  0x52 on  (35, 203)  33         0x53 off   (165, 203)  34
//     video screen    0x56 big (35, 244)  35         0x57 small (137, 244)  36
//     limit blasts    0x58 on  (35, 283)  37         0x59 off   (165, 283)  38
//     digital filter  0x54 on  (35, 323)  39         0x55 off   (165, 323)  40
//   repeating buttons (1310:032B), shape 49 for -, 50 for +, frame 1 pressed:
//     sound volume  0x5C (435, 146), 0x5D (597, 146)
//     music volume  0x5E (435, 197), 0x5F (597, 197)
//     mouse speed   0x60 (435, 249), 0x61 (597, 249)
//   "quit" button 0x62 (1378:0000), shape 43 at (517, 318), frame 1 selected.
//
// Over the background (13D0:17C5), shape 48 marks each setting: at
// x = 456 + 6 n, y 179 (sound), 230 (music), 282 (mouse), where n is the
// volume step, or (speed << 16) / 0x2164, minus 1 for speed 3 (0, 7, 15,
// 22). The focused line (DS:5BEA, 0..8, kept between screens) gets an
// outline of colour 0x0C (13D0:1E62); line 8, "quit", is shown by the
// button's frame instead.
//
// Volumes go by steps: 23 values for the sounds (DS:5BB6, 0 to 0x40) and the
// music (DS:5BCD, 0 to 0x30). The screen starts at the first step not below
// the current volume.
//
// Keys (13D0:20B7): up / '8' and down / '2' move the focus (0..8); left / '4'
// and right / '6' act on the focused line as its first or second widget;
// Enter on "quit" and Escape close the screen. Mouse: pressing a two-choice
// widget not chosen yet chooses it; a repeating button acts when pressed, then
// every 11 timer ticks while held inside (1310:04FD, the deadline is
// "now + 10" and must be passed); "quit" acts when released inside, as the
// menu buttons. A widget acting moves the focus to its line (13D0:1D81).
//
// Settings (Options_key, 13D0:236E): animations DS:0636 (0 on, 1 off), one
// frame in two DS:2887 (3 or 0), video size DS:68CE, limit blasts DS:2888,
// filter (_ASS_SetOversampling / _ASS_ClrOversampling), volumes
// (_ASS_SetSoundVolume / _ASS_SetMusicVolume), mouse speed DS:6001 (0..3).
// The volume and mouse buttons play sound 0x98 even at the end of the range.
//
// Port addition: an "ameliorations" button (line 9, left of "quit", a text
// button with a "+" icon in the colours of the quit one) opens the
// enhancements window (enhancements_screen.hpp), which holds the controller
// screen and gets every event until it closes. Left from "quit" and right
// from the button move between them; Enter opens it. Without fonts the
// button is absent.

#include "core/game_config.hpp"
#include "core/surface.hpp"
#include "engine/ui/enhancements_screen.hpp"
#include "engine/ui/gamepad_screen.hpp"
#include "engine/world/render.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace crusader {

class Audio;

class OptionsScreen {
public:
    enum Widget : int {
        kAnimationOn = 0x50, kAnimationOff, kFrameSkipOn, kFrameSkipOff, kFilterOn, kFilterOff,
        kVideoBig, kVideoSmall, kBlastsOn, kBlastsOff,
        kSoundDown = 0x5C, kSoundUp, kMusicDown, kMusicUp, kMouseDown, kMouseUp, kQuit,
    };
    static constexpr int kWidgets = 17;
    static constexpr int kQuitLine = 8;
    static constexpr int kPadLine = 9;  // the controller button
    static constexpr int kSteps = 23;
    static constexpr std::array<std::uint8_t, kSteps> kSoundSteps = {  // DS:5BB6
        0x00, 0x02, 0x05, 0x08, 0x0B, 0x0E, 0x11, 0x14, 0x17, 0x1A, 0x1D, 0x20,
        0x22, 0x25, 0x28, 0x2B, 0x2E, 0x31, 0x34, 0x37, 0x3A, 0x3D, 0x40};
    static constexpr std::array<std::uint8_t, kSteps> kMusicSteps = {  // DS:5BCD
        0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0D, 0x0F, 0x11, 0x13, 0x15, 0x18,
        0x1A, 0x1C, 0x1E, 0x20, 0x22, 0x25, 0x27, 0x29, 0x2B, 0x2D, 0x30};
    static constexpr int kClickSound = 0x98;

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // `focus` is DS:5BEA; `audio` may be null (no sound).
    OptionsScreen(ShapeCache& gumps, GameConfig& config, Audio* audio, int& focus, const FontSet* fonts = nullptr,
                  std::function<void()> vibration_preview = {});

    void draw(Surface8& screen) const;

    // Gump key code (see gump_keys.hpp).
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    void mouse_move(int x, int y);
    void mouse_up(int x, int y);
    // Timer ticks, at every pass of the modal loop: the repeating buttons.
    void update(std::uint32_t ticks);
    // The controller screen waits for a button press to bind (see Gamepad::set_capture).
    bool waiting_for_pad() const { return enh_ && enh_->waiting(); }
    void pad_button(int button) {
        if (enh_) enh_->pad_button(button);
    }

    bool closed() const { return closed_; }
    int focus() const { return *focus_; }
    const EnhancementsScreen* enhancements() const { return enh_.get(); }
    const GamepadScreen* gamepad() const { return enh_ ? enh_->gamepad() : nullptr; }
    Rect enhancements_button() const { return pad_rect_; }
    int sound_step() const { return sound_step_; }
    int music_step() const { return music_step_; }
    // Whether a two-choice widget is chosen, or another widget drawn pressed
    // or selected (frame 1).
    bool chosen(Widget w) const;
    bool pressed(Widget w) const;
    Rect rect(Widget w) const { return widgets_[index(w)].rect; }

private:
    struct Item {
        Widget id;
        std::uint16_t shape;
        Rect rect;
    };
    static int index(Widget w);
    static bool is_choice(Widget w) { return w < kSoundDown; }
    static bool is_repeating(Widget w) { return w >= kSoundDown && w < kQuit; }
    static int line_of(Widget w);

    void draw_pad_button(Surface8& screen) const;
    void open_enhancements();
    void act(Widget w);           // Options_key
    void widget_acted(Widget w);  // 13D0:1D81
    const Item* hit(int x, int y) const;
    void draw_shape(Surface8& screen, std::uint16_t shape, int frame, int x, int y) const;

    ShapeCache* gumps_;
    GameConfig* config_;
    Audio* audio_;
    int* focus_;
    std::array<Item, kWidgets> widgets_{};
    int sound_step_ = 0;
    int music_step_ = 0;
    bool quit_selected_ = false;
    int captured_ = -1;          // widget holding the mouse (repeating buttons, quit)
    bool inside_ = false;        // the mouse is over the captured widget
    std::uint32_t now_ = 0;
    std::uint32_t next_repeat_ = 0;
    bool closed_ = false;
    const FontSet* fonts_ = nullptr;
    std::function<void()> vibration_preview_;
    Rect pad_rect_{352, 318, 503, 339};
    bool pad_pressed_ = false;  // the mouse holds the enhancements button
    int pad_focus_ = 0;         // the controller screen's row, kept between openings
    std::unique_ptr<EnhancementsScreen> enh_;
};

}  // namespace crusader
