#include "engine/world/pickup_list.hpp"

#include "core/dtable.hpp"
#include "core/language.hpp"
#include "engine/kernel.hpp"
#include "engine/world/world.hpp"

namespace crusader {

namespace {

constexpr std::uint16_t kKeycard = 0x111;
constexpr std::uint16_t kCredits = 0x4ED;
constexpr std::uint32_t kTicksPerSecond = 60;

struct Description {
    const char* name = nullptr;
    std::uint16_t shape = 0;
    std::uint16_t frame = 0;
};

// @orig 1190:070F
Description describe(const World& world, const Item& item) {
    if (item.shape == kKeycard) return {tr("KEYCARD"), 5, static_cast<std::uint16_t>(item.frame + 13)};
    if (item.shape == kCredits) return {tr("CREDITS"), 3, 13};
    switch (world.family_of(item.shape)) {
    case 10: return {dtable::weapon_name(item.shape), 3, dtable::weapon_icon(item.shape)};
    case 11: return {dtable::ammo_name(item.shape), 5, dtable::ammo_icon(item.shape)};
    case 12: return {dtable::family12_name(item.shape), 4, dtable::item_icon_family12(item.shape)};
    case 13: return {dtable::item_name(item.shape), 5, dtable::item_icon(item.shape)};
    default: return {};  // fatal assertion in the original
    }
}

}  // namespace

// @orig 1190:049C
void PickupList::add(const World& world, ObjId item_id) {
    const Item* item = world.get(item_id);
    if (!item) return;
    const Description d = describe(world, *item);
    if (!d.name) return;
    for (PickupEntry& e : entries_) {
        if (!e.name || e.icon_shape != d.shape || e.icon_frame != d.frame) continue;
        // The original compares the icon shape, not the item's, with 0x4ED.
        e.count += 1;
        e.seconds = kSeconds;
        return;
    }
    for (PickupEntry& e : entries_) {
        if (e.name) continue;
        e = {d.name, d.shape, d.frame, 1, kSeconds};
        if (item->shape == kCredits) e.count = item->quality ? static_cast<std::int16_t>(item->quality) : 1;
        return;
    }
}

// @orig 1190:0414
void PickupList::count_second() {
    for (PickupEntry& e : entries_) {
        if (e.name && --e.seconds == 0) e = {};
    }
}

// @orig 1190:0000
PickupListProcess::PickupListProcess(GameState& state) : state_(&state) { set_runs_at_any_level(true); }

void PickupListProcess::on_added() { last_ = kernel().ticks(); }

// @orig 1190:0057
void PickupListProcess::run() {
    const std::uint32_t now = kernel().ticks();
    if (now - last_ < kTicksPerSecond) return;
    last_ = now;
    state_->pickups.count_second();
}

}  // namespace crusader
