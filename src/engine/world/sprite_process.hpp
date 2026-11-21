#pragma once
// Animated sprites: impacts, explosions (SPRITE.C 1140:00DE..0668, type
// 0x249, and SPRITE 1150:02DF for impacts). See docs/combat.md, "Sprites".
//
// A sprite shows frames `first`..`last` of a shape at a fixed place. Each
// turn the frame moves one step (down when last < first), and the sprite
// ends once past `last` (or below 0); with a delay, the frame is shown and
// the process sleeps that many ticks.
// Ten explosion and flash shapes (0x31C, 0x31F..0x327, list DS:29C2) are
// moved to z 240 at the same screen place, then by a per-shape offset
// (DS:299A / DS:29AE), so that they are drawn over the scene
// (SPRITE_1140_0007).
//
// The port draws a sprite as a world item flagged sprite_only (no
// collisions, not seen by searches or the fast area); the original keeps
// sprites outside the item table (SPMGR.C). Not reproduced: the low detail
// mode (DS:2887: frames by 2), the merging of nearby flashes (DS:2888),
// translucency (TYPEFLAG byte 1 bit 3 -> sprite flag 0x1000), catching up
// when the game lags.
//
// The marker of the designated target (shape 0x59A, aim_mode.hpp) is a
// screen sprite (screen_sprites.hpp), as in the original: its creation
// records the slot and the pid (DS:288C / 288E); at its end the slot is
// kept on frame 5 rather than freed while it is still the marker's
// (SPRITE_1140_0444), and the pid is forgotten.

#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

#include <cstdint>

namespace crusader {

class SpriteProcess : public Process {
public:
    static constexpr std::uint16_t kType = 0x249;

    static constexpr std::uint16_t kTargetMarker = 0x59A;

    SpriteProcess(World& world, std::uint16_t shape, int first, int last, int delay, World::Point at);

    ObjId sprite() const { return sprite_; }
    // Screen sprite slot (the target marker), -1 otherwise.
    int slot() const { return slot_; }

protected:
    void on_added() override;
    void run() override;
    void on_terminate() override;

private:
    World& world_;
    std::uint16_t shape_;
    int first_, last_, frame_, delay_;
    World::Point at_;
    ObjId sprite_ = 0;
    int slot_ = -1;
};

// The usecode offset (dx, dy) a shape drawn over the scene is moved by after
// being raised to z 240 (SPRITE_1140_0007); false for the other shapes.
// The point (x - dx, y - dy, 240) projects where the sprite was created
// (the port's lighting puts an explosion's smoke there).
bool overlay_offset(std::uint16_t shape, int& dx, int& dy);

// SpriteProcess::I_createSprite (1140:00DE). Returns the pid.
Pid create_sprite(Kernel& kernel, World& world, std::uint16_t shape, int first, int last, int delay,
                  World::Point at);

// SPRITE_1150_02DF: the impact sprite of a fire type, and its sound played
// as an interface sound (AudioProcess::I_playSFX).
//   1, 11, 13: shape 0x1D8 (sparks; one of 4 frame ranges)   2: 0x1D8
//   3, 4: 0x326 / 0x320 / 0x321   10: 0x31C / 0x31F / 0x322   6: 0x578
//   14: 0x56B   15: 0x59B   others: 0x537 (frames 0..10)
// Shown with a delay of 3 ticks.
void impact_sprite(Kernel& kernel, World& world, int fire_type, World::Point at);

}  // namespace crusader
