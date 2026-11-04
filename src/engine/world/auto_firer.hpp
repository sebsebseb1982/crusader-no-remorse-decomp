#pragma once
// The AutoFirer (CRUSADER.C 1138:2FFB, process type 0x260): weapon 909 (0x38D, fire type 6) fires
// continuously while the player holds the fire key.
//
// The firing frame of the player's shot action (WPNCACHE_1160_005f, Game::fire_frame) starts it when
// the fire key is held and none runs. From then on, the firing frames of the actions no longer fire
// (only the AutoFirer's shots do), and the first one seen after the key is released ends it. At 6 Hz,
// while the active weapon of the controlled NPC is 909 and has rounds left, it fires the firing frame
// of that NPC's shot action (8, or 0x2A kneeling, remapped for the weapon) in its current direction
// (SPRITE_1150_076c); otherwise it ends. It does not use rounds itself. The fire reset of the
// Controller (1138:2F9F) ends it too.
//
// Not reproduced: its place in the saved games (the port does not save processes of this type).

#include "core/anim_dat.hpp"
#include "engine/process.hpp"
#include "engine/world/world.hpp"

#include <functional>

namespace crusader {

class Kernel;

// The frame that fires in the shot action of the controlled NPC, facing its direction; nullptr if
// none (SPRITE_1150_076c).
const AnimFrame* firing_frame(const World& world, const AnimDat& anims);

class AutoFirer : public TimedProcess {
public:
    static constexpr std::uint16_t kType = 0x260;
    static constexpr std::uint16_t kWeapon = 0x38D;  // 909
    using Fire = std::function<void(ObjId actor, int muzzle_a, int muzzle_b, int muzzle_z)>;

    AutoFirer(World& world, const AnimDat& anims, Fire fire);

protected:
    void tick() override;
    void on_terminate() override;

private:
    World& world_;
    const AnimDat& anims_;
    Fire fire_;
};

// Ends the AutoFirer, if one runs (the end of 1138:2F9F).
void stop_auto_firer(Kernel& kernel, World& world);

}  // namespace crusader
