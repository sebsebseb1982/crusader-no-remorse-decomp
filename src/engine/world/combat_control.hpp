#pragma once
// Firing the controlled NPC's weapon: the fire part of the "Controller"
// process (CRUSADER.C, segment 1138; type 0x247, 60 Hz, keyboard messages).
//
// Keys (message handler 1138:220A): space, or keypad Ins (scancode 0x52
// without E0), only once the movement handler (AvatarMover, 1138:12CF) has not
// used the event. No key does anything (movement included) while the
// controlled NPC is the player in stasis (DS:7E10, GameState::avatar_in_stasis).
// - While a shot is under way (DS:2856), pressing them again does nothing.
// - Press: the first press notes the time and clears "shot since the press"
//   (DS:287A); if the NPC is not busy, it fires (CRUSADER_1138_2789 with
//   explicit = 1) when it stands in a combat stance (last action other than
//   0, 1, 0x3A, 0xB, 0x10), otherwise it raises its weapon (action 7).
// - Release: fire with explicit = 0.
// Nothing happens on map 40 (NPC_10f0_0122).
//
// Fire (CRUSADER_1138_2789): DS:2854 says a shot is wanted; it is set at the
// end of each call to `explicit` (so the first press only arms it) and the
// shot happens on the next call: a key repeat (KeyDaemon, after 10 ticks)
// while the key is held, or the release when no shot happened since the press
// (a tap fires once). With an active weapon:
// - no round left (rounds()) and a clip of its ammunition in the equipment:
//   reload (NPC_10f0_3ba3), then wait 15 ticks; no clip: a click (sound
//   0x2A), DS:2854 cleared, wait 20 ticks;
// - weapon 0x332 (the shotgun, shape 818) not yet cocked (DS:2879), player
//   standing: action 0xA (reload / pump, replacing the current ones), cocked;
// - otherwise the shot: action 8 (0x2A kneeling), one round used
//   (NPC_10f0_3ab2), then a wait of the weapon's shot delay; a burst of n > 1
//   arms n - 1 more shots, fired by the 60 Hz timer (message 2) once the NPC
//   is free again.
// "Wait" is Process_sleep on the controller: the "shot under way" flag
// clears when it wakes (message 4).
//
// Rounds (NPC_10f0_3a1f): the weapon's quality for weapons using ammunition;
// energy / energy per shot for energy weapons; 1 (unlimited) for weapons with
// neither (the starting pistol). A shot (3ab2) takes one from the quality or
// the energy. Reloading (3ba3): weapon quality = clip size, clip count - 1,
// clip destroyed at 0, then action 0xA if the player stands.
//
// A shot wakes the NPCs on screen (wake_npcs_on_screen).
//
// Sounds, restarted on the NPC's voice: the click, the reload sound of the
// weapon (DTABLE +0xF) when reloading and when pumping the shotgun; the shot's
// sound comes with the firing frame (Game).
//
// Other keys of the handler, also once the movement handler has not used
// them, on their first press only (player_commands.hpp): keypad * the next
// weapon (13E8:0C72), keypad - the next item (13E8:0DC0, the player being
// controlled, DS:285A), keypad Del a 'u' for the game view (World_on_key:
// use the active item), keypad + an 's' (the item selection mode). Tab and
// J, at every repeat: a jump (jump).
//
// Its run (1138:087F), at every slice while it is not asleep, places the
// crosshair of the aiming mode again when an aiming action asked for it
// (aim_mode.hpp). Actions of the controlled NPC that leave the aiming mode
// also reset the fire state (reset_fire, 1138:2F9F).
//
// Not reproduced: whether the controller's sleep also delays its timer
// messages (assumed: no timer message while asleep). The AutoFirer that
// 2F9F ends is in auto_firer.hpp.

#include "core/anim_dat.hpp"
#include "engine/input/keyboard.hpp"
#include "engine/kernel.hpp"
#include "engine/world/avatar_mover.hpp"
#include "engine/world/world.hpp"

namespace crusader {

class CombatControl : public TimedProcess {
public:
    static constexpr std::uint16_t kType = 0x247;

    CombatControl(World& world, const AnimDat& anims, const Keyboard& keyboard, AvatarMover& mover);

    // The controller's keyboard message: movement first, then fire keys.
    void handle_key(const GameKeyEvent& e);

    // NPC_10f0_3a1f: shots left before reloading.
    static int rounds(const World& world, ObjId npc);

    bool shot_under_way() const { return firing_; }
    bool shot_wanted() const { return wanted_; }

    // Controller_fire_reset (1138:2FD4): no shot wanted or under way, no
    // burst, timer off.
    void reset_fire();

protected:
    void run() override;
    void tick() override;

private:
    bool keypad_command(const GameKeyEvent& e);
    bool fire(bool explicit_call);
    void use_round(ObjId npc);
    void reload(ObjId npc);
    // A sound restarted on the NPC's voice (I_playSFXCru 0x045); 0: none.
    void play_at(ObjId npc, int sfx);
    void sleep_ticks(std::uint32_t ticks);
    bool fire_key_down() const;

    World& world_;
    const AnimDat& anims_;
    const Keyboard& keyboard_;
    AvatarMover& mover_;

    bool wanted_ = false;         // DS:2854
    bool firing_ = false;         // DS:2856
    bool single_ = false;         // DS:2857, set while a timer message fires
    int burst_left_ = 0;          // DS:2858
    bool shot_since_press_ = false;  // DS:287A
    bool cocked_ = false;         // DS:2879, weapon 0x332
    bool timer_on_ = false;       // timed messages enabled (01A5 / end of burst)
    std::uint32_t wake_at_ = 0;
    // DS:2871, when the NPCs on screen were last woken (the original starts
    // from 0 on the game clock; here the first shot always wakes them).
    bool woke_ = false;
    std::uint32_t last_wake_ = 0;
    bool asleep_ = false;
};

}  // namespace crusader
