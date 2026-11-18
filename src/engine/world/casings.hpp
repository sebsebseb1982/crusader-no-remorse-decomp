#pragma once
// Casings (port addition, the original has nothing like it; Enhancements
// screen, GameConfig::casings): a firearm ejects a casing at each shot, a
// gun firing pellets a red shell. It flies out to the shooter's right and a
// little back, spinning, falls, bounces once and lies on the floor where it
// lands, as a mark of DamageMarks (lay_casing) that stays until the map
// changes.
//
// The weapons that eject are those firing bullets (fire types 1, 2, 0xB,
// 0xD, as mark_kind): BA-40, BA-41 (0xB), RP-22, RP-32 (1) eject casings, the
// SG-A1 (2) and the AC-88 (0xD), which fire several pellets, shells. Whoever
// fires them ejects: the player, an NPC, a robot. The flight starts at
// the muzzle pulled back towards the shooter, at its height, and lands at the
// height of the shooter's feet when it fired: on the floor there, if any
// (none down a ledge). It moves one step per game tick, so it stops when the
// game does. A casing in flight is drawn right after its shooter, so that
// what stands in front of the shooter covers it; one whose shooter is not
// drawn is not seen.
//
// Casings only read the world and use their own numbers, never the game's
// random generators; they are not saved, and a load or a map change forgets
// those in flight.

#include "core/surface.hpp"
#include "engine/world/damage_marks.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <vector>

namespace crusader {

enum class Ejection : std::uint8_t { None, Casing, Shell };

// What a shot of `fire_type` ejects.
Ejection ejection_of(int fire_type);

class Casings {
public:
    static constexpr std::size_t kMaxFlying = 64;
    static constexpr int kMaxTicks = 120;  // a flight never lasts longer

    struct Flying {
        ObjId shooter = 0;
        bool shell = false;
        double x = 0, y = 0, z = 0;     // usecode units
        double vx = 0, vy = 0, vz = 0;  // units a tick
        std::int32_t ground = 0;        // the shooter's feet when it fired
        int bounces = 0;
        int age = 0;               // ticks flown
        std::uint32_t tick = 0;    // game tick of this state
        std::uint32_t seed = 0;
    };

    // `shooter` fired, facing `dir` (0..15, 0 towards -y), from the muzzle
    // offset (dx, dy, dz) at game tick `now`: `kind` flies out.
    void eject(const World& world, ObjId shooter, int dx, int dy, int dz, int dir, Ejection kind, std::uint32_t now);

    // Moves the casings in flight on to game tick `now`; those that landed
    // lie on the floor as marks of `marks`.
    void advance(const World& world, std::uint32_t now, DamageMarks& marks);

    // The casings in flight of the item just drawn, in the colours of
    // `marks` (prepared).
    void draw(Surface8& screen, const DrawItem& d, const DamageMarks& marks) const;

    void clear() { flying_.clear(); }
    const std::vector<Flying>& flying() const { return flying_; }

private:
    double unit();  // 0..1, the casings' own numbers

    std::vector<Flying> flying_;
    int map_ = -1;
    std::uint32_t next_seed_ = 0x2545F491u;
};

}  // namespace crusader
