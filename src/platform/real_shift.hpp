#pragma once
// Port addition (issue #14): Shift as the keyboard really reports it, on Windows.
//
// With Num Lock on, Windows turns the keypad digits into arrows while Shift
// is held: it sends a fake Shift release before each keypad key goes down and
// a fake Shift press after it comes up. In the key messages these look like
// the real key (scancode 0x2A / 0x36, not extended), so SDL passes them on and
// the game saw Shift let go: Shift + keypad 8 then keypad 6 walked instead of
// running, and ran again when 6 came up. Under DOS the keyboard sent no such
// thing for the keypad, so the original ran all along.
//
// Raw Input reports the fake ones too, but with the scancode of the keypad
// key (0x48, 0x4D...) or the E0 prefix, never a plain 0x2A / 0x36. Shift is
// followed from those reports: SDL's own Shift events are dropped, one is sent
// for each real change, and the Shift bits of every key event are replaced.

#include <cstdint>
#include <optional>
#include <vector>

namespace crusader {

class RealShift {
public:
    struct Change {
        bool right = false;  // right Shift (0x36), else left (0x2A)
        bool pressed = false;
        bool repeat = false;  // a press while already held (typematic)
    };

    // One Raw Input keyboard report (RAWKEYBOARD: VKey, MakeCode, Flags).
    // Returns the change to send, nothing for a fake Shift or another key.
    std::optional<Change> raw_key(std::uint16_t vkey, std::uint16_t make_code, std::uint16_t flags);
    // The SDL modifiers with the Shift bits of the keys really held.
    std::uint16_t apply(std::uint16_t sdl_mod) const;
    // Lets go of both (the window lost the keyboard); the releases to send.
    std::vector<Change> release_all();

    bool left() const { return held_[0]; }
    bool right() const { return held_[1]; }

private:
    bool held_[2] = {false, false};
};

// Windows: registers the keyboard for Raw Input and installs the SDL message
// hook and event filter doing the above. Call once the video is initialised.
// Elsewhere, and if the registration fails, does nothing.
void install_real_shift();

}  // namespace crusader
