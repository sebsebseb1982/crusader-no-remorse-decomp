#pragma once
// Keyboard control of the controlled NPC (CRUSADER.C 12CF, called for every
// key event; 10C8 turning in targeting modes, 0F8D redirectable actions).
//
// Arrow keys (E0-extended) steer relative to the actor: up walks (runs with
// shift; caps lock swaps), down retreats, left / right turn. The keypad gives
// absolute screen directions (8 up-left = 14, 6 = 2, 2 = 6, 4 = 10, 7 = 12,
// 9 = 0, 1 = 8, 3 = 4), relative ones with Num Lock. Ctrl and Alt select
// other moves (rolls, side steps, kneeling...). Held keys keep the actor
// moving through the KeyDaemon's repeated events: each event starts the next
// animation when the actor is idle, or turns a running walk. Releasing the
// last movement key queues the matching stand.
//
// In a combat stance, the arrows (and the keypad ones with Num Lock) turn
// the crosshair of the aiming mode (10C8, aim_mode.hpp); the keypad 4 / 6
// and the screen directions turn the NPC with action 4, its animation
// leaving the aiming mode until it is over (0692 / 2E55).
//
// The mouse sends arrow keys too (world_mouse.hpp); while it does
// (GameState::mouse_turn, DS:5FF6), a running walk turns at most once every
// 60 / (speed * 3 + 3) ticks (0F8D). Weapon-dependent
// actions are remapped by do_anim (remap_action); firing is in
// CombatControl, which calls this handler first.

#include "core/anim_dat.hpp"
#include "engine/input/keyboard.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

namespace crusader {

class AvatarMover {
public:
    AvatarMover(Kernel& kernel, World& world, const AnimDat& anims, const Keyboard& keyboard)
        : kernel_(kernel), world_(world), anims_(anims), keyboard_(keyboard) {}

    // Returns true when the event was used.
    bool handle_key(const GameKeyEvent& e);

    ObjId actor() const { return world_.state.controlled_npc; }

    // Port addition (directional movement, GameConfig::directional_move): at
    // a release, the keypad's diagonals (7, 9, 1, 3) count as movement keys
    // still held. The original counts the gray Home, PgUp, End and PgDn
    // only, so turning from a keypad 8 to a keypad 7 (the new key down, then
    // the old one up) stopped the actor, and the KeyDaemon's repeat of the
    // 7 started him again: a run broke off at each turn to a diagonal.
    void set_keypad_diagonals_held(bool on) { keypad_diagonals_held_ = on; }

private:
    std::uint16_t last_anim() const;
    bool kneeling() const;

    Kernel& kernel_;
    World& world_;
    const AnimDat& anims_;
    const Keyboard& keyboard_;
    bool keypad_diagonals_held_ = false;
};

// CRUSADER.C 0F8D: actions whose direction may change while they run; a
// walk turned by the mouse is rate-limited (and the time noted).
bool action_redirectable(GameState::MouseTurn& mouse, std::uint32_t now, std::uint16_t action);

// CRUSADER.C 0070: the last animation of `id` is none of standing, walking,
// running (and starting to run), the jumps 0x3A and 0x37, or 0xB / 0x10
// (holstering): in practice a combat stance.
bool stance_not_moving(const World& world, ObjId id);

}  // namespace crusader
