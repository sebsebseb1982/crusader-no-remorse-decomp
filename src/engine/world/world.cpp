#include "engine/world/world.hpp"

#include "core/bytes.hpp"
#include "core/dtable.hpp"
#include "core/language.hpp"
#include "engine/audio/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <tuple>

namespace crusader {

namespace {
constexpr std::size_t kRecordSize = 16;
constexpr std::uint8_t kFamilyGlobEgg = 3;
// The item table of a saved game (entry 0): one array per field, in this
// order and with these widths (x, y, z, shape, frame, status, quality, NPC
// number, map, next, array 11, hit points). The entry is 20 bytes per slot:
// the last 10240 are not written by the original (left as they are).
constexpr std::size_t kFieldWidths[] = {2, 2, 1, 2, 1, 2, 2, 1, 1, 2, 2, 1};
constexpr std::size_t kItemTableSize = kItemSlots * 20;
// Entry 1: map, fast area, cell heads, cell counts, then the list words.
constexpr std::size_t kCellWords = 64 * 64;
constexpr std::size_t kListWordsAt = 10 + 2 * 2 * kCellWords;
}  // namespace

// @orig 10B0:01E7
std::vector<GlobItem> read_glob(const FlexArchive& globs, std::size_t index) {
    const auto d = globs.entry(index);
    std::vector<GlobItem> items;
    if (d.size() < 2) return items;
    const std::uint16_t count = read_u16(d, 0);
    items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t pos = 2 + 6 * i;
        items.push_back({read_u8(d, pos), read_u8(d, pos + 1), read_u8(d, pos + 2), read_u16(d, pos + 3),
                         read_u8(d, pos + 5)});
    }
    return items;
}

// @orig 10B0:01E7
std::size_t World::expand_glob_egg(const FlexArchive& globs, ObjId egg_id) {
    // GLOB.C: each item is created, popped to its place and flagged 0x82.
    // Coordinates in usecode units: x = (egg.x & ~0x1FF) + 2 * gx + 1.
    const Item* egg = get(egg_id);
    if (!egg || family(*egg) != kFamilyGlobEgg || egg->quality == 0) return 0;
    const std::int32_t base_x = egg->x & ~0x1FF, base_y = egg->y & ~0x1FF, base_z = egg->z;
    std::size_t created = 0;
    // Far from the drawn area the original loads the items straight into the
    // cell (10B0:00A0), without Item::I_enterFastArea.
    expanding_glob_ = !glob_near_view(base_x, base_y);
    for (const GlobItem& g : read_glob(globs, egg->quality)) {
        const ObjId id = create(g.shape, g.frame);
        if (!id) continue;
        pop_to_coords(base_x + 2 * g.x + 1, base_y + 2 * g.y + 1, (base_z + g.z) & 0xFF);
        get(id)->flags |= kItemDisposable | 0x0002;
        ++created;
    }
    expanding_glob_ = false;
    return created;
}

// @orig 10A0:039C
bool World::glob_near_view(std::int32_t base_x, std::int32_t base_y) const {
    // The egg's chunk corner projected without z, widened by 376 / 250 / 255
    // pixels, against the drawn area (768 x 640 for a 640 x 480 view).
    const auto wx = static_cast<std::uint16_t>(base_x + 1), wy = static_cast<std::uint16_t>(base_y + 1);
    const int sx = static_cast<std::int16_t>((wx >> 1) - (wy >> 1));
    const int sy = static_cast<std::int16_t>((wx >> 2) + (wy >> 2));
    const int right = drawn_left_ + state.screen_right + 128, bottom = drawn_top_ + state.screen_bottom + 160;
    return sx - 376 <= right && sx + 375 >= drawn_left_ && sy - 250 <= bottom && sy + 255 >= drawn_top_;
}

// @orig 10B0:01E7
std::size_t World::expand_glob_eggs(const FlexArchive& globs) {
    std::vector<ObjId> eggs;
    for_each([&](const Item& i) {
        if (family(i) == kFamilyGlobEgg && i.parent == 0) eggs.push_back(i.id);
    });
    // Items already in the world, by position and look, to skip eggs whose
    // items a saved game already contains.
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t, std::uint16_t, std::uint16_t>> present;
    for_each([&](const Item& i) {
        if (i.parent == 0) present.emplace(i.x, i.y, i.z, i.shape, i.frame);
    });
    std::size_t created = 0;
    for (ObjId egg : eggs) {
        const Item& e = *get(egg);
        if (e.quality == 0) continue;
        bool saved = false;
        for (const GlobItem& g : read_glob(globs, e.quality)) {
            saved |= present.count({(e.x & ~0x1FF) + 2 * g.x + 1, (e.y & ~0x1FF) + 2 * g.y + 1, (e.z + g.z) & 0xFF,
                                    g.shape, g.frame}) != 0;
        }
        if (!saved) created += expand_glob_egg(globs, egg);
    }
    return created;
}

// @orig 10B8:03EF 10B0:05D6
std::vector<FixedRecord> read_fixed_map(const FlexArchive& fixed, std::size_t map) {
    const auto d = fixed.entry(map);
    std::vector<FixedRecord> records;
    records.reserve(d.size() / kRecordSize);
    for (std::size_t pos = 0; pos + kRecordSize <= d.size(); pos += kRecordSize) {
        FixedRecord r;
        r.x = read_u16(d, pos);
        r.y = read_u16(d, pos + 2);
        r.z = read_u8(d, pos + 4);
        r.shape = read_u16(d, pos + 5);
        r.frame = read_u8(d, pos + 7);
        r.flags = read_u16(d, pos + 8);
        r.quality = read_u16(d, pos + 10);
        r.npc_number = read_u8(d, pos + 12);
        r.map_number = read_u8(d, pos + 13);
        r.next = read_u16(d, pos + 14);
        records.push_back(r);
    }
    return records;
}

Item* World::get(ObjId id) {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

const Item* World::get(ObjId id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

// @orig 10B0:262A
Item& World::add(Item item) {
    // The head of the free item slots. With none left (viewers expanding a
    // whole map's globs), the port goes on past the table, where the
    // original stops on an assertion.
    ObjId id = 0;
    if (!free_items_.ids.empty()) {
        id = free_items_.ids.front();
        remove_free(id);
    } else {
        id = static_cast<ObjId>(std::max<std::size_t>(next_id_, kItemSlots));
        while (items_.contains(id)) {
            if (id == 0xFFFF) throw std::runtime_error("item table full");
            ++id;
        }
    }
    stale_.erase(id);
    item.id = id;
    if (id >= next_id_) next_id_ = static_cast<ObjId>(id + 1);
    Item& added = items_.emplace(id, std::move(item)).first->second;
    link_to_cell(added);
    return added;
}

// @orig none: an item placed at its saved id
Item& World::add_with_id(Item item) {
    if (item.id == 0) throw std::runtime_error("item id 0 is reserved");
    if (item.id >= next_id_) next_id_ = static_cast<ObjId>(item.id + 1);
    stale_.erase(item.id);
    remove_free(item.id);


    if (const Item* old = get(item.id)) unlink_from_cell(*old);
    items_.erase(item.id);
    const ObjId id = item.id;
    Item& added = items_.emplace(id, std::move(item)).first->second;
    link_to_cell(added);
    return added;
}

std::vector<ObjId>* World::cell_of(const Item& item) {
    const int cx = (item.x & 0xFFFF) >> kCellShift, cy = (item.y & 0xFFFF) >> kCellShift;
    if (cx >= kGridSize || cy >= kGridSize) return nullptr;
    return &cells_[static_cast<std::size_t>(cx * kGridSize + cy)];
}

void World::link_to_cell(const Item& item) {
    if (!on_map(item)) return;
    if (auto* c = cell_of(item)) c->insert(c->begin(), item.id);
}

void World::unlink_from_cell(const Item& item) {
    if (auto* c = cell_of(item)) std::erase(*c, item.id);
}

std::span<const ObjId> World::cell(int cx, int cy) const {
    if (cx < 0 || cy < 0 || cx >= kGridSize || cy >= kGridSize) return {};
    return cells_[static_cast<std::size_t>(cx * kGridSize + cy)];
}

// @orig none: consistency check for the tests
bool World::cells_consistent() const {
    std::size_t linked = 0;
    for (int cx = 0; cx < kGridSize; ++cx) {
        for (int cy = 0; cy < kGridSize; ++cy) {
            for (ObjId id : cell(cx, cy)) {
                const Item* i = get(id);
                if (!i || !on_map(*i) || ((i->x & 0xFFFF) >> kCellShift) != cx ||
                    ((i->y & 0xFFFF) >> kCellShift) != cy) {
                    return false;
                }
                ++linked;
            }
        }
    }
    std::size_t expected = 0;
    for (const auto& [id, i] : items_) {
        if (on_map(i) && ((i.x & 0xFFFF) >> kCellShift) < kGridSize && ((i.y & 0xFFFF) >> kCellShift) < kGridSize)
            ++expected;
    }
    return linked == expected;
}

// @orig 10B0:0FED (entries 0 and 1; entries 10 and 20 as NPC_10f0_00e5 and 38dd)
std::size_t World::load_saved_items(const FlexArchive& save) {
    // Item table, saved as one array per field (10240 slots; slot number =
    // object id).
    constexpr std::size_t kSlots = kItemSlots;
    const auto d = save.entry(0);
    const auto& widths = kFieldWidths;
    std::size_t offsets[12];
    std::size_t pos = 0;
    for (std::size_t f = 0; f < 12; ++f) {
        offsets[f] = pos;
        pos += kSlots * widths[f];
    }
    check_range(d, 0, pos);
    auto field = [&](std::size_t f, std::size_t slot) -> std::uint16_t {
        const std::size_t at = offsets[f] + slot * widths[f];
        return widths[f] == 2 ? read_u16(d, at) : read_u8(d, at);
    };

    items_.clear();
    clear_tracked();
    for (auto& c : cells_) c.clear();
    next_id_ = 1;
    init_free_slots();

    // Entry 1 ends with the free lists (counts, heads, tails) and the
    // pending stack (top, count), chained through array 9 like the cells.
    const auto lists = save.entry(1);
    constexpr std::size_t kListsEnd = kListWordsAt;
    std::vector<ObjId> free_items, free_npcs, pending;

    ObjId item_tail = static_cast<ObjId>(kItemSlots - 1), npc_tail = static_cast<ObjId>(kNpcSlots - 1);
    std::vector<bool> is_free(kSlots);
    auto chain = [&](ObjId head) {
        std::vector<ObjId> ids;
        for (ObjId id = head; id != 0 && id < kSlots && ids.size() < kSlots; id = field(9, id)) ids.push_back(id);
        return ids;
    };
    const bool has_lists = lists.size() >= kListsEnd + 16;
    if (has_lists) {
        free_items = chain(read_u16(lists, kListsEnd + 4));
        free_npcs = chain(read_u16(lists, kListsEnd + 6));
        item_tail = read_u16(lists, kListsEnd + 8);
        npc_tail = read_u16(lists, kListsEnd + 10);
        pending = chain(read_u16(lists, kListsEnd + 12));
        for (ObjId id : free_items) is_free[id] = true;
        for (ObjId id : free_npcs) is_free[id] = true;
    }

    slot0_array11_ = field(10, 0);
    std::size_t count = 0;
    for (std::size_t slot = 1; slot < kSlots; ++slot) {

        const std::uint16_t shape = field(3, slot);
        if (shape == 0 && !is_free[slot]) continue;
        Item item;
        item.id = static_cast<ObjId>(slot);
        item.x = field(0, slot);
        item.y = field(1, slot);
        item.z = field(2, slot);
        item.shape = shape;
        item.frame = static_cast<std::uint8_t>(field(4, slot));
        // ITEMCACH_10b0_0fed clears status bit 0 of every slot once read.
        item.flags = static_cast<std::uint16_t>(field(5, slot) & ~0x0001);

        item.quality = field(6, slot);
        item.npc_number = static_cast<std::uint8_t>(field(7, slot));
        item.map_number = static_cast<std::uint8_t>(field(8, slot));
        item.save_u2 = field(10, slot);
        item.hit_points = static_cast<std::uint8_t>(field(11, slot));
        if (is_free[slot]) {
            stale_[item.id] = std::move(item);  // a free slot, with what it held last
            continue;
        }
        add_with_id(std::move(item));
        ++count;
    }
    if (has_lists) {
        free_items_ = {};
        free_npcs_ = {};
        free_mask_.reset();
        // Port saves made before 2026-10-15 left array 9 at 0 for slots never
        // used (z = 0xFE, shape 0), which cut the free chains short; games
        // loaded from them saved the short lists again. Such slots, in no
        // list (the original's saves have none), come after the chain in id
        // order, as init_free_slots leaves them.
        auto repair = [&](std::vector<ObjId>& chained, ObjId from, ObjId to) {
            for (ObjId id = from; id < to; ++id) {
                if (!is_free[id] && !items_.contains(id) && field(3, id) == 0 && field(2, id) == 0xFE) {
                    chained.push_back(id);
                    is_free[id] = true;
                }
            }
        };
        repair(free_items, static_cast<ObjId>(kFirstItemId), static_cast<ObjId>(kSlots));
        repair(free_npcs, 2, static_cast<ObjId>(kNpcSlots));
        for (ObjId id : free_items) free_items_.ids.push_back(id);
        for (ObjId id : free_npcs) free_npcs_.ids.push_back(id);
        for (ObjId id : free_items) free_mask_.set(id);
        for (ObjId id : free_npcs) free_mask_.set(id);
        free_items_.tail = item_tail;
        free_npcs_.tail = npc_tail;
    } else {
        // No lists saved: every unused slot is free, in id order.
        free_items_ = {};
        free_npcs_ = {};
        free_mask_.reset();
        for (std::size_t id = kSlots - 1; id >= 2; --id) {
            if (id != 1 && !items_.contains(static_cast<ObjId>(id))) push_free(static_cast<ObjId>(id));
        }
    }
    pending_.clear();
    for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        if (items_.contains(*it)) pending_.push_back(*it);
    }

    // Entry 1 starts with the u16 map number, then the fast area (s0, s1, d0, d1).
    if (lists.size() >= 10) {
        auto word = [&](std::size_t at) { return static_cast<std::int16_t>(read_u16(lists, at)); };
        current_map_ = word(0);
        fast_area_ = {word(2), word(4), word(6), word(8)};
    }

    // Map grid: cell heads follow the header (64 x 64 words, x-major), and
    // each chain continues through array 9. Rebuilt in that order.
    if (lists.size() >= 10 + kGridSize * kGridSize * 2) {
        for (auto& c : cells_) c.clear();
        for (int cx = 0; cx < kGridSize; ++cx) {
            for (int cy = 0; cy < kGridSize; ++cy) {
                auto& c = cells_[static_cast<std::size_t>(cx * kGridSize + cy)];
                ObjId id = read_u16(lists, 10 + 2 * static_cast<std::size_t>(cx * kGridSize + cy));
                while (id != 0 && id < kSlots && c.size() < kSlots) {
                    const Item* i = get(id);
                    if (!i || !on_map(*i)) break;
                    c.push_back(id);
                    id = field(9, id);
                }
                const std::size_t at = static_cast<std::size_t>(cx * kGridSize + cy);
                cell_count_bias_[at] =
                    static_cast<std::int16_t>(read_u16(lists, 10 + kGridSize * kGridSize * 2 + 2 * at) - c.size());
            }
        }
    }
    // An NPC of another map, in no cell, is kept off the map (ITEMCACH_10b0_04c9 unlinks it).
    auto in_cell = [&](ObjId id) {
        return std::ranges::any_of(cells_, [&](const auto& c) { return std::ranges::find(c, id) != c.end(); });
    };
    for (auto& [id, item] : items_) {
        if (is_npc_id(id) && item.map_number != 0 && item.map_number != current_map_ && !(item.flags & kItemContained) && !in_cell(id)) {
            item.off_map = true;
        }
    }

    // Entry 10: NPC table.
    const auto npcs = save.entry(10);
    if (npcs.size() == npcs_.size()) {
        std::ranges::copy(npcs, npcs_.begin());
        // High byte of the NPCs' frames.
        for (ObjId id = 1; id < kNpcSlots; ++id) {
            if (Item* npc = get(id)) npc->frame |= static_cast<std::uint16_t>(npcs_[id * kNpcRecordSize + 0x0F] << 8);
        }
    }

    // Entry 20 (NPC_10f0_390d): the 21-byte block at DS:7E0E (player id,
    // ...); +0xB is the difficulty (DS:7E19, World::I_gameDifficulty), +0xD
    // the keycards (DS:7E1B), +0x11 the battery (DS:7E1F, MainActor::I_getMaxEnergy).
    const auto player = save.entry(20);
    state.player_block.fill(0);
    std::copy_n(player.begin(), std::min(player.size(), state.player_block.size()), state.player_block.begin());
    if (player.size() >= 0x04) state.avatar_in_stasis = (player[0x02] | player[0x03] << 8) != 0;
    if (player.size() >= 0x0D) state.difficulty
 = static_cast<std::uint16_t>(player[0x0B] | player[0x0C] << 8);
    if (player.size() >= 0x11) state.keycards = read_u32(player, 0x0D);
    if (player.size() >= 0x13) state.battery = static_cast<std::uint16_t>(player[0x11] | player[0x12] << 8);
    // +4: the state of SYSTEM.C's generator (DS:7E12), set back by
    // NPC_10f0_38dd through random_seed.
    if (player.size() >= 0x08) state.random_state = random_seed(read_u32(player, 0x04));

    // Array 9 links items in lists: map grid cells (heads in entry 1), the
    // pending stack, and container contents, which start at the container's
    // quality field (families 6 and 7) and hold the container id in x.
    std::set<ObjId> linked;
    for (auto& [id, item] : items_) {
        const std::uint8_t fam = family(item);
        if (fam != kFamilyContainer && fam != kFamilyActorContainer) continue;
        for (ObjId c = item.quality; c != 0 && c < kSlots && linked.size() < kSlots; c = field(9, c)) {
            Item* child = get(c);
            if (!child || !(child->flags & kItemContained) || child->x != id || !linked.insert(c).second) break;
            child->parent = id;
            item.contents.push_back(c);
        }
    }
    // Contained items missing from the chains (in SAVEGAME.13, 16 of 65,
    // probably carried by NPCs): attached by their x field.
    for (auto& [id, item] : items_) {
        if (!(item.flags & kItemContained) || linked.count(id)) continue;
        if (Item* parent = get(static_cast<ObjId>(item.x))) {
            item.parent = parent->id;
            item.unchained = true;
            parent->contents.push_back(id);
        }
    }
    return count;
}

// @orig 10B0:0B67 (entry 0)
std::vector<std::uint8_t> World::saved_item_table() const {
    std::vector<std::uint8_t> out(kItemTableSize, 0);
    std::size_t offsets[12];
    std::size_t pos = 0;
    for (std::size_t f = 0; f < 12; ++f) {
        offsets[f] = pos;
        pos += kItemSlots * kFieldWidths[f];
    }
    auto put = [&](std::size_t f, std::size_t slot, std::uint32_t v) {
        const std::size_t at = offsets[f] + slot * kFieldWidths[f];
        out[at] = static_cast<std::uint8_t>(v);
        if (kFieldWidths[f] == 2) out[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };

    // Array 9 chains the lists: map cells, container contents, the pending
    // stack from its top, the free lists. The last item of a list, and an
    // item in none (NPCs of other maps), get 0. Sprites (port-only transient
    // effects) are filtered out: they must not come back on load.
    std::vector<ObjId> next(kItemSlots, 0);
    auto link = [&](auto first, auto last) {
        for (auto it = first; it != last && std::next(it) != last; ++it) next[*it] = *std::next(it);
    };
    auto link_filtered = [&](const auto& ids) {
        ObjId prev = 0;
        for (ObjId id : ids) {
            const Item* item = get(id);
            if (!item || item->sprite_only) continue;
            if (prev != 0) next[prev] = id;
            prev = id;
        }
    };
    for (const auto& c : cells_) link_filtered(c);
    auto chained = [&](const Item& container) {
        std::vector<ObjId> ids;
        for (ObjId c : container.contents) {
            if (const Item* child = get(c); child && !child->unchained && !child->sprite_only) ids.push_back(c);
        }
        return ids;
    };
    for (const auto& [id, item] : items_) {
        const auto ids = chained(item);
        link_filtered(ids);
    }
    // Pending stack: link from top down.
    {
        ObjId prev = 0;
        for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
            const Item* item = get(*it);
            if (!item || item->sprite_only) continue;
            if (prev != 0) next[prev] = *it;
            prev = *it;
        }
    }
    link(free_items_.ids.begin(), free_items_.ids.end());
    link(free_npcs_.ids.begin(), free_npcs_.ids.end());

    // Slots never used are at z = 0xFE (ITEMCACH_10b0_161b), chained in
    // their free list like the others.
    for (std::size_t slot = 0; slot < kItemSlots; ++slot) {
        put(2, slot, 0xFE);
        put(9, slot, next[slot]);
    }
    auto write = [&](const Item& item) {
        if (item.id >= kItemSlots) throw std::runtime_error("item " + std::to_string(item.id) + " past the item table");
        const std::size_t s = item.id;
        put(0, s, static_cast<std::uint32_t>(item.x));
        put(1, s, static_cast<std::uint32_t>(item.y));
        put(2, s, static_cast<std::uint32_t>(item.z));
        put(3, s, item.shape);
        put(4, s, item.frame);
        put(5, s, item.flags);
        put(6, s, item.quality);
        put(7, s, item.npc_number);
        put(8, s, item.map_number);
        put(9, s, next[s]);
        put(10, s, item.save_u2);
        put(11, s, item.hit_points);
    };
    put(10, 0, slot0_array11_);
    for (const auto& [id, item] : stale_) write(item);
    for (const auto& [id, item] : items_) {
        if (item.sprite_only) continue;
        write(item);
    }
    // A container's quality heads its chain.
    for (const auto& [id, item] : items_) {
        const std::uint8_t fam = family(item);
        if (fam != kFamilyContainer && fam != kFamilyActorContainer) continue;
        const auto ids = chained(item);
        put(6, id, ids.empty() ? 0 : ids.front());


    }
    return out;
}

// @orig 10B0:0B67 (entry 1)
std::vector<std::uint8_t> World::saved_item_lists() const {
    std::vector<std::uint8_t> out(kListWordsAt + 16, 0);
    auto word = [&](std::size_t at, std::uint32_t v) {
        out[at] = static_cast<std::uint8_t>(v);
        out[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    word(0, static_cast<std::uint32_t>(current_map_));  // DS:10FE
    word(2, static_cast<std::uint32_t>(fast_area_.s0));  // DS:7DC0
    word(4, static_cast<std::uint32_t>(fast_area_.s1));
    word(6, static_cast<std::uint32_t>(fast_area_.d0));
    word(8, static_cast<std::uint32_t>(fast_area_.d1));
    // Cell heads (DS:1100) and counts (DS:1104), x-major. Sprites (port-only
    // transient effects) are skipped in the heads and counts.
    for (std::size_t at = 0; at < kCellWords; ++at) {
        const auto& c = cells_[at];
        ObjId head = 0;
        std::size_t count = 0;
        for (ObjId id : c) {
            const Item* item = get(id);
            if (item && !item->sprite_only) {
                if (head == 0) head = id;
                ++count;
            }
        }
        word(10 + 2 * at, head);
        word(10 + 2 * kCellWords + 2 * at, static_cast<std::uint32_t>(static_cast<int>(count) + cell_count_bias_[at]));
    }
    auto head = [](const FreeSlots& l) -> std::uint32_t { return l.ids.empty() ? 0 : l.ids.front(); };
    word(kListWordsAt + 0, static_cast<std::uint32_t>(free_items_.ids.size()));  // DS:7DD0
    word(kListWordsAt + 2, static_cast<std::uint32_t>(free_npcs_.ids.size()));   // DS:7DD2
    word(kListWordsAt + 4, head(free_items_));                                    // DS:7DC8
    word(kListWordsAt + 6, head(free_npcs_));                                     // DS:7DCA
    word(kListWordsAt + 8, free_items_.tail);                                     // DS:7DCC
    word(kListWordsAt + 10, free_npcs_.tail);                                     // DS:7DCE
    // Pending stack: its top is the last non-sprite, its count excludes them.
    ObjId pending_top = 0;
    std::size_t pending_count = 0;
    for (ObjId id : pending_) {
        const Item* item = get(id);
        if (item && !item->sprite_only) {
            pending_top = id;
            ++pending_count;
        }
    }
    word(kListWordsAt + 12, pending_top);                                          // DS:7DD4
    word(kListWordsAt + 14, static_cast<std::uint32_t>(pending_count));            // DS:7DD6
    return out;
}

// @orig 1128:05B7
std::vector<std::uint8_t> World::saved_tracker(int part) const {
    // 50: the slot count (DS:26E4), 51: the items tracked (DS:26E6), 52: the
    // slots (DS:26E0).
    std::vector<std::uint8_t> out;
    auto word = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    if (part == 0) word(kTrackedSlots);
    else if (part == 1) word(static_cast<std::uint32_t>(tracked_count_));
    else for (ObjId id : tracked_) word(id);
    return out;
}

// @orig 10C8:18C0
std::vector<ObjId> World::rect_search(
std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) const {
    // 16-bit coordinates; negative bounds become 0 (low) or 0x7FFF (high).
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 < 0) x1 = 0x7FFF;
    if (y1 < 0) y1 = 0x7FFF;
    std::vector<ObjId> found;
    for (int cy = y0 >> kCellShift; cy <= (y1 >> kCellShift) && cy < kGridSize; ++cy) {
        for (int cx = x0 >> kCellShift; cx <= (x1 >> kCellShift) && cx < kGridSize; ++cx) {
            for (ObjId id : cell(cx, cy)) {
                const Item* i = get(id);
                if (i && !i->sprite_only && i->x >= x0 && i->x <= x1 && i->y >= y0 && i->y <= y1) found.push_back(id);
            }
        }
    }
    return found;
}

std::uint8_t World::initial_hit_points(std::uint16_t shape) const {
    const std::size_t at = std::size_t{shape} * 6;
    if (at + 6 > damage_.size() || !(damage_[at] & 1)) return 1;
    return damage_[at + 5];
}

// @orig ?: outermost container position, not located
World::Point World::location(const Item& item) const {
    const Item* top = &item;
    while (top->flags & kItemContained) {
        const Item* parent = get(top->parent);
        if (!parent) break;
        top = parent;
    }
    return {top->x, top->y, top->z};
}

// @orig 10F0:236B 10B0:268D
ObjId World::create_npc(std::uint16_t shape, std::uint16_t frame) {
    ObjId slot = free_npcs_.ids.empty() ? 0 : free_npcs_.ids.front();
    for (ObjId id = 2; id < kNpcSlots && slot == 0; ++id) {
        const Item* i = get(id);
        if (!i) continue;

        // Status bit 0x0002 is required by the French executable only: the US
        // one (10F0:2378) takes any NPC outside the fast area.
        const std::uint16_t s = i->shape;
        if ((i->z & 0xFF) != 0xFE && s != 0x3AC && s != 0x4DC && s != 0x524 && s != 0x4D9 && s != 0x4E0 &&
            ((i->flags & 0x0002) || us_version()) && !(i->flags & kItemFastArea)) {
            destroy(id);
            slot = id;
        }
    }
    if (slot == 0) return 0;
    Item item;
    item.id = slot;
    item.shape = shape;
    item.npc_number = static_cast<std::uint8_t>(slot);
    // NPC_10f0_236b: a new or reused NPC belongs to the current map. Teleport
    // uses this field to unlink it before changing its coordinates.
    item.map_number = static_cast<std::uint8_t>(current_map_);
    add_with_id(std::move(item));
    // NPC_10f0_2610: the record starts cleared.
    auto r = npc_record(slot);
    std::fill(r.begin(), r.end(), std::uint8_t{0});
    set_frame(*get(slot), frame);
    push(slot);
    return slot;
}

// @orig 10A8:11E7 10B0:262A
ObjId World::create(std::uint16_t shape, std::uint8_t frame) {
    // Item::I_create (ITEM.C). The original reuses a free slot, keeping its
    // stale x/y/z; flags, quality, NPC and map numbers are cleared.
    if (shape > 0x7FF) return 0;
    Item item;
    item.shape = shape;
    item.frame = frame;
    item.hit_points = initial_hit_points(shape);
    const std::uint8_t fam = family(item);
    // Item::I_create (10A8:11E7): a weapon (family 10) gets the rounds of a
    // full clip (DTABLE_1120_0180), families 11 and 12 get quality 1; the
    // item's usecode event 0x0C ("combine") is then started (setQAndCombine).
    if (fam == 10) item.quality = dtable::clip_size(shape);
    else if (fam == 11 || fam == 12) item.quality = 1;
    const ObjId id = add(std::move(item)).id;
    push(id);
    if ((fam == 10 || fam == 11 || fam == 12) && hooks_.start_event) hooks_.start_event(id, 0x0C);
    return id;
}

void World::detach(Item& item) {
    if (Item* parent = get(item.parent)) std::erase(parent->contents, item.id);
    item.parent = 0;
}

// @orig 10B0:1C21
void World::push(ObjId id) {
    // ITEMCACH.C: already out of the world -> nothing. Otherwise the item
    // leaves its container or map cell and goes on top of the stack. An
    // equipped item leaves its NPC's equipment slots first (10B0:1C21).
    Item* item = get(id);
    if (!item || (item->flags & kItemEthereal)) return;
    if (item->flags & kItemEquipped) {
        if (item->flags & kItemContained) unequip(item->parent, id);
        item->flags &= static_cast<std::uint16_t>(~kItemEquipped);
    }
    if (on_map(*item)) unlink_from_cell(*item);
    detach(*item);
    item->unchained = false;
    item->flags |= kItemEthereal;
    pending_.push_back(id);
}

// @orig 10B0:1D29
ObjId World::pop_to_coords(std::int32_t x, std::int32_t y, std::int32_t z) {
    // Not reproduced yet: display list, camera follow when the item is the
    // controlled NPC.
    if (pending_.empty()) return 0;
    const ObjId id = pending_.back();
    pending_.pop_back();
    Item& item = *get(id);
    // ITEMCACH.C 1D29: the large shield's sprite moves with the player.
    if (id == 1 && state.shield_sprite != 0) {
        if (const Item* s = get(state.shield_sprite_item)) {
            move_item(s->id, s->x + (x - item.x), s->y + (y - item.y), (s->z + (z - item.z)) & 0xFF);
        }
    }
    if (id == state.controlled_npc && id == state.camera_target) track_camera_target(x, y, z);
    item.x = x;
    item.y = y;
    item.z = std::min<std::int32_t>(z, 250);
    if (fast_area_.contains(x, y)) {
        if (!(item.flags & kItemFastArea) && !expanding_glob_) enter_fast_area(item);
    } else if (item.flags & kItemFastArea) {
        leave_fast_area(item);
    }
    item.flags &= static_cast<std::uint16_t>(~(kItemContained | kItemEthereal));
    link_to_cell(item);
    return id;
}

FastArea FastArea::around(std::int32_t x, std::int32_t y) {
    const std::int32_t cx = (x & 0xFFFF) >> 9, cy = (y & 0xFFFF) >> 9;
    return {cx + cy - 5, cx + cy + 5, cx - cy - 3, cx - cy + 3};
}

bool FastArea::contains_cell(std::int32_t cx, std::int32_t cy) const {
    if (cx < 0 || cx >= 64 || cy < 0 || cy >= 64) return false;
    return s0 <= cx + cy && cx + cy <= s1 && d0 <= cx - cy && cx - cy <= d1;
}

bool World::has_fast_area_events(const Item& item) const {
    // NPCs, and shapes with TYPEFLAG byte 0 bit 7.
    const ShapeInfo* info = shape_info(item.shape);
    return is_npc_id(item.id) || (info && info->is(ShapeInfo::kNoisy));
}

// @orig 10A8:2B30
std::uint16_t World::enter_fast_area(Item& item) {
    // Families 4/7/8 and the NPC reset are handled below through hooks. Not
    // reproduced: the shape 0x4FE hook (CRUSYS_1060_021b) and the EggHatcher
    // a monster egg gets while DS:0907 is clear (only under the computer gump).
    item.flags |= kItemFastArea;
    if (item.sprite_only) return 0;  // port-only sprite items: no events, not tracked
    if (is_trackable(item.shape) && !(item.flags & kItemBroken)) track(item.id);
    // Dead NPCs stop here, except four shapes.
    if (is_npc_id(item.id) && is_dead(item.id) && item.shape != 0x576 && item.shape != 0x596 &&
        item.shape != 0x59C && item.shape != 0x58F) {
        return 0;
    }
    // Monster eggs (family 7) make their NPC (ITEMFIND_10e0_0247) and stop
    // here. Trigger eggs (family 4, teleport eggs of family 8 at frame 0)
    // get an EggHatcher process (SHAPHAND_1098_068b).
    const std::uint8_t fam = family(item);
    if (fam == 7 && hooks_.monster_egg) {
        hooks_.monster_egg(item.id);
        return 0;
    }
    if ((fam == 4 || (fam == 8 && item.frame == 0)) && hooks_.egg_hatcher) hooks_.egg_hatcher(item.id);
    if (is_npc_id(item.id) && hooks_.npc_entered_fast_area) hooks_.npc_entered_fast_area(item.id);
    if (has_fast_area_events(item) && hooks_.start_event) return hooks_.start_event(item.id, 0x0F);
    return 0;
}

// @orig 10A8:2DAD
void World::leave_fast_area(Item& item) {
    item.flags &= static_cast<std::uint16_t>(~kItemFastArea);
    if (item.sprite_only) return;
    if (is_trackable(item.shape) && !(item.flags & kItemBroken)) untrack(item.id);
    if (hooks_.fail_processes) hooks_.fail_processes(item.id);
    if (is_npc_id(item.id) && is_dead(item.id)) return;
    // Shapes with the sound flag (TYPEFLAG byte 6 bit 3) fall silent.
    const ShapeInfo* info = shape_info(item.shape);
    if (audio_ && info && info->is(ShapeInfo::kSound)) audio_->sources.stop_item(item.id);
    if (has_fast_area_events(item) && hooks_.start_event) hooks_.start_event(item.id, 0x10);
}

bool World::is_trackable(std::uint16_t shape) const {
    // ITEM_10a8_30a8 / 30d2.
    const ShapeInfo* info = shape_info(shape);
    return info && (info->is(ShapeInfo::kOccludes) || info->is(ShapeInfo::kTargetable));
}

// @orig 1128:0098
void World::track(ObjId id) {
    // First free slot; nothing happens when the list is full. An item can be
    // listed twice, as in the original.
    for (ObjId& slot : tracked_) {
        if (slot == 0) {
            slot = id;
            ++tracked_count_;
            return;
        }
    }
}

// @orig 1128:00FE
void World::untrack(ObjId id) {
    // First slot holding the id.
    for (ObjId& slot : tracked_) {
        if (slot == id) {
            slot = 0;
            --tracked_count_;
            return;
        }
    }
}

void World::clear_tracked() {
    tracked_.fill(0);
    tracked_count_ = 0;
}

// @orig 10A8:06A5
void World::set_shape(Item& item, std::uint16_t shape) {
    // Item_I_setShape (10A8:06A5). The fast area is not checked.
    const bool was = is_trackable(item.shape), will = is_trackable(shape);
    const bool broken = item.flags & kItemBroken;
    if (was && !broken && !will) untrack(item.id);
    else if (will && !was && !broken) track(item.id);
    item.shape = shape;
}

void World::set_broken(Item& item) {
    if (item.flags & kItemBroken) return;
    untrack(item.id);
    item.flags |= kItemBroken;
}

// @orig 1188:0CBA
void World::set_camera(std::int32_t x, std::int32_t y, std::int32_t z) {
    // 16-bit arithmetic of ANIMCACH.C: halves and quarters are taken before
    // the sum or difference.
    const std::uint16_t ux = static_cast<std::uint16_t>(x), uy = static_cast<std::uint16_t>(y);
    set_camera_view(x, y, z, (ux >> 1) - (uy >> 1) - view_origin_x(),
                    (ux >> 2) + (uy >> 2) - (z & 0xFF) - view_origin_y());
}

void World::set_camera_view(std::int32_t x, std::int32_t y, std::int32_t z, int left, int top) {
    const std::uint16_t ux = static_cast<std::uint16_t>(x), uy = static_cast<std::uint16_t>(y);
    // ANIMCACH.C 0CBA moves the drawn area after the fast area.
    if (!drawn_reset_) {
        drawn_left_ = state.view_left & ~127;
        drawn_top_ = state.view_top & ~127;
    }
    state.camera_x = ux;
    state.camera_y = uy;
    state.camera_z = z & 0xFF;
    state.view_left = static_cast<std::int16_t>(left);
    state.view_top = static_cast<std::int16_t>(top);
    // A rebuilt fast area stands for the saved one, whose globs were placed
    // around the view of the time.
    if (drawn_reset_) {
        drawn_left_ = state.view_left & ~127;
        drawn_top_ = state.view_top & ~127;
        drawn_reset_ = false;
    }
    update_fast_area(ux, uy);
    if (audio_) audio_->sources.update_volumes(*this);
}

// 640 x 480: 0x140 and 300.
int World::view_origin_x() const { return (state.screen_right + 1) / 2; }
int World::view_origin_y() const { return 300 + ((state.screen_bottom + 1) - 480) / 2; }

// @orig none: port-only native resolution
void World::set_view_size(int w, int h) {
    if (state.screen_right == w - 1 && state.screen_bottom == h - 1) return;
    state.screen_right = static_cast<std::int16_t>(w - 1);
    state.screen_bottom = static_cast<std::int16_t>(h - 1);
    set_camera(state.camera_x, state.camera_y, state.camera_z);
}

// @orig 10D8:01C7
std::uint8_t direction_16(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2) {
    // 16-bit differences; dy is positive towards -y.
    const auto dx = static_cast<std::int16_t>(x2 - x1), dy = static_cast<std::int16_t>(y1 - y2);
    if (dx == 0 && dy == 0) return 16;
    if (dy == 0) return dx > 0 ? 4 : 12;
    if (dx == 0) return dy > 0 ? 0 : 8;
    // PFLOAT.C 16.16 division: integer part in 16 bits, then the remainder
    // scaled by 65536. Thresholds 4, 4/3, 3/4 and 1/4 computed the same way.
    auto fdiv = [](std::uint32_t a, std::uint32_t b) {  // a, b: 16-bit integers
        const std::uint32_t q = a / b, r = a % b;
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int16_t>(q)) << 16 |
                                         ((r << 16) / b));
    };
    const std::int32_t ratio = fdiv(static_cast<std::uint16_t>(dy < 0 ? -dy : dy),
                                    static_cast<std::uint16_t>(dx < 0 ? -dx : dx));
    const std::int32_t four_thirds = fdiv(4, 3), three_quarters = fdiv(3, 4), quarter = fdiv(1, 4);
    // Per quadrant: steep, 4/3, 3/4, else, flat (ratio <= 1/4).
    auto pick = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint8_t flat) {
        if (ratio > 0x40000) return a;
        if (ratio > four_thirds) return b;
        if (ratio > three_quarters) return c;
        if (ratio <= quarter) return flat;
        return d;
    };
    if (dy > 0 && dx > 0) return pick(0, 1, 2, 3, 4);
    if (dy > 0) return pick(0, 15, 14, 13, 12);
    if (dx < 0) return pick(8, 9, 10, 11, 12);
    return pick(8, 7, 6, 5, 4);
}

// @orig 10D8:0120
std::uint8_t direction_8(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2) {
    // 16-bit arithmetic, as in the original.
    const auto ux = static_cast<std::int16_t>(x1 - x2), uy = static_cast<std::int16_t>(y1 - y2);
    if (ux == 0 && uy == 0) return 16;
    const auto ax = static_cast<std::int16_t>(ux < 0 ? -ux : ux), ay = static_cast<std::int16_t>(uy < 0 ? -uy : uy);
    const auto ax2 = static_cast<std::int16_t>(ax * 2);
    if (ay < ax / 2 || ax2 < ay) {  // close to an axis
        if (ay < ax2) return ux >= 0 ? 12 : 4;
        return uy >= 0 ? 0 : 8;
    }
    if (ux > 0 && uy > 0) return 14;
    if (ux < 0 && uy < 0) return 6;
    if (ux > 0 && uy < 0) return 10;
    return 2;
}

// @orig 10D8:0000
int closest_direction_in_range(std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2, int steps, int lo,
                               int hi) {
    const int dir = steps == 8 ? direction_8(x1, y1, x2, y2) : direction_16(x1, y1, x2, y2);
    if (dir >= lo && dir <= hi) return dir;
    auto wrap = [steps](int d) { return d < 0 ? d + steps : d; };
    const int to_lo = std::min(wrap(dir - lo), wrap(lo - dir));
    const int to_hi = std::min(wrap(dir - hi), wrap(hi - dir));
    return to_lo < to_hi ? lo : hi;
}

// @orig 10A8:0810
void World::set_frame(Item& item, std::uint16_t frame) {
    if (is_npc_id(item.id)) {
        item.frame = frame;
        npcs_[item.id * kNpcRecordSize + 0x0F] = static_cast<std::uint8_t>(frame >> 8);
    } else {
        item.frame = frame & 0xFF;
    }
}

std::span<std::uint8_t> World::npc_record(ObjId id) {
    if (id >= kNpcSlots) return {};
    return std::span(npcs_).subspan(id * kNpcRecordSize, kNpcRecordSize);
}

std::span<const std::uint8_t> World::npc_record(ObjId id) const {
    if (id >= kNpcSlots) return {};
    return std::span(npcs_).subspan(id * kNpcRecordSize, kNpcRecordSize);
}

bool World::is_dead(ObjId id) const {
    if (!is_npc_id(id)) return true;
    return npcs_[id * kNpcRecordSize + 0x13] & 1;
}

std::uint8_t World::npc_direction(ObjId id) const {
    return id < kNpcSlots ? npcs_[id * kNpcRecordSize + 0x0C] : 0;
}

// @orig ?: camera follow of CameraProcess (segment 1188), not located
void World::track_camera_target(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (follow_player_) {
        const Item* target = get(state.camera_target);
        if (target && target->x == x && target->y == y && target->z == z) return;
        if (!follow_active_) follow_history_.clear();
        follow_goal_[0] = x;
        follow_goal_[1] = y;
        follow_goal_[2] = z;
        follow_active_ = true;
        return;
    }
    // Screen position of the target (SHAPHAND.C projection), feet at z.
    const auto ux = static_cast<std::int16_t>(x), uy = static_cast<std::int16_t>(y);
    const int sx = static_cast<std::int16_t>(((ux - uy) >> 1) - state.view_left);
    const int sy = static_cast<std::int16_t>((static_cast<std::uint16_t>(ux + uy) >> 2) - state.view_top - (z & 0xFF));
    const int w = state.screen_right, h = state.screen_bottom;  // x1 - x0, y1 - y0 with x0 = y0 = 0
    if (sy >= 0x8C && sy <= h - 0x46 && sx <= w - 100 && sx >= 100) return;

    // Where to put the target: away from the edge it came near.
    int target_x = (w + 1) / 2;
    if (sy < 0xA0) {
        if (sx < 0x78) target_x = w - 0x77;
        else if (w - 0x77 < sx) target_x = 0x78;
    } else if (h - 0x59 < sy) {
        if (w - 0x77 < sx) target_x = 0x78;
    } else if (sx < 0x78) {
        target_x = w - 0x77;
    } else if (w - 0x77 < sx) {
        target_x = 0x78;
    }
    shift_camera(sx - target_x, sy - ((h + 1) / 2 + 0x28));
}

// @orig none: port-only camera follow
void World::shift_camera(int dx, int dy) {
    // Move the camera point by the same amount on screen (ANIMCACH.C 0467:
    // x = sx + 2 sy, y = 2 sy - sx in absolute screen coordinates).
    const auto cx = static_cast<std::int16_t>(state.camera_x), cy = static_cast<std::int16_t>(state.camera_y);
    const int cam_sx = ((cx - cy) >> 1) - state.view_left;
    const int cam_sy = (static_cast<std::uint16_t>(cx + cy) >> 2) - state.view_top;
    const int abs_x = cam_sx + dx + state.view_left, abs_y = cam_sy + dy + state.view_top;
    const int base_x = abs_x + 2 * abs_y, base_y = 2 * abs_y - abs_x;
    if (!follow_player_) {
        set_camera(base_x, base_y, state.camera_z);
        return;
    }
    set_camera_view(base_x, base_y, state.camera_z, state.view_left + dx, state.view_top + dy);
}

// @orig none: port-only camera follow
void World::update_follow_camera(std::uint32_t elapsed_us) {
    if (!follow_player_ || !follow_active_ || state.camera_target == 0 ||
        state.camera_target != state.controlled_npc) return;
    const auto ux = static_cast<std::int16_t>(follow_goal_[0]), uy = static_cast<std::int16_t>(follow_goal_[1]);
    const int sx = static_cast<std::int16_t>(((ux - uy) >> 1) - state.view_left);
    const int sy = static_cast<std::int16_t>((static_cast<std::uint16_t>(ux + uy) >> 2) - state.view_top -
                                             (follow_goal_[2] & 0xFF));
    const int left = sx - (state.screen_right + 1) / 2 + state.view_left;
    const int top = sy - ((state.screen_bottom + 1) / 2 + 0x28) + state.view_top;
    // About half a stride of the walk: the steps' uneven lengths average out.
    constexpr std::int64_t kWindowUs = 300000;
    constexpr double kSmoothUs = 60000;
    follow_clock_us_ += elapsed_us;
    const std::int64_t start = follow_clock_us_ - kWindowUs;
    if (follow_history_.empty()) {
        follow_history_.push_back({start, state.view_left, state.view_top});
        follow_view_[0] = state.view_left;
        follow_view_[1] = state.view_top;
    }
    const FollowSample& last = follow_history_.back();
    if (std::abs(left - last.left) > 200 || std::abs(top - last.top) > 150) {
        // A teleport or a map change far away is taken at once.
        follow_history_.assign(1, {start, left, top});
        follow_view_[0] = left;
        follow_view_[1] = top;
    } else if (left != last.left || top != last.top) {
        follow_history_.push_back({follow_clock_us_, left, top});
    }
    while (follow_history_.size() > 1 && follow_history_[1].at_us <= start) follow_history_.pop_front();
    double sum[2] = {0, 0};
    for (std::size_t i = 0; i < follow_history_.size(); ++i) {
        const std::int64_t from = std::max(follow_history_[i].at_us, start);
        const std::int64_t to = i + 1 < follow_history_.size() ? follow_history_[i + 1].at_us : follow_clock_us_;
        sum[0] += static_cast<double>(follow_history_[i].left) * static_cast<double>(to - from);
        sum[1] += static_cast<double>(follow_history_[i].top) * static_cast<double>(to - from);
    }
    const double k = 1 - std::exp(-static_cast<double>(elapsed_us) / kSmoothUs);
    for (int a = 0; a < 2; ++a) follow_view_[a] += (sum[a] / kWindowUs - follow_view_[a]) * k;
    const int dx = static_cast<int>(std::lround(follow_view_[0])) - state.view_left;
    const int dy = static_cast<int>(std::lround(follow_view_[1])) - state.view_top;
    if (dx != 0 || dy != 0) shift_camera(dx, dy);
}

// @orig 10A8:40CE
bool World::is_partly_on_screen(const Item& item) const {
    if (!frame_lookup_) return false;
    const auto box = frame_lookup_(item.shape, item.frame);
    if (!box) return false;
    // SHAPHAND.C projection: signed half difference, unsigned quarter sum.
    const auto x = static_cast<std::int16_t>(item.x), y = static_cast<std::int16_t>(item.y);
    const int sx = static_cast<std::int16_t>(((x - y) >> 1) - state.view_left);
    const int sy = static_cast<std::int16_t>((static_cast<std::uint16_t>(x + y) >> 2) - state.view_top -
                                             (item.z & 0xFF));
    const int left = sx - box->xoff, top = sy - box->yoff;
    const int right = left + box->width - 1, bottom = top + box->height - 1;
    // rect_overlaps: inclusive bounds.
    return left <= state.screen_right && right >= 0 && top <= state.screen_bottom && bottom >= 0 &&
           right >= left && bottom >= top;
}

// @orig 10A8:41F6
bool World::is_mostly_on_screen(const Item& item) const {
    if (!frame_lookup_) return false;
    const auto box = frame_lookup_(item.shape, item.frame);
    if (!box) return false;
    const auto x = static_cast<std::int16_t>(item.x), y = static_cast<std::int16_t>(item.y);
    const int sx = static_cast<std::int16_t>(((x - y) >> 1) - state.view_left);
    const int sy = static_cast<std::int16_t>((static_cast<std::uint16_t>(x + y) >> 2) - state.view_top -
                                             (item.z & 0xFF));
    const int left = sx - box->xoff, top = sy - box->yoff;
    const int right = left + box->width - 1, bottom = top + box->height - 1;
    // rect_clip_to the screen.
    const int cl = std::max(left, 0), ct = std::max(top, 0);
    const int cr = std::min<int>(right, state.screen_right), cb = std::min<int>(bottom, state.screen_bottom);
    return (box->width >> 1) <= cr - cl + 1 && cb - ct + 1 == box->height;
}

// @orig ?: fast area emptied on arrival, not located
void World::clear_fast_area() {
    std::vector<ObjId> globs;
    for (auto& [id, item] : items_) {
        if ((item.flags & (kItemDisposable | 0x0002)) == (kItemDisposable | 0x0002) && item.parent == 0 &&
            !(item.flags & kItemEthereal)) {
            globs.push_back(id);
        }
        item.flags &= static_cast<std::uint16_t>(~kItemFastArea);
    }
    for (ObjId id : globs) destroy(id);
    fast_area_ = {};
    clear_tracked();
    drawn_reset_ = true;
}

// @orig 10B0:13BE
void World::update_fast_area(std::int32_t x, std::int32_t y) {
    const FastArea before = fast_area_;
    const FastArea after = FastArea::around(x, y);
    if (after == before) return;
    auto on_map = [](const Item& i) { return i.parent == 0 && !(i.flags & (kItemContained | kItemEthereal)); };
    auto in = [](const FastArea& area, const Item& i) { return area.contains(i.x, i.y); };

    // Cells left behind: falling items drop first, then the items leave.
    std::vector<ObjId> leaving;
    for_each([&](const Item& i) {
        if (on_map(i) && in(before, i) && !in(after, i)) leaving.push_back(i.id);
    });
    if (hooks_.drop_falling) {
        for (ObjId id : leaving) {
            if (get(id)) hooks_.drop_falling(id);
        }
    }
    for (ObjId id : leaving) {
        Item* item = get(id);
        if (!item) continue;
        if (!(item->flags & kItemDisposable)) {
            leave_fast_area(*item);
            continue;
        }
        const ShapeInfo* info = shape_info(item->shape);
        if (info && info->is(ShapeInfo::kNoisy) && (item->flags & kItemFastArea)) leave_fast_area(*item);
        destroy(id);
    }
    fast_area_ = after;

    // Cells reached: glob eggs first, then the items.
    std::vector<ObjId> entering;
    for_each([&](const Item& i) {
        if (on_map(i) && in(after, i) && !in(before, i)) entering.push_back(i.id);
    });
    if (globs_) {
        for (ObjId id : entering) {
            if (family(*get(id)) == kFamilyGlobEgg) expand_glob_egg(*globs_, id);
        }
    }
    for (ObjId id : entering) {
        Item* item = get(id);
        if (!item || item->z == 254 || (item->flags & kItemDisposable)) continue;
        if (is_npc_id(id) && (item->flags & kItemFastArea)) continue;
        enter_fast_area(*item);
    }
}

// @orig 10B0:229D 10B0:2467
ObjId World::pop_into(ObjId container_id, bool at_end) {
    Item* container = get(container_id);
    if (pending_.empty() || !container) return 0;
    const std::uint8_t fam = family(*container);
    // popToContainer only accepts containers (family 6), and never puts an
    // NPC inside; popToEnd also accepts family 7. Otherwise the item is
    // placed where the container is.
    const bool into = at_end ? (fam == kFamilyContainer || fam == kFamilyActorContainer)
                             : (fam == kFamilyContainer && !is_npc_id(pending_.back()));
    if (!into) {
        const Point p = location(*container);
        return pop_to_coords(p.x, p.y, p.z);
    }
    const ObjId id = pending_.back();
    pending_.pop_back();
    Item& item = *get(id);
    item.parent = container_id;
    item.x = container_id;
    item.z = 0;
    item.flags = static_cast<std::uint16_t>((item.flags & ~kItemEthereal) | kItemContained);
    auto& c = get(container_id)->contents;
    c.insert(at_end ? c.end() : c.begin(), id);
    return id;
}

ObjId World::pop_to_container(ObjId container) { return pop_into(container, false); }
ObjId World::pop_to_end(ObjId container) { return pop_into(container, true); }

// @orig 10B0:1C21 10B0:1D29
void World::move_item(ObjId id, std::int32_t x, std::int32_t y, std::int32_t z) {
    // CONVEYOR pushes its passenger before legalMoveToPoint. push is then
    // a no-op, but the successful move must still pop it back onto the map.
    // Only accept an ethereal item at the top, so an unrelated pending item
    // is never popped in its place.
    Item* item = get(id);
    if (!item || ((item->flags & kItemEthereal) && pending_top() != id)) return;
    push(id);
    pop_to_coords(x, y, z);
    // Item_I_move: the marker follows the designated target.
    if (state.target_designation && id == state.aim.target && hooks_.designated_target_moved)
        hooks_.designated_target_moved(id, x, y, z);
}

const TrigTables& World::trig() const {
    static const TrigTables computed = TrigTables::computed();
    return trig_ ? *trig_ : computed;
}

// @orig 10B0:23DC
ObjId World::pop() {
    if (pending_.empty()) return 0;
    const Item& item = *get(pending_.back());
    // A pushed contained item keeps its container id in x.
    if (item.flags & kItemContained) return pop_to_container(static_cast<ObjId>(item.x));
    return pop_to_coords(item.x, item.y, item.z);
}

// @orig 10A8:1365 10B0:1AB3
void World::destroy(ObjId id) {
    Item* item = get(id);
    if (!item) return;
    // Item_I_destroy -> ITEMCACH_10b0_1ab3 (not for the player).
    if (id != 1 && is_trackable(item->shape) && !(item->flags & kItemBroken)) untrack(id);
    if (id != 1 && state.target_designation && id == state.aim.target && hooks_.designated_target_forgotten)
        hooks_.designated_target_forgotten(id);
    item = get(id);
    if (!item) return;
    for (ObjId child : std::vector<ObjId>(item->contents)) destroy(child);
    if (Item* parent = get(item->parent)) std::erase(parent->contents, id);
    unlink_from_cell(*item);
    std::erase(pending_, id);
    // ITEMCACH_10b0_1a23: the slot goes to the head of its free list and
    // keeps what it held, at z = 0xFE. The player's slot is never freed.
    if (id != 1 && id < kItemSlots) {
        Item& last = stale_[id] = std::move(*item);
        last.z = 0xFE;
        push_free(id);
    }
    items_.erase(id);
}

// @orig 10B0:161B
void World::init_free_slots() {
    // Every slot free: NPCs 2..0xFF, items 0x100..0x27FF, each in id order.
    free_items_ = {};
    free_npcs_ = {};
    free_mask_.reset();
    for (std::size_t id = kNpcSlots - 1; id >= 2; --id) push_free(static_cast<ObjId>(id));
    for (std::size_t id = kItemSlots - 1; id >= kFirstItemId; --id) push_free(static_cast<ObjId>(id));
    free_npcs_.tail = static_cast<ObjId>(kNpcSlots - 1);
    free_items_.tail = static_cast<ObjId>(kItemSlots - 1);
    stale_.clear();
    cell_count_bias_.fill(0);
}

// @orig 10B0:1A23
void World::push_free(ObjId id) {
    FreeSlots& list = is_npc_id(id) ? free_npcs_ : free_items_;
    // As the original: the tail moves to the new slot when the head was the
    // tail (an empty list, but also a list of one).
    const ObjId head = list.ids.empty() ? 0 : list.ids.front();
    if (head == list.tail) list.tail = id;
    list.ids.push_front(id);
    free_mask_.set(id);
}

// @orig 10B0:1986
void World::remove_free(ObjId id) {
    if (id >= kItemSlots || !free_mask_.test(id)) return;
    FreeSlots& list = is_npc_id(id) ? free_npcs_ : free_items_;
    if (id == list.tail) list.tail = 0;
    if (!list.ids.empty() && list.ids.front() == id) list.ids.pop_front();
    else list.ids.erase(std::find(list.ids.begin(), list.ids.end(), id));
    free_mask_.reset(id);
}

// @orig 10B0:05D6
std::vector<ObjId> World::load_fixed_map(const FlexArchive& fixed, std::size_t map) {
    std::vector<ObjId> top_level;
    std::vector<ObjId> containers;  // open containers, innermost last
    for (const FixedRecord& r : read_fixed_map(fixed, map)) {
        // Inside a container the x field is the nesting depth.
        while (!containers.empty() && r.x != containers.size()) containers.pop_back();

        Item item;
        item.shape = r.shape;
        item.frame = r.frame;
        item.flags = r.flags;
        item.quality = r.quality;
        item.npc_number = r.npc_number;
        item.map_number = r.map_number;
        if (containers.empty()) {
            item.x = r.x;
            item.y = r.y;
            item.z = r.z;
        } else {
            item.parent = containers.back();
            item.x = item.parent;
            item.flags |= kItemContained;
        }
        const ObjId id = add(std::move(item)).id;
        if (Item* parent = get(get(id)->parent)) {
            parent->contents.push_back(id);
        } else {
            top_level.push_back(id);
        }
        const ShapeInfo* info = shape_info(r.shape);
        if (info && info->family == kFamilyContainer) containers.push_back(id);
    }
    return top_level;
}

}  // namespace crusader
