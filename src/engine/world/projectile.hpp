#pragma once
// Shots: Item::I_fireWeapon (CRUSADER.C 1130:11DA), the projectiles
// ("SuperSprite" processes, SPRITE.C segment 1140, type 0x250) and the damage
// they deal (CRUSADER.C 1130:026B). See docs/combat.md.
//
// fire_weapon: the shot starts at the shooter's position (its outermost
// container's) plus the muzzle offset of the firing frame (x, y in 16ths of
// the frame value, z in 8ths). If that point is already inside an item
// (search with shape 0x1D9), the item is hit at once. Otherwise the fire type
// gives the number of projectiles. With aiming asked, the target is
// find_target's for the controlled NPC, the controlled NPC for other
// shooters; each projectile flies towards aim_point of the target, or
// without one towards a point 0x500 direction steps straight ahead
// (SPRITE_1140_0e01). A random spread is added when aiming and the fire type
// allows it (SPRITE_1140_09c9):
//   spread = Chebyshev distance / k, at most 80; x and y move by up to
//   +-spread, z by up to +-spread / 3 (at most 24) and stays in 0..250.
//   k for the controlled NPC: 8 (types 2, 13), 12 (5), 5 (10), 10 otherwise.
//   For an NPC: 2 when its record +0x68 is set; 8 when the controlled NPC has
//   not moved lately; otherwise the movement divisor (2 or 3, see
//   GameState::player_moved_at). For other shooters: 5 when the controlled
//   NPC kneels, 3 x the movement divisor otherwise.
// The damage of the shot is drawn once from the fire type's range.
//
// Projectile (run every 2 ticks, 3 for type 9): step i puts it at
//   start + (target - start) * i / n,  n = (Chebyshev distance / 16) / speed
// (+1 when that distance / 16 is not a multiple of 16 or is 0). Each step is
// swept from the previous position with shape 0x1D9; a clear sweep moves the
// projectile on. Blocked, at z 0 or 250, or beyond its range (range + 25
// steps), it hits: the item hit, if any, takes the damage (passing through
// its own shooter during the first 5 steps), then the projectile goes.
//
// Damage (1130:026B, reproduced part): the difficulty (DS:7E19) multiplies
// the damage taken by other NPCs by 5 (level 1) or 3 (level 2) and divides
// the damage taken by the controlled NPC / the player by the same; robots
// (shapes 0x4C8, 0x338, 0x45D, 0x2CB, 0x4E6, 0x383, 0x385) take a third of
// fire types 1, 2, 11 and 13; the result is kept in 1..250.
// - An item that is not an NPC gets usecode event 6 ("gotHit") with the
//   words 0x4000 and (fire type << 8) | damage, when its class has it.
// - A living NPC loses that many hit points (record +0, not below 0) and
//   notes the time (+0x69). At 0 its processes stop, it plays its death
//   (action 0x14, falling forward, when the shot came from behind: projectile
//   direction within 3 of its own; 0x12 when it came from the front; either
//   at random at 4; 0x12 for shapes without action 0x14) and becomes dead
//   (record +0x13 bit 0, status 0x8000).
//
// Fire type 9 is guided (set_homing) and type 5 bounces off some items
// (ricochet), see projectile.cpp. Splash damage, impact sprites, items (hit_item) and the NPCs' reaction
// are in item_damage.hpp, sprite_process.hpp and attack.hpp.
//
// Not reproduced: the aiming mode of DS:2863 (SPRITE_1140_0e88), the target
// shift of fire types 2 and 13 (written after the steps are set, unused by
// the straight flight), trails, the exact refinement of the impact point in steps of 16
// (the sweep's stopping point is used), pushing items (types 3, 4), special
// deaths (explosion, burning, electrocution: types 3, 4, 6, 10, 12, 14,
// 15) beyond the shape change. The
// projectile is drawn as a world item of shape 0x1D9 instead of a sprite.

#include "core/anim_dat.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

constexpr std::uint16_t kProjectileShape = 0x1D9;

// TRACKER_1128_0158: what the controlled NPC aims at when firing in `dir`.
// Tracked items (World::tracked, in slot order) other than the shooter and
// partly on screen, whose direction from the shooter is `dir` (16
// directions, or 8 when the shooter's last action has 8: then the direction
// is clamped to 0..7 before being doubled, as in the original). NPCs win
// over other items, then shapes with TYPEFLAG byte 0 bit 4; among equals the
// nearest (Chebyshev distance, z included). 0 if none or beyond 0x400.
// Port addition (World::precise_aim, with `anims`): for the controlled NPC,
// an item it has no line of fire to in `dir` (fire_distance 0, when its shot
// action has firing frames) is passed over, as if it were not there: the
// nearest enemy behind a floor or a ceiling no longer takes the shot from the
// one in sight, nor does a nearer one behind a wall.
ObjId find_target(const World& world, ObjId shooter, int dir, bool sixteen_directions,
                  const AnimDat* anims = nullptr);

// CRUSADER_1130_00ee: the point aimed at on `target`, the centre of its
// footprint in x and y (Item::I_getCX / CY). In z, with f the footpad height:
// target z + 8f, minus 8 when f is 1 or 2; for f >= 3 minus 16, then +8 when
// the shooter stands at least 48 lower, or when it stands at least 48
// higher -16 (f = 6) or -24 (f >= 7).
World::Point aim_point(const World& world, ObjId shooter, ObjId target);

// Item::I_fireWeapon. Returns the pid of the last projectile, 0 if none.
Pid fire_weapon(Kernel& kernel, World& world, const AnimDat& anims, ObjId shooter, int dx, int dy, int dz,
                int dir, int fire_type, bool aim);

// CRUSADER_1130_026b (reproduced part, see above). `dir` is the direction the
// projectile travelled in, `at` the point hit (null from I_receiveHit, where
// the original reads a null pointer).
void apply_damage(Kernel& kernel, World& world, const AnimDat& anims, ObjId target, ObjId attacker, int dir,
                  int damage, int fire_type, const World::Point* at = nullptr);

class ProjectileProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0x250;

    ProjectileProcess(World& world, const AnimDat& anims, ObjId shooter, World::Point start, World::Point target,
                      int fire_type, int damage, std::uint8_t frame);

    const World::Point& position() const { return pos_; }
    const World::Point& target() const { return target_; }
    // Fire type 9: guided towards `target` (an item, 0 = straight on in `dir`).
    void set_homing(ObjId target, int dir);
    ObjId sprite() const { return sprite_; }

protected:
    void on_added() override;
    void run() override;
    void on_terminate() override;

private:
    void impact(ObjId hit);
    World::Point homing_step();
    bool ricochet(ObjId blocker, const World::Point& at);

    World& world_;
    const AnimDat& anims_;
    ObjId shooter_;
    World::Point start_, target_, pos_;
    int fire_type_;
    int damage_;
    std::uint8_t frame_;
    std::int32_t steps_ = 1;   // n
    std::int32_t step_ = 1;    // i (+0x66)
    std::int32_t range_ = 0;
    ObjId sprite_ = 0;
    bool homing_ = false;      // +0xB3
    bool lost_ = false;        // +0xB4, past the range
    ObjId homing_target_ = 0;  // +0xAF
    int dir_ = 0;              // +0xBD
    World::Point velocity_{};  // +0x36, +0x38, +0x3A
    int ricochets_ = 0;        // +0xB1
};

}  // namespace crusader
