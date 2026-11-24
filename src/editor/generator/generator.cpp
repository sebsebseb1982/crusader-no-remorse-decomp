#include "editor/generator/generator.hpp"

#include "editor/item_semantics.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
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

}  // namespace

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

    // Building.
    Builder b(biome, rng, r.level);
    std::uint8_t next_door = 1;
    // Door gaps of each room side: [room][N E S W].
    std::vector<std::array<std::vector<Gap>, 4>> gaps(r.rooms.size());
    struct Door {
        int axis;
        std::int32_t x, y;
        std::uint8_t link;
    };
    std::vector<Door> doors;
    for (Link& l : r.links) {
        Room& ra = r.rooms[l.a];
        Room& rb = r.rooms[l.b];
        const bool horizontal = ra.slot_y == rb.slot_y;
        const int first = horizontal ? (ra.slot_x < rb.slot_x ? l.a : l.b) : (ra.slot_y < rb.slot_y ? l.a : l.b);
        const Room& lo = r.rooms[first];
        const Room& hi = r.rooms[first == l.a ? l.b : l.a];
        const int lo_index = first, hi_index = first == l.a ? l.b : l.a;
        const std::uint8_t lo_door = next_door++, hi_door = next_door++;
        if (first == l.a) l.door_a = lo_door, l.door_b = hi_door;
        else l.door_a = hi_door, l.door_b = lo_door;
        if (horizontal) {
            // Corridor along x, on tile 1 of the row.
            const std::int32_t yc = slot_y0(lo.slot_y) + kBlock;
            const Gap gap{yc + kDoorOffset, yc + kDoorOffset + kDoorWidth};
            gaps[lo_index][1].push_back(gap);
            gaps[hi_index][3].push_back(gap);
            b.floor(lo.x1, yc, hi.x0, yc + kBlock, 0);
            b.wall(kBack, kAlongX, yc + kWall - 1, lo.x1, hi.x0, 0);
            b.wall(kFront, kAlongX, yc + kBlock - 1, lo.x1, hi.x0, 0);
            doors.push_back({kAlongY, lo.x1 - 1, gap.to - 1, lo_door});
            doors.push_back({kAlongY, hi.x0 + kWall - 1, gap.to - 1, hi_door});
        } else {
            const std::int32_t xc = slot_x0(lo.slot_x) + kBlock;
            const Gap gap{xc + kDoorOffset, xc + kDoorOffset + kDoorWidth};
            gaps[lo_index][2].push_back(gap);
            gaps[hi_index][0].push_back(gap);
            b.floor(xc, lo.y1, xc + kBlock, hi.y0, 0);
            b.wall(kBack, kAlongY, xc + kWall - 1, lo.y1, hi.y0, 0);
            b.wall(kFront, kAlongY, xc + kBlock - 1, lo.y1, hi.y0, 0);
            doors.push_back({kAlongX, gap.to - 1, lo.y1 - 1, lo_door});
            doors.push_back({kAlongX, gap.to - 1, hi.y0 + kWall - 1, hi_door});
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

    char note[160];
    std::snprintf(note, sizeof note, "seed %u: %s, %zu rooms, %zu passages, exit %d links from the start, %zu items",
                  options.seed, biome.name().c_str(), r.rooms.size(), r.links.size(), exit.depth,
                  r.level.record_count());
    r.notes.push_back(note);
    return r;
}

std::string check_solvable(const Result& result) {
    if (result.rooms.empty()) return "no room";
    std::vector<bool> seen(result.rooms.size());
    std::deque<int> queue{result.start_room};
    seen[result.start_room] = true;
    while (!queue.empty()) {
        const int a = queue.front();
        queue.pop_front();
        for (const Link& l : result.links) {
            const int b = l.a == a ? l.b : l.b == a ? l.a : -1;
            if (b >= 0 && !seen[b]) seen[b] = true, queue.push_back(b);
        }
    }
    if (!seen[result.exit_room]) return "the exit room cannot be reached";
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) return "room " + std::to_string(i) + " cannot be reached";
    }
    return {};
}

}  // namespace crusader::editor::gen
