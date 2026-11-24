#include "editor/generator/generator.hpp"

#include "editor/item_semantics.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace crusader::editor::gen {

std::uint32_t Rng::next() {
    // splitmix64
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 16);
}

namespace {

// Layout grid: rooms in slots of 5 x 5 floor tiles; a room takes 2 or 3
// tiles of tiles 0 to 3 of its slot and always covers tile 1, the line its
// corridors follow, so that tile 4 leaves at least one tile of corridor.
constexpr int kSlotBlocks = 5;
constexpr std::int32_t kSlot = kSlotBlocks * kBlock;
constexpr std::int32_t kOrigin = 2048;
constexpr int kGrid = 4;
constexpr std::int32_t kWall = kUnit;            // wall thickness
constexpr std::int32_t kDoorWidth = 6 * kUnit;   // MDOOR_N / MDOOR_E
constexpr std::int32_t kDoorOffset = 80;         // door gap from the corridor's side

// Shapes of the original game used as they are.
constexpr std::uint16_t kShapeDoorN = 0x0A9;  // MDOOR_N: 6 x 1, in a wall along x
constexpr std::uint16_t kShapeDoorE = 0x046;  // MDOOR_E: 1 x 6, in a wall along y
constexpr std::uint16_t kShapeTelepad = 0x1DB;  // TELEPAD, 7 x 7
constexpr std::uint8_t kDoorEggType = 0x0A;     // trigger egg class 0x90A DOOREGG
constexpr std::uint8_t kDoorEggZone = 0x44;     // +-128 units, as on the original maps
constexpr std::uint8_t kExitZone = 0x22;        // +-64 units
// Locks (shapes and settings of the original maps).
constexpr std::uint16_t kShapeCardNS = 0x31D;    // CARD_NS, on the south face of a wall along x
constexpr std::uint16_t kShapeCardEW = 0x31E;    // CARD_EW, on the east face of a wall along y
constexpr std::uint16_t kShapeKeycard = 0x111;   // the card; quality = its number
constexpr std::uint16_t kShapeEwallNS = 0x32A;   // EWALLNS, 6 x 0, in a wall along x
constexpr std::uint16_t kShapeEwallEW = 0x329;   // EWALLEW, 0 x 6, in a wall along y
constexpr std::uint16_t kShapeSwitchNS = 0x2A3;  // SWITCHNS, 2 x 1 x 4
constexpr std::uint16_t kShapePanelNS = 0x0A1;   // PANELNS, 2 x 1 x 4
constexpr std::uint16_t kShapeKeypadNS = 0x44B;  // KEYPADNS, 1 x 1 x 2
constexpr std::uint16_t kShapeKeypadEW = 0x44C;  // KEYPADEW
constexpr std::uint16_t kShapeValueBox = 0x251;  // VALUEBOX: the code (0: drawn when the map loads)
constexpr std::uint16_t kShapeMonitorNS = 0x102; // MONITNS, 4 x 2 x 3, on a desk
constexpr std::uint16_t kShapeDeskNS = 0x1DC;    // MDESKNS, 7 x 5 x 5
constexpr std::uint8_t kCodeMessages[] = {10, 18, 19};  // TEXTFILE texts that give a code
constexpr std::uint8_t kTriggerOff = 7;          // map number bits: action 3 (into a marker), at the first use
constexpr std::uint8_t kArgDoor = 3, kArgWall = 1;
constexpr std::int32_t kReaderZ = 40, kSwitchZ = 48;
// Links (quality low) of doors and switches start above the card numbers:
// a DOOREGG opens every door of its link, a locked door included.
constexpr int kFirstLink = 32;

std::int32_t slot_x0(int sx) { return kOrigin + sx * kSlot; }
std::int32_t slot_y0(int sy) { return kOrigin + sy * kSlot; }

struct Gap {
    std::int32_t from, to;
};

class Builder {
public:
    Builder(const Biome& biome, Rng& rng, Level& level) : biome_(biome), rng_(rng), level_(level) {}

    LevelItem& add(std::uint16_t shape, std::uint8_t frame, std::int32_t x, std::int32_t y, std::int32_t z) {
        LevelItem item;
        item.uid = new_uid();
        item.x = x;
        item.y = y;
        item.z = z;
        item.shape = shape;
        item.frame = frame;
        level_.items.push_back(item);
        return level_.items.back();
    }

    void floor(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t z) {
        // One look per room: the frames of a floor are other patterns.
        const int variants = std::min<int>(3, static_cast<int>(biome_.floor_variants.size()));
        const Piece& p = biome_.floor_variants[rng_.below(variants)];
        for (std::int32_t x = x0; x < x1; x += kBlock) {
            for (std::int32_t y = y0; y < y1; y += kBlock) {
                add(p.shape, p.frame, x + kBlock - 1, y + kBlock - 1, z);
            }
        }
    }

    // A straight wall along `axis` from `from` to `to` (excluded), its far
    // side at `fixed` on the other axis, open where `gaps` say.
    // `side`: kBack for the north and west sides of a room, kFront for the
    // south and east ones (black walls that let the room be seen).
    void wall(int side, int axis, std::int32_t fixed, std::int32_t from, std::int32_t to, std::int32_t z,
              std::vector<Gap> gaps = {}) {
        std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) { return a.from < b.from; });
        std::int32_t pos = from;
        for (const Gap& g : gaps) {
            run(side, axis, fixed, pos, g.from, z);
            pos = g.to;
        }
        run(side, axis, fixed, pos, to, z);
    }

private:
    void run(int side, int axis, std::int32_t fixed, std::int32_t from, std::int32_t to, std::int32_t z) {
        std::int32_t pos = from;
        for (int i = 4; i >= 0; --i) {
            const Piece& p = biome_.walls[side][axis][i];
            const std::int32_t len = kWallLengths[i] * kUnit;
            if (!p.valid()) continue;
            while (to - pos >= len) {
                if (axis == kAlongX) add(p.shape, p.frame, pos + len - 1, fixed, z);
                else add(p.shape, p.frame, fixed, pos + len - 1, z);
                pos += len;
            }
        }
    }

    const Biome& biome_;
    Rng& rng_;
    Level& level_;
};

const std::array<std::pair<int, int>, 4> kDirs = {{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};  // N E S W

int other_end(const Link& l, int room) { return l.a == room ? l.b : l.b == room ? l.a : -1; }

// Rooms reached from `from` through the links that are not closed.
std::vector<bool> reach(const Result& r, int from, const std::vector<bool>& closed) {
    std::vector<bool> seen(r.rooms.size());
    std::deque<int> queue{from};
    seen[from] = true;
    while (!queue.empty()) {
        const int a = queue.front();
        queue.pop_front();
        for (std::size_t li = 0; li < r.links.size(); ++li) {
            if (closed[li]) continue;
            const int b = other_end(r.links[li], a);
            if (b >= 0 && !seen[b]) seen[b] = true, queue.push_back(b);
        }
    }
    return seen;
}

}  // namespace

const char* lock_name(LockKind kind) {
    switch (kind) {
        case LockKind::kCardDoor: return "card door";
        case LockKind::kSwitchWall: return "switch and energy wall";
        case LockKind::kPanelDoor: return "panel door";
        case LockKind::kKeypadDoor: return "keypad door";
    }
    return "?";
}

Result generate(const std::vector<Biome>& biomes, const Options& options) {
    Result r;
    Rng rng(options.seed);
    if (biomes.empty()) {
        r.notes.push_back("no biome");
        return r;
    }
    r.biome = options.biome >= 0 && options.biome < static_cast<int>(biomes.size())
                  ? options.biome
                  : rng.below(static_cast<int>(biomes.size()));
    const Biome& biome = biomes[r.biome];
    const int wanted = options.rooms > 1 ? std::min(options.rooms, kGrid * kGrid) : rng.range(6, 10);

    // Mission graph and layout: rooms grown from a start slot, each new one
    // next to a room already placed, plus a few extra passages (loops).
    std::array<std::array<int, kGrid>, kGrid> slot{};
    for (auto& row : slot) row.fill(-1);
    auto add_room = [&](int sx, int sy) {
        Room room;
        room.slot_x = sx;
        room.slot_y = sy;
        slot[sy][sx] = static_cast<int>(r.rooms.size());
        r.rooms.push_back(room);
    };
    auto linked = [&](int a, int b) {
        return std::any_of(r.links.begin(), r.links.end(),
                           [&](const Link& l) { return (l.a == a && l.b == b) || (l.a == b && l.b == a); });
    };
    add_room(rng.below(kGrid), rng.below(kGrid));
    for (int tries = 0; static_cast<int>(r.rooms.size()) < wanted && tries < 1000; ++tries) {
        const int from = rng.below(static_cast<int>(r.rooms.size()));
        const auto [dx, dy] = kDirs[rng.below(4)];
        const int sx = r.rooms[from].slot_x + dx, sy = r.rooms[from].slot_y + dy;
        if (sx < 0 || sy < 0 || sx >= kGrid || sy >= kGrid || slot[sy][sx] >= 0) continue;
        add_room(sx, sy);
        r.links.push_back({from, static_cast<int>(r.rooms.size()) - 1});
    }
    for (int a = 0; a < static_cast<int>(r.rooms.size()); ++a) {
        for (int d = 1; d <= 2; ++d) {  // east and south neighbours
            const int sx = r.rooms[a].slot_x + kDirs[d].first, sy = r.rooms[a].slot_y + kDirs[d].second;
            if (sx >= kGrid || sy >= kGrid || slot[sy][sx] < 0) continue;
            const int b = slot[sy][sx];
            if (!linked(a, b) && rng.chance(20)) r.links.push_back({a, b});
        }
    }

    // Start and exit: the exit is the room farthest from the start.
    {
        std::vector<int> depth(r.rooms.size(), -1);
        std::deque<int> queue{0};
        depth[0] = 0;
        while (!queue.empty()) {
            const int a = queue.front();
            queue.pop_front();
            for (const Link& l : r.links) {
                const int b = l.a == a ? l.b : l.b == a ? l.a : -1;
                if (b >= 0 && depth[b] < 0) depth[b] = depth[a] + 1, queue.push_back(b);
            }
        }
        r.start_room = 0;
        r.exit_room = 0;
        for (std::size_t i = 0; i < r.rooms.size(); ++i) {
            r.rooms[i].depth = depth[i];
            if (depth[i] > r.rooms[r.exit_room].depth) r.exit_room = static_cast<int>(i);
        }
    }

    // Room rectangles in their slots.
    for (Room& room : r.rooms) {
        const int ax = rng.below(2), wx = rng.range(2, 3);
        const int ay = rng.below(2), wy = rng.range(2, 3);
        room.x0 = slot_x0(room.slot_x) + ax * kBlock;
        room.x1 = room.x0 + wx * kBlock;
        room.y0 = slot_y0(room.slot_y) + ay * kBlock;
        room.y1 = room.y0 + wy * kBlock;
    }

    // Locks: on passages of the way to the exit that no other way goes
    // round (bridges), each one's key in a room reached before it.
    {
        std::vector<int> path_links;  // from the start
        std::vector<int> path_rooms;
        std::vector<int> bridges;     // indices in path_links
        auto analyse = [&] {
            std::vector<int> parent_link(r.rooms.size(), -1);
            std::vector<int> depth(r.rooms.size(), -1);
            std::deque<int> queue{r.start_room};
            depth[r.start_room] = 0;
            while (!queue.empty()) {
                const int a = queue.front();
                queue.pop_front();
                for (std::size_t li = 0; li < r.links.size(); ++li) {
                    const int b = other_end(r.links[li], a);
                    if (b >= 0 && depth[b] < 0) {
                        depth[b] = depth[a] + 1, parent_link[b] = static_cast<int>(li), queue.push_back(b);
                    }
                }
            }
            for (std::size_t i = 0; i < r.rooms.size(); ++i) r.rooms[i].depth = depth[i];
            path_links.clear();
            path_rooms.assign(1, r.exit_room);
            for (int room = r.exit_room; room != r.start_room;) {
                const int li = parent_link[room];
                path_links.insert(path_links.begin(), li);
                room = other_end(r.links[li], room);
                path_rooms.insert(path_rooms.begin(), room);
            }
            bridges.clear();
            for (std::size_t i = 0; i < path_links.size(); ++i) {
                std::vector<bool> closed(r.links.size());
                closed[path_links[i]] = true;
                if (!reach(r, r.start_room, closed)[r.exit_room]) bridges.push_back(static_cast<int>(i));
            }
        };
        analyse();
        const int wanted_locks = options.locks >= 0 ? options.locks : rng.range(1, 3);
        // Loops round every passage of the way: without them (the first
        // links make a tree), each passage of the way is a bridge.
        if (bridges.empty() && wanted_locks > 0 && r.links.size() >= r.rooms.size()) {
            r.links.resize(r.rooms.size() - 1);
            analyse();
        }
        for (std::size_t i = bridges.size(); i > 1; --i) std::swap(bridges[i - 1], bridges[rng.below(static_cast<int>(i))]);
        if (static_cast<int>(bridges.size()) > wanted_locks) bridges.resize(static_cast<std::size_t>(wanted_locks));
        std::sort(bridges.begin(), bridges.end());
        const std::set<int> on_path(path_rooms.begin(), path_rooms.end());
        for (std::size_t k = 0; k < bridges.size(); ++k) {
            Lock lock;
            lock.link = path_links[bridges[k]];
            lock.room = path_rooms[bridges[k] + 1];
            // Its key: in what is reached with the locks before it open.
            std::vector<bool> closed(r.links.size());
            for (std::size_t j = k; j < bridges.size(); ++j) closed[path_links[bridges[j]]] = true;
            const std::vector<bool> region = reach(r, r.start_room, closed);
            std::vector<int> side, others;
            for (std::size_t room = 0; room < r.rooms.size(); ++room) {
                if (!region[room] || static_cast<int>(room) == r.start_room) continue;
                (on_path.count(static_cast<int>(room)) ? others : side).push_back(static_cast<int>(room));
            }
            lock.kind = static_cast<LockKind>(rng.below(4));
            // TRIGGER looks for its targets within global 0x40 * 32 = 3200
            // units of the switch or panel (loop type 6, TRIGGER event
            // 0x20), and a monitor for its VALUEBOX as far: their room stays
            // within one slot of the barrier's.
            if (lock.kind != LockKind::kCardDoor) {
                auto in_range = [&](int room) {
                    return std::abs(r.rooms[room].slot_x - r.rooms[lock.room].slot_x) <= 1 &&
                           std::abs(r.rooms[room].slot_y - r.rooms[lock.room].slot_y) <= 1;
                };
                for (auto* list : {&side, &others}) {
                    list->erase(std::remove_if(list->begin(), list->end(), [&](int room) { return !in_range(room); }),
                                list->end());
                }
                if (side.empty() && others.empty() && !in_range(r.start_room)) lock.kind = LockKind::kCardDoor;
            }
            const std::vector<int>& pick = !side.empty() ? side : others;
            lock.key_room = pick.empty() ? r.start_room : pick[rng.below(static_cast<int>(pick.size()))];
            r.links[lock.link].lock = static_cast<int>(r.locks.size());
            r.locks.push_back(lock);
        }
    }

    // Building.
    Builder b(biome, rng, r.level);
    int next_link = kFirstLink;
    int next_card = options.first_card;
    auto new_link = [&] { return static_cast<std::uint8_t>(next_link++); };
    // Door gaps of each room side: [room][N E S W].
    std::vector<std::array<std::vector<Gap>, 4>> gaps(r.rooms.size());
    struct Door {
        int axis;
        std::int32_t x, y;
        std::uint8_t link;
    };
    std::vector<Door> doors;
    // A locked doorway: where its barrier stands, and where a card reader
    // goes in the corridor (on the corridor's far wall, which the camera
    // sees, 48 units from the doorway's wall).
    struct Doorway {
        int lock;
        int axis;  // of the wall it is in
        std::int32_t x, y;
        Spot reader;
    };
    std::vector<Doorway> locked;
    for (std::size_t li = 0; li < r.links.size(); ++li) {
        Link& l = r.links[li];
        Room& ra = r.rooms[l.a];
        Room& rb = r.rooms[l.b];
        const bool horizontal = ra.slot_y == rb.slot_y;
        const int first = horizontal ? (ra.slot_x < rb.slot_x ? l.a : l.b) : (ra.slot_y < rb.slot_y ? l.a : l.b);
        const Room& lo = r.rooms[first];
        const Room& hi = r.rooms[first == l.a ? l.b : l.a];
        const int lo_index = first, hi_index = first == l.a ? l.b : l.a;
        const int locked_end = l.lock >= 0 ? r.locks[l.lock].room : -1;
        const std::uint8_t lo_door = locked_end == lo_index ? 0 : new_link();
        const std::uint8_t hi_door = locked_end == hi_index ? 0 : new_link();
        if (first == l.a) l.door_a = lo_door, l.door_b = hi_door;
        else l.door_a = hi_door, l.door_b = lo_door;
        auto place = [&](int axis, std::int32_t x, std::int32_t y, std::uint8_t link, Spot reader) {
            if (link) doors.push_back({axis, x, y, link});
            else locked.push_back({l.lock, axis, x, y, reader});
        };
        if (horizontal) {
            // Corridor along x, on tile 1 of the row.
            const std::int32_t yc = slot_y0(lo.slot_y) + kBlock;
            const Gap gap{yc + kDoorOffset, yc + kDoorOffset + kDoorWidth};
            gaps[lo_index][1].push_back(gap);
            gaps[hi_index][3].push_back(gap);
            b.floor(lo.x1, yc, hi.x0, yc + kBlock, 0);
            b.wall(kBack, kAlongX, yc + kWall - 1, lo.x1, hi.x0, 0);
            b.wall(kFront, kAlongX, yc + kBlock - 1, lo.x1, hi.x0, 0);
            place(kAlongY, lo.x1 - 1, gap.to - 1, lo_door, {lo.x1 + 63, yc + 2 * kWall - 1, kReaderZ});
            place(kAlongY, hi.x0 + kWall - 1, gap.to - 1, hi_door, {hi.x0 - 48, yc + 2 * kWall - 1, kReaderZ});
        } else {
            const std::int32_t xc = slot_x0(lo.slot_x) + kBlock;
            const Gap gap{xc + kDoorOffset, xc + kDoorOffset + kDoorWidth};
            gaps[lo_index][2].push_back(gap);
            gaps[hi_index][0].push_back(gap);
            b.floor(xc, lo.y1, xc + kBlock, hi.y0, 0);
            b.wall(kBack, kAlongY, xc + kWall - 1, lo.y1, hi.y0, 0);
            b.wall(kFront, kAlongY, xc + kBlock - 1, lo.y1, hi.y0, 0);
            place(kAlongX, gap.to - 1, lo.y1 - 1, lo_door, {xc + 2 * kWall - 1, lo.y1 + 63, kReaderZ});
            place(kAlongX, gap.to - 1, hi.y0 + kWall - 1, hi_door, {xc + 2 * kWall - 1, hi.y0 - 48, kReaderZ});
        }
    }
    for (std::size_t i = 0; i < r.rooms.size(); ++i) {
        const Room& room = r.rooms[i];
        b.floor(room.x0, room.y0, room.x1, room.y1, room.z);
        // Walls along x span the whole side; walls along y fit between them.
        b.wall(kBack, kAlongX, room.y0 + kWall - 1, room.x0, room.x1, room.z, gaps[i][0]);
        b.wall(kFront, kAlongX, room.y1 - 1, room.x0, room.x1, room.z, gaps[i][2]);
        b.wall(kFront, kAlongY, room.x1 - 1, room.y0 + kWall, room.y1 - kWall, room.z, gaps[i][1]);
        b.wall(kBack, kAlongY, room.x0 + kWall - 1, room.y0 + kWall, room.y1 - kWall, room.z, gaps[i][3]);
    }
    // Automatic doors: the door, linked by its quality low to a DOOREGG at
    // its centre whose map number names it (original maps 3 and 4).
    for (const Door& d : doors) {
        LevelItem& door = b.add(d.axis == kAlongX ? kShapeDoorN : kShapeDoorE, 0, d.x, d.y, 0);
        door.quality = d.link;
        const std::int32_t ex = d.axis == kAlongX ? d.x - kDoorWidth / 2 : d.x - kWall / 2;
        const std::int32_t ey = d.axis == kAlongX ? d.y - kWall / 2 : d.y - kDoorWidth / 2;
        LevelItem& egg = b.add(kShapeTriggerEgg, 0, ex, ey, 0);
        egg.quality = kDoorEggType;
        egg.npc_number = kDoorEggZone;
        egg.map_number = d.link;
    }

    // Locked doorways and their keys. Switches and panels stand on the
    // north wall of their room, facing south (as the camera sees them),
    // away from its doorways; cards lie on the floor.
    std::vector<int> keys_in_room(r.rooms.size());
    // Along the north wall of each room: what is already taken (x ranges).
    std::vector<std::vector<Gap>> taken(r.rooms.size());
    for (std::size_t i = 0; i < r.rooms.size(); ++i) taken[i] = gaps[i][0];
    // The far corner x of `width` units against the north wall, clear of its
    // doorways and of what stands there, 16 units apart.
    auto wall_x = [&](int room_index, std::int32_t width) {
        const Room& room = r.rooms[room_index];
        for (std::int32_t x = room.x0 + 2 * kWall + width - 1; x + 2 * kWall < room.x1; x += kUnit) {
            const bool free = std::none_of(taken[room_index].begin(), taken[room_index].end(), [&](const Gap& g) {
                return x - width + 1 < g.to + kUnit && x >= g.from - kUnit;
            });
            if (free) {
                taken[room_index].push_back({x - width + 1, x + 1});
                return x;
            }
        }
        return room.x0 + 2 * kWall + width - 1;
    };
    auto wall_spot = [&](int room_index) {
        const Room& room = r.rooms[room_index];
        return Spot{wall_x(room_index, 32), room.y0 + 2 * kWall - 1, room.z + kSwitchZ};
    };
    auto floor_spot = [&](int room_index) {
        const Room& room = r.rooms[room_index];
        const int n = keys_in_room[room_index]++;
        return Spot{room.x0 + (room.x1 - room.x0) * 3 / 4 - 48 * n, room.y0 + (room.y1 - room.y0) * 3 / 4, room.z};
    };
    auto trigger = [&](LevelItem& user, std::uint16_t target, std::uint8_t link, std::uint8_t arg) {
        set_trigger_target(user, target);
        user.map_number = static_cast<std::uint8_t>(user.map_number | kTriggerOff);
        user.quality = static_cast<std::uint16_t>(arg << 8 | link);
    };
    for (const Doorway& d : locked) {
        Lock& lock = r.locks[d.lock];
        const bool along_x = d.axis == kAlongX;
        const std::uint16_t door_shape = along_x ? kShapeDoorN : kShapeDoorE;
        lock.barrier = {d.x, d.y, 0};
        switch (lock.kind) {
            case LockKind::kCardDoor: {
                lock.number = static_cast<std::uint8_t>((next_card++ - 1) % 31 + 1);
                lock.barrier_shape = door_shape;
                b.add(door_shape, 0, d.x, d.y, 0).quality = lock.number;
                // The reader on the corridor's far wall: along x for a door
                // in a wall along y, and the other way round.
                lock.reader_shape = along_x ? kShapeCardEW : kShapeCardNS;
                lock.reader = d.reader;
                trigger(b.add(lock.reader_shape, 0, d.reader.x, d.reader.y, d.reader.z), door_shape, lock.number,
                        kArgDoor);
                lock.key_shape = kShapeKeycard;
                lock.key = floor_spot(lock.key_room);
                b.add(kShapeKeycard, 0, lock.key.x, lock.key.y, lock.key.z).quality = lock.number;
                break;
            }
            case LockKind::kSwitchWall: {
                lock.number = new_link();
                lock.barrier_shape = along_x ? kShapeEwallNS : kShapeEwallEW;
                b.add(lock.barrier_shape, 0, d.x, d.y, 0).quality = lock.number;
                lock.key_shape = kShapeSwitchNS;
                lock.key = wall_spot(lock.key_room);
                trigger(b.add(kShapeSwitchNS, 0, lock.key.x, lock.key.y, lock.key.z), lock.barrier_shape,
                        lock.number, kArgWall);
                break;
            }
            case LockKind::kKeypadDoor: {
                lock.number = new_link();
                lock.barrier_shape = door_shape;
                b.add(door_shape, 0, d.x, d.y, 0).quality = lock.number;
                lock.reader_shape = along_x ? kShapeKeypadEW : kShapeKeypadNS;
                lock.reader = d.reader;
                trigger(b.add(lock.reader_shape, 0, d.reader.x, d.reader.y, d.reader.z), door_shape, lock.number,
                        kArgDoor);
                lock.code_box = {d.reader.x, d.reader.y, 0};
                b.add(kShapeValueBox, 0, lock.code_box.x, lock.code_box.y, 0).quality = lock.number;
                // The monitor on its desk, against the key room's north wall
                // (as on the original maps: 32 and 48 units from the desk's
                // far corner, on its top).
                const Room& room = r.rooms[lock.key_room];
                lock.desk = {wall_x(lock.key_room, 7 * kUnit), room.y0 + 2 * kWall + 5 * kUnit - 1, room.z};
                b.add(kShapeDeskNS, 0, lock.desk.x, lock.desk.y, lock.desk.z);
                lock.key_shape = kShapeMonitorNS;
                lock.key = {lock.desk.x - 32, lock.desk.y - 48, lock.desk.z + 5 * 8};
                lock.message = kCodeMessages[rng.below(3)];
                b.add(kShapeMonitorNS, 0, lock.key.x, lock.key.y, lock.key.z).quality =
                    static_cast<std::uint16_t>(lock.message << 8 | lock.number);
                break;
            }
            case LockKind::kPanelDoor: {
                lock.number = new_link();
                lock.barrier_shape = door_shape;
                b.add(door_shape, 0, d.x, d.y, 0).quality = lock.number;
                lock.key_shape = kShapePanelNS;
                lock.key = wall_spot(lock.key_room);
                trigger(b.add(kShapePanelNS, 0, lock.key.x, lock.key.y, lock.key.z), door_shape, lock.number,
                        kArgDoor);
                break;
            }
        }
    }

    // Mission start, and the exit teleporter.
    const Room& start = r.rooms[r.start_room];
    r.start = {start.centre_x(), start.centre_y(), start.z};
    LevelItem& arrival = b.add(kShapeTeleportEgg, 1, r.start.x, r.start.y, r.start.z);
    arrival.quality = kStartEgg;
    const Room& exit = r.rooms[r.exit_room];
    r.exit = {exit.centre_x(), exit.centre_y(), exit.z};
    b.add(kShapeTelepad, 0, r.exit.x + 56, r.exit.y + 56, r.exit.z);
    if (options.next_map > 0) {
        LevelItem& departure = b.add(kShapeTeleportEgg, 0, r.exit.x, r.exit.y, r.exit.z);
        departure.quality = static_cast<std::uint16_t>(options.next_egg | 0x100);
        departure.npc_number = kExitZone;
        departure.map_number = static_cast<std::uint8_t>(options.next_map);
    }

    char note[200];
    std::snprintf(note, sizeof note,
                  "seed %u: %s, %zu rooms, %zu passages, %zu locks, exit %d links from the start, %zu items",
                  options.seed, biome.name().c_str(), r.rooms.size(), r.links.size(), r.locks.size(), exit.depth,
                  r.level.record_count());
    r.notes.push_back(note);
    for (const Lock& lock : r.locks) {
        std::snprintf(note, sizeof note, "  %s %u: passage to room %d, key in room %d", lock_name(lock.kind),
                      lock.number, lock.room, lock.key_room);
        r.notes.push_back(note);
    }
    return r;
}

std::string check_solvable(const Result& result, std::vector<int>* order) {
    if (result.rooms.empty()) return "no room";
    std::vector<bool> open(result.locks.size());
    std::vector<bool> closed(result.links.size());
    std::vector<bool> region;
    for (;;) {
        for (std::size_t li = 0; li < result.links.size(); ++li) {
            const int lock = result.links[li].lock;
            closed[li] = lock >= 0 && !open[static_cast<std::size_t>(lock)];
        }
        region = reach(result, result.start_room, closed);
        bool opened = false;
        for (std::size_t k = 0; k < result.locks.size(); ++k) {
            const Lock& lock = result.locks[k];
            if (open[k] || !region[lock.key_room]) continue;
            // A card reader stands in the passage, on the side of the room
            // before the lock: that room too must be reached.
            const Link& link = result.links[static_cast<std::size_t>(lock.link)];
            const bool in_passage = lock.kind == LockKind::kCardDoor || lock.kind == LockKind::kKeypadDoor;
            if (in_passage && !region[other_end(link, lock.room)]) continue;
            open[k] = opened = true;
            if (order) order->push_back(static_cast<int>(k));
        }
        if (!opened) break;
    }
    if (!region[result.exit_room]) return "the exit room cannot be reached";
    for (std::size_t i = 0; i < region.size(); ++i) {
        if (!region[i]) return "room " + std::to_string(i) + " cannot be reached";
    }
    return {};
}

}  // namespace crusader::editor::gen
