#include "editor/generator/mission.hpp"

#include "editor/generator/campaign.hpp"
#include "editor/item_semantics.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace crusader::editor::gen {

namespace {

constexpr std::int32_t kCell = 16;
constexpr std::int32_t kUnitW = 16;            // wall thickness
constexpr std::int32_t kMargin = 64;           // between a room and anything else
constexpr std::int32_t kStepDown = 9;          // a floor this much under its storey bears it
constexpr std::int32_t kOriginX = 16384, kOriginY = 16384;
constexpr std::int32_t kMapMin = 1024, kMapMax = 31744;
constexpr std::uint16_t kShapeDoorN = 0x0A9, kShapeDoorE = 0x046;  // MDOOR_N, MDOOR_E
constexpr std::uint16_t kShapeCardNS = 0x31D, kShapeCardEW = 0x31E, kShapeKeycard = 0x111;
constexpr std::uint16_t kShapeTelepad = 0x1DB;
constexpr std::uint8_t kDoorEggType = 0x0A, kDoorEggZone = 0x44, kExitZone = 0x22;
constexpr std::uint8_t kTriggerOff = 7;
constexpr int kFirstLink = 32;

bool is_door_shape(std::uint16_t s) {
    switch (s) {
        case 0x005: case 0x046: case 0x07B: case 0x095: case 0x099: case 0x0A9: case 0x0C3: case 0x0C4:
        case 0x0C5: case 0x0C6: case 0x0C7: case 0x30A: case 0x30B: case 0x3F8: case 0x3FF:
            return true;
        default:
            return false;
    }
}

bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Scripted objects that make no sense away from their map: teleporters and
// eggs, map events, lifts (their other storey is not taken along).
bool dropped_class(const std::string& name) {
    for (const char* p : {"TELEPAD", "RTELEPAD", "MTELEPAD", "STELEPAD", "GTELEPAD", "SPIDPAD", "NPC_ONLY", "EVENT",
                          "TIMER", "TRIGPAD", "CRUZTRIG", "PPLATE", "UPPLATE", "LWPLATE", "SFXTRIG", "MUSIC",
                          "SECRTEGG", "ONCEEGG", "TRIGEGG", "ELEVPLAT", "ELEVATOR", "ELEVAT", "LIFT", "MISS",
                          "CAMERA", "ALRMTRIG"}) {
        if (starts(name, p)) return true;
    }
    return false;
}

int opposite(int side) { return (side + 2) % 4; }

struct Rect {
    std::int32_t x0, y0, x1, y1;
    bool overlaps(const Rect& o, std::int32_t margin) const {
        return x0 < o.x1 + margin && o.x0 < x1 + margin && y0 < o.y1 + margin && o.y0 < y1 + margin;
    }
};

// A room of the library placed on the new map.
struct Placed {
    const RoomPiece* piece = nullptr;
    std::int32_t dx = 0, dy = 0;
    std::vector<bool> used;  // by opening
    int in = -1;             // the opening its parent's joint comes in by
    Rect box() const { return {piece->x0 + dx, piece->y0 + dy, piece->x1 + dx, piece->y1 + dy}; }
    Opening opening(std::size_t k) const {
        Opening o = piece->openings[k];
        const bool along_x = o.side == kNorth || o.side == kSouth;
        o.from += along_x ? dx : dy;
        o.to += along_x ? dx : dy;
        o.edge += along_x ? dy : dx;
        return o;
    }
};

// Where a room opens into its parent, as on the original maps: their
// openings face each other across one line of wall (16 units), and a door
// stands in it.
struct Joint {
    int parent = 0, child = 0;
    int side = kEast;              // of the parent's opening
    std::int32_t line = 0;         // the wall's cells: [line, line + 16) across
    std::int32_t centre = 0;       // the door's centre, along the wall
    std::int32_t from = 0, to = 0; // the two openings together, along the wall
    bool along_x() const { return side == kNorth || side == kSouth; }
};

// A door fits in: an opening of the original map 96 to 224 units wide, or a
// wall piece where one can be cut (128 or 256).
bool usable(const Opening& o) { return o.width() >= 96 && (o.cut || o.width() <= 224); }

int usable_openings(const RoomPiece& r) {
    return static_cast<int>(std::count_if(r.openings.begin(), r.openings.end(), usable));
}

// How well a room suits a purpose (higher is better; < 0: not at all).
int suits(const RoomPiece& r, Purpose p, int openings_needed) {
    const int open = usable_openings(r);
    if (open < openings_needed) return -1;
    // A dead end on its own map (doors cut in its walls aside).
    const int natural = static_cast<int>(
        std::count_if(r.openings.begin(), r.openings.end(), [](const Opening& o) { return usable(o) && !o.cut; }));
    const bool dead_end = natural <= 1;
    const int area = r.area / 64;  // walkable, in 128 x 128 squares
    // Rooms big enough for their purpose (bounding box, in 128 x 128
    // squares: 16 is 512 x 512), the furnished ones first.
    const int box = (r.x1 - r.x0) / 128 * ((r.y1 - r.y0) / 128);
    int least = 16;
    if (p == Purpose::kKeyRoom) least = 12;
    if (p == Purpose::kLocked) least = 20;
    if (p == Purpose::kClimax) least = 30;
    if (p == Purpose::kExtraction) least = 9;
    if (box < least) return -1;
    // A room, not a fragment of a larger space: its border closed on the
    // original map but for a fifth at most (an open side, a balcony).
    if (r.open_border * 5 > r.border) return -1;
    // Nor a balcony or a catwalk: railings along a tenth of its border.
    if (r.low_border * 10 > r.border) return -1;
    // Furnished rooms: their furniture per 512 x 512 (16 squares), what hangs
    // on the walls aside; below 4,
    // a bare hall.
    const int density = r.furniture * 16 / std::max(box, 1);
    if (density < 4) return -1;
    // The key lies in a furnished office, not in a bare corridor.
    if (p == Purpose::kKeyRoom && density < 8) return -1;
    const int furnished = 2 * std::min(density, 20);
    // What a room holds counts up to a point: a few crates make a store.
    std::array<int, kTagCount> tags{};
    for (int t = 0; t < kTagCount; ++t) tags[static_cast<std::size_t>(t)] = std::min(r.tags[static_cast<std::size_t>(t)], 5);
    return furnished + [&] {
    switch (p) {
        case Purpose::kArrival: return 10 * tags[kStorage] + area / 2 - 4 * tags[kSecurity];
        case Purpose::kContact: return 20 + area - tags[kSecurity];
        case Purpose::kKeyRoom:
            return dead_end ? 40 + 10 * tags[kOffice] - std::max(0, area - 40) : 5 * tags[kOffice];
        case Purpose::kLocked: return 8 * (tags[kOffice] + tags[kSecurity]) + area / 2 + 5 * r.npcs;
        case Purpose::kTransit: return 10 * tags[kMachines] + area / 3 + 2 * r.npcs;
        case Purpose::kClimax: return 2 * area + 6 * r.npcs + 3 * tags[kSecurity];
        case Purpose::kExtraction: return (dead_end ? 60 : 0) - area + 2 * (tags[kMachines] + tags[kOffice]);
    }
    return 0;
    }();
}

// A wall run along `axis` (0: x, 1: y), its far side at `fixed`, with the
// biome's walls of that side.
void wall_run(Level& level, const Biome& biome, int side, int axis, std::int32_t fixed, std::int32_t from,
              std::int32_t to, std::int32_t z, std::vector<std::pair<std::int32_t, std::int32_t>> gaps = {}) {
    std::sort(gaps.begin(), gaps.end());
    auto run = [&](std::int32_t a, std::int32_t b) {
        std::int32_t pos = a;
        for (int i = 4; i >= 0; --i) {
            const Piece& p = biome.walls[side][axis][i];
            const std::int32_t len = kWallLengths[i] * kUnit;
            if (!p.valid()) continue;
            while (b - pos >= len) {
                LevelItem w;
                w.uid = new_uid();
                w.shape = p.shape;
                w.frame = p.frame;
                w.x = axis == kAlongX ? pos + len - 1 : fixed;
                w.y = axis == kAlongX ? fixed : pos + len - 1;
                w.z = z;
                level.items.push_back(w);
                pos += len;
            }
        }
    };
    std::int32_t pos = from;
    for (const auto& [a, b] : gaps) {
        run(pos, a);
        pos = b;
    }
    run(pos, to);
}

LevelItem make_item(std::uint16_t shape, std::uint8_t frame, std::int32_t x, std::int32_t y, std::int32_t z) {
    LevelItem i;
    i.uid = new_uid();
    i.shape = shape;
    i.frame = frame;
    i.x = x;
    i.y = y;
    i.z = z;
    return i;
}

}  // namespace

std::vector<int> look_alike_maps(const GameData& data, int source_map) {
    // A map's look: the share of each floor tile and wall shape among its
    // floors and walls (black void tiles aside).
    auto look = [&](int m) {
        std::map<std::uint16_t, double> share;
        if (m < 0 || static_cast<std::size_t>(m) >= data.maps.size()) return share;
        double total = 0;
        for (const LevelItem& i : flatten(data.maps[static_cast<std::size_t>(m)], data)) {
            if (i.shape >= data.shapes.size()) continue;
            const ShapeInfo& s = data.shapes[i.shape];
            int axis = 0, length = 0;
            const bool piece = is_floor_tile(s) || wall_kind(s, axis, length);
            if (!piece || (i.shape < data.class_names.size() && data.class_names[i.shape].rfind("BLACK", 0) == 0)) continue;
            share[i.shape] += 1;
            total += 1;
        }
        for (auto& [shape, v] : share) v /= total;
        return share;
    };
    const auto base = look(source_map);
    std::vector<std::pair<double, int>> alike;
    // Among the 63 maps of the missions (beyond them: maps the designers did
    // not ship in the game's missions).
    for (int m = 1; m < std::min(64, static_cast<int>(data.maps.size())); ++m) {
        if (m == source_map) continue;
        const auto other = look(m);
        double shared = 0;
        for (const auto& [shape, v] : other) {
            const auto it = base.find(shape);
            if (it != base.end()) shared += std::min(v, it->second);
        }
        if (shared >= 0.75) alike.push_back({shared, m});
    }
    std::sort(alike.begin(), alike.end(), std::greater<>());
    std::vector<int> maps{source_map};
    for (std::size_t k = 0; k < alike.size() && maps.size() < 5; ++k) maps.push_back(alike[k].second);
    return maps;
}

const char* purpose_name(Purpose p) {
    switch (p) {
        case Purpose::kArrival: return "arrival";
        case Purpose::kContact: return "contact";
        case Purpose::kKeyRoom: return "key room";
        case Purpose::kLocked: return "behind the locked door";
        case Purpose::kTransit: return "transit";
        case Purpose::kClimax: return "climax";
        case Purpose::kExtraction: return "extraction";
    }
    return "?";
}

std::vector<MissionStep> demo_mission() {
    return {
        // purpose, parent, locked, max guards, other parent, min guards
        {Purpose::kArrival, -1, false, 0, -1, 0},
        {Purpose::kContact, 0, false, 2, -1, 1},
        {Purpose::kKeyRoom, 0, false, 1, 1, 1},
        {Purpose::kLocked, 1, true, 3, -1, 2},
        {Purpose::kTransit, 3, false, 2, -1, 1},
        {Purpose::kClimax, 4, false, 6, -1, 4},
        {Purpose::kExtraction, 5, false, 0, -1, 0},
    };
}

std::vector<MissionStep> mission_steps(std::uint32_t seed, std::string* name) {
    // The plan from the seed's own bits, not its low ones: the attempts of a
    // layout (seed + attempt * 7919) keep the same plan.
    const std::uint32_t plan = (seed * 2654435761u) >> 30;  // 0 to 3
    if (plan == 1) {
        if (name) *name = "two cards";
        auto step = [](Purpose p, int parent, bool locked, int max_guards, int min_guards, int key = -1) {
            MissionStep s;
            s.purpose = p, s.parent = parent, s.locked = locked, s.max_guards = max_guards, s.min_guards = min_guards;
            s.key = key;
            return s;
        };
        return {
            step(Purpose::kArrival, -1, false, 0, 0),
            step(Purpose::kContact, 0, false, 2, 1),
            step(Purpose::kKeyRoom, 1, false, 1, 1),        // the first card
            step(Purpose::kTransit, 1, false, 2, 1),
            step(Purpose::kLocked, 3, true, 3, 2, 2),       // behind the first door
            step(Purpose::kKeyRoom, 4, false, 2, 1),        // the second card, guarded
            step(Purpose::kClimax, 4, true, 6, 4, 5),       // behind the second door
            step(Purpose::kExtraction, 6, false, 0, 0),
        };
    }
    if (plan == 2) {
        if (name) *name = "crossing";
        auto step = [](Purpose p, int parent, bool locked, int max_guards, int min_guards, int key = -1) {
            MissionStep s;
            s.purpose = p, s.parent = parent, s.locked = locked, s.max_guards = max_guards, s.min_guards = min_guards;
            s.key = key;
            return s;
        };
        return {
            step(Purpose::kArrival, -1, false, 0, 0),
            step(Purpose::kTransit, 0, false, 1, 0),        // a quiet way in
            step(Purpose::kContact, 1, false, 2, 1),
            step(Purpose::kKeyRoom, 2, false, 1, 1),        // the card, in a dead end half-way
            step(Purpose::kContact, 2, false, 3, 2),        // a second, harder fight
            step(Purpose::kLocked, 4, true, 3, 2, 3),
            step(Purpose::kClimax, 5, false, 6, 4),
            step(Purpose::kExtraction, 6, false, 0, 0),
        };
    }
    if (name) *name = "demo";
    return demo_mission();
}

namespace {
// A piece of the source map, by its storey and origin.
using PieceKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
Result build_mission(const GameData& data, const std::vector<MissionStep>& steps, const MissionOptions& options,
                     const std::set<PieceKey>& banned);
}  // namespace

std::string check_walkable(const Result& r, const GameData& data, int* failed_room) {
    if (r.rooms.empty()) return "no room";
    const WalkGrid g = walk_grid(r.level.items, data, 0);
    // Cells where the player's box fits, with a margin: their 4 x 4 cells
    // (the player is 48 units wide; the engine's moves need a little more
    // than the grid's cells say) walkable or doors (they open), unless shut
    // (card doors not opened).
    std::vector<std::uint8_t> shut(g.walk.size());
    auto open = [&](int row, int col) {
        if (row < 1 || col < 1 || row >= g.H - 2 || col >= g.W - 2) return false;
        for (int dr = -1; dr <= 2; ++dr) {
            for (int dc = -1; dc <= 2; ++dc) {
                const std::size_t k = static_cast<std::size_t>((row + dr) * g.W + col + dc);
                if ((!g.walk[k] && !g.door[k]) || shut[k]) return false;
            }
        }
        return true;
    };
    // The open cell nearest a spot (the player's far corner), within 3 cells.
    auto cell_of = [&](const Spot& at) -> std::pair<int, int> {
        const int row = (at.y - 24) / kCell, col = (at.x - 24) / kCell;
        for (int d = 0; d <= 3; ++d) {
            for (int dr = -d; dr <= d; ++dr) {
                for (int dc = -d; dc <= d; ++dc) {
                    if (open(row + dr, col + dc)) return {row + dr, col + dc};
                }
            }
        }
        return {-1, -1};
    };
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(g.W) * static_cast<std::size_t>(g.H));
    auto walk = [&](int sr, int sc) {
        std::fill(seen.begin(), seen.end(), std::uint8_t{0});
        std::deque<std::pair<int, int>> queue{{sr, sc}};
        seen[static_cast<std::size_t>(sr * g.W + sc)] = 1;
        while (!queue.empty()) {
            const auto [row, col] = queue.front();
            queue.pop_front();
            const int dr[4] = {-1, 0, 1, 0}, dc[4] = {0, 1, 0, -1};
            for (int d = 0; d < 4; ++d) {
                const int nr = row + dr[d], nc = col + dc[d];
                if (!open(nr, nc)) continue;
                std::uint8_t& v = seen[static_cast<std::size_t>(nr * g.W + nc)];
                if (!v) v = 1, queue.push_back({nr, nc});
            }
        }
    };
    const auto [sr, sc] = cell_of(r.start);
    if (sr < 0) return "the start stands on no open floor";
    walk(sr, sc);
    auto reached = [&](const Spot& at) {
        const int row = (at.y - 24) / kCell, col = (at.x - 24) / kCell;
        for (int d = 0; d <= 3; ++d) {
            for (int dr = -d; dr <= d; ++dr) {
                for (int dc = -d; dc <= d; ++dc) {
                    const int rr = row + dr, cc = col + dc;
                    if (rr >= 0 && cc >= 0 && rr < g.H && cc < g.W && seen[static_cast<std::size_t>(rr * g.W + cc)]) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    for (std::size_t k = 1; k < r.rooms.size(); ++k) {
        if (!reached(entry(r, static_cast<int>(k)))) {
            if (failed_room) *failed_room = static_cast<int>(k);
            return "room " + std::to_string(k) + " cannot be walked to";
        }
    }
    if (!reached(r.exit)) return "the exit cannot be walked to";
    // Each key, where it lies (a room's cells may hold a corner that the
    // player's box cannot get into).
    for (const Lock& lock : r.locks) {
        if (!reached(lock.key)) {
            if (failed_room) *failed_room = lock.key_room;
            return "the key of room " + std::to_string(lock.room) + " cannot be walked to";
        }
    }
    // With its card door shut, the room behind it must be out of reach (no
    // other way round, through rooms that ended up side by side).
    for (const Lock& lock : r.locks) {
        std::fill(shut.begin(), shut.end(), std::uint8_t{0});
        if (lock.barrier_shape >= data.shapes.size()) continue;
        const ShapeInfo& s = data.shapes[lock.barrier_shape];
        const int c1 = lock.barrier.x / kCell, r1 = lock.barrier.y / kCell;
        for (int rr = std::max(0, r1 - s.footpad_y + 1); rr <= r1 && rr < g.H; ++rr) {
            for (int cc = std::max(0, c1 - s.footpad_x + 1); cc <= c1 && cc < g.W; ++cc) {
                shut[static_cast<std::size_t>(rr * g.W + cc)] = 1;
            }
        }
        walk(sr, sc);
        if (reached(entry(r, lock.room))) {
            if (failed_room) *failed_room = lock.room;
            return "room " + std::to_string(lock.room) + " can be walked to round its card door";
        }
    }
    return {};
}

Result generate_mission(const GameData& data, const std::vector<MissionStep>& steps, const MissionOptions& options) {
    // A layout whose floor does not join up (an opening that a room's own
    // furniture blocks) is drawn again, without the room that failed.
    std::vector<std::string> refused;
    std::set<PieceKey> banned;
    for (int attempt = 0; attempt < 40; ++attempt) {
        MissionOptions o = options;
        o.seed = options.seed + static_cast<std::uint32_t>(attempt) * 7919u;
        Result r = build_mission(data, steps, o, banned);
        if (r.rooms.empty()) {
            std::vector<MissionStep> other = steps;
            for (MissionStep& s : other) {
                if (s.alt_parent >= 0) s.parent = s.alt_parent;
            }
            r = build_mission(data, other, o, banned);
        }
        if (r.rooms.empty()) {
            refused.push_back(r.notes.empty() ? "no layout" : r.notes.front());
            continue;
        }
        int failed = -1;
        const std::string problem = check_walkable(r, data, &failed);
        if (failed >= 0) {
            const Spot src = r.rooms[static_cast<std::size_t>(failed)].source;
            banned.insert({src.z, src.x, src.y});
        }
        if (problem.empty()) {
            for (const std::string& p : refused) r.notes.push_back("  layout drawn again: " + p);
            return r;
        }
        refused.push_back(problem);
    }
    Result none;
    none.notes.push_back("no layout of the mission can be walked through: " + refused.back());
    for (const std::string& p : refused) none.notes.push_back("  tried: " + p);
    return none;
}

namespace {

Result build_mission(const GameData& data, const std::vector<MissionStep>& steps, const MissionOptions& options,
                     const std::set<PieceKey>& banned) {
    Result r;
    Rng rng(options.seed);
    // The rooms of the source map and of the maps that look like it (those
    // of their upper storeys brought down to z 0), each room once.
    std::vector<RoomPiece> library;
    for (int m : look_alike_maps(data, options.source_map)) {
        for (std::int32_t z : {0, kStorey}) {
            for (RoomPiece& piece : extract_rooms(data, m, z)) {
                // The same room on a variant of the map (map 60 repeats map 1).
                const bool twin = std::any_of(library.begin(), library.end(), [&](const RoomPiece& o) {
                    return o.z == piece.z && std::abs(o.x0 - piece.x0) <= 64 && std::abs(o.y0 - piece.y0) <= 64 &&
                           std::abs(o.x1 - piece.x1) <= 64 && std::abs(o.y1 - piece.y1) <= 64;
                });
                if (!twin) library.push_back(std::move(piece));
            }
        }
    }
    auto name = [&](std::uint16_t s) { return s < data.class_names.size() ? data.class_names[s] : std::string(); };
    auto scripted = [&](std::uint16_t s) { return s < data.scripted.size() && data.scripted[s]; };
    const std::size_t n = steps.size();
    std::vector<int> children(n);
    for (const MissionStep& s : steps) {
        if (s.parent >= 0) ++children[static_cast<std::size_t>(s.parent)];
    }

    // Layout: each step in turn, a room that suits it joined by a corridor
    // to an opening of its parent's room, without overlapping; a step that
    // finds none goes back to the one before (depth-first search).
    std::vector<Placed> placed(n);
    std::vector<Joint> joints(n);
    std::vector<bool> taken(library.size());
    // The placed rooms' cells, on the whole map: a room keeps a wall's width
    // (a cell) from the others.
    constexpr int kGridCells = 32768 / kCell;
    std::vector<std::uint8_t> occupied(static_cast<std::size_t>(kGridCells) * kGridCells);
    auto mark = [&](const Placed& p, std::uint8_t v) {
        const RoomPiece& piece = *p.piece;
        for (int row = 0; row < piece.rows; ++row) {
            for (int col = 0; col < piece.cols; ++col) {
                if (!piece.outline[static_cast<std::size_t>(row * piece.cols + col)]) continue;
                const int gr = (piece.y0 + p.dy) / kCell + row, gc = (piece.x0 + p.dx) / kCell + col;
                occupied[static_cast<std::size_t>(gr * kGridCells + gc)] = v;
            }
        }
    };
    auto fits = [&](const Placed& p) {
        const RoomPiece& piece = *p.piece;
        for (int row = 0; row < piece.rows; ++row) {
            for (int col = 0; col < piece.cols; ++col) {
                if (!piece.outline[static_cast<std::size_t>(row * piece.cols + col)]) continue;
                const int gr = (piece.y0 + p.dy) / kCell + row, gc = (piece.x0 + p.dx) / kCell + col;
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (occupied[static_cast<std::size_t>((gr + dr) * kGridCells + gc + dc)]) return false;
                    }
                }
            }
        }
        return true;
    };
    int tries = 0;
    std::size_t deepest = 0;  // the furthest step reached, for the note when it fails
    std::function<bool(std::size_t)> place = [&](std::size_t k) -> bool {
        if (k == n) return true;
        deepest = std::max(deepest, k);
        if (++tries > 20000) return false;
        const MissionStep& step = steps[k];
        const int needed = children[k] + (step.parent >= 0 ? 1 : 0);
        std::vector<std::pair<int, std::size_t>> scored;
        int best_score = 0;
        for (std::size_t i = 0; i < library.size(); ++i) {
            if (taken[i] || banned.count({library[i].z, library[i].x0, library[i].y0})) continue;
            const int score = suits(library[i], step.purpose, needed);
            if (score >= 0) scored.push_back({score, i}), best_score = std::max(best_score, score);
        }
        // The score orders the rooms that suit; chance, as large as half the
        // best score, decides among the good ones, so that seeds give other
        // levels.
        std::vector<std::pair<int, std::size_t>> candidates;
        for (const auto& [score, i] : scored) candidates.push_back({score + rng.below(std::max(40, best_score / 2)), i});
        std::sort(candidates.begin(), candidates.end(), std::greater<>());
        if (candidates.size() > 8) candidates.resize(8);
        for (const auto& [score, i] : candidates) {
            const RoomPiece& piece = library[i];
            Placed p;
            p.piece = &piece;
            p.used.assign(piece.openings.size(), false);
            if (step.parent < 0) {
                p.dx = kOriginX - piece.x0;
                p.dy = kOriginY - piece.y0;
                taken[i] = true;
                placed[k] = p;
                mark(p, 1);
                if (place(k + 1)) return true;
                mark(p, 0);
                taken[i] = false;
                continue;
            }
            Placed& parent = placed[static_cast<std::size_t>(step.parent)];
            std::vector<std::size_t> order(parent.used.size());
            for (std::size_t q = 0; q < order.size(); ++q) order[q] = q;
            for (std::size_t q = order.size(); q > 1; --q) std::swap(order[q - 1], order[rng.below(static_cast<int>(q))]);
            // The designers' openings first, then the doors cut in walls.
            std::stable_partition(order.begin(), order.end(),
                                  [&](std::size_t q) { return !parent.piece->openings[q].cut; });
            std::vector<std::size_t> child_order(piece.openings.size());
            for (std::size_t q = 0; q < child_order.size(); ++q) child_order[q] = q;
            std::stable_partition(child_order.begin(), child_order.end(),
                                  [&](std::size_t q) { return !piece.openings[q].cut; });
            for (const std::size_t po : order) {
                if (parent.used[po] || !usable(parent.piece->openings[po])) continue;
                const Opening a = parent.opening(po);
                // A card door's reader goes on the face of the wall the camera
                // sees (south, east): the parent must lie south or east of it.
                if (step.locked && a.side != kNorth && a.side != kWest) continue;
                for (const std::size_t co : child_order) {
                    const Opening& b = piece.openings[co];
                    if (!usable(b) || b.side != opposite(a.side)) continue;
                    Joint j;
                    j.parent = step.parent;
                    j.child = static_cast<int>(k);
                    j.side = a.side;
                    j.line = a.side == kEast || a.side == kSouth ? a.edge : a.edge - kUnitW;
                    // The child's border on the far side of the wall, its
                    // opening facing the parent's.
                    const std::int32_t far = a.side == kEast || a.side == kSouth ? a.edge + kUnitW : a.edge - kUnitW;
                    const std::int32_t along = (a.centre() - b.centre()) / kCell * kCell;
                    if (a.side == kEast || a.side == kWest) p.dx = far - b.edge, p.dy = along;
                    else p.dx = along, p.dy = far - b.edge;
                    // Both openings take the door (96 units).
                    const std::int32_t lo = std::max(a.from, b.from + along), hi = std::min(a.to, b.to + along);
                    if (hi - lo < 96) continue;
                    j.centre = std::clamp((lo + hi) / 2 / kCell * kCell, lo + 48, hi - 48);
                    j.from = std::min(a.from, b.from + along);
                    j.to = std::max(a.to, b.to + along);
                    const Rect box = p.box();
                    if (box.x0 < kMapMin || box.y0 < kMapMin || box.x1 > kMapMax || box.y1 > kMapMax) continue;
                    if (!fits(p)) continue;
                    p.used.assign(piece.openings.size(), false);
                    p.used[co] = true;
                    p.in = static_cast<int>(co);
                    parent.used[po] = true;
                    taken[i] = true;
                    placed[k] = p;
                    joints[k] = j;
                    mark(p, 1);
                    if (place(k + 1)) return true;
                    mark(p, 0);
                    taken[i] = false;
                    parent.used[po] = false;
                }
            }
        }
        return false;
    };
    if (library.empty() || !place(0)) {
        int suitable = 0;
        if (deepest < n) {
            const int needed = children[deepest] + (steps[deepest].parent >= 0 ? 1 : 0);
            for (const RoomPiece& piece : library) suitable += suits(piece, steps[deepest].purpose, needed) >= 0;
        }
        r.notes.push_back("the mission's rooms cannot be laid out from map " + std::to_string(options.source_map) +
                          ": no room for step " + std::to_string(deepest) + " (" +
                          purpose_name(steps[std::min(deepest, n - 1)].purpose) + ", " + std::to_string(suitable) +
                          " rooms suit it)");
        return r;
    }

    // The look of the walls added (joints, openings walled up): the biome whose
    // pieces the rooms use most.
    std::map<std::uint16_t, int> uses;
    for (const Placed& p : placed) {
        for (const LevelItem& i : p.piece->items) ++uses[i.shape];
    }
    int best = 0, best_score = -1;
    for (std::size_t b = 0; b < data.biomes.size(); ++b) {
        const Biome& biome = data.biomes[b];
        int score = 0;
        for (const Piece& f : biome.floor_variants) score += uses[f.shape];
        for (const auto& side : biome.walls) {
            for (const auto& axis : side) {
                for (const Piece& w : axis) score += w.valid() ? uses[w.shape] : 0;
            }
        }
        if (score > best_score) best_score = score, best = static_cast<int>(b);
    }
    r.biome = best;
    const Biome& biome = data.biomes[static_cast<std::size_t>(best)];

    // The rooms, their items moved, thinned out and relinked.
    int next_link = kFirstLink;
    int next_card = options.first_card;
    Level& level = r.level;
    for (std::size_t k = 0; k < n; ++k) {
        const Placed& p = placed[k];
        const RoomPiece& piece = *p.piece;
        const MissionStep& step = steps[k];
        Room room;
        room.x0 = p.box().x0, room.y0 = p.box().y0, room.x1 = p.box().x1, room.y1 = p.box().y1;
        room.z = 0;
        room.depth = step.parent < 0 ? 0 : r.rooms[static_cast<std::size_t>(step.parent)].depth + 1;
        room.source = {piece.x0, piece.y0, piece.z};
        r.rooms.push_back(room);

        // Doors in the openings go (a corridor's own door replaces them, or
        // a wall); the openings left are walled up.
        auto in_opening = [&](const LevelItem& i) {
            const ShapeInfo* s = i.shape < data.shapes.size() ? &data.shapes[i.shape] : nullptr;
            const std::int32_t cx = i.x - (s ? s->footpad_x * 8 : 0), cy = i.y - (s ? s->footpad_y * 8 : 0);
            for (std::size_t q = 0; q < piece.openings.size(); ++q) {
                const Opening& o = piece.openings[q];
                if (o.cut && !p.used[q]) continue;
                const bool along_x = o.side == kNorth || o.side == kSouth;
                const std::int32_t a = along_x ? cx : cy, d = along_x ? cy : cx;
                if (a >= o.from - 32 && a < o.to + 32 && std::abs(d - o.edge) <= 64) return true;
            }
            return false;
        };
        // A door cut in a wall: its wall piece goes, with what hangs on it
        // (the joint makes the wall whole again round its door).
        auto in_cut = [&](const LevelItem& i) {
            const ShapeInfo* s = i.shape < data.shapes.size() ? &data.shapes[i.shape] : nullptr;
            if (!s) return false;
            int axis = 0, length = 0;
            const bool wall = wall_kind(*s, axis, length);
            const bool thin = s->footpad_x <= 1 || s->footpad_y <= 1;
            // What stands flat against it: a sign, a panel, a fake lift door.
            const bool hung = thin && !wall && !is_floor_tile(*s) && s->footpad_z > 0;
            if (!wall && !hung) return false;
            const std::int32_t x0 = i.x - std::max(1, int{s->footpad_x}) * kCell + 1, y0 = i.y - std::max(1, int{s->footpad_y}) * kCell + 1;
            for (std::size_t q = 0; q < piece.openings.size(); ++q) {
                const Opening& o = piece.openings[q];
                if (!o.cut || !p.used[q]) continue;
                const bool along_x = o.side == kNorth || o.side == kSouth;
                if (wall && (axis == kAlongX) != along_x) continue;
                const std::int32_t a0 = along_x ? x0 : y0, a1 = along_x ? i.x : i.y;
                const std::int32_t c0 = along_x ? y0 : x0, c1 = along_x ? i.y : i.x;
                if (a1 < o.from || a0 >= o.to) continue;
                // The wall's cell, and the room's first cell for what hangs on it.
                const bool far = o.side == kNorth || o.side == kWest;
                const std::int32_t l0 = far ? o.edge - kCell : o.edge - (hung ? kCell : 0);
                const std::int32_t l1 = far ? o.edge + (hung ? kCell : 0) - 1 : o.edge + kCell - 1;
                if (c1 >= l0 && c0 <= l1) return true;
            }
            return false;
        };
        // The guard posts kept: MONSTER places and their kinds.
        std::vector<const LevelItem*> places;
        for (const LevelItem& i : piece.items) {
            if (i.shape == kShapeMonster && i.frame == 0) places.push_back(&i);
        }
        for (std::size_t a = places.size(); a > 1; --a) std::swap(places[a - 1], places[rng.below(static_cast<int>(a))]);
        if (static_cast<int>(places.size()) > step.max_guards) places.resize(static_cast<std::size_t>(step.max_guards));
        std::map<std::uint8_t, std::uint8_t> relink;  // old link -> new
        for (std::uint8_t l : piece.local_links) {
            if (next_link < 250) relink[l] = static_cast<std::uint8_t>(next_link++);
        }
        std::set<std::uint8_t> guard_links;
        for (const LevelItem* pl : places) guard_links.insert(pl->quality_low());
        std::set<std::uint8_t> door_links;  // doors kept inside the room, with their DOOREGG
        for (const LevelItem& i : piece.items) {
            if (is_door_shape(i.shape) && !in_opening(i) && i.quality_low()) {
                const bool egg = std::any_of(piece.items.begin(), piece.items.end(), [&](const LevelItem& e) {
                    return e.shape == kShapeTriggerEgg && e.quality == kDoorEggType && e.map_number == i.quality_low();
                });
                if (egg) door_links.insert(i.quality_low());
            }
        }
        std::map<std::uint8_t, std::uint8_t> door_relink;
        for (std::uint8_t l : door_links) door_relink[l] = static_cast<std::uint8_t>(next_link++);
        for (const LevelItem& src : piece.items) {
            if (in_cut(src)) continue;
            LevelItem i = src;
            const std::string cls = name(i.shape);
            if (i.shape == kShapeMonster) {
                // A kind goes with a kept place of its link within 64 units.
                const bool kept = std::any_of(places.begin(), places.end(), [&](const LevelItem* pl) {
                    return pl->quality_low() == i.quality_low() && std::abs(pl->x - i.x) <= 64 &&
                           std::abs(pl->y - i.y) <= 64;
                });
                if (!kept) continue;
                if (!relink.count(i.quality_low())) relink[i.quality_low()] = static_cast<std::uint8_t>(next_link++);
                i.quality = static_cast<std::uint16_t>((i.quality & 0xFF00) | relink[i.quality_low()]);
                if (i.frame == 0) i.map_number = static_cast<std::uint8_t>(i.map_number | 8);  // appears on approach
            } else if (is_door_shape(i.shape)) {
                if (in_opening(i) || !door_links.count(i.quality_low())) continue;
                i.quality = static_cast<std::uint16_t>((i.quality & 0xFF00) | door_relink[i.quality_low()]);
            } else if (i.shape == kShapeTriggerEgg) {
                if (i.quality != kDoorEggType || !door_links.count(i.map_number)) continue;
                i.map_number = door_relink[i.map_number];
            } else if (i.shape == kShapeTeleportEgg || i.shape == kShapeMonsterEgg || dropped_class(cls)) {
                continue;
            } else if (scripted(i.shape) && i.quality_low()) {
                const auto it = relink.find(i.quality_low());
                if (it == relink.end()) continue;  // linked to something outside the room
                i.quality = static_cast<std::uint16_t>((i.quality & 0xFF00) | it->second);
            }
            i.uid = new_uid();
            i.x += p.dx;
            i.y += p.dy;
            i.z -= piece.z;
            if (i.z < 0) {
                // Below the storey (a map's z is one byte): its floors a few
                // units lower go to z 0, what stood lower down stays behind.
                const ShapeInfo* s = i.shape < data.shapes.size() ? &data.shapes[i.shape] : nullptr;
                if (!s || s->footpad_z != 0 || i.z < -kStepDown) continue;
                i.z = 0;
            }
            i.next = 0;
            level.items.push_back(i);
        }
        // The room closed: along its border, every cell outside it that
        // nothing solid stands on (an opening to a room left behind, a side
        // open on the original map) gets a wall, but where a corridor comes in.
        {
            const int cols = piece.cols + 2, rows = piece.rows + 2;  // one cell round the room
            std::vector<std::uint8_t> solid(static_cast<std::size_t>(cols * rows));
            for (const LevelItem& i : level.items) {
                if (i.shape >= data.shapes.size()) continue;
                const ShapeInfo& s = data.shapes[i.shape];
                if (!s.is(ShapeInfo::kSolid) || s.footpad_z * 8 < 16 || i.z >= 48) continue;
                const int c1 = (i.x - p.dx - piece.x0) / kCell + 1, r1 = (i.y - p.dy - piece.y0) / kCell + 1;
                for (int rr = std::max(0, r1 - s.footpad_y + 1); rr <= std::min(rows - 1, r1); ++rr) {
                    for (int cc = std::max(0, c1 - s.footpad_x + 1); cc <= std::min(cols - 1, c1); ++cc) {
                        solid[static_cast<std::size_t>(rr * cols + cc)] = 1;
                    }
                }
            }
            // Its outline (pieces.hpp): a dent narrower than a doorway is
            // walled straight across its mouth, not round its sides.
            auto in_room = [&](int rr, int cc) {
                return rr >= 0 && cc >= 0 && rr < piece.rows && cc < piece.cols &&
                       piece.outline[static_cast<std::size_t>(rr * piece.cols + cc)];
            };
            const int dr[4] = {-1, 0, 1, 0}, dc[4] = {0, 1, 0, -1};  // N E S W
            for (int d = 0; d < 4; ++d) {
                const bool along_x = d == kNorth || d == kSouth;
                // Runs of open border cells, by border line.
                std::map<int, std::vector<std::pair<int, int>>> runs;  // line -> [from, to) in cells
                for (int rr = 0; rr < piece.rows; ++rr) {
                    for (int cc = 0; cc < piece.cols; ++cc) {
                        if (!in_room(rr, cc) || in_room(rr + dr[d], cc + dc[d])) continue;
                        const int orr = rr + dr[d] + 1, occ = cc + dc[d] + 1;  // the cell outside, in `solid`
                        if (solid[static_cast<std::size_t>(orr * cols + occ)]) continue;
                        // Its outer border only: furniture within the room
                        // has the room again past it.
                        bool within = false;
                        for (int m = 2; m <= 8 && !within; ++m) within = in_room(rr + dr[d] * m, cc + dc[d] * m);
                        if (within) continue;
                        // A corridor's opening stays open.
                        const std::int32_t along = along_x ? piece.x0 + cc * kCell : piece.y0 + rr * kCell;
                        const std::int32_t edge = (d == kNorth) ? piece.y0 + rr * kCell
                                                  : (d == kSouth) ? piece.y0 + (rr + 1) * kCell
                                                  : (d == kWest) ? piece.x0 + cc * kCell
                                                                 : piece.x0 + (cc + 1) * kCell;
                        bool corridor = false;
                        for (std::size_t o = 0; o < piece.openings.size(); ++o) {
                            const Opening& op = piece.openings[o];
                            if (p.used[o] && op.side == d && op.edge == edge && along >= op.from && along < op.to) {
                                corridor = true;
                            }
                        }
                        if (corridor) continue;
                        const int line = along_x ? rr : cc, at = along_x ? cc : rr;
                        auto& v = runs[line];
                        if (!v.empty() && v.back().second == at) v.back().second = at + 1;
                        else v.push_back({at, at + 1});
                    }
                }
                for (const auto& [line, spans] : runs) {
                    const int side = kBack;  // a wall seen from the room beyond, never the black frame of a facade
                    std::int32_t fixed = 0;
                    if (d == kNorth) fixed = piece.y0 + p.dy + line * kCell - 1;
                    if (d == kSouth) fixed = piece.y0 + p.dy + (line + 1) * kCell + kUnitW - 1;
                    if (d == kWest) fixed = piece.x0 + p.dx + line * kCell - 1;
                    if (d == kEast) fixed = piece.x0 + p.dx + (line + 1) * kCell + kUnitW - 1;
                    const std::int32_t base = along_x ? piece.x0 + p.dx : piece.y0 + p.dy;
                    for (const auto& [from, to] : spans) {
                        wall_run(level, biome, side, along_x ? kAlongX : kAlongY, fixed, base + from * kCell,
                                 base + to * kCell, 0);
                    }
                }
            }
        }
    }
    // Where the player comes into each room: 80 units inside the opening its
    // corridor reaches (a far corner, as the player's position).
    for (std::size_t k = 1; k < n; ++k) {
        const Placed& p = placed[k];
        if (p.in < 0) continue;
        const std::size_t o = static_cast<std::size_t>(p.in);
        const Opening op = p.opening(o);
        const std::int32_t across = op.centre() + 24;
        Spot& in = r.rooms[k].entrance;
        switch (op.side) {
            case kWest: in = {op.edge + 80 + 47, across, 0}; break;
            case kEast: in = {op.edge - 80, across, 0}; break;
            case kNorth: in = {across, op.edge + 80 + 47, 0}; break;
            default: in = {across, op.edge - 80, 0}; break;
        }
    }
    // The NPCs kept, where the designers put them. Fighters guard and
    // attack on seeing the player (activities 8, 5, 5), facing the way the
    // player comes in: the designers turned them to their room's original
    // doorway, set them to walk or to wait for an alarm. Workers keep their
    // own ways (they loiter, run, surrender). Other kinds (turrets, rare
    // robots) are left out for now.
    auto fighter = [](int type) {
        // Not 10 (THERMATR) nor the squads 16 (GUARDSQ) and 22 (EGRENADE): alone, they never fire.
        for (int t : {4, 8, 9, 11, 13, 15, 17, 18, 19}) {
            if (type == t) return true;
        }
        return false;
    };
    auto worker = [](int type) {
        for (int t : {1, 2, 3, 5, 6, 7, 12}) {
            if (type == t) return true;
        }
        return false;
    };
    std::vector<std::uint8_t> unwanted;
    for (LevelItem& post : level.items) {
        if (post.shape != kShapeMonster || post.frame != 0) continue;
        for (LevelItem& kind : level.items) {
            if (kind.shape != kShapeMonster || kind.frame != 1 || kind.quality_low() != post.quality_low()) continue;
            Guard g;
            for (std::size_t k = 0; k < n; ++k) {
                const Rect b = placed[k].box();
                if (post.x >= b.x0 && post.x < b.x1 + 16 && post.y >= b.y0 && post.y < b.y1 + 16) {
                    g.room = static_cast<int>(k);
                }
            }
            MonsterPair m = read_monster_pair(post, kind);
            if (!fighter(m.npc_type)) {
                if (!worker(m.npc_type)) unwanted.push_back(post.quality_low());
                break;
            }
            const Spot in = entry(r, g.room);
            const std::int32_t dx = in.x - post.x, dy = in.y - post.y;
            const int sx = dx > std::abs(dy) / 2 ? 1 : dx < -std::abs(dy) / 2 ? -1 : 0;
            const int sy = dy > std::abs(dx) / 2 ? 1 : dy < -std::abs(dx) / 2 ? -1 : 0;
            constexpr int kDir[3][3] = {{7, 6, 5}, {0, 0, 4}, {1, 2, 3}};  // [sx + 1][sy + 1]
            m.direction = kDir[sx + 1][sy + 1];
            // Their own ways of fighting that work alone (5 attack, 10 the
            // elites' and enforcers'), from a guard's post. A squad's ways
            // (9, 11) wait for the rest of the squad: alone, the squad member
            // never fires, and attacks instead.
            auto combat = [](int a) { return a == 5 || a == 10; };
            m.activity[0] = 8;
            if (!combat(m.activity[1])) m.activity[1] = 5;
            if (!combat(m.activity[2])) m.activity[2] = m.activity[1];
            m.on_enter = true;
            write_monster_pair(post, kind, m);
            g.place = {post.x, post.y, post.z};
            g.npc_type = m.npc_type;
            g.min_difficulty = m.min_difficulty;
            g.direction = m.direction;
            g.link = post.quality_low();
            r.guards.push_back(g);
            break;
        }
    }
    std::erase_if(level.items, [&](const LevelItem& i) {
        return i.shape == kShapeMonster &&
               std::find(unwanted.begin(), unwanted.end(), i.quality_low()) != unwanted.end();
    });
    // The joints: the wall line between the two openings, made whole where
    // neither room has a wall, with a door in it.
    for (std::size_t k = 1; k < n; ++k) {
        const Joint& j = joints[k];
        const MissionStep& step = steps[k];
        Link link;
        link.a = j.parent;
        link.b = j.child;
        const bool along_x = j.along_x();
        // The line's cells (16 units each) that a wall already holds.
        const int cells = (j.to - j.from) / kCell;
        std::vector<std::uint8_t> held(static_cast<std::size_t>(std::max(cells, 0)));
        for (const LevelItem& i : level.items) {
            if (i.shape >= data.shapes.size()) continue;
            const ShapeInfo& sh = data.shapes[i.shape];
            if (!sh.is(ShapeInfo::kSolid) || sh.footpad_z * 8 < 64 || i.z >= 48) continue;
            const std::int32_t a1 = along_x ? i.x : i.y, a0 = a1 - (along_x ? sh.footpad_x : sh.footpad_y) * kCell + 1;
            const std::int32_t c1 = along_x ? i.y : i.x, c0 = c1 - (along_x ? sh.footpad_y : sh.footpad_x) * kCell + 1;
            if (c1 < j.line || c0 >= j.line + kUnitW) continue;
            for (int q = 0; q < cells; ++q) {
                const std::int32_t at = j.from + q * kCell;
                if (at + kCell - 1 >= a0 && at <= a1) held[static_cast<std::size_t>(q)] = 1;
            }
        }
        std::vector<std::pair<std::int32_t, std::int32_t>> gaps{{j.centre - 48, j.centre + 48}};
        for (int q = 0; q < cells; ++q) {
            if (held[static_cast<std::size_t>(q)]) gaps.push_back({j.from + q * kCell, j.from + (q + 1) * kCell});
        }
        // Gaps merged, so that the runs between them are whole.
        std::sort(gaps.begin(), gaps.end());
        std::vector<std::pair<std::int32_t, std::int32_t>> merged;
        for (const auto& g : gaps) {
            if (!merged.empty() && g.first <= merged.back().second) merged.back().second = std::max(merged.back().second, g.second);
            else merged.push_back(g);
        }
        wall_run(level, biome, kBack, along_x ? kAlongX : kAlongY, j.line + kUnitW - 1, j.from, j.to, 0, merged);
        // The door.
        const std::uint16_t door_shape = along_x ? kShapeDoorN : kShapeDoorE;
        const std::int32_t door_x = along_x ? j.centre + 47 : j.line + kUnitW - 1;
        const std::int32_t door_y = along_x ? j.line + kUnitW - 1 : j.centre + 47;
        // On the floor of the way through: a room's floor may stand a few
        // units higher (a raised floor), and the door opens up to its top
        // plus 88, over the player's head.
        std::int32_t floor_z = 0;
        for (const LevelItem& i : level.items) {
            if (i.shape >= data.shapes.size()) continue;
            const ShapeInfo& sh = data.shapes[i.shape];
            const std::int32_t top = i.z + sh.footpad_z * 8;
            if (!sh.is(ShapeInfo::kSolid) || sh.footpad_z > 2 || top <= 0 || top > 9) continue;
            const std::int32_t a1 = along_x ? i.x : i.y, a0 = a1 - (along_x ? sh.footpad_x : sh.footpad_y) * kCell + 1;
            const std::int32_t c1 = along_x ? i.y : i.x, c0 = c1 - (along_x ? sh.footpad_y : sh.footpad_x) * kCell + 1;
            if (a1 < j.centre - 48 || a0 >= j.centre + 48 || c1 < j.line - 3 * kCell || c0 >= j.line + 4 * kCell) continue;
            floor_z = std::max(floor_z, top);
        }
        LevelItem door = make_item(door_shape, 0, door_x, door_y, floor_z);
        if (!step.locked) {
            const auto l = static_cast<std::uint8_t>(next_link++);
            door.quality = l;
            level.items.push_back(door);
            LevelItem egg = make_item(kShapeTriggerEgg, 0, along_x ? door_x - 48 : door_x - kUnitW / 2,
                                      along_x ? door_y - kUnitW / 2 : door_y - 48, floor_z);
            egg.quality = kDoorEggType;
            egg.npc_number = kDoorEggZone;
            egg.map_number = l;
            level.items.push_back(egg);
            link.door_a = l;
        } else {
            Lock lock;
            lock.kind = LockKind::kCardDoor;
            lock.link = static_cast<int>(r.links.size());
            lock.room = j.child;
            lock.number = static_cast<std::uint8_t>((next_card++ - 1) % 31 + 1);
            door.quality = lock.number;
            level.items.push_back(door);
            lock.barrier_shape = door_shape;
            lock.barrier = {door_x, door_y, floor_z};
            // The reader beside the door, on the wall's face that the camera
            // sees, in the parent's room (south or east of the wall).
            lock.reader_shape = along_x ? kShapeCardNS : kShapeCardEW;
            lock.reader = along_x ? Spot{j.centre + 48 + 23, j.line + 2 * kUnitW - 1, 40 + floor_z}
                                  : Spot{j.line + 2 * kUnitW - 1, j.centre + 48 + 23, 40 + floor_z};
            LevelItem reader = make_item(lock.reader_shape, 0, lock.reader.x, lock.reader.y, lock.reader.z);
            set_trigger_target(reader, door_shape);
            reader.map_number = static_cast<std::uint8_t>(reader.map_number | kTriggerOff);
            reader.quality = static_cast<std::uint16_t>(3 << 8 | lock.number);
            level.items.push_back(reader);
            // Its key: the card in its step's key room, else in the first one.
            lock.key_room = -1;
            if (step.key >= 0) lock.key_room = step.key;
            for (std::size_t q = 0; q < n && lock.key_room < 0; ++q) {
                if (steps[q].purpose == Purpose::kKeyRoom) lock.key_room = static_cast<int>(q);
            }
            lock.key_shape = kShapeKeycard;
            link.lock = static_cast<int>(r.locks.size());
            r.locks.push_back(lock);
        }
        r.links.push_back(link);
    }

    // Free spots of a room: cells with `clear` cells around them all in the
    // room, the one that `better` prefers.
    auto spot = [&](std::size_t k, int clear, auto better) {
        const Placed& p = placed[k];
        const RoomPiece& piece = *p.piece;
        Spot best_spot{-1, -1, 0};
        for (int row = clear; row < piece.rows - clear; ++row) {
            for (int col = clear; col < piece.cols - clear; ++col) {
                bool ok = true;
                for (int dr = -clear; dr <= clear && ok; ++dr) {
                    for (int dc = -clear; dc <= clear && ok; ++dc) {
                        ok = piece.cells[static_cast<std::size_t>((row + dr) * piece.cols + col + dc)];
                    }
                }
                if (!ok) continue;
                const Spot s{piece.x0 + p.dx + col * kCell + 8, piece.y0 + p.dy + row * kCell + 8, 0};
                if (best_spot.x < 0 || better(s, best_spot)) best_spot = s;
            }
        }
        if (best_spot.x < 0) {
            const Rect b = p.box();
            best_spot = {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, 0};
        }
        return best_spot;
    };
    auto centre_of = [&](std::size_t k) {
        const Rect b = placed[k].box();
        return Spot{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, 0};
    };
    auto dist2 = [](const Spot& a, const Spot& b) {
        const std::int64_t dx = a.x - b.x, dy = a.y - b.y;
        return dx * dx + dy * dy;
    };
    // The key card: at the back of the key room, as far as can be from its door.
    for (Lock& lock : r.locks) {
        const std::size_t k = static_cast<std::size_t>(lock.key_room);
        const Opening door = placed[k].opening(static_cast<std::size_t>(std::max(placed[k].in, 0)));
        const bool along_x = door.side == kNorth || door.side == kSouth;
        const Spot at_door = along_x ? Spot{door.centre(), door.edge, 0} : Spot{door.edge, door.centre(), 0};
        Spot key = spot(k, 2, [&](const Spot& a, const Spot& b) { return dist2(a, at_door) > dist2(b, at_door); });
        lock.key = {key.x + 16, key.y + 16, 0};  // the card's far corner, on the cell
        LevelItem card = make_item(kShapeKeycard, 0, lock.key.x, lock.key.y, 0);
        card.quality = lock.number;
        level.items.push_back(card);
    }
    // Start and exit: the room's most central clear spots.
    r.start_room = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (steps[k].purpose == Purpose::kExtraction) r.exit_room = static_cast<int>(k);
    }
    const Spot c0 = centre_of(0);
    const Spot s0 = spot(0, 3, [&](const Spot& a, const Spot& b) { return dist2(a, c0) < dist2(b, c0); });
    r.start = {s0.x + 24, s0.y + 24, 0};
    LevelItem arrival = make_item(kShapeTeleportEgg, 1, r.start.x, r.start.y, 0);
    arrival.quality = kStartEgg;
    level.items.push_back(arrival);
    const std::size_t xr = static_cast<std::size_t>(r.exit_room);
    const Spot ce = centre_of(xr);
    const Spot se = spot(xr, 5, [&](const Spot& a, const Spot& b) { return dist2(a, ce) < dist2(b, ce); });
    r.exit = {se.x, se.y, 0};
    level.items.push_back(make_item(kShapeTelepad, 0, r.exit.x + 56, r.exit.y + 56, 0));
    if (options.next_map > 0) {
        LevelItem departure = make_item(kShapeTeleportEgg, 0, r.exit.x, r.exit.y, 0);
        departure.quality = static_cast<std::uint16_t>(options.next_egg | 0x100);
        departure.npc_number = kExitZone;
        departure.map_number = static_cast<std::uint8_t>(options.next_map);
        level.items.push_back(departure);
    }

    // Guards added where the room's own posts fall short of its step: the
    // source map's most common fighter, away from the doorway, facing it.
    int map_fighter = 4;
    {
        std::map<int, int> kinds;
        for (const LevelItem& i : data.maps[static_cast<std::size_t>(options.source_map)].items) {
            if (i.shape == kShapeMonster && i.frame == 1 && fighter(i.npc_number)) ++kinds[i.npc_number];
        }
        int most = 0;
        for (const auto& [type, count] : kinds) {
            if (count > most) most = count, map_fighter = type;
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        int have = static_cast<int>(std::count_if(r.guards.begin(), r.guards.end(),
                                                  [&](const Guard& g) { return g.room == static_cast<int>(k); }));
        if (have >= steps[k].min_guards) continue;
        const Spot in = k == 0 ? r.start : entry(r, static_cast<int>(k));
        std::vector<Spot> taken_spots{in, r.start, {r.exit.x + 56, r.exit.y + 56, 0}};
        for (const Lock& lock : r.locks) taken_spots.push_back(lock.key);
        for (const Guard& g : r.guards) taken_spots.push_back(g.place);
        const Placed& p = placed[k];
        const RoomPiece& piece = *p.piece;
        std::vector<Spot> free;
        for (int row = 3; row < piece.rows - 3; row += 2) {
            for (int col = 3; col < piece.cols - 3; col += 2) {
                bool ok = true;
                for (int dr = -3; dr <= 3 && ok; ++dr) {
                    for (int dc = -3; dc <= 3 && ok; ++dc) {
                        ok = piece.cells[static_cast<std::size_t>((row + dr) * piece.cols + col + dc)];
                    }
                }
                if (!ok) continue;
                const Spot s{piece.x0 + p.dx + col * kCell + 8 + 24, piece.y0 + p.dy + row * kCell + 8 + 24, 0};
                if (dist2(s, in) < 256 * 256) continue;
                if (std::any_of(taken_spots.begin(), taken_spots.end(),
                                [&](const Spot& t) { return dist2(s, t) < 112 * 112; })) {
                    continue;
                }
                free.push_back(s);
            }
        }
        for (; have < steps[k].min_guards && !free.empty() && next_link < 250; ++have) {
            const std::size_t pick = static_cast<std::size_t>(rng.below(static_cast<int>(free.size())));
            Guard g;
            g.room = static_cast<int>(k);
            g.place = free[pick];
            g.npc_type = map_fighter;
            const std::int32_t dx = in.x - g.place.x, dy = in.y - g.place.y;
            const int sx = dx > std::abs(dy) / 2 ? 1 : dx < -std::abs(dy) / 2 ? -1 : 0;
            const int sy = dy > std::abs(dx) / 2 ? 1 : dy < -std::abs(dx) / 2 ? -1 : 0;
            constexpr int kDir[3][3] = {{7, 6, 5}, {0, 0, 4}, {1, 2, 3}};  // [sx + 1][sy + 1]
            g.direction = kDir[sx + 1][sy + 1];
            g.link = static_cast<std::uint8_t>(next_link++);
            auto [post, kind] = make_monster_pair(g.place.x, g.place.y, 0, g.npc_type, g.link);
            MonsterPair m = read_monster_pair(post, kind);
            m.direction = g.direction;
            m.on_enter = true;
            write_monster_pair(post, kind, m);
            level.items.push_back(post);
            level.items.push_back(kind);
            r.guards.push_back(g);
            const Spot placed_at = g.place;
            std::erase_if(free, [&](const Spot& s) { return dist2(s, placed_at) < 112 * 112; });
        }
    }

    // A room's near walls (south, east) with nothing past them show the
    // black frame that the designers drew on a building's near side, not a
    // face to the void (a room whose neighbour on its map stayed behind).
    {
        const std::map<std::uint16_t, std::uint8_t>& black = data.black_frames;
        auto outlined = [&](int gr, int gc) {
            return gr >= 0 && gc >= 0 && gr < kGridCells && gc < kGridCells &&
                   occupied[static_cast<std::size_t>(gr * kGridCells + gc)];
        };
        std::vector<std::uint8_t> bare(level.items.size());  // a post with no black frame, facing the void
        for (std::size_t q = 0; q < level.items.size(); ++q) {
            LevelItem& i = level.items[q];
            if (i.shape >= data.shapes.size() || i.z > 24) continue;  // (some rooms' walls stand on a 16-unit step)
            const auto it = black.find(i.shape);
            if (it != black.end() && i.frame == it->second) continue;
            const ShapeInfo& sh = data.shapes[i.shape];
            int axis = 0, length = 0;
            if (!wall_kind(sh, axis, length)) continue;
            const int wc1 = i.x / kCell, wr1 = i.y / kCell, wc0 = wc1 - sh.footpad_x + 1, wr0 = wr1 - sh.footpad_y + 1;
            auto facing = [&](bool along_x) {
                // The room on its far side (north or west), nothing within
                // 48 units on its near side.
                bool room = false, clear = true;
                for (int t = along_x ? wc0 : wr0; t <= (along_x ? wc1 : wr1); ++t) {
                    room = room || (along_x ? outlined(wr0 - 1, t) : outlined(t, wc0 - 1));
                    for (int k = 1; k <= 3; ++k) clear = clear && !(along_x ? outlined(wr1 + k, t) : outlined(t, wc1 + k));
                }
                return room && clear;
            };
            const bool post = sh.footpad_x == 1 && sh.footpad_y == 1;
            if (!(post ? (facing(true) || facing(false)) : facing(axis == kAlongX))) continue;
            if (it != black.end()) {
                i.frame = it->second;
                continue;
            }
            // A shape with no black frame (a lattice panel, a pillar of the
            // face that the next room saw): the biome's black frame of the
            // same length in its place, else nothing.
            const Piece& front = biome.walls[kFront][post ? kAlongX : axis][length];
            const auto fb = front.valid() ? black.find(front.shape) : black.end();
            if (fb != black.end() && fb->second == front.frame) {
                i.shape = front.shape;
                i.frame = front.frame;
            } else if (post) {
                bare[q] = 1;
            }
        }
        for (std::size_t q = level.items.size(); q-- > 0;) {
            if (bare[q]) level.items.erase(level.items.begin() + static_cast<std::ptrdiff_t>(q));
        }
    }

    // Floor past the rooms (a tile of 256 or 128 units under a room's border
    // runs on where its neighbour was): black tiles (BLACK8, 128 units, laid
    // a unit above the floor, at any 16-unit place) hide it, as the void
    // round a building, without touching a room.
    std::string mask_note;
    {
        constexpr std::uint16_t kShapeBlack8 = 0x003;
        const int span = 128 / kCell;
        std::set<std::pair<int, int>> over;  // floor cells past the rooms
        for (const LevelItem& i : level.items) {
            if (i.shape >= data.shapes.size()) continue;
            const ShapeInfo& sh = data.shapes[i.shape];
            if (!sh.is(ShapeInfo::kSolid) || sh.footpad_z > 0 || i.z > 8 || !sh.footpad_x || !sh.footpad_y) continue;
            const int fc1 = i.x / kCell, fr1 = i.y / kCell;
            for (int fr = fr1 - sh.footpad_y + 1; fr <= fr1; ++fr) {
                for (int fc = fc1 - sh.footpad_x + 1; fc <= fc1; ++fc) {
                    bool near = false;  // a room's outline, or the line of its walls
                    for (int a = -1; a <= 1 && !near; ++a) {
                        for (int e = -1; e <= 1 && !near; ++e) {
                            const int gr = fr + a, gc = fc + e;
                            near = gr >= 0 && gc >= 0 && gr < kGridCells && gc < kGridCells &&
                                   occupied[static_cast<std::size_t>(gr * kGridCells + gc)];
                        }
                    }
                    if (!near) over.insert({fr, fc});
                }
            }
        }
        auto free_square = [&](int sr0, int sc0) {  // no room nor its wall line under it
            for (int sr = sr0 - 1; sr <= sr0 + span; ++sr) {
                for (int sc = sc0 - 1; sc <= sc0 + span; ++sc) {
                    if (sr < 0 || sc < 0 || sr >= kGridCells || sc >= kGridCells) return false;
                    if (occupied[static_cast<std::size_t>(sr * kGridCells + sc)]) return false;
                }
            }
            return true;
        };
        std::set<std::pair<int, int>> hidden;
        int masks = 0;
        for (const auto& [orow, ocol] : over) {
            if (hidden.count({orow, ocol})) continue;
            // A square that holds the cell, the nearest to its top left.
            bool laid = false;
            for (int a = 0; a < span && !laid; ++a) {
                for (int e = 0; e < span && !laid; ++e) {
                    const int br0 = orow - a, bc0 = ocol - e;
                    if (!free_square(br0, bc0)) continue;
                    LevelItem black = make_item(kShapeBlack8, 0, (bc0 + span) * kCell - 1, (br0 + span) * kCell - 1, 1);
                    level.items.push_back(black);
                    for (int rr = br0; rr < br0 + span; ++rr) {
                        for (int cc = bc0; cc < bc0 + span; ++cc) hidden.insert({rr, cc});
                    }
                    laid = true;
                    ++masks;
                }
            }
        }
        char t[96];
        std::snprintf(t, sizeof t, "  floor past the rooms: %zu cells, %d black tiles over it", over.size(), masks);
        mask_note = t;
    }

    // Tidying up what the cutting left: what stands on no floor, and what
    // hangs on no wall (a sign, a poster, a lamp whose wall stayed on the
    // original map).
    int tidied = 0;
    {
        const WalkGrid grid = walk_grid(level.items, data, 0);
        std::vector<std::uint8_t> walls(grid.support.size());
        for (const LevelItem& i : level.items) {
            if (i.shape >= data.shapes.size()) continue;
            const ShapeInfo& s = data.shapes[i.shape];
            int axis = 0, length = 0;
            if (!wall_kind(s, axis, length) && !(s.is(ShapeInfo::kSolid) && s.footpad_z * 8 >= 64)) continue;
            const int c1 = i.x / kCell, r1 = i.y / kCell;
            for (int rr = std::max(0, r1 - s.footpad_y + 1); rr <= r1 && rr < grid.H; ++rr) {
                for (int cc = std::max(0, c1 - s.footpad_x + 1); cc <= c1 && cc < grid.W; ++cc) {
                    walls[static_cast<std::size_t>(rr * grid.W + cc)] = 1;
                }
            }
        }
        auto cell_set = [&](const std::vector<std::uint8_t>& g, int rr, int cc) {
            return rr >= 0 && cc >= 0 && rr < grid.H && cc < grid.W && g[static_cast<std::size_t>(rr * grid.W + cc)];
        };
        std::vector<LevelItem> gone_items;
        std::erase_if(level.items, [&](const LevelItem& i) {
            if (i.shape >= data.shapes.size() || i.shape == kShapeMonster || i.shape == kShapeTriggerEgg ||
                i.shape == kShapeTeleportEgg || i.shape == kShapeKeycard || is_door_shape(i.shape)) {
                return false;
            }
            const ShapeInfo& s = data.shapes[i.shape];
            int axis = 0, length = 0;
            if (is_floor_tile(s) || wall_kind(s, axis, length)) return false;
            // (Flat ones, a decal or a sign, count one cell.)
            const int c1 = i.x / kCell, r1 = i.y / kCell;
            const int c0 = c1 - std::max(1, int{s.footpad_x}) + 1, r0 = r1 - std::max(1, int{s.footpad_y}) + 1;
            bool borne = false, backed = false;
            for (int rr = r0 - 1; rr <= r1 + 1; ++rr) {
                for (int cc = c0 - 1; cc <= c1 + 1; ++cc) {
                    const bool on = rr >= r0 && rr <= r1 && cc >= c0 && cc <= c1;
                    if (on && cell_set(grid.support, rr, cc)) borne = true;
                    if (cell_set(walls, rr, cc)) backed = true;
                }
            }
            const bool thin = s.footpad_x <= 1 || s.footpad_y <= 1;
            const bool hung = i.z >= 16 && thin && s.footpad_z <= 6;
            const bool gone = hung ? !backed : (i.z <= 8 && !borne);
            tidied += gone;
            if (gone) gone_items.push_back(i);
            return gone;
        });
        // And what stood on what went (a crate on a crate left behind), in
        // turn: higher up, a thing stands on another whose top it touches.
        for (bool again = true; again;) {
            again = false;
            auto box = [&](const LevelItem& i, std::int32_t& x0, std::int32_t& y0, std::int32_t& top) {
                const ShapeInfo& s = data.shapes[i.shape];
                x0 = i.x - std::max(1, int{s.footpad_x}) * 16;
                y0 = i.y - std::max(1, int{s.footpad_y}) * 16;
                top = i.z + s.footpad_z * 8;
            };
            std::vector<std::uint8_t> drop(level.items.size());
            for (std::size_t k = 0; k < level.items.size(); ++k) {
                const LevelItem& i = level.items[k];
                if (i.shape >= data.shapes.size() || i.z <= 8 || i.shape == kShapeMonster ||
                    i.shape == kShapeTriggerEgg || i.shape == kShapeTeleportEgg || i.shape == kShapeKeycard ||
                    is_door_shape(i.shape)) {
                    continue;
                }
                const ShapeInfo& s = data.shapes[i.shape];
                int axis = 0, length = 0;
                const bool thin = s.footpad_x <= 1 || s.footpad_y <= 1;
                if (is_floor_tile(s) || wall_kind(s, axis, length) || (thin && s.footpad_z <= 6) ||
                    !s.is(ShapeInfo::kSolid)) {
                    continue;
                }
                std::int32_t x0, y0, top;
                box(i, x0, y0, top);
                auto on = [&](const LevelItem& o) {
                    if (&o == &i || o.shape >= data.shapes.size()) return false;
                    std::int32_t ox0, oy0, otop;
                    box(o, ox0, oy0, otop);
                    return otop >= i.z - 2 && otop <= i.z + 2 && ox0 < i.x && x0 < o.x && oy0 < i.y && y0 < o.y;
                };
                // Only what stood on something taken out (a sink on its
                // wall, a lamp, stand on nothing by design).
                if (std::none_of(gone_items.begin(), gone_items.end(), on)) continue;
                if (std::any_of(level.items.begin(), level.items.end(), on)) continue;
                drop[k] = 1;
            }
            for (std::size_t k = level.items.size(); k-- > 0;) {
                if (!drop[k]) continue;
                gone_items.push_back(level.items[k]);
                level.items.erase(level.items.begin() + static_cast<std::ptrdiff_t>(k));
                ++tidied;
                again = true;
            }
        }
    }

    char note[320];
    std::string sources;
    for (int m : look_alike_maps(data, options.source_map)) sources += (sources.empty() ? "" : " ") + std::to_string(m);
    std::snprintf(note, sizeof note,
                  "mission%s%s (seed %u) from map %d (rooms of maps %s): %zu rooms, %zu locks, %zu items (%d left behind: "
                  "on no floor, on no wall)",
                  options.plan.empty() ? "" : " ", options.plan.c_str(), options.seed, options.source_map, sources.c_str(), r.rooms.size(), r.locks.size(),
                  level.record_count(), tidied);
    r.notes.push_back(note);
    r.notes.push_back(mask_note);
    for (std::size_t k = 0; k < n; ++k) {
        int guards = 0;
        for (const LevelItem& i : level.items) {
            const Rect b = placed[k].box();
            if (i.shape == kShapeMonster && i.frame == 0 && i.x >= b.x0 && i.x < b.x1 && i.y >= b.y0 && i.y < b.y1) ++guards;
        }
        const Rect b = placed[k].box();
        std::snprintf(note, sizeof note, "  %d %-22s at (%d..%d, %d..%d): %s, %d guard(s)%s", static_cast<int>(k),
                      purpose_name(steps[k].purpose), b.x0, b.x1, b.y0, b.y1, placed[k].piece->describe().c_str(),
                      guards, steps[k].locked ? ", behind a card door" : "");
        r.notes.push_back(note);
    }
    return r;
}

}  // namespace

}  // namespace crusader::editor::gen
