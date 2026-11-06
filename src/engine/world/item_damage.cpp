#include "engine/world/item_damage.hpp"

#include "core/dtable.hpp"
#include "engine/audio/audio.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/projectile.hpp"
#include "engine/world/sprite_process.hpp"

#include <algorithm>
#include <cstdlib>

namespace crusader {

namespace {

constexpr std::uint16_t kItemDamaged = 0x0100;   // broken by damage (ITEMDAM)
constexpr std::uint16_t kItemExploded = 0x0040;  // SPRITE_1150_0891 ran

// random_range(n) (SYSTEM.C 12D8:04FF), and the C library's rand() * n /
// 0x8000 (core/random.hpp), each where the original uses it.
int rnd(World& w, int n) { return random_range(w.state.random_state, static_cast<std::uint16_t>(n)); }
int crt_rnd(World& w, int n) { return crt_rand_below(w.state.crt_rand_state, n); }

}  // namespace

// @orig 10C0:030C
bool damage_item(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, int damage) {
    Item* item = world.get(id);
    if (!item) return false;
    const auto record = world.damage_record(item->shape);
    if (record.empty() || (item->flags & kItemDamaged)) return false;
    bool broken_by_damage = false;
    if (item->hit_points != 0) {
        if (damage == 0) return false;
        if (item->hit_points > damage) {
            item->hit_points = static_cast<std::uint8_t>(item->hit_points - damage);
            return false;
        }
        broken_by_damage = true;
        item->hit_points = 0;
    }
    item->flags |= kItemDamaged;
    world.set_broken(*item);
    if (const int size = (record[0] >> 1) & 3) explode(kernel, world, anims, id, size - 1, record[0] & 0x20, record[0] & 0x08);
    // record[1]: its sound, at the item (I_playSFXCru).
    if (record[1] != 0) {
        if (Audio* audio = world.audio()) audio->sources.play(world, record[1], id);
    }
    item = world.get(id);
    if (!item) return true;
    if (record[0] & 0x40) {
        // Replaced by the shape of bytes 2-3, frame 4, keeping the quality.
        const std::uint16_t quality = item->quality;
        const World::Point at{item->x, item->y, item->z};
        if ((item->z & 0xFF) <= 0xFB) world.destroy(id);
        const ObjId created = legal_create_at(world, static_cast<std::uint16_t>(record[2] | record[3] << 8), record[4], at);
        if (Item* replacement = world.get(created)) replacement->quality = quality;
    } else if (record[0] & 0x80) {
        const int choices = 1 + (record[3] != 0) + (record[4] != 0);
        world.set_frame(*item, record[2 + rnd(world, choices)]);
    } else {
        const int choices = (record[2] != 0) + (record[3] != 0) + (record[4] != 0);
        if (choices != 0) {
            const int pick = rnd(world, choices);
            world.set_frame(*item, static_cast<std::uint16_t>(item->frame + record[2 + pick]));
        } else if (broken_by_damage && (item->z & 0xFF) <= 0xFB) {
            world.destroy(id);
        }
    }
    return true;
}

// @orig 10A8:275F
void got_hit(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, ObjId other, std::uint16_t force) {
    const Item* item = world.get(id);
    if (!item) return;
    // Damage only comes with the hitter 0x4000: the force's low byte.
    const int damage = other == kHitByDamage ? (force & 0xFF) : 0;
    const auto record = world.damage_record(item->shape);
    if (!record.empty() && !(record[0] & 0x10)) {
        if (damage_item(kernel, world, anims, id, damage) && world.hooks().fail_processes) world.hooks().fail_processes(id);
        return;
    }
    if (!record.empty()) {
        // ITEMDAM_10c0_025d: would it break now?
        const bool breaks = !(item->flags & kItemDamaged) &&
                            (item->hit_points == 0 || (damage != 0 && item->hit_points <= damage));
        if (!breaks) {
            damage_item(kernel, world, anims, id, damage);
            return;
        }
    }
    const bool has_event = world.hooks().has_event && world.hooks().has_event(id, 6);
    if (!has_event) return;  // CRUSADER_1130_1090: nothing
    if (world.hooks().start_event_args) {
        const std::uint8_t args[4] = {static_cast<std::uint8_t>(other), static_cast<std::uint8_t>(other >> 8),
                                      static_cast<std::uint8_t>(force), static_cast<std::uint8_t>(force >> 8)};
        world.hooks().start_event_args(id, 6, args);
    }
    item = world.get(id);
    if (item && (item->z & 0xFF) != 0xFE) damage_item(kernel, world, anims, id, damage);
}

// @orig 10A8:275F
void hit_item(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, int damage, int fire_type) {
    got_hit(kernel, world, anims, id, kHitByDamage, static_cast<std::uint16_t>((fire_type & 0xFF) << 8 | (damage & 0xFF)));
}

// @orig 1150:0891
void explode(Kernel& kernel, World& world, const AnimDat& anims, ObjId id, int size, bool destroy, bool splash) {
    Item* item = world.get(id);
    if (!item || (item->z & 0xFF) == 0xFE) return;
    world.set_broken(*item);
    if (item->flags & kItemExploded) return;
    item->flags |= kItemExploded | kItemDamaged;
    if (world.hooks().explosion) world.hooks().explosion(std::clamp(size, 0, 2));
    Audio* audio = world.audio();
    if (audio) audio->sources.stop_item(id);
    int divisor = size + 1;
    if (divisor == 1) divisor = 3;
    else if (divisor == 3) divisor = 1;
    const dtable::FireType* ft = dtable::fire_type(4);
    const int radius = (ft ? ft->splash() : 0) / divisor;
    const int damage = (ft ? ft->min_damage() + rnd(world, ft->max_damage() - ft->min_damage() + 1) : 0) / divisor;
    // The explosion sprite (tables DS:2A8A, 2A8E, 2A94) at the item's
    // centre, frames 0..0x27, one tick each. Then sound 0x1C or 0x6C, played
    // at the controlled NPC (DS:2852), so always at full volume on screen.
    static constexpr std::uint16_t kSmall[] = {0x31C, 0x31F};
    static constexpr std::uint16_t kMedium[] = {0x326, 0x320, 0x321};
    static constexpr std::uint16_t kLarge[] = {0x324, 0x323, 0x325};
    const std::uint16_t sprite = size == 0 ? kSmall[crt_rnd(world, 2)] : size == 1 ? kMedium[crt_rnd(world, 3)] : kLarge[crt_rnd(world, 3)];
    const ShapeInfo* info = world.shape_info(item->shape);
    const World::Point centre{item->x - (info ? info->footpad_x * 8 : 0), item->y - (info ? info->footpad_y * 8 : 0),
                              (item->z & 0xFF) + (info ? info->footpad_z * 4 : 0)};
    create_sprite(kernel, world, sprite, 0, 0x27, 1, centre);
    auto boom = [&] {
        const int sfx = rnd(world, 2) == 0 ? 0x1C : 0x6C;
        if (audio) audio->sources.play(world, sfx, world.state.controlled_npc);
    };
    if (!splash) {
        if (destroy) world.destroy(id);
        boom();
        return;
    }
    const World::Point at{item->x, item->y, item->z & 0xFF};
    boom();
    splash_damage(kernel, world, anims, at, radius, damage, 4, id, 0);
    if (destroy && world.get(id)) world.destroy(id);
}

// @orig 1128:039C
void splash_damage(Kernel& kernel, World& world, const AnimDat& anims, const World::Point& at, int radius,
                   int damage, int fire_type, ObjId exclude, ObjId exclude2) {
    const int reach = radius * 16;
    damage = std::min(damage, 250);
    for (ObjId id : world.rect_search(at.x - reach, at.y - reach, at.x + reach, at.y + reach)) {
        const Item* item = world.get(id);
        if (!item || (item->z & 0xFF) == 0xFE || item->shape == kProjectileShape || id == exclude) continue;
        if (id == exclude2 && exclude2 != world.state.controlled_npc) continue;
        int dealt = damage;
        if (fire_type == 3 || fire_type == 4 || fire_type == 10) {
            const int distance = std::max({std::abs(static_cast<std::int16_t>(at.x - item->x)),
                                           std::abs(static_cast<std::int16_t>(at.y - item->y)),
                                           std::abs(at.z - (item->z & 0xFF))});
            const int steps = std::max(1, distance / 16 / 3);
            dealt = damage / steps;
        }
        const int dir = direction_16(at.x, at.y, item->x, item->y);
        apply_damage(kernel, world, anims, id, 0, dir, dealt, fire_type, &at);
    }
}

}  // namespace crusader
