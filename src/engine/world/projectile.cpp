#include "engine/world/projectile.hpp"

#include "core/dtable.hpp"
#include "engine/audio/audio.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/aim_mode.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/item_damage.hpp"
#include "engine/world/sprite_process.hpp"
#include "engine/world/pathfind.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace crusader {

namespace {

constexpr int kAimDistance = 0x500;  // SPRITE_1140_0e01
constexpr int kMaxSpread = 0x50;
constexpr int kMaxZSpread = 0x18;
constexpr int kTopZ = 250;
constexpr int kStepsBeyondRange = 0x19;
constexpr int kShooterGrace = 5;

int chebyshev(const World::Point& a, const World::Point& b) {
    return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)});
}

// UCMachine::I_rndRange (12E0:0293), signed bounds.
int rnd_between(World& w, int lo, int hi) {
    return random_between(w.state.random_state, static_cast<std::int16_t>(lo), static_cast<std::int16_t>(hi));
}

// The C library's rand() * n / 0x8000 (core/random.hpp).
int crt_rnd(World& w, int n) { return crt_rand_below(w.state.crt_rand_state, n); }

// DeathSilence (CRUSADER_1130_000e, type 0x261): started by the player's
// death, a 1 Hz timer held back (flag 1) until its third period (1130:00D6),
// then plays sound 9 and ends (1130:00B1): three seconds after the death.
// @orig 1130:000E 1130:00D6 1130:00B1
class DeathSilence : public TimedProcess {
public:
    static constexpr std::uint16_t kType = 0x261;
    explicit DeathSilence(World& world) : world_(world) {
        set_type(kType);
        set_rate(1);
    }
    bool runs_in_slice() const override { return true; }

protected:
    void tick() override { ++periods_; }
    void run() override {
        if (periods_ < 3) return;
        if (Audio* audio = world_.audio()) audio->play_sfx(9);
        terminate();
    }

private:
    World& world_;
    int periods_ = 0;
};

// A sound at the NPC unless it already plays there (I_isSFXPlayingForObject
// then I_playSFXCru).
void play_once(World& world, ObjId npc, int sfx) {
    Audio* audio = world.audio();
    if (audio && !audio->sources.is_playing(sfx, npc)) audio->sources.play(world, sfx, npc);
}

// Record +0x5A bit 2: a woman (her own cries).
bool is_female(World& world, ObjId npc) {
    const auto record = world.npc_record(npc);
    return !record.empty() && (record[0x5A] & 0x04);
}

// CRUSADER_1130_01f7: shapes that take full damage from bullets (not robots).
// @orig 1130:01F7
bool takes_full_bullet_damage(std::uint16_t shape) {
    switch (shape) {
    case 0x4C8: case 0x338: case 0x45D: case 0x2CB: case 0x4E6: case 0x383: case 0x385:
        return false;
    default:
        return true;
    }
}

// Sprite frame of the projectile by fire type (Item::I_fireWeapon).
// @orig 1130:11DA
std::uint8_t projectile_frame(int fire_type, int dir, World& w) {
    switch (fire_type) {
    case 3: case 9: case 10: return static_cast<std::uint8_t>(dir + 0x11);
    case 5: return static_cast<std::uint8_t>(dir + 1);
    case 6: return 0x46;
    case 0xE: return static_cast<std::uint8_t>(crt_rnd(w, 5) + 0x47);
    case 0xF: return 0x4C;
    default: return 0;
    }
}

// SPRITE_1140_09c9: divisor of the aiming spread.
// @orig 1140:09C9
int spread_divisor(const World& w, ObjId shooter, int fire_type, std::uint32_t now) {
    if (shooter == w.state.controlled_npc) {
        switch (fire_type) {
        case 2: case 0xD: return 8;
        case 5: return 12;
        case 10: return 5;
        default: return 10;
        }
    }
    const int moving = std::max<int>(1, w.state.player_move_accuracy);
    if (is_npc_id(shooter)) {
        const auto record = w.npc_record(shooter);
        if (!record.empty() && record[0x68] != 0) return 2;
        return controlled_npc_still(w, now) ? 8 : moving;
    }
    const auto player = w.npc_record(w.state.controlled_npc);
    if (!player.empty() && (player[npc::kFlags13] & 0x04)) return 5;  // Actor::I_isKneeling
    return moving * 3;
}

}  // namespace

// @orig 1128:0158
ObjId find_target(const World& world, ObjId shooter, int dir, bool sixteen_directions, const AnimDat* anims) {
    const Item* from = world.get(shooter);
    if (!from) return 0;
    // Port addition: precise aim. Checked only for an item about to be kept,
    // which gives the same choice as leaving the unreachable ones out first
    // (the NPC / preferred flags only come from kept items).
    const bool reach_only = anims && world.precise_aim() && shooter == world.state.controlled_npc;
    const auto unreachable = [&](ObjId id) {
        bool muzzle = false;
        return fire_distance(world, *anims, shooter, id, dir, 0, 0, 0, &muzzle) == 0 && muzzle;
    };
    const auto direction_to = [&](const Item& item) {
        return sixteen_directions ? closest_direction_in_range(from->x, from->y, item.x, item.y, 16, 0, 15)
                                  : closest_direction_in_range(from->x, from->y, item.x, item.y, 8, 0, 7) * 2;
    };
    const auto distance_to = [&](const Item& item) {
        return static_cast<std::uint32_t>(
            std::max({std::abs(static_cast<std::int16_t>(from->x - item.x)),
                      std::abs(static_cast<std::int16_t>(from->y - item.y)), std::abs((from->z & 0xFF) - (item.z & 0xFF))}));
    };
    // Port addition: the target the right stick points at on screen
    // (aim_at_angle) goes first while it stays a fair one in this direction.
    if (const ObjId picked = world.state.aim.stick_target; reach_only && picked != 0 && picked != shooter) {
        const Item* item = world.get(picked);
        const auto& slots = world.tracked();
        if (item && std::find(slots.begin(), slots.end(), picked) != slots.end() &&
            world.is_partly_in_combat_zone(*item) && direction_to(*item) == dir && distance_to(*item) <= 0x400 &&
            !unreachable(picked))
            return picked;
    }
    std::uint32_t best_distance = 0xFFFF;
    ObjId best = 0;
    bool found_npc = false, found_preferred = false;
    std::size_t seen = 0;
    for (std::size_t slot = 0; slot < World::kTrackedSlots && seen < world.tracked_count(); ++slot) {
        const ObjId id = world.tracked()[slot];
        if (id == 0) continue;
        ++seen;
        const Item* item = world.get(id);
        if (!item) continue;
        const bool npc = is_npc_id(id);
        const ShapeInfo* info = world.shape_info(item->shape);
        const bool preferred = info && info->is(ShapeInfo::kOccludes);  // ITEM_10a8_30a8
        if ((!npc && found_npc) || (!preferred && found_preferred)) continue;
        if (id == shooter || !world.is_partly_in_combat_zone(*item)) continue;
        if (direction_to(*item) != dir) continue;
        const auto distance = distance_to(*item);
        if (distance < best_distance || (npc && !found_npc) || (preferred && !found_preferred)) {
            if (reach_only && unreachable(id)) continue;
            best_distance = distance;
            best = id;
            found_npc = found_npc || npc;
            found_preferred = found_preferred || preferred;
        }
    }
    return best_distance > 0x400 ? 0 : best;
}

// @orig 1130:00EE
World::Point aim_point(const World& world, ObjId shooter, ObjId target) {
    const Item* item = world.get(target);
    const Item* from = world.get(shooter);
    if (!item) return {};
    const ShapeInfo* info = world.shape_info(item->shape);
    const int fx = info ? info->footpad_x : 0, fy = info ? info->footpad_y : 0, fz = info ? info->footpad_z : 0;
    const int tz = item->z & 0xFF;
    int z = tz + fz * 8;
    if (fz < 3) {
        if (fz != 0) z -= 8;
    } else {
        z -= 0x10;
        const int dz = (from ? from->z & 0xFF : 0) - tz;
        if (dz < -0x2F) z += 8;
        else if (dz > 0x2F) z -= fz >= 7 ? 0x18 : fz == 6 ? 0x10 : 0;
    }
    return {static_cast<std::uint16_t>(item->x - fx * 8), static_cast<std::uint16_t>(item->y - fy * 8), z & 0xFF};
}

// @orig 1130:11DA
Pid fire_weapon(Kernel& kernel, World& world, const AnimDat& anims, ObjId shooter, int dx, int dy, int dz,
                int dir, int fire_type, bool aim) {
    const Item* item = world.get(shooter);
    const dtable::FireType* ft = dtable::fire_type(fire_type);
    if (!item || !ft) return 0;
    World::Point origin = world.location(*item);
    origin.x += dx;
    origin.y += dy;
    origin.z += dz;
    dir &= 0xF;

    const auto random_damage = [&world](int type) {  // FireType::getRandomDamage
        const dtable::FireType* t = dtable::fire_type(type);
        return t ? rnd_between(world, t->min_damage(), t->max_damage()) : 0;
    };

    // Muzzle already inside an item (ITEMFIND.C 1874): immediate hit on the
    // first solid item there, with the damage of the fire type, then its
    // splash around the muzzle and its impact sprite (1130:11DA).
    ObjId inside = 0;
    if (!position_free(world, kProjectileShape, origin, &inside)) {
        if (const Item* hit = world.get(inside)) {
            const int dir8 = direction_8(origin.x, origin.y, hit->x, hit->y);
            apply_damage(kernel, world, anims, inside, shooter, dir8, random_damage(fire_type), fire_type, &origin);
            if (ft->splash() != 0) {
                splash_damage(kernel, world, anims, origin, ft->splash(), random_damage(fire_type), fire_type, inside,
                              shooter);
            }
            if (ft->impact_sprite()) impact_sprite(kernel, world, fire_type, origin);
        }
        return 0;
    }

    // Damage of the shot: for NPCs (NPC_10f0_3106), the active inventory item
    // when it fires this type, else the active weapon, 0 without one; the fire
    // type otherwise.
    int damage = 0;
    if (is_npc_id(shooter)) {
        const Item* inv = world.get(world.active_inventory_item(shooter));
        const bool from_item = inv && dtable::weapon(inv->shape).fire_type() == fire_type;
        if (const Item* source = from_item ? inv : world.get(world.active_weapon(shooter))) {
            damage = random_damage(dtable::weapon(source->shape).fire_type());
        }
    } else {
        damage = random_damage(fire_type);
    }

    // Target: for the controlled NPC, the tracked item in the direction of
    // the shot (TRACKER_1128_0158, 16 directions when the shooter's last
    // action has them); for others, the controlled NPC.
    ObjId target_item = 0;
    if (aim) {
        if (shooter == world.state.controlled_npc) {
            const auto record = world.npc_record(shooter);
            const std::uint16_t last =
                record.empty() ? 0 : static_cast<std::uint16_t>(record[npc::kLastAnim] | record[npc::kLastAnim + 1] << 8);
            const AnimAction* action = anims.action(item->shape, last);
            target_item = find_target(world, shooter, dir, action && action->sixteen_directions(), &anims);
        } else {
            target_item = world.state.controlled_npc;
        }
    }
    // A shot of the controlled NPC enters the aiming mode; without a target,
    // it then flies towards the crosshair (SPRITE_1140_0e88), with spread, at
    // the muzzle's height (param_4 already holds the shooter's z + dz).
    if (shooter == world.state.controlled_npc && !world.state.aim.active) enter_aim_mode(kernel, world, anims);
    const auto crosshair = target_item == 0 && shooter == world.state.controlled_npc && world.state.aim.active
                               ? crosshair_aim(world, origin.z)
                               : std::nullopt;
    // A target is aimed with spread (SPRITE_1140_09c9 with 1), a shot
    // straight ahead when aiming is asked (SPRITE_1140_0e01): both need `aim`.
    const bool spread_shot = (aim || crosshair) && !ft->no_spread();

    Pid last = 0;
    for (int i = 0; i < ft->projectiles(); ++i) {
        World::Point target = target_item != 0 ? aim_point(world, shooter, target_item)
                              : crosshair      ? *crosshair
                                               : World::Point{origin.x + kDirectionDx[dir] * kAimDistance,
                                                              origin.y + kDirectionDy[dir] * kAimDistance, origin.z};
        if (spread_shot) {
            int spread = chebyshev(origin, target) / spread_divisor(world, shooter, fire_type, kernel.ticks());
            spread = std::min(spread, kMaxSpread);
            target.x += rnd_between(world, -spread, spread);
            target.y += rnd_between(world, -spread, spread);
            spread = std::min(spread / 3, kMaxZSpread);
            target.z = std::clamp(target.z + rnd_between(world, -spread, spread), 0, kTopZ);
        }
        auto projectile = std::make_unique<ProjectileProcess>(world, anims, shooter, origin, target, fire_type, damage,
                                                              projectile_frame(fire_type, dir, world));
        if (fire_type == 9) projectile->set_homing(target_item, dir);
        last = kernel.add(std::move(projectile));
    }
    return last;
}

// @orig 1130:026B
void apply_damage(Kernel& kernel, World& world, const AnimDat& anims, ObjId target, ObjId attacker, int dir,
                  int damage, int fire_type, const World::Point* at) {
    Item* item = world.get(target);
    if (!item) return;
    const std::uint16_t difficulty = world.state.difficulty;
    const bool victim_is_player = target == world.state.controlled_npc || target == 1;
    if (!victim_is_player) {
        if (difficulty == 1) damage *= 5;
        else if (difficulty == 2) damage *= 3;
    } else {
        if (difficulty == 1) damage /= 5;
        else if (difficulty == 2) damage /= 3;
    }
    if (!takes_full_bullet_damage(item->shape) &&
        (fire_type == 1 || fire_type == 2 || fire_type == 0xB || fire_type == 0xD)) {
        damage /= 3;
    }
    damage = std::clamp(damage, 1, 250);

    if (!is_npc_id(target)) {
        hit_item(kernel, world, anims, target, damage, fire_type);
        // Explosives (types 3, 4) throw items that have a weight (TYPEFLAG
        // byte 7) along the hit's direction: tables DS:26F8 / DS:2718 times
        // 10..25 (y drawn first), gravity 2.
        static constexpr int kThrowX[16] = {0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2, -2, -2, -2, -1};
        static constexpr int kThrowY[16] = {-2, -2, -2, -1, 0, 1, 2, 2, 2, 2, 2, 1, 0, -1, -2, -2};
        const Item* thrown = world.get(target);
        const ShapeInfo* info = thrown ? world.shape_info(thrown->shape) : nullptr;
        if (info && info->weight != 0 && (fire_type == 3 || fire_type == 4)) {
            const int ry = rnd_between(world, 10, 25);
            const int rx = rnd_between(world, 10, 25);
            hurl(kernel, world, target, kThrowX[dir & 0xF] * rx, kThrowY[dir & 0xF] * ry, 0, 2);
        }
        return;
    }
    // Vargas (shape 0x3AC) with his shield up: the hit only flashes the
    // shield (actions 0x21 then 0x20; ignored while one is showing) and
    // takes from it.
    if (item->shape == 0x3AC && world.state.vargas_shield != 0) {
        const auto* anim = dynamic_cast<const ActorAnimProcess*>(kernel.find_process(target, ActorAnimProcess::kType));
        if (anim && (anim->action() == 0x21 || anim->action() == 0x20)) return;
        do_anim(kernel, world, anims, target, 0x21, kDirCurrent, 10000, true);
        do_anim(kernel, world, anims, target, 0x20, kDirCurrent, 1, false);
        world.state.vargas_shield = static_cast<std::int16_t>(std::max(0, world.state.vargas_shield - damage));
        return;
    }
    if (world.is_dead(target)) return;
    auto record = world.npc_record(target);
    if (record.empty()) return;
    const std::uint32_t now = kernel.ticks();
    record[0x69] = static_cast<std::uint8_t>(now);
    record[0x6A] = static_cast<std::uint8_t>(now >> 8);
    record[0x6B] = static_cast<std::uint8_t>(now >> 16);
    record[0x6C] = static_cast<std::uint8_t>(now >> 24);
    // The victim fights back: activity 5 (or its default activity 2) and the
    // controlled NPC as target. Walking (record +0x59 bit 5) and hit by the
    // controlled NPC within 0x280, in the combat zone, with a line of fire back to it,
    // it stops walking (its path finder, type 0x204, ends).
    if (target != world.state.controlled_npc && target != 1) {
        const ObjId controlled = world.state.controlled_npc;
        if (in_combat(world, target)) {
            if (world.npc_word(target, npc::kActivity) == 8) set_activity(kernel, world, anims, target, 5);
            set_target(kernel, world, anims, target, controlled);
        } else {
            set_activity(kernel, world, anims, target, 0x72);
            if (in_combat(world, target)) set_target(kernel, world, anims, target, controlled);
        }
        const Item* shooter = world.get(attacker);
        const auto victim_record = world.npc_record(target);
        if (shooter && is_npc_id(attacker) && attacker == controlled && !victim_record.empty() &&
            (victim_record[npc::kCombatFlags] & 0x20)) {
            const int distance = std::max({std::abs(static_cast<std::int16_t>(item->x - shooter->x)),
                                           std::abs(static_cast<std::int16_t>(item->y - shooter->y)),
                                           std::abs((item->z & 0xFF) - (shooter->z & 0xFF))});
            if (distance <= 0x280 && world.is_partly_in_combat_zone(*item)) {
                const int back = closest_direction_in_range(item->x, item->y, shooter->x, shooter->y, 16, 0, 15);
                if (fire_distance(world, anims, target, attacker, back) != 0)
                    kernel.fail_processes(target, PathfinderProcess::kType, 1);
            }
        }
    }
    // Shields of the player (record +0x66: 1, 2 or 3, taken as 4): when the
    // fire type's mask (+0xA) has the shield's bit and the energy covers its
    // cost (+8), the energy pays, sound 0x48 plays and the shot does nothing.
    // Unless the large shield's sprite is up: shields 1 and 2 flash at the
    // point hit (0x5A9, frames 7..13 or 0..6), the large one shows 0x52B
    // (frames 0..8) on the player, which follows him. Not reproduced: the
    // status bar flash.
    if (target == 1) {
        int shield = world.npc_word(1, 0x66);
        if (shield == 3) shield = 4;
        const dtable::FireType* ft = dtable::fire_type(fire_type);
        if (shield != 0 && ft && (shield & ft->shield_mask()) && ft->shield_cost() <= world.npc_mana(1)) {
            const auto energy = static_cast<std::uint16_t>(world.npc_mana(1) - ft->shield_cost());
            record[0x57] = static_cast<std::uint8_t>(energy);
            record[0x58] = static_cast<std::uint8_t>(energy >> 8);
            if (Audio* audio = world.audio()) audio->sources.retrigger(world, 0x48, 1);
            damage = 0;
            if (world.state.shield_sprite == 0) {
                if (shield == 4) {
                    const World::Point on{item->x + 16, item->y + 24, item->z & 0xFF};
                    const Pid pid = create_sprite(kernel, world, 0x52B, 0, 8, 4, on);
                    if (auto* sprite = dynamic_cast<SpriteProcess*>(kernel.get(pid))) {
                        world.state.shield_sprite = pid;
                        world.state.shield_sprite_item = sprite->sprite();
                    }
                } else if (at) {
                    create_sprite(kernel, world, 0x5A9, shield == 1 ? 7 : 0, shield == 1 ? 13 : 6, 4, *at);
                }
                item = world.get(target);
                if (!item) return;
            }
        }
    }
    if (record[npc::kFlags13] & 0x02) damage = 0;  // immortal (NPC_10f0_214b)
    const int old_hp = world.npc_hp(target);
    const int hp = std::max(0, old_hp - damage);
    record[0] = static_cast<std::uint8_t>(hp);
    record[1] = static_cast<std::uint8_t>(hp >> 8);
    const int dealt = old_hp - hp;
    if (dealt > 0 && target == world.state.controlled_npc && world.hooks().player_damaged) {
        world.hooks().player_damaged(dealt);
    }
    if (hp != 0) {
        // Hurt: humans (and the player by fire type 8) cry out, 0x8F or 0xD8
        // for a woman.
        if (damage != 0 && (takes_full_bullet_damage(item->shape) || (fire_type == 8 && target == 1)))
            play_once(world, target, is_female(world, target) ? 0xD8 : 0x8F);
        // Electric shots (types 7, 15) shake the player (action 0x37, its
        // animations cut); robots (0x4E6, 0x338, 0x385, 0x383) fight for 8 to
        // 17 seconds one time in three.
        if (damage != 0 && (fire_type == 0xF || fire_type == 7)) {
            if (target == 1) {
                kernel.fail_processes(1, ActorAnimProcess::kType, 1);
                do_anim(kernel, world, anims, 1, 0x37, kDirCurrent, 0, false);
            } else if ((item->shape == 0x4E6 || item->shape == 0x338 || item->shape == 0x385 || item->shape == 0x383) &&
                       rnd_between(world, 0, 2) == 0) {
                if (auto* attack = dynamic_cast<AttackProcess*>(kernel.find_process(target, AttackProcess::kType)))
                    attack->set_timed(kernel.ticks(), crt_rnd(world, 10));
            }
        }
        return;
    }

    // Death: the NPC's sounds stop (I_stopSFXCru 0x077).
    if (Audio* audio = world.audio()) audio->sources.stop_item(target);
    if (world.hooks().fail_processes) world.hooks().fail_processes(target);
    // The controlled NPC: the player's death only starts DeathSilence; a
    // controlled robot leaves no NPC controlled.
    if (target == world.state.controlled_npc) {
        designate(kernel, world, anims, 0);
        if (target == 1) kernel.spawn<DeathSilence>(world);
        else set_controlled_npc(kernel, world, anims, 0);
    }
    // Special deaths of humans (not the robot shapes), with VIOLENCE=1 (the
    // option string DS:0953 of the executable) where noted.
    const bool human = takes_full_bullet_damage(item->shape);
    if (human && (fire_type == 3 || fire_type == 4 || fire_type == 10 || fire_type == 12)) {
        // Blown apart when the damage reaches a fraction of a damage roll of
        // the fire type (3/5 for type 3, 2/5 otherwise) and shape 0x576 fits.
        const dtable::FireType* ft = dtable::fire_type(fire_type);
        int threshold = ft ? rnd_between(world, ft->min_damage(), ft->max_damage()) : 0;
        threshold = threshold * (fire_type == 3 ? 3 : 2) / 5;
        if (damage >= threshold) {
            // The original pushes the NPC out of the world for the check
            // (ITEMFIND.C 1874) and puts it back.
            const World::Point at{item->x, item->y, item->z};
            const bool fits = position_free(world, 0x576, at, nullptr, target);
            if (fits) {
                int turn = world.npc_direction(target);
                world.set_shape(*item, 0x576);
                set_pose(world, anims, target, 1);
                const int spins = crt_rnd(world, 9);
                if (turn & 1) turn = (turn + 1) & 0xF;
                for (int i = 0; i < spins; ++i) {
                    turn = (turn + 2 * rnd_between(world, -1, 1)) & 0xF;
                    do_anim(kernel, world, anims, target, 1, turn, 10000, false);
                }
                do_anim(kernel, world, anims, target, 0x12, kDirCurrent, 10000, false);
                const int falls = crt_rnd(world, 3) + 1;
                for (int i = 0; i < falls; ++i) do_anim(kernel, world, anims, target, 0x14, kDirCurrent, 10000, false);
                do_anim(kernel, world, anims, target, 0x17, kDirCurrent, 10000, false);
                // Its screams come from the special frames of shape 0x576 (special_frame).
                world.set_broken(*item);
                record[npc::kFlags13] |= 0x01;
                return;
            }
        }
    } else if (human && fire_type == 6) {
        world.set_shape(*item, 0x58F);  // burnt
        set_pose(world, anims, target, 0);
    } else if (human && fire_type == 0xE) {
        world.set_shape(*item, 0x596);
        set_pose(world, anims, target, 0x12);
    } else if (human && fire_type == 0xF) {
        world.set_shape(*item, 0x59C);
        set_pose(world, anims, target, 0x12);
    } else if (fire_type == 7 && target == 1) {
        do_anim(kernel, world, anims, 1, 0x37, kDirCurrent, 0, false);
    }
    const int facing = world.npc_direction(target);
    std::array<int, 9> around{};
    around[0] = facing;
    for (int k = 1; k < 9; ++k) around[k] = (facing + ((k + 1) >> 1) * ((k & 1) ? 1 : -1)) & 0xF;
    bool random_side = facing >= 0x10 || item->shape == 899 || item->shape == 0x58F || item->shape == 0x596;
    bool forward = false;
    if (!random_side) {
        for (int k = 0; k < 9; ++k) {
            if (around[k] != dir) continue;
            if (k == 8) random_side = true;
            else forward = true;
            break;
        }
    }
    std::uint16_t action = 0x12;
    if (anims.action(item->shape, 0x14)) {
        if (random_side) forward = crt_rnd(world, 2) == 0;
        if (forward) action = 0x14;
    }
    do_anim(kernel, world, anims, target, action, kDirCurrent);
    // Death cry of humans (with VIOLENCE=1): electrocuted (type 15) DS:273E,
    // else DS:2738 for men, DS:2742 for women.
    if (takes_full_bullet_damage(item->shape)) {
        static constexpr int kElectrocuted[] = {0xD9, 0xDA};  // SCREAM11, SCREAM14
        static constexpr int kMan[] = {0x88, 0x8C, 0x8F};     // DETHUM01, 02, 05
        static constexpr int kWoman[] = {0xD8, 0x10};         // FW04, FEMSCRM3
        int cry;
        if (fire_type == 0xF) cry = kElectrocuted[crt_rnd(world, 2)];
        else if (!is_female(world, target)) cry = kMan[crt_rnd(world, 3)];
        else cry = kWoman[crt_rnd(world, 2)];
        play_once(world, target, cry);
    }
    world.set_broken(*item);  // Item::I_setBroken
    record[npc::kFlags13] |= 0x01;  // Actor::I_setDead
}

// @orig 1140:06CE 1140:08C2
ProjectileProcess::ProjectileProcess(World& world, const AnimDat& anims, ObjId shooter, World::Point start,
                                     World::Point target, int fire_type, int damage, std::uint8_t frame)
    : world_(world),
      anims_(anims),
      shooter_(shooter),
      start_(start),
      target_(target),
      pos_(start),
      fire_type_(fire_type),
      damage_(damage),
      frame_(frame) {
    set_type(kType);
    const dtable::FireType* ft = dtable::fire_type(fire_type);
    const int sixteenths = chebyshev(start, target) / 16;
    const int speed = ft && ft->speed() ? ft->speed() : 1;
    steps_ = sixteenths / speed;
    if (sixteenths % 16 != 0) ++steps_;
    if (sixteenths == 0) ++steps_;
    if (steps_ <= 0) steps_ = 1;
    range_ = (ft ? ft->range() : 0) + kStepsBeyondRange;
}

// @orig 1140:09C9 1140:0E01 1140:0E88
void ProjectileProcess::on_added() {
    sprite_ = world_.create(kProjectileShape, frame_);
    if (sprite_) {
        world_.get(sprite_)->sprite_only = true;
        world_.pop_to_coords(pos_.x, pos_.y, pos_.z);
    }
}

void ProjectileProcess::on_terminate() {
    if (sprite_ && world_.get(sprite_)) world_.destroy(sprite_);
    sprite_ = 0;
}

// @orig 1140:1098 1140:1E09
void ProjectileProcess::impact(ObjId hit) {
    if (hit != 0) {
        // An NPC is hit from the shot's direction; an item from a random one
        // (rand() * 8 / 0x8000, SPRITE_1140_1098), which explosives throw
        // it along. Not reproduced there: the original only damages an item
        // that is not fixed (TYPEFLAG byte 0 bit 0) after its hit event.
        const Item* item = world_.get(hit);
        const int dir = !is_npc_id(hit) ? crt_rnd(world_, 8)
                        : item          ? direction_8(pos_.x, pos_.y, item->x, item->y)
                                        : 0x10;
        apply_damage(kernel(), world_, anims_, hit, shooter_, dir, damage_, fire_type_, &pos_);
    }
    // SPRITE_1140_1e09: splash damage of the fire type around the impact,
    // sparing the item hit and the shooter (unless it is the controlled NPC).
    const dtable::FireType* ft = dtable::fire_type(fire_type_);
    if (ft && ft->splash() != 0) {
        splash_damage(kernel(), world_, anims_, pos_, ft->splash(), damage_, fire_type_, hit, shooter_);
    }
    impact_sprite(kernel(), world_, fire_type_, pos_);
    terminate();
}

// @orig 1140:09C9
void ProjectileProcess::set_homing(ObjId target, int dir) {
    // SPRITE_1140_09c9, first launch of fire type 9.
    homing_ = true;
    homing_target_ = target;
    dir_ = dir & 0xF;
    velocity_ = {kDirectionDx[dir_] * 16, kDirectionDy[dir_] * 16, 0};
}

// @orig 1140:151D
World::Point ProjectileProcess::homing_step() {
    // SPRITE_1140_151d: within the fire type's range the velocity turns
    // towards the target (or keeps the launch direction): a step per axis,
    // doubled against the current motion; z goes towards the target's centre
    // + 8. Past the range the missile is lost and drops. Velocity at most 32
    // per axis (16 in z).
    const dtable::FireType* ft = dtable::fire_type(fire_type_);
    const int range = ft ? ft->range() : 0;
    auto sign = [](int v) { return (v > 0) - (v < 0); };
    int target_z = pos_.z;
    if (!lost_ && step_ < range) {
        int d = dir_;
        if (const Item* t = world_.get(homing_target_)) {
            d = direction_8(pos_.x, pos_.y, t->x, t->y) & 0xF;
            const ShapeInfo* info = world_.shape_info(t->shape);
            target_z = (t->z & 0xFF) + (info ? info->footpad_z * 4 : 0) + 8;
        }
        int ax = kDirectionDx[d], ay = kDirectionDy[d];
        if (sign(velocity_.x) != sign(ax)) ax *= 2;
        if (sign(velocity_.y) != sign(ay)) ay *= 2;
        velocity_.x += ax;
        velocity_.y += ay;
    } else if (step_ >= range) {
        lost_ = true;
    }
    if (!lost_) {
        if (pos_.z < target_z) ++velocity_.z;
        else if (target_z < pos_.z) --velocity_.z;
    } else {
        --velocity_.z;
    }
    velocity_.x = std::clamp(velocity_.x, -0x20, 0x20);
    velocity_.y = std::clamp(velocity_.y, -0x20, 0x20);
    velocity_.z = std::clamp(velocity_.z, -0x10, 0x10);
    const World::Point next{pos_.x + velocity_.x, pos_.y + velocity_.y, pos_.z + velocity_.z};
    if (!lost_) {
        const int d = direction_8(pos_.x, pos_.y, next.x, next.y);
        if (d != 0x10 && sprite_ && world_.get(sprite_)) world_.set_frame(*world_.get(sprite_), static_cast<std::uint16_t>(d + 0x11));
    }
    return next;
}

// @orig 1140:151D
bool ProjectileProcess::ricochet(ObjId blocker, const World::Point& at) {
    // SPRITE_1140_151d: fire type 5 bounces off items whose TYPEFLAG byte 1
    // has bit 2, five times at most: the x or y part of the flight is
    // reversed depending on the face hit, and the flight starts again from
    // here, level, without spread.
    const Item* item = world_.get(blocker);
    const ShapeInfo* info = item ? world_.shape_info(item->shape) : nullptr;
    if (fire_type_ != 5 || !info || !info->is(ShapeInfo::kRoof) || ricochets_ >= 5) return false;
    impact_sprite(kernel(), world_, fire_type_, at);
    int dx = target_.x - start_.x, dy = target_.y - start_.y;
    if (pos_.x < item->x - info->footpad_x * 16 + 1 || item->x < pos_.x) dx = -dx;
    else dy = -dy;
    start_ = pos_;
    target_ = {pos_.x + dx, pos_.y + dy, pos_.z};
    ++ricochets_;
    const dtable::FireType* ft = dtable::fire_type(fire_type_);
    const int sixteenths = chebyshev(start_, target_) / 16;
    const int speed = ft && ft->speed() ? ft->speed() : 1;
    steps_ = sixteenths / speed;
    if (sixteenths % 16 != 0) ++steps_;
    if (sixteenths == 0) ++steps_;
    if (steps_ <= 0) steps_ = 1;
    step_ = 1;
    const int d = direction_8(start_.x, start_.y, target_.x, target_.y);
    if (sprite_ && world_.get(sprite_)) world_.set_frame(*world_.get(sprite_), static_cast<std::uint16_t>((d & 0xF) + 1));
    return true;
}

// @orig 1140:151D
void ProjectileProcess::run() {
    World::Point next = homing_ ? homing_step()
                                : World::Point{start_.x + (target_.x - start_.x) * step_ / steps_,
                                               start_.y + (target_.y - start_.y) * step_ / steps_,
                                               start_.z + (target_.z - start_.z) * step_ / steps_};
    next.z = std::clamp(next.z, 0, kTopZ);
    ++step_;
    if (next.z == 0 || next.z == kTopZ || step_ >= range_) {
        impact(0);
        return;
    }
    SweepResult r = sweep(world_, kProjectileShape, sprite_, pos_, next, kSweepNoGround);
    if (r.blocker == shooter_ && shooter_ != 0 && step_ < kShooterGrace) {
        // SPRITE_1140_151d: the shooter is set aside and the step retried.
        r = sweep(world_, kProjectileShape, shooter_, pos_, next, kSweepNoGround);
    }
    if (r.blocker == 0) {
        pos_ = next;
        if (sprite_ && world_.get(sprite_)) world_.move_item(sprite_, pos_.x, pos_.y, pos_.z);
        sleep(fire_type_ == 9 ? 3 : 2);
        return;
    }
    if (ricochet(r.blocker, next)) return;
    pos_ = r.end;
    impact(r.blocker);
}

}  // namespace crusader
