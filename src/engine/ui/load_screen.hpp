#pragma once
// The load / save screen (segment 1008, FUN_1008_0088), opened by the main
// menu's "load" (mode 1) and "save" (mode 0) buttons, F8 and F9.
//
// Layout: the screen is cleared; background GUMPS.FLX shapes 64 + 5 * mode ..
// 67 + 5 * mode (load: 69..72, save: 64..67, 208 x 174 each) at (111, 66), (319, 66),
// (111, 240), (319, 240) (FUN_1008_05a6: a 417 x 348 area centred on the
// screen). Eleven original slots (FUN_1008_0758), slot i at (183, 109 + 27 i):
// shape 68 frame 0 (314 x 17); the selected one (DS:0098, 0 at start-up, kept
// between screens) gets an outline of colour 0xFD around its rectangle; the
// slot name is written in font 14 at (+4, +12) (FUN_1008_0c9a). The load screen
// adds SAVEGAME.12 (the autosave) as a twelfth row at y=406; the save screen
// keeps the original eleven. Palette: MISC.PAL.
//
// Slot i is SAVEGAME/SAVEGAME.<i + 1>; its name is the 32-byte entry 9 of
// that file, read at start-up into DS:7964 (CRUSYS_1058_000d, 13 names; an
// absent file gives an empty name). The original writes the name with printf
// as the format string; the port writes it as is.
//
// Keys (FUN_1008_0312): up / '8' select the previous slot (not before 0), down
// / '2' the next (not past the last visible row), Enter chooses the selected
// slot, Escape closes with result 0. On a gamepad, directions and confirm map
// to the same inputs. Mouse (FUN_1008_0aea): a button press on a slot selects
// and chooses it. Loading: choosing a slot with an empty name does nothing
// (FUN_1008_0b63); otherwise the slot goes to DS:0C94 and the screen closes
// with result 1.
//
// Saving: choosing a slot edits its name (FUN_1008_0b63, FUN_1008_089c),
// starting from the name it has, with '_' drawn after the last character;
// the mouse does nothing meanwhile. Escape leaves the name as it was; Enter,
// with at least one character, gives the name (CRUSYS_1058_0528) and closes
// with result 1 (the caller saves; FUN_1008_041a first checks the free disk
// space, see Game). Backspace removes the last character. A character is
// added if it is a letter, a digit or one of < space : ; . ? ! , / " ' (the
// US version leaves out < : ;) (codes below 0x100), the name has fewer than
// 25, and the name with '_' still fits in the slot less 4 pixels; letters
// become capitals. The French original also turns QWERTY into AZERTY
// (FUN_1008_0000; not in the US one) because its BIOS gave QWERTY codes; the
// port's codes already follow the player's layout, so it does not.
// Not reproduced: the mouse cursor hidden while a name is edited.
//
// Port addition, for consoles without a keyboard (the PortMaster build, see
// set_auto_name): choosing a slot to save proposes a name made by the caller
// (the date and time, dated_name) instead of the slot's; Enter (the pad's A)
// gives it, Escape (B) leaves the slot as it was, and a keyboard plugged in
// can still edit it.

#include "core/font.hpp"
#include "core/surface.hpp"
#include "engine/world/render.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace crusader {

class LoadScreen {
public:
    static constexpr int kSaveSlots = 11;
    static constexpr int kSlots = 12;
    static constexpr int kNames = 13;  // DS:7964 .. DS:7998
    static constexpr std::uint16_t kSlotShape = 68;
    static constexpr std::uint8_t kSelectedColour = 0xFD;
    static constexpr int kFont = 14;

    using Names = std::array<std::string, kNames>;

    struct Rect {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive
        bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    };

    // CRUSYS_1058_000d: the names of SAVEGAME.1 .. SAVEGAME.13.
    static Names read_names(const std::filesystem::path& savegame_dir);

    enum class Mode { kSave = 0, kLoad = 1 };
    static constexpr std::size_t kMaxNameLength = 25;

    // `selected` is DS:0098, kept by the caller.
    LoadScreen(ShapeCache& gumps, const FontSet& fonts, const Names& names, int& selected, Mode mode = Mode::kLoad);

    void draw(Surface8& screen) const;
    void key(std::uint16_t code);
    void mouse_down(int x, int y);
    // Port addition: the name proposed when a slot is chosen to save (an
    // empty function keeps the original's, the slot's own name).
    void set_auto_name(std::function<std::string()> auto_name) { auto_name_ = std::move(auto_name); }
    // The proposed name: the local date and time, "DD/MM/YYYY HH:MM" (French)
    // or "MM/DD/YYYY HH:MM" (US); "SAUVEGARDE" / "SAVED GAME" when the clock
    // is plainly not set (a console without a real-time clock starts in 1970).
    static std::string dated_name(const std::tm& local);

    // Set once closed: 0 (cancelled) or 1 (slot chosen, see slot()).
    std::optional<int> result() const { return result_; }
    int slot() const { return *selected_; }  // DS:0C94 when result() is 1
    Mode mode() const { return mode_; }
    // Saving: the name being edited (editing()), then the name given.
    bool editing() const { return editing_; }
    const std::string& name() const { return name_; }
    const Rect& slot_rect(int i) const { return slots_[i]; }

private:
    int visible_slots() const { return mode_ == Mode::kLoad ? kSlots : kSaveSlots; }
    void choose(int i);
    void edit_key(std::uint16_t code);

    ShapeCache* gumps_;
    const FontSet* fonts_;
    const Names* names_;
    int* selected_;
    std::array<Rect, kSlots> slots_{};
    std::optional<int> result_;
    Mode mode_;
    bool editing_ = false;
    std::string name_;
    std::function<std::string()> auto_name_;
};


}  // namespace crusader
