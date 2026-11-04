#pragma once
// NPC activities and combat (NPC.C 10F0, ATTACK.C 1110, SPRITE 1150:009C,
// CRUSADER.C 1130:1755). See docs/combat.md, "L'IA des ennemis".
//
// An NPC's activity (record +2, previous one at +4) says what it does:
// Actor::I_setActivity ends its current animations and activity processes,
// then starts the process of the new activity. Activities 5, 9, 10, 11 and
// 12 are combat: the NPC is flagged "in combat" (record +0x59 bit 3) and an
// AttackProcess (type 0x259) runs its combat tactic (record +0x5C; 0 = the
// built-in behaviour below, others are COMBAT.DAT scripts, not reproduced).
//
// The other activities are in activity.hpp. COMBAT.DAT tactics (AttackProcess
// 1110:06B9, tactics 1 to 31) are not reproduced: the game only ever sets
// tactic 0 (Actor_I_createActorCru, MonsterEgg_spawn, and the NPC usecode
// class through Actor::I_setCombatTactic), so they never run. A bark (by shape, at the first round one time in three, and
// later for shapes 0x2DF, 0x383) is a sound at the NPC; while it plays, the
// NPC does not aim or fire.

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

// NPC record fields used by activities and combat.
namespace npc {
constexpr std::size_t kActivity = 0x02;          // u16
constexpr std::size_t kPreviousActivity = 0x04;  // u16
constexpr std::size_t kDefaultActivity = 0x06;   // u16 x 3 (Actor::I_getDefaultActivity0..2)
constexpr std::size_t kCombatFlags = 0x59;       // bit 3 in combat, bit 5 hit reaction allowed
constexpr std::size_t kStance = 0x5A;            // bit 0 activity locked, bit 1 weapon drawn
constexpr std::size_t kTactic = 0x5C;            // u16, COMBAT.DAT entry
constexpr std::size_t kEggQuality = 0x63;        // byte, the creating egg's quality low (Actor::I_getUnkByte)
constexpr std::size_t kTarget = 0x64;            // u16
constexpr std::size_t kLongBurst = 0x68;         // byte, set during a long burst
constexpr std::size_t kLastHit = 0x69;           // u32, time of the last hit
}  // namespace npc

bool in_combat(const World& world, ObjId npc);   // Actor::I_isInCombat
bool weapon_drawn(const World& world, ObjId npc);
// NPC_10f0_1f40: the NPC's last action is a combat one (not 0, 1, 0x3A,
// 0xB or 0x10).
bool in_combat_stance(const World& world, ObjId npc);
// CRUSADER_1130_1180: the controlled NPC has not started a moving action
// lately (see GameState::player_moved_at).
bool controlled_npc_still(const World& world, std::uint32_t now);

// Actor::I_setActivity (10F0:2D02). 0x70..0x72 stand for the default
// activities 0..2. Nothing happens for a dead or locked NPC (+0x5A bit 0),
// the controlled NPC, activity 0, the current activity, or an NPC in combat
// unless the new activity is 12. At difficulty 4, 5 and 9 become 10 and 11
// half of the time.
void set_activity(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, std::uint16_t activity);
// Actor::I_setTarget (10F0:1E71): an NPC that is not in combat ends its
// attack process and takes activity 5; the target is stored (+0x64).
void set_target(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, ObjId target);
// Actor::I_clrInCombat (10F0:31A9): holsters a drawn weapon (action 0xB) and
// ends the attack process.
void clear_in_combat(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc);
// World::I_setControlledNPCNum (CRUSADER_1138_2c02): the player's commands
// go to `npc` (a robot, or 0 for none). The previous one, alive, leaves
// combat; the camera follows the new one and jumps to it; a new NPC other
// than the player has its processes stopped and leaves combat. The aiming
// mode is left, its angle reset, and DS:2864 notes whether the new shape
// lacks action 1 (aim_mode.hpp). Not reproduced: the cursor frame.
void set_controlled_npc(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc);

// Item::I_enterFastArea for an NPC (ITEM.C 2CA8): activity and combat state
// reset, standing pose, then its activity is set again.
void restart_activity(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc);

// SPRITE_1150_009C: turns `npc` to `dir` one step at a time, queueing its
// standing action in each direction. A turn by one step is skipped unless
// `always`. `relaxed` picks the non-combat stance. Returns the last pid.
Pid turn_to(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, int dir, bool always, bool relaxed);

// Item::I_fireDistance (1130:1755): can `shooter` facing `dir` hit `target`?
// For an NPC the muzzle points come from the firing frames of its shot
// action in that direction (the first two); otherwise (dx, dy, dz) is used.
// From each point, unless something other than the target is there, a sweep
// with shape 0x1D9 goes to the aim point of the target; when it meets
// nothing or the target, the result is the distance in x and y
// (Chebyshev) over 16, so 0 under 16 units. 0 when no point sees the target.
// `muzzle_found` (DS:2746) tells whether an NPC shooter had firing frames.
int fire_distance(const World& world, const AnimDat& anims, ObjId shooter, ObjId target, int dir, int dx = 0,
                  int dy = 0, int dz = 0, bool* muzzle_found = nullptr);

// The combat process of an NPC (ATTACK.C, type 0x259), built-in tactic 0.
class AttackProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0x259;

    AttackProcess(World& world, const AnimDat& anims, ObjId npc);

    // ATTACK_1110_1485 (activity 12): fight for 8..17 seconds, then
    // activity 5.
    void set_timed(std::uint32_t now, int seconds_minus_8);
    void set_stationary() { stationary_ = true; }        // +0x7B, activities 9 and 11
    void set_cautious() { cautious_ = true; }            // +0x7C, activities 10 and 11
    void set_from_activity_8() { from_activity_8_ = true; }  // +0x97
    void force_sight() { forced_sight_ = true; }             // ATTACK_1110_252d
    int burst_left() const { return burst_left_; }
    Pid listening() const { return listening_; }

protected:
    void run() override;

private:
    bool recently_started(std::uint32_t now);          // ATTACK_1110_03aa
    bool bark_timer(std::uint32_t now);                // ATTACK_1110_0334
    void play_bark(std::int16_t sfx);                  // +0x88 = sfx, I_playSFXCru
    bool bark_playing();                               // else +0x88 = -1
    bool can_hit(int dir, std::uint32_t now);          // ATTACK_1110_13b7
    void approach();                                   // ATTACK_1110_141b
    void note_sighting(std::uint32_t now);             // ATTACK_1110_24a5
    bool sighting_old(std::uint32_t now) const;        // ATTACK_1110_24c6
    void fire(std::uint32_t now, ObjId weapon);
    void next_burst_shot();                            // message handler 1110:22CA
    bool bark(std::uint16_t shape);
    int target_direction(ObjId target, int dirs) const;

    World& world_;
    const AnimDat& anims_;
    std::uint32_t started_ = 0;      // +0x6F, when the weapon was drawn / first shot
    std::uint32_t seen_ = 0;         // +0x73
    std::uint32_t timeout_ = 0;      // +0x77
    std::uint32_t bark_time_ = 0;    // +0x84
    std::uint32_t reference_ = 0;    // +0x8A, copy of +0x6F
    std::uint32_t last_shot_ = 0;    // +0x90
    std::uint16_t first_delay_ = 0;  // +0x8E, after drawing the weapon
    std::uint16_t shot_delay_ = 0;   // +0x94, between bursts
    int burst_left_ = 0;             // +0x81
    Pid listening_ = 0;              // animation whose end fires the next shot of a burst
    std::int16_t sound_ = -1;        // +0x88
    bool stationary_ = false;        // +0x7B
    bool cautious_ = false;          // +0x7C
    bool double_time_ = false;       // +0x7D (set by COMBAT.DAT scripts only)
    bool sighted_ = false;           // +0x7E
    bool forced_sight_ = false;      // +0x7F
    bool timed_ = false;             // +0x80
    bool barked_ = false;            // +0x83
    bool moved_ = false;             // +0x96, came back from moving
    bool from_activity_8_ = false;   // +0x97
};

}  // namespace crusader
