#include "engine/world/death_loot.hpp"

#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace crusader {

namespace {

// The death shapes NPCDEATH (0x0A20 in the French data) makes no body for:
// it destroys the NPC's contents and leaves it, or puts remains in its place.
constexpr std::uint16_t kRemains[] = {0x58F, 0x59C, 0x576, 0x596};

constexpr std::uint8_t kFamilyWeapon = 10;

// Around the remains, nearest first: the 8 directions at 32 units, then at 64.
constexpr std::array<std::array<int, 2>, 16> kSpots = {{{32, 0},   {0, 32},   {-32, 0},  {0, -32},
                                                       {32, 32},  {-32, 32}, {32, -32}, {-32, -32},
                                                       {64, 0},   {0, 64},   {-64, 0},  {0, -64},
                                                       {64, 64},  {-64, 64}, {64, -64}, {-64, -64}}};

// The first shape of family 6 (a container): the box the loot function fills.
// @orig none: port-only cheat (loot from remains)
std::uint16_t container_shape(const World& world) {
    for (std::uint16_t shape = 1; shape < 0x800; ++shape) {
        if (world.family_of(shape) == kFamilyContainer) return shape;
    }
    return 0;
}

// Whether an action of `shape` has a frame that calls usecode event
// calledFromAnim (flag 0x4000), from which NPCDEATH starts.
// @orig none: port-only cheat (loot from remains)
bool calls_event(const AnimDat& anims, std::uint16_t shape) {
    for (std::uint16_t a = 0; a < 256; ++a) {
        const AnimAction* action = anims.action(shape, a);
        if (action && std::ranges::any_of(action->frames, [](const AnimFrame& f) { return f.event; })) return true;
    }
    return false;
}

// Each item to the floor beside `npc`, one spot each while free ones last
// (the NPC itself left out of the test), else on the NPC's own place; then it
// falls if nothing holds it.
// @orig none: port-only cheat (loot from remains)
void put_down(Kernel& kernel, World& world, ObjId npc, const std::vector<ObjId>& items) {
    const Item* remains = world.get(npc);
    if (!remains) return;
    const World::Point at{remains->x, remains->y, remains->z & 0xFF};
    std::size_t next = 0;
    for (ObjId id : items) {
        const Item* item = world.get(id);
        if (!item) continue;
        World::Point spot = at;
        while (next < kSpots.size()) {
            const World::Point p{at.x + kSpots[next][0], at.y + kSpots[next][1], at.z};
            ++next;
            if (p.x >= 0 && p.y >= 0 && position_free(world, item->shape, p, nullptr, npc)) {
                spot = p;
                break;
            }
        }
        world.push(id);
        world.pop_to_coords(spot.x, spot.y, spot.z);
        fall(kernel, world, id);
    }
}

}  // namespace

// @orig none: port-only cheat (loot from remains)
bool leaves_remains(std::uint16_t shape) {
    return std::ranges::find(kRemains, shape) != std::end(kRemains);
}

// @orig none: port-only cheat (loot from remains)
void note_death_loot(Kernel& kernel, World& world, const AnimDat& anims, ObjId npc, std::uint16_t alive_shape) {
    world.death_loot().erase(npc);
    if (!world.remains_loot() || npc == 1) return;
    const Item* item = world.get(npc);
    if (!item || item->shape == alive_shape || leaves_remains(alive_shape) || !leaves_remains(item->shape)) return;
    world.death_loot()[npc] = alive_shape;
    if (!calls_event(anims, item->shape)) drop_death_loot(kernel, world, npc);
}

// @orig none: port-only cheat (loot from remains)
void drop_death_loot(Kernel& kernel, World& world, ObjId npc) {
    auto& notes = world.death_loot();
    const auto noted = notes.find(npc);
    if (noted == notes.end()) return;
    const std::uint16_t alive_shape = noted->second;
    notes.erase(noted);
    Item* item = world.get(npc);
    if (!item || !world.is_dead(npc) || !leaves_remains(item->shape)) return;

    // NPCDEATH 0x21: a body gets the NPC's weapons.
    std::vector<ObjId> loot;
    for (ObjId id : item->contents) {
        const Item* c = world.get(id);
        if (c && world.family_of(c->shape) == kFamilyWeapon) loot.push_back(id);
    }
    // NPCDEATH 0x22, the loot of a body, given the dead NPC and a box out of
    // the world in place of the body (popToContainer only fills family 6).
    // It reads the NPC's shape only: the one it had alive is put back for
    // the call.
    const std::uint16_t box_shape = container_shape(world);
    const ObjId box = box_shape != 0 && world.hooks().call_class_event ? world.create(box_shape, 0) : 0;
    if (box != 0) {
        const std::uint16_t remains_shape = item->shape;
        item->shape = alive_shape;
        const std::uint8_t body[2] = {static_cast<std::uint8_t>(box), static_cast<std::uint8_t>(box >> 8)};
        world.hooks().call_class_event("NPCDEATH", 0x22, npc, body);
        if (Item* again = world.get(npc)) again->shape = remains_shape;
        if (const Item* filled = world.get(box)) loot.insert(loot.end(), filled->contents.begin(), filled->contents.end());
    }
    put_down(kernel, world, npc, loot);
    if (box != 0) world.destroy(box);
}

}  // namespace crusader
