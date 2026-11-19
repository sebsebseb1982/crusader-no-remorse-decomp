#pragma once
// The mouse in play: the game view's mouse methods (WORLD.C, vtable DS:6243:
// +4 idle 13E8:307F, +8 left button 2B74, +0xC right button 2DAD, +0x10
// motion 2A13). The cursor stays hidden; the mouse drives the controlled
// NPC by sending key events to the Controller (DS:284E +0x28), as the
// keyboard does (CombatControl::handle_key):
//
// - Motion (out of the item selection): the mouse goes back to (320, 200)
//   and dx = 320 - x. Unless the player is busy (DS:7E10), the rate is |dx|
//   >> (3 - speed) in a combat stance (1138:0070), else |dx| >> (7 - 2 x
//   speed), speed being the options' mouse speed (DS:6001, 0..3). A non-zero
//   rate sends a press of the left arrow (dx > 0) or the right one (E0,
//   count 1, the BIOS shift state), flagged as coming from the mouse
//   (DS:5FF6) with the rate (DS:5FF8): the crosshair turns by rate / 2
//   degrees (at most 20), a walk turns at most every 60 / (3 speed + 3)
//   ticks. Then, walking or firing, the idle method runs.
// - Right button: the press (after the double-click delay, mouse_queue.hpp)
//   starts walking (view flag 1); in a combat stance with a button held, the
//   NPC first stands up if kneeling (0x29) and holsters (0xB). The release
//   ends it with a release of the up arrow (E0). A double click, when the
//   NPC is not busy, holsters in a combat stance (standing up first), else
//   draws (7).
// - Left button: a press or a double click starts firing (view flag 8); the
//   release sends a release of Space once and ends it. In the item
//   selection, a press picks or uses the selected item (selection_click).
// - Idle (every input pass with no event): walking, when the NPC is not
//   busy, a press of the up arrow (E0; Alt held in a combat stance, so 0x24);
//   firing, a press of Space (count 1) when the controlled NPC is not busy
//   (SPRITE_1150_05E8).
//
// The player in stasis (DS:7E10, being the controlled NPC) ignores the
// buttons, except the release that ends a walk.
//
// The "hack mover" (DS:5FFA, H with the cheats, cheats.hpp): the mouse moves
// the cursor and no longer turns the NPC, and the left button moves items
// (the mover object DS:8C1C, DEBUGUMP.C, in "existing item" mode):
// - a press (or a double click) with a button held, the view doing nothing
//   (no flag), takes the item drawn on top under the mouse (DLIST 1198:0E71,
//   ITEM 10A8:3042); the mouse's offset from the item's place on screen
//   ((x >> 1) - (y >> 1), (x >> 2) + (y >> 2) - z) is kept; the items resting
//   on it fall and those it rests on are released (ITEM.C 2970); view flag
//   2; then a motion at once;
// - each motion (the mover still on) takes the item out of the world and
//   puts it back where the mouse less the offset points to (target), if its
//   box is free there (ITEMFIND.C 00D9), else where it was;
// - the release ends it: a stack (families 2 and 9) once put down somewhere
//   is merged with an item of its shape drawn under the mouse of its last
//   good place, set down at that item's place (SHAPHAND 1088:0000); an item
//   out of any container then falls (Item::I_fall).
// The port keeps the mouse relative in this mode, as the DOS driver: the
// cursor moves by the motion, over the 640 x 480 window centred in the view.
//
// Not reproduced: the mover's start clears the player's equipment slot 7
// (an ammunition slot in Crusader: a leftover of Ultima 8's held item), and
// its end moves the slot 7 item into the slot 6 one when the moved item was
// in an NPC; view flags 4 and 0x40 (never set) and the item under the mouse
// noted on every left press (DS:8C3D) for them; the "both buttons" event
// (+0x24 does nothing).

#include "core/anim_dat.hpp"
#include "engine/input/keyboard.hpp"
#include "engine/input/mouse_queue.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <functional>
#include <utility>

namespace crusader {

// What the hack mover needs of the view, in mouse coordinates.
struct MouseView {
    // DLIST 1198:0E71 then ITEM 10A8:3042: the item drawn on top at (x, y), 0
    // for none (the items out of the world are not drawn).
    std::function<ObjId(int x, int y)> item_at;
    // The scroll (DS:2BE9, DS:2BEB): the projected place, (x - y) / 2 and
    // (x + y) / 4 - z, of the mouse's (0, 0).
    std::function<std::pair<int, int>()> origin;
};

class WorldMouse {
public:
    static constexpr int kCentreX = 320, kCentreY = 200;
    using SendKey = std::function<void(const GameKeyEvent&)>;

    WorldMouse(Kernel& kernel, World& world, const AnimDat& anims, const Keyboard& keyboard, SendKey send,
               MouseView view = {})
        : kernel_(kernel), world_(world), anims_(anims), keyboard_(keyboard), send_(std::move(send)),
          view_(std::move(view)) {}

    // An event of the mouse queue. Returns true when the mouse goes back to
    // (kCentreX, kCentreY).
    bool event(const MouseInput& e);
    // The idle event (+4) at the mouse position.
    void idle();

    bool walking() const { return walking_; }
    bool firing() const { return firing_; }
    // View flag 2: the hack mover holds `held()`.
    bool dragging() const { return dragging_; }
    ObjId held() const { return mover_.item; }

    // DEBUGUMP 13B0:0837 with ANIMCACH 1188:0467 / 049E: where the held item goes for
    // the mouse at (x, y) (usecode coordinates).
    World::Point target(int x, int y) const;

private:
    // The mover (DS:8C1C).
    struct Mover {
        ObjId item = 0;                  // +2
        int grab_x = 0, grab_y = 0;      // +0x14: the mouse less the item's place on screen
        int last_x = -1, last_y = -1;    // +0x18: the mouse of the last move made
    };

    bool motion(const MouseInput& e);
    void left(const MouseInput& e);
    void right(const MouseInput& e);
    void send_arrow(std::uint8_t scancode, bool pressed, std::uint16_t flags);
    void send_fire(bool pressed);
    bool player_held() const;
    void grab(int x, int y);
    void drag(int x, int y);
    void drop();
    ObjId item_at(int x, int y) const { return view_.item_at ? view_.item_at(x, y) : 0; }
    std::pair<int, int> origin() const { return view_.origin ? view_.origin() : std::pair<int, int>{0, 0}; }
    // ANIMCACH 1188:049E: the map point under the mouse at (x, y); `hit` gets the
    // item drawn there and `inside` whether the point is within its box.
    void under(int x, int y, int& wx, int& wy, int& wz, ObjId* hit, bool* inside) const;

    Kernel& kernel_;
    World& world_;
    const AnimDat& anims_;
    const Keyboard& keyboard_;
    SendKey send_;
    MouseView view_;
    bool walking_ = false;   // view flag 1
    bool firing_ = false;    // view flag 8
    bool dragging_ = false;  // view flag 2
    Mover mover_;
};

}  // namespace crusader
