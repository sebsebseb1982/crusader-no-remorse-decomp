#include "engine/world/aim_mode.hpp"

#include "engine/world/actor_anim.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/projectile.hpp"
#include "engine/world/sprite_process.hpp"

#include <algorithm>
#include <initializer_list>

namespace crusader {

namespace {

constexpr int kCrosshairDistance = 200;
constexpr std::uint32_t kRecheckTicks = 60;
constexpr std::uint16_t kNoAimShape = 0x576;
constexpr std::uint16_t kAlsoAimShape = 0x580;
constexpr std::uint16_t kActionRaise = 7;
constexpr std::uint8_t kKeyLeft = 0x4B;
constexpr std::uint8_t kKeyRight = 0x4D;

bool in(std::uint16_t v, std::initializer_list<std::uint16_t> set) {
    return std::find(set.begin(), set.end(), v) != set.end();
}

bool kneeling(const World& world, ObjId id) {  // Actor::I_isKneeling
    const auto r = world.npc_record(id);
    return !r.empty() && (r[npc::kFlags13] & 0x04);
}

// ITEM_10a8_30a8: TYPEFLAG byte 0 bit 4.
bool markable(const World& world, ObjId id) {
    const Item* item = world.get(id);
    const ShapeInfo* info = item ? world.shape_info(item->shape) : nullptr;
    return info && info->is(ShapeInfo::kOccludes);
}

int npc_dir(const World& world, ObjId id) { return world.npc_direction(id) & 0xF; }

// TRACKER_1128_0158 for the controlled NPC, in 16 directions (with precise
// aim, only a target it can reach).
ObjId target_in(const World& world, const AnimDat& anims, int dir) {
    return find_target(world, world.state.controlled_npc, dir, true, &anims);
}

// 1138:0459 / Item_I_move: where the marker of `target` goes when the target
// is at (x, y, z).
// @orig 1138:0459
World::Point marker_place(const World& world, ObjId target, std::int32_t x, std::int32_t y, std::int32_t z) {
    const Item* item = world.get(target);
    const ShapeInfo* info = item ? world.shape_info(item->shape) : nullptr;
    const int fx = info ? info->footpad_x : 0, fy = info ? info->footpad_y : 0;
    int h = info ? info->footpad_z : 0;
    if (h >= 4) h = (h >> 1) + 1;
    else if (h >= 2) h >>= 1;
    if (!is_npc_id(target)) {
        if (h >= 2) h -= 2;
        else if (h == 1) h = 0;
    }
    World::Point p{static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y), (h * 8 + z) & 0xFF};
    if (fx > fy) p.x = static_cast<std::uint16_t>(p.x - (fx >> 1) * 16);
    else if (fy > fx) p.y = static_cast<std::uint16_t>(p.y - (fy >> 1) * 16);
    return p;
}

// SPRITE_1150_065D: height of the muzzle in the first firing frame of the
// NPC's shot facing `dir` (0 when there is none; the original then adds an
// unset value).
// @orig 1150:065D
int muzzle_height(const World& world, const AnimDat& anims, ObjId id, int dir) {
    const Item* item = world.get(id);
    if (!item) return 0;
    const std::uint16_t action = remap_action(world, id, kneeling(world, id) ? 0x2A : 8);
    const AnimAction* a = anims.action(item->shape, action);
    for (std::size_t i = 0; a && i < a->frame_count(); ++i) {
        const AnimFrame* f = a->frame(dir, i);
        if (f && f->fires_weapon()) return f->muzzle_z * 8;
    }
    return 0;
}

void search_after_turn(Kernel& kernel, World& world, const AnimDat& anims, int dir) {
    const ObjId found = target_in(world, anims, dir);
    if (found == 0) designate(kernel, world, anims, 0);
    else if (found != world.state.aim.target) designate(kernel, world, anims, markable(world, found) ? found : 0);
}

}  // namespace

bool aim_action(std::uint16_t a) {
    return in(a, {2, 4, 5, 6, 7, 8, 0xC, 0xD, 0xF, 0x24, 0x25, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x32,
                  0x36});
}

bool turn_action(std::uint16_t a) { return in(a, {0x17, 0x18, 0x19, 0x1C, 0x1D, 0x27, 0x3D, 0x3E}); }

// @orig 10F0:0245
void aim_after_do_anim(Kernel& kernel, World& world, const AnimDat& anims, ObjId actor, std::uint16_t action,
                       Pid pid) {
    const Item* item = world.get(actor);
    if (actor != world.state.controlled_npc && !(item && item->shape == kAlsoAimShape)) return;
    if (aim_action(action)) {
        enter_aim_mode(kernel, world, anims);
    } else {
        leave_aim_mode(kernel, world);
        if (world.hooks().reset_fire) world.hooks().reset_fire();
    }
    if (turn_action(action)) {
        if (auto* anim = dynamic_cast<ActorAnimProcess*>(kernel.get(pid))) anim->mark_turning();
    }
}

// @orig 1138:0299
void enter_aim_mode(Kernel& kernel, World& world, const AnimDat& anims) {
    GameState::Aim& aim = world.state.aim;
    const Item* npc = world.get(world.state.controlled_npc);
    if (aim.turns != 0 || !npc || npc->shape == kNoAimShape) return;
    aim.crosshair_dirty = true;
    if (aim.active || aim.no_stand) return;
    aim.marked_dir = 0x10;
    place_crosshair(world, anims);
    aim.active = true;
    redesignate(kernel, world, anims);
}

// @orig 1138:0692
void leave_aim_mode(Kernel& kernel, World& world) {
    GameState::Aim& aim = world.state.aim;
    if (!aim.active) return;
    world.state.screen_sprites.remove(aim.crosshair);
    aim.crosshair = -1;
    aim.angle = 1000;
    aim.active = false;
    if (aim.marker_pid != 0) {
        if (Process* marker = kernel.get(aim.marker_pid)) marker->terminate();
    }
    if (aim.marker != -1) {
        world.state.screen_sprites.remove(aim.marker);
        aim.marker = -1;
    }
    aim.target = 0;
}

// @orig 1138:0708
void place_crosshair(World& world, const AnimDat& anims) {
    GameState::Aim& aim = world.state.aim;
    const ObjId id = world.state.controlled_npc;
    const Item* npc = world.get(id);
    if (!npc) return;
    const ShapeInfo* info = world.shape_info(npc->shape);
    std::int16_t x = static_cast<std::int16_t>(npc->x - (info ? info->footpad_x : 0) * 8);  // getCX
    std::int16_t y = static_cast<std::int16_t>(npc->y - (info ? info->footpad_y : 0) * 8);  // getCY
    const int dir = npc_dir(world, id);
    const auto z = static_cast<std::uint8_t>(world.location(*npc).z + muzzle_height(world, anims, id, dir));
    if (aim.angle > 360) aim.angle = (22 * dir + 5 + 270) % 360;
    const TrigTables& trig = world.trig();
    // PFLOAT_12d0_02a7: 16.16 times 200, whole part.
    x = static_cast<std::int16_t>(x + ((static_cast<std::int64_t>(trig.cos_deg(aim.angle)) * kCrosshairDistance) >> 16));
    y = static_cast<std::int16_t>(y + ((static_cast<std::int64_t>(trig.sin_deg(aim.angle)) * kCrosshairDistance) >> 16));
    if (aim.crosshair == -1) aim.crosshair = world.state.screen_sprites.add(kCrosshairShape, 0, x, y, z);
    else world.state.screen_sprites.move(aim.crosshair, x, y, z);
    aim.crosshair_dirty = false;
}

void refresh_crosshair(World& world, const AnimDat& anims) {
    if (world.state.aim.active && world.state.aim.crosshair_dirty) place_crosshair(world, anims);
}

// @orig 1138:10C8
void turn_crosshair(Kernel& kernel, World& world, const AnimDat& anims, std::uint8_t scancode, bool walk_only,
                    int& dir, int& action) {
    GameState::Aim& aim = world.state.aim;
    const ObjId id = world.state.controlled_npc;
    const int current = npc_dir(world, id);
    int wanted = current;
    bool turned = false;
    if (!aim.active) {
        if (!aim.no_stand || actor_busy(kernel, id)) return;
        if (scancode == kKeyLeft) wanted = (current - 1) & 0xF;
        else if (scancode == kKeyRight) wanted = (current + 1) & 0xF;
        else return;
    }
    if (aim.no_stand) {
        // `dir` stays the mover's (a turn code for the arrows).
        action = kneeling(world, id) ? 0x2E : 4;
    } else {
        // 5 degrees (2 walking only), or half the mouse rate, at most 20.
        const GameState::MouseTurn& mouse = world.state.mouse_turn;
        const int step = mouse.active ? std::min(mouse.rate >> 1, 20) : walk_only ? 2 : 5;
        if (scancode == kKeyLeft) {
            turned = true;
            aim.angle -= step;
        } else if (scancode == kKeyRight) {
            turned = true;
            aim.angle += step;
        }
        if (turned) {
            place_crosshair(world, anims);
            const Item* npc = world.get(id);
            const ScreenSprite* cross = world.state.screen_sprites.get(aim.crosshair);
            if (npc && cross) {
                const ShapeInfo* info = world.shape_info(npc->shape);
                const std::int32_t cx = static_cast<std::uint16_t>(npc->x - (info ? info->footpad_x : 0) * 8);
                const std::int32_t cy = static_cast<std::uint16_t>(npc->y - (info ? info->footpad_y : 0) * 8);
                wanted = direction_16(cx, cy, cross->x, cross->y);
            }
            dir = wanted;
        }
        if (!turned || wanted != current) action = kneeling(world, id) ? 0x2E : 4;
    }
    if (world.state.target_designation && turned && wanted != current) search_after_turn(kernel, world, anims, wanted);
}

// @orig none: port-only directional aiming
void aim_at_angle(Kernel& kernel, World& world, const AnimDat& anims, int angle, bool draw_when_holstered) {
    GameState::Aim& aim = world.state.aim;
    const ObjId id = world.state.controlled_npc;
    if (id == 0 || aim.no_stand) return;
    if (!aim.active) {
        // A side step (0x1C, 0x1D: the strafe of the left stick sideways) is
        // a turning animation, which leaves the aiming mode until it ends
        // (aim_after_do_anim): the crosshair would vanish for the whole
        // strafe. While the stick aims, it stays shown at the stick's angle,
        // in the mode's own slot, which entering the mode again then moves.
        // Its process, from do_anim on (the last action is noted only once it
        // starts).
        bool side_step = false;
        kernel.for_each_process([&](const Process& p) {
            const auto* anim = dynamic_cast<const ActorAnimProcess*>(&p);
            side_step |= anim && anim->owner() == id && (anim->action() == 0x1C || anim->action() == 0x1D);
        });
        if (aim.turns != 0 && side_step) {
            aim.angle = ((angle % 360) + 360) % 360;
            place_crosshair(world, anims);
            return;
        }
        drop_stick_crosshair(world);
        if (aim.turns != 0) return;
        // With the weapon already out (a loaded game), the stick only has to
        // enter the aim mode. Otherwise it draws the weapon, as the fire key
        // does (7, remapped by the weapon; an aiming action, so the aim mode
        // follows), when the option allows it. The stick would otherwise do
        // nothing until the trigger was pressed first.
        if (actor_busy(kernel, id)) return;
        if (in_combat_stance(world, id)) enter_aim_mode(kernel, world, anims);
        else if (draw_when_holstered) do_anim(kernel, world, anims, id, kActionRaise, kDirCurrent);
        return;
    }
    if (aim.turns != 0) return;
    angle = ((angle % 360) + 360) % 360;
    const int current = npc_dir(world, id);
    const Item* npc = world.get(id);
    if (!npc) return;
    // Placed again at every call, not only when the angle changes: the NPC
    // moving while aiming (a strafe with the left stick) carries the crosshair
    // along, instead of leaving it where it was and turning round it.
    aim.angle = angle;
    place_crosshair(world, anims);
    const ScreenSprite* cross = world.state.screen_sprites.get(aim.crosshair);
    if (!cross) return;
    const ShapeInfo* info = world.shape_info(npc->shape);
    const std::int32_t cx = static_cast<std::uint16_t>(npc->x - (info ? info->footpad_x : 0) * 8);
    const std::int32_t cy = static_cast<std::uint16_t>(npc->y - (info ? info->footpad_y : 0) * 8);
    const int wanted = direction_16(cx, cy, cross->x, cross->y);
    if (wanted == current || actor_busy(kernel, id)) return;
    do_anim(kernel, world, anims, id, kneeling(world, id) ? 0x2E : 4, wanted);
    if (world.state.target_designation) search_after_turn(kernel, world, anims, wanted);
}

// @orig none: port-only directional aiming
void drop_stick_crosshair(World& world) {
    GameState::Aim& aim = world.state.aim;
    if (aim.active || aim.crosshair == -1) return;
    world.state.screen_sprites.remove(aim.crosshair);
    aim.crosshair = -1;
}

std::optional<World::Point> crosshair_aim(const World& world, int z) {
    const ScreenSprite* cross = world.state.screen_sprites.get(world.state.aim.crosshair);
    if (!cross) return std::nullopt;
    return World::Point{cross->x, cross->y, z & 0xFF};
}

// @orig 1138:0459
void designate(Kernel& kernel, World& world, const AnimDat& anims, ObjId target) {
    GameState::Aim& aim = world.state.aim;
    if (!world.state.target_designation || aim.target == target) return;
    if (target == 0) aim.marked_dir = 0x10;
    aim.target = 0;
    if (aim.marker_pid != 0) {
        world.state.screen_sprites.remove(aim.marker);
        aim.marker = -1;
        if (Process* marker = kernel.get(aim.marker_pid)) marker->terminate();
    } else if (aim.marker != -1) {
        world.state.screen_sprites.remove(aim.marker);
        aim.marker = -1;
    }
    if (target == 0) return;
    const ObjId npc = world.state.controlled_npc;
    const int dir = npc_dir(world, npc);
    // The original quirk (a new target in an unchanged direction stays
    // unmarked) is left out with precise aim: the marker always shows the
    // target the shot goes to.
    if (aim.marked_dir == dir && !world.precise_aim()) return;
    aim.checked_at = kernel.ticks();
    const Item* item = world.get(target);
    if (item && fire_distance(world, anims, npc, target, dir) != 0) {
        create_sprite(kernel, world, SpriteProcess::kTargetMarker, 0, 5, 10,
                      marker_place(world, target, item->x, item->y, item->z & 0xFF));
        aim.target = target;
    }
    aim.marked_dir = dir;
}

// @orig 1138:03D0
void redesignate(Kernel& kernel, World& world, const AnimDat& anims) {
    if (!world.state.target_designation) return;
    if (world.state.aim.target != 0) designate(kernel, world, anims, 0);
    if (!world.state.aim.active) return;
    const ObjId found = target_in(world, anims, npc_dir(world, world.state.controlled_npc));
    if (found != 0 && markable(world, found)) designate(kernel, world, anims, found);
}

// @orig 1138:02F3
void check_designation(Kernel& kernel, World& world, const AnimDat& anims) {
    GameState::Aim& aim = world.state.aim;
    if (!world.state.target_designation) {
        designate(kernel, world, anims, 0);
        return;
    }
    if (!aim.active) return;
    const std::uint32_t now = kernel.ticks();
    if (now < aim.checked_at + kRecheckTicks) return;
    aim.checked_at = now;
    const ObjId found = target_in(world, anims, npc_dir(world, world.state.controlled_npc));
    if (found == 0) designate(kernel, world, anims, 0);
    else if (found != aim.target && markable(world, found)) designate(kernel, world, anims, found);
}

// @orig 10A8:1531 1138:0459
void designated_target_moved(Kernel& kernel, World& world, const AnimDat& anims, ObjId target, std::int32_t x,
                             std::int32_t y, std::int32_t z) {
    const ObjId npc = world.state.controlled_npc;
    if (npc == 0 || !world.get(npc) || world.is_dead(npc)) return;
    const ObjId found = target_in(world, anims, npc_dir(world, npc));
    if (found == target) {
        const World::Point p = marker_place(world, target, x, y, z);
        world.state.screen_sprites.move(world.state.aim.marker, p.x, p.y, static_cast<std::uint8_t>(p.z));
        return;
    }
    designate(kernel, world, anims, found != 0 && markable(world, found) ? found : 0);
}

// @orig 1430:0313
void DumbTimer::run() {
    const std::uint32_t now = kernel().ticks();
    if (world_.state.aim.timer_at + kRecheckTicks < now) {
        check_designation(kernel(), world_, anims_);
        world_.state.aim.timer_at = now;
    }
}

}  // namespace crusader
