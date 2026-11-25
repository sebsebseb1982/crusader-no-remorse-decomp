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
// Lifts (map 5): the platform and its pad, call boxes, railings.
constexpr std::uint16_t kShapeLiftPlatform = 0x12F;  // ELEVPLAT, 8 x 8 x 1
constexpr std::uint16_t kShapeLiftPad = 0x4CD;       // TRIGPAD, 8 x 8 x 0, on the platform
constexpr std::uint16_t kShapeCallBoxNS = 0x081;     // BOX_NS, 2 x 1 x 4
constexpr std::uint16_t kShapeCallBoxEW = 0x080;     // BOX_EW, 1 x 2 x 4
constexpr std::uint16_t kShapeTriggerRelay = 0x4B1;  // 2 x 2 x 0, invisible: TRIGGER's second state
constexpr std::uint8_t kTriggerEquip = 6;            // map number bits: action 2 (equip), bit 2
constexpr std::uint8_t kTriggerState1 = 8;           // map number bit 3: acts in state 1
constexpr std::int32_t kLiftSize = 8 * kUnit;
constexpr std::int32_t kLiftBottom = 1;              // the platform's z at the bottom (88 at the top)
// Railings 6 high, 1 thick, by length (8, 4, 2, 1): along x, along y.
constexpr std::uint16_t kRailings[2][4] = {{0x08B, 0x08C, 0x08D, 0x093}, {0x091, 0x092, 0x070, 0x093}};
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

    // An item whose footprint is the rectangle (x1, y1 excluded).
    LevelItem& rect(std::uint16_t shape, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                    std::int32_t z) {
        (void)x0, (void)y0;
        return add(shape, 0, x1 - 1, y1 - 1, z);
    }

    // A railing filling the rectangle, one unit thick, along `axis`.
    void railing(int axis, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t z) {
        std::int32_t pos = axis == kAlongX ? x0 : y0;
        const std::int32_t end = axis == kAlongX ? x1 : y1;
        for (int i = 0; i < 4; ++i) {
            const std::int32_t len = (8 >> i) * kUnit;
            while (end - pos >= len) {
                if (axis == kAlongX) add(kRailings[axis][i], 0, pos + len - 1, y1 - 1, z);
                else add(kRailings[axis][i], 0, x1 - 1, pos + len - 1, z);
                pos += len;
            }
        }
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

    // Storeys: the rooms from a depth drawn on the way to the exit up are on
    // the upper storey (z 96), so that the level climbs to its exit; the
    // passages between storeys become lifts.
    {
        const int storeys = options.storeys >= 1 ? std::min(options.storeys, 2) : (rng.chance(75) ? 2 : 1);
        const int exit_depth = r.rooms[r.exit_room].depth;
        if (storeys == 2 && exit_depth >= 1) {
            const int cut = rng.range(1, exit_depth);
            for (Room& room : r.rooms) room.z = room.depth >= cut ? kStorey : 0;
            r.storeys = 2;
        }
        for (std::size_t li = 0; li < r.links.size(); ++li) {
            Link& l = r.links[li];
            if (r.rooms[l.a].z == r.rooms[l.b].z) continue;
            const bool lower_a = r.rooms[l.a].z < r.rooms[l.b].z;
            Lift lift;
            lift.link = static_cast<int>(li);
            lift.lower_room = lower_a ? l.a : l.b;
            lift.upper_room = lower_a ? l.b : l.a;
            l.lift = static_cast<int>(r.lifts.size());
            r.lifts.push_back(lift);
            // A lift corridor has two tiles at least: the shaft and a landing.
            Room& ra = r.rooms[l.a];
            Room& rb = r.rooms[l.b];
            if (ra.slot_y == rb.slot_y) {
                Room& lo = ra.slot_x < rb.slot_x ? ra : rb;
                const Room& hi = ra.slot_x < rb.slot_x ? rb : ra;
                if (hi.x0 - lo.x1 < 2 * kBlock) lo.x1 -= kBlock;
            } else {
                Room& lo = ra.slot_y < rb.slot_y ? ra : rb;
                const Room& hi = ra.slot_y < rb.slot_y ? rb : ra;
                if (hi.y0 - lo.y1 < 2 * kBlock) lo.y1 -= kBlock;
            }
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
        std::int32_t x, y, z;
        std::uint8_t link;
    };
    std::vector<Door> doors;
    // A locked doorway: where its barrier stands, and where a card reader
    // goes in the corridor (on the corridor's far wall, which the camera
    // sees, 48 units from the doorway's wall).
    struct Doorway {
        int lock;
        int axis;  // of the wall it is in
        std::int32_t x, y, z;
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
        auto place = [&](int axis, std::int32_t x, std::int32_t y, std::int32_t z, std::uint8_t link, Spot reader) {
            if (link) doors.push_back({axis, x, y, z, link});
            else locked.push_back({l.lock, axis, x, y, z, reader});
        };
        // Corridor coordinates: u along it (x for a corridor along x, on tile
        // 1 of the row; y otherwise, on tile 1 of the column), v across it,
        // from `c` to c + 256.
        const int along = horizontal ? kAlongX : kAlongY, across = 1 - along;
        const std::int32_t c = horizontal ? slot_y0(lo.slot_y) + kBlock : slot_x0(lo.slot_x) + kBlock;
        const std::int32_t u0 = horizontal ? lo.x1 : lo.y1, u1 = horizontal ? hi.x0 : hi.y0;
        auto x_of = [&](std::int32_t u, std::int32_t v) { return horizontal ? u : v; };
        auto y_of = [&](std::int32_t u, std::int32_t v) { return horizontal ? v : u; };
        auto spot = [&](std::int32_t u, std::int32_t v, std::int32_t z) { return Spot{x_of(u, v), y_of(u, v), z}; };
        // An item on the rectangle [ua, ub) x [va, vb).
        auto rect = [&](std::uint16_t shape, std::int32_t ua, std::int32_t ub, std::int32_t va, std::int32_t vb,
                        std::int32_t z) -> LevelItem& {
            return b.rect(shape, x_of(ua, va), y_of(ua, va), x_of(ub, vb), y_of(ub, vb), z);
        };
        auto railing = [&](bool along_u, std::int32_t ua, std::int32_t ub, std::int32_t va, std::int32_t vb) {
            b.railing(along_u ? along : across, x_of(ua, va), y_of(ua, va), x_of(ub, vb), y_of(ub, vb), kStorey);
        };
        auto floor_part = [&](std::int32_t ua, std::int32_t ub, std::int32_t z) {
            b.floor(x_of(ua, c), y_of(ua, c), x_of(ub, c + kBlock), y_of(ub, c + kBlock), z);
        };
        auto sides = [&](std::int32_t ua, std::int32_t ub, std::int32_t z) {
            b.wall(kBack, along, c + kWall - 1, ua, ub, z);
            b.wall(kFront, along, c + kBlock - 1, ua, ub, z);
        };
        const Gap gap{c + kDoorOffset, c + kDoorOffset + kDoorWidth};
        gaps[lo_index][horizontal ? 1 : 2].push_back(gap);
        gaps[hi_index][horizontal ? 3 : 0].push_back(gap);
        if (l.lift < 0) {
            floor_part(u0, u1, lo.z);
            sides(u0, u1, lo.z);
            if (lo.z > 0) sides(u0, u1, 0);  // under a corridor of the upper storey
        } else {
            // The lower part ends with the shaft's tile, the platform against
            // the upper part (boundary `bu`), whose floor its top meets.
            Lift& lift = r.lifts[static_cast<std::size_t>(l.lift)];
            const bool lower_lo = lo.z < hi.z;
            const int tiles = (u1 - u0) / kBlock;
            const int low_tiles = tiles - tiles / 2;
            const std::int32_t bu = lower_lo ? u0 + low_tiles * kBlock : u1 - low_tiles * kBlock;
            const std::int32_t dir = lower_lo ? 1 : -1;  // from the lower part to the upper one
            floor_part(lower_lo ? u0 : bu, lower_lo ? bu : u1, 0);
            floor_part(lower_lo ? bu : u0, lower_lo ? u1 : bu, kStorey);
            sides(u0, u1, 0);
            sides(lower_lo ? bu : u0, lower_lo ? u1 : bu, kStorey);
            // The lower part closed under the upper one.
            b.wall(lower_lo ? kFront : kBack, across, lower_lo ? bu - 1 : bu + kWall - 1, c + kWall,
                   c + kBlock - kWall, 0);
            const std::int32_t pu0 = lower_lo ? bu - kLiftSize : bu, pu1 = pu0 + kLiftSize;
            const std::int32_t pv0 = c + 4 * kUnit, pv1 = pv0 + kLiftSize;
            lift.number = new_link();
            lift.x0 = x_of(pu0, pv0), lift.y0 = y_of(pu0, pv0);
            lift.x1 = x_of(pu1, pv1), lift.y1 = y_of(pu1, pv1);
            lift.platform = {lift.x1 - 1, lift.y1 - 1, kLiftBottom};
            auto lift_trigger = [&](LevelItem& item) {
                set_trigger_target(item, kShapeLiftPlatform);
                item.map_number = static_cast<std::uint8_t>(item.map_number | kTriggerEquip);
                item.quality = lift.number;
            };
            rect(kShapeLiftPlatform, pu0, pu1, pv0, pv1, kLiftBottom).quality = lift.number;
            lift_trigger(rect(kShapeLiftPad, pu0, pu1, pv0, pv1, kLiftBottom + 8));
            // Railings round the shaft at the top: beside the platform, at
            // its lower end, and from the corridor's walls to them where the
            // upper floor ends.
            const std::int32_t ru0 = lower_lo ? bu - kLiftSize - kUnit : bu;
            const std::int32_t ru1 = ru0 + kLiftSize + kUnit;
            railing(true, ru0, ru1, pv0 - kUnit, pv0);
            railing(true, ru0, ru1, pv1, pv1 + kUnit);
            const std::int32_t end_u = lower_lo ? ru0 : ru1 - kUnit;
            railing(false, end_u, end_u + kUnit, pv0, pv1);
            const std::int32_t edge_u = lower_lo ? bu : bu - kUnit;
            railing(false, edge_u, edge_u + kUnit, c + kWall, pv0 - kUnit);
            railing(false, edge_u, edge_u + kUnit, pv1 + kUnit, c + kBlock - kWall);
            // Call boxes on the corridor's far wall: below, in the shaft's
            // tile before the platform; above, past the upper floor's edge.
            lift.box_shape = horizontal ? kShapeCallBoxNS : kShapeCallBoxEW;
            const std::int32_t bottom_u = bu - dir * (kLiftSize + kUnit) - (lower_lo ? 2 * kUnit : 0);
            const std::int32_t top_u = bu + dir * 3 * kUnit - (lower_lo ? 0 : 2 * kUnit);
            // A box acts through TRIGGER in its first state (frame 0); in the
            // other one, TRIGGER passes the use on to the 0x4B1 objects of
            // its link within 160 units whose map number has bit 3 (state 1),
            // so each box has one beside it, as on map 1: every use calls.
            auto call_box = [&](std::int32_t u, std::int32_t z) {
                LevelItem& box = rect(lift.box_shape, u, u + 2 * kUnit, c + kWall, c + 2 * kWall, z);
                lift_trigger(box);
                const Spot at{box.x, box.y, box.z};
                LevelItem& relay = b.add(kShapeTriggerRelay, 0, at.x, at.y, at.z + 2 * kUnit);
                lift_trigger(relay);
                relay.map_number = static_cast<std::uint8_t>(relay.map_number | kTriggerState1);
                return at;
            };
            lift.bottom_box = call_box(bottom_u, kSwitchZ);
            lift.top_box = call_box(top_u, kStorey + kSwitchZ);
            // In front of the platform (the player's far corner), clear of it.
            const std::int32_t v_mid = c + kBlock / 2 + 24;
            lift.bottom = spot(lower_lo ? pu0 - kUnit : pu1 + kUnit + 47, v_mid, 0);
            lift.top = spot(lower_lo ? bu + kUnit + 47 : bu - kUnit, v_mid, kStorey);
        }
        place(across, x_of(u0 - 1, gap.to - 1), y_of(u0 - 1, gap.to - 1), lo.z, lo_door,
              spot(u0 + 63, c + 2 * kWall - 1, lo.z + kReaderZ));
        place(across, x_of(u1 + kWall - 1, gap.to - 1), y_of(u1 + kWall - 1, gap.to - 1), hi.z, hi_door,
              spot(u1 - 48, c + 2 * kWall - 1, hi.z + kReaderZ));
    }
    for (std::size_t i = 0; i < r.rooms.size(); ++i) {
        const Room& room = r.rooms[i];
        b.floor(room.x0, room.y0, room.x1, room.y1, room.z);
        // Walls along x span the whole side; walls along y fit between them.
        // Under a room of the upper storey, walls without doorways.
        for (std::int32_t z = 0; z <= room.z; z += kStorey) {
            const bool top = z == room.z;
            auto g = [&](int side) { return top ? gaps[i][side] : std::vector<Gap>{}; };
            b.wall(kBack, kAlongX, room.y0 + kWall - 1, room.x0, room.x1, z, g(0));
            b.wall(kFront, kAlongX, room.y1 - 1, room.x0, room.x1, z, g(2));
            b.wall(kFront, kAlongY, room.x1 - 1, room.y0 + kWall, room.y1 - kWall, z, g(1));
            b.wall(kBack, kAlongY, room.x0 + kWall - 1, room.y0 + kWall, room.y1 - kWall, z, g(3));
        }
    }
    // Automatic doors: the door, linked by its quality low to a DOOREGG at
    // its centre whose map number names it (original maps 3 and 4).
    for (const Door& d : doors) {
        LevelItem& door = b.add(d.axis == kAlongX ? kShapeDoorN : kShapeDoorE, 0, d.x, d.y, d.z);
        door.quality = d.link;
        const std::int32_t ex = d.axis == kAlongX ? d.x - kDoorWidth / 2 : d.x - kWall / 2;
        const std::int32_t ey = d.axis == kAlongX ? d.y - kWall / 2 : d.y - kDoorWidth / 2;
        LevelItem& egg = b.add(kShapeTriggerEgg, 0, ex, ey, d.z);
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
        lock.barrier = {d.x, d.y, d.z};
        switch (lock.kind) {
            case LockKind::kCardDoor: {
                lock.number = static_cast<std::uint8_t>((next_card++ - 1) % 31 + 1);
                lock.barrier_shape = door_shape;
                b.add(door_shape, 0, d.x, d.y, d.z).quality = lock.number;
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
                b.add(lock.barrier_shape, 0, d.x, d.y, d.z).quality = lock.number;
                lock.key_shape = kShapeSwitchNS;
                lock.key = wall_spot(lock.key_room);
                trigger(b.add(kShapeSwitchNS, 0, lock.key.x, lock.key.y, lock.key.z), lock.barrier_shape,
                        lock.number, kArgWall);
                break;
            }
            case LockKind::kKeypadDoor: {
                lock.number = new_link();
                lock.barrier_shape = door_shape;
                b.add(door_shape, 0, d.x, d.y, d.z).quality = lock.number;
                lock.reader_shape = along_x ? kShapeKeypadEW : kShapeKeypadNS;
                lock.reader = d.reader;
                trigger(b.add(lock.reader_shape, 0, d.reader.x, d.reader.y, d.reader.z), door_shape, lock.number,
                        kArgDoor);
                lock.code_box = {d.reader.x, d.reader.y, d.z};
                b.add(kShapeValueBox, 0, lock.code_box.x, lock.code_box.y, lock.code_box.z).quality = lock.number;
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
                b.add(door_shape, 0, d.x, d.y, d.z).quality = lock.number;
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

    // Guards: MONSTER pairs in the rooms but the start one, more of them
    // deeper in the level and in the exit room, some only from difficulty 2
    // or 3; clear of doorways, keys, controls and the exit pad, facing the
    // room the player comes from.
    if (options.guards != 0) {
        std::vector<std::pair<Spot, std::int32_t>> reserved;  // spot, clearance
        for (const Door& d : doors) reserved.push_back({{d.x, d.y, d.z}, 128});
        for (const Doorway& d : locked) reserved.push_back({{d.x, d.y, d.z}, 128});
        for (const Lock& lock : r.locks) {
            reserved.push_back({lock.key, 112});
            if (lock.kind == LockKind::kKeypadDoor) reserved.push_back({lock.desk, 144});
        }
        reserved.push_back({r.start, 160});
        reserved.push_back({{r.exit.x + 56, r.exit.y + 56, r.exit.z}, 176});
        // The level's NPC types, among the fighters most used on the original
        // maps: guards, soldiers and storm troopers, sometimes elites (not the
        // chemsuits, 190 pairs: workers, they loiter and surrender).
        constexpr int kTypes[] = {4, 9, 8, 15};  // GUARD, SOLDIER, STORMTRO, ELITE
        constexpr int kWeights[] = {5, 4, 4, 2};
        auto draw_type = [&] {
            int roll = rng.below(15);
            for (int t = 0; t < 4; ++t) {
                if ((roll -= kWeights[t]) < 0) return kTypes[t];
            }
            return kTypes[0];
        };
        for (std::size_t i = 0; i < r.rooms.size(); ++i) {
            const Room& room = r.rooms[i];
            if (static_cast<int>(i) == r.start_room) continue;
            int count = rng.range(0, 1) + (room.depth >= 2) + (static_cast<int>(i) == r.exit_room);
            const int extra = rng.chance(50) ? 1 : 0;  // only at higher difficulties
            // Where the player comes in (from a neighbour nearer the start).
            const Spot door = entry(r, static_cast<int>(i));
            for (int n = 0; n < count + extra && next_link < 250; ++n) {
                // A free spot: the NPC's far corner, 64 units from the walls.
                std::vector<Spot> free;
                for (std::int32_t x = room.x0 + 96; x <= room.x1 - 48; x += 32) {
                    for (std::int32_t y = room.y0 + 96; y <= room.y1 - 48; y += 32) {
                        const bool clear = std::none_of(reserved.begin(), reserved.end(), [&](const auto& res) {
                            return res.first.z == room.z && std::abs(res.first.x - x) < res.second &&
                                   std::abs(res.first.y - y) < res.second;
                        });
                        if (clear) free.push_back({x, y, room.z});
                    }
                }
                if (free.empty()) break;
                Guard g;
                g.room = static_cast<int>(i);
                g.place = free[rng.below(static_cast<int>(free.size()))];
                reserved.push_back({g.place, 96});
                g.npc_type = draw_type();
                g.min_difficulty = n < count ? 1 : rng.range(2, 3);
                {
                    // Facing the doorway (both far corners). Eight directions
                    // from north (-y), clockwise.
                    const std::int32_t dx = door.x - g.place.x, dy = door.y - g.place.y;
                    const int sx = dx > std::abs(dy) / 2 ? 1 : dx < -std::abs(dy) / 2 ? -1 : 0;
                    const int sy = dy > std::abs(dx) / 2 ? 1 : dy < -std::abs(dx) / 2 ? -1 : 0;
                    constexpr int kDir[3][3] = {{7, 6, 5}, {0, 0, 4}, {1, 2, 3}};  // [sx + 1][sy + 1]
                    g.direction = kDir[sx + 1][sy + 1];
                }
                g.link = new_link();
                auto [place, kind] = make_monster_pair(g.place.x, g.place.y, g.place.z, g.npc_type, g.link);
                MonsterPair m = read_monster_pair(place, kind);
                m.direction = g.direction;
                m.min_difficulty = g.min_difficulty;
                m.on_enter = true;
                m.other_weapon = rng.chance(20);
                write_monster_pair(place, kind, m);
                r.level.items.push_back(place);
                r.level.items.push_back(kind);
                r.guards.push_back(g);
            }
        }
    }

    char note[200];
    std::snprintf(note, sizeof note,
                  "seed %u: %s, %zu rooms, %zu passages, %zu locks, %d storey(s), %zu lift(s), %zu guard(s), exit %d "
                  "links from the start, %zu items",
                  options.seed, biome.name().c_str(), r.rooms.size(), r.links.size(), r.locks.size(), r.storeys,
                  r.lifts.size(), r.guards.size(), exit.depth, r.level.record_count());
    r.notes.push_back(note);
    for (const Lock& lock : r.locks) {
        std::snprintf(note, sizeof note, "  %s %u: passage to room %d, key in room %d", lock_name(lock.kind),
                      lock.number, lock.room, lock.key_room);
        r.notes.push_back(note);
    }
    for (const Lift& lift : r.lifts) {
        std::snprintf(note, sizeof note, "  lift %u: from room %d up to room %d", lift.number, lift.lower_room,
                      lift.upper_room);
        r.notes.push_back(note);
    }
    return r;
}

Spot entry(const Result& result, int room_index) {
    const Room& room = result.rooms[static_cast<std::size_t>(room_index)];
    if (room.entrance.x > 0) return room.entrance;
    for (const Link& l : result.links) {
        const int o = other_end(l, room_index);
        if (o < 0 || result.rooms[o].depth >= room.depth) continue;
        const Room& from = result.rooms[o];
        // The doorway's middle across, on tile 1 of the row or column.
        if (from.slot_y == room.slot_y) {
            const std::int32_t y = slot_y0(room.slot_y) + kBlock + kDoorOffset + kDoorWidth / 2 + 24;
            return {from.slot_x < room.slot_x ? room.x0 + kWall + 80 + 47 : room.x1 - kWall - 80, y, room.z};
        }
        const std::int32_t x = slot_x0(room.slot_x) + kBlock + kDoorOffset + kDoorWidth / 2 + 24;
        return {x, from.slot_y < room.slot_y ? room.y0 + kWall + 80 + 47 : room.y1 - kWall - 80, room.z};
    }
    return {room.centre_x(), room.centre_y(), room.z};
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
