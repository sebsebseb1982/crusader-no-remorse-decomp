#include "editor/generator/pieces.hpp"

#include "editor/generator/biome.hpp"
#include "editor/generator/campaign.hpp"
#include "editor/item_semantics.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace crusader::editor::gen {

namespace {

constexpr std::int32_t kCell = 16;
constexpr int kErode = 4;            // cells: passages narrower than 128 units part rooms
// Floor tiles (256 units) and their quarters (128) of the same look.
struct FloorQuarter {
    std::uint16_t whole, part;
};
constexpr FloorQuarter kFloorQuarters[] = {{0x014, 0x016}, {0x0CC, 0x0CE}};
constexpr std::int32_t kHead = 80;   // what stands lower than this over a cell blocks it (the player is 80 high)
constexpr std::int32_t kStep = 9;    // a floor this far from the storey bears the player
constexpr std::uint8_t kFamilyGlobEgg = 3;

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

// The kind of room an object suggests, by its class name (-1: none).
int tag_of(const std::string& name) {
    for (const char* p : {"MCRATE", "BOX_", "BBOX", "BARREL", "LUGGAGE", "CHEST", "CRATE", "GBOX", "SSBOX"}) {
        if (starts(name, p)) return kStorage;
    }
    for (const char* p : {"SMALCHAI", "MONIT", "BIGCOM", "MDESK", "DESK", "COPY_", "CHAIR", "PEPSI", "JOHN", "SIGN"}) {
        if (starts(name, p)) return kOffice;
    }
    for (const char* p : {"TRANSFOR", "GENERATR", "WSHOCK", "SHOCK", "CONV", "RECH", "STEAM", "VALVE", "HOVER"}) {
        if (starts(name, p)) return kMachines;
    }
    for (const char* p : {"CAMERA", "SURCAM", "ALARM", "MALRM", "GATGUN", "WALGUN", "BASEGUN", "LAZER", "FLORMINE"}) {
        if (starts(name, p)) return kSecurity;
    }
    return -1;
}

}  // namespace

const char* tag_name(int tag) {
    switch (tag) {
        case kStorage: return "storage";
        case kOffice: return "office";
        case kMachines: return "machines";
        case kSecurity: return "security";
    }
    return "?";
}

bool RoomPiece::cell(std::int32_t x, std::int32_t y) const {
    const std::int32_t c = (x - x0) / kCell, r = (y - y0) / kCell;
    return x >= x0 && y >= y0 && c < cols && r < rows && cells[static_cast<std::size_t>(r * cols + c)];
}

std::string RoomPiece::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "map %d (%d..%d, %d..%d, z %d) %d x %d, %zu openings, %d NPCs, %zu items", map, x0,
                  x1, y0, y1, z, x1 - x0, y1 - y0, openings.size(), npcs, items.size());
    std::string s = buf;
    s += ", open border " + std::to_string(border ? 100 * open_border / border : 0) + "% (railings " +
         std::to_string(border ? 100 * low_border / border : 0) + "%), furniture " +
         std::to_string(furniture);
    for (int t = 0; t < kTagCount; ++t) {
        if (tags[t]) s += std::string(", ") + tag_name(t) + " " + std::to_string(tags[t]);
    }
    if (overhang) s += ", floor past its walls " + std::to_string(overhang);
    if (unfloored) s += ", cells with no floor " + std::to_string(unfloored);
    if (squared) s += ", squared " + std::to_string(squared) + "%";
    return s;
}

std::vector<LevelItem> flatten(const Level& level, const GameData& data) {
    std::vector<LevelItem> out;
    for (const LevelItem& item : level.items) {
        const bool glob_egg = item.shape < data.shapes.size() && data.shapes[item.shape].family == kFamilyGlobEgg;
        if (!glob_egg) {
            out.push_back(item);
            continue;
        }
        if (item.quality == 0 || item.quality >= data.globs.size()) continue;
        for (const GlobPiece& p : data.globs[item.quality].pieces) {
            const Point3 at = glob_piece_position(item, p);
            LevelItem piece;
            piece.x = at.x;
            piece.y = at.y;
            piece.z = at.z;
            piece.shape = p.shape;
            piece.frame = p.frame;
            out.push_back(piece);
        }
    }
    return out;
}

WalkGrid walk_grid(const std::vector<LevelItem>& items, const GameData& data, std::int32_t z0) {
    auto info = [&](std::uint16_t s) -> const ShapeInfo* { return s < data.shapes.size() ? &data.shapes[s] : nullptr; };
    auto name = [&](std::uint16_t s) { return s < data.class_names.size() ? data.class_names[s] : std::string(); };
    // The grid, over the items' extent.
    std::int32_t max_x = 0, max_y = 0;
    for (const LevelItem& i : items) max_x = std::max(max_x, i.x), max_y = std::max(max_y, i.y);
    const int W = max_x / kCell + 2, H = max_y / kCell + 2;
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<std::uint8_t> support(N), block(N), door(N);
    // (A flat thing standing up, a sign or a fake door, holds the cell it
    // stands on.)
    auto fill = [&](std::vector<std::uint8_t>& g, const LevelItem& i, const ShapeInfo& s) {
        const int c1 = i.x / kCell, r1 = i.y / kCell;
        const int c0 = std::max(0, c1 - std::max(1, int{s.footpad_x}) + 1), r0 = std::max(0, r1 - std::max(1, int{s.footpad_y}) + 1);
        for (int r = r0; r <= r1 && r < H; ++r) {
            for (int c = c0; c <= c1 && c < W; ++c) g[static_cast<std::size_t>(r * W + c)] = 1;
        }
    };
    for (const LevelItem& i : items) {
        const ShapeInfo* s = info(i.shape);
        if (!s || (!s->footpad_x && !s->footpad_y)) continue;
        const std::int32_t top = i.z + s->footpad_z * 8;
        if (is_door_shape(i.shape)) {
            if (i.z >= z0 - 8 && i.z <= z0 + 8) fill(door, i, *s);
            continue;
        }
        if (!s->is(ShapeInfo::kSolid)) continue;
        if (top >= z0 - kStep && top <= z0 + kStep && s->footpad_z <= 2 && !starts(name(i.shape), "BLACK") &&
            !(i.shape < data.scripted.size() && data.scripted[i.shape] && starts(name(i.shape), "DEATH"))) {
            fill(support, i, *s);
        }
        if (i.z < z0 + kHead && top > z0 + kStep) fill(block, i, *s);
    }
    WalkGrid g;
    g.W = W, g.H = H;
    g.walk.assign(N, 0);
    for (std::size_t k = 0; k < N; ++k) g.walk[k] = support[k] && !block[k] && !door[k];
    g.door = std::move(door);
    g.support = std::move(support);
    return g;

}

bool WalkGrid::walkable(std::int32_t x, std::int32_t y) const {
    const std::int32_t c = x / kCell, r = y / kCell;
    return x >= 0 && y >= 0 && c < W && r < H && walk[static_cast<std::size_t>(r * W + c)];
}

std::vector<RoomPiece> extract_rooms(const GameData& data, int map, std::int32_t z0, std::int32_t min_side,
                                     std::int32_t max_side) {
    std::vector<RoomPiece> rooms;
    if (map < 0 || static_cast<std::size_t>(map) >= data.maps.size()) return rooms;
    const std::vector<LevelItem> items = flatten(data.maps[static_cast<std::size_t>(map)], data);
    if (items.empty()) return rooms;
    auto info = [&](std::uint16_t s) -> const ShapeInfo* { return s < data.shapes.size() ? &data.shapes[s] : nullptr; };
    auto name = [&](std::uint16_t s) { return s < data.class_names.size() ? data.class_names[s] : std::string(); };

    const WalkGrid grid = walk_grid(items, data, z0);
    const int W = grid.W, H = grid.H;
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    const std::vector<std::uint8_t>& walk = grid.walk;
    const std::vector<std::uint8_t>& door = grid.door;

    // The cells that a floor tile (256 units) bears: a flat piece over one
    // is laid on the floor (a hazard stripe, a hatch); elsewhere it is the
    // floor itself (a smaller piece of floor).
    std::vector<std::uint8_t> tiled(N);
    for (const LevelItem& i : items) {
        const ShapeInfo* s = info(i.shape);
        if (!s || !is_floor_tile(*s) || i.z < z0 - kStep || i.z > z0 + kStep) continue;
        const int c1 = i.x / kCell, r1 = i.y / kCell;
        for (int r = std::max(0, r1 - 15); r <= r1 && r < H; ++r) {
            for (int c = std::max(0, c1 - 15); c <= c1 && c < W; ++c) tiled[static_cast<std::size_t>(r * W + c)] = 1;
        }
    }

    // The storey's walls (and doors): the inside of a room stops at them.
    std::vector<std::uint8_t> walls(N);
    for (const LevelItem& i : items) {
        const ShapeInfo* s = info(i.shape);
        if (!s || !s->footpad_x || !s->footpad_y || i.z >= z0 + kHead || i.z + s->footpad_z * 8 <= z0 + kStep) continue;
        int axis = 0, length = 0;
        if (!wall_kind(*s, axis, length) && !is_door_shape(i.shape)) continue;
        const int c1 = i.x / kCell, r1 = i.y / kCell;
        for (int r = std::max(0, r1 - s->footpad_y + 1); r <= r1 && r < H; ++r) {
            for (int c = std::max(0, c1 - s->footpad_x + 1); c <= c1 && c < W; ++c) walls[static_cast<std::size_t>(r * W + c)] = 1;
        }
    }

    // Erosion by a square of kErode cells (summed-area table of the cells
    // that are not walkable).
    std::vector<int> sat(static_cast<std::size_t>(W + 1) * static_cast<std::size_t>(H + 1));
    auto at = [&](int r, int c) -> int& { return sat[static_cast<std::size_t>(r * (W + 1) + c)]; };
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            at(r + 1, c + 1) = !walk[static_cast<std::size_t>(r * W + c)] + at(r, c + 1) + at(r + 1, c) - at(r, c);
        }
    }
    std::vector<int> label(N);
    std::vector<std::uint8_t> core(N);
    for (int r = kErode; r < H - kErode; ++r) {
        for (int c = kErode; c < W - kErode; ++c) {
            const int r0 = r - kErode, c0 = c - kErode, r1 = r + kErode + 1, c1 = c + kErode + 1;
            core[static_cast<std::size_t>(r * W + c)] = at(r1, c1) - at(r0, c1) - at(r1, c0) + at(r0, c0) == 0;
        }
    }
    // Components of the eroded cells, then every walkable cell to the
    // nearest one (a breadth-first search from all of them at once).
    int count = 0;
    std::deque<std::size_t> queue;
    const int dr[4] = {-1, 0, 1, 0}, dc[4] = {0, 1, 0, -1};  // N E S W
    for (std::size_t k = 0; k < N; ++k) {
        if (!core[k] || label[k]) continue;
        label[k] = ++count;
        std::deque<std::size_t> q{k};
        while (!q.empty()) {
            const std::size_t p = q.front();
            q.pop_front();
            queue.push_back(p);
            const int r = static_cast<int>(p / W), c = static_cast<int>(p % W);
            for (int d = 0; d < 4; ++d) {
                const int nr = r + dr[d], nc = c + dc[d];
                if (nr < 0 || nc < 0 || nr >= H || nc >= W) continue;
                const std::size_t n = static_cast<std::size_t>(nr * W + nc);
                if (core[n] && !label[n]) label[n] = count, q.push_back(n);
            }
        }
    }
    while (!queue.empty()) {
        const std::size_t p = queue.front();
        queue.pop_front();
        const int r = static_cast<int>(p / W), c = static_cast<int>(p % W);
        for (int d = 0; d < 4; ++d) {
            const int nr = r + dr[d], nc = c + dc[d];
            if (nr < 0 || nc < 0 || nr >= H || nc >= W) continue;
            const std::size_t n = static_cast<std::size_t>(nr * W + nc);
            if (walk[n] && !label[n]) label[n] = label[p], queue.push_back(n);
        }
    }

    // Each room.
    struct Box {
        int c0 = std::numeric_limits<int>::max(), r0 = std::numeric_limits<int>::max(), c1 = -1, r1 = -1, area = 0;
    };
    std::vector<Box> boxes(static_cast<std::size_t>(count) + 1);
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            const int l = label[static_cast<std::size_t>(r * W + c)];
            if (!l) continue;
            Box& b = boxes[static_cast<std::size_t>(l)];
            b.c0 = std::min(b.c0, c), b.r0 = std::min(b.r0, r), b.c1 = std::max(b.c1, c), b.r1 = std::max(b.r1, r);
            ++b.area;
        }
    }
    for (int l = 1; l <= count; ++l) {
        const Box& b = boxes[static_cast<std::size_t>(l)];
        const std::int32_t w = (b.c1 - b.c0 + 1) * kCell, h = (b.r1 - b.r0 + 1) * kCell;
        if (w < min_side || h < min_side || w > max_side || h > max_side) continue;
        RoomPiece room;
        room.map = map;
        room.z = z0;
        room.x0 = b.c0 * kCell, room.y0 = b.r0 * kCell;
        room.x1 = (b.c1 + 1) * kCell, room.y1 = (b.r1 + 1) * kCell;
        room.cols = b.c1 - b.c0 + 1, room.rows = b.r1 - b.r0 + 1;
        room.cells.assign(static_cast<std::size_t>(room.cols * room.rows), 0);
        room.area = b.area;
        for (int r = b.r0; r <= b.r1; ++r) {
            for (int c = b.c0; c <= b.c1; ++c) {
                if (label[static_cast<std::size_t>(r * W + c)] == l) {
                    room.cells[static_cast<std::size_t>((r - b.r0) * room.cols + (c - b.c0))] = 1;
                }
            }
        }
        // Openings: border runs where the next cell out belongs to another
        // room or is a door; grouped by side and border line.
        auto in_room = [&](int r, int c) {
            return r >= 0 && c >= 0 && r < H && c < W && label[static_cast<std::size_t>(r * W + c)] == l;
        };
        for (int d = 0; d < 4; ++d) {
            // Cells of the border on that side, ordered along it.
            struct Run {
                int line, from, to;
                bool door;
            };
            std::vector<Run> runs;
            const bool along_x = d == kNorth || d == kSouth;
            for (int a = (along_x ? b.r0 : b.c0); a <= (along_x ? b.r1 : b.c1); ++a) {
                for (int t = (along_x ? b.c0 : b.r0); t <= (along_x ? b.c1 : b.r1); ++t) {
                    const int r = along_x ? a : t, c = along_x ? t : a;
                    if (!in_room(r, c)) continue;
                    const int nr = r + dr[d], nc = c + dc[d];
                    if (nr < 0 || nc < 0 || nr >= H || nc >= W) continue;
                    const std::size_t n = static_cast<std::size_t>(nr * W + nc);
                    if (label[n] == l) continue;  // not the border
                    // A door: the cells past it, through its thickness.
                    bool through_door = false;
                    for (int k = 0; k < 3 && !through_door; ++k) {
                        const int kr = r + dr[d] * (k + 1), kc = c + dc[d] * (k + 1);
                        if (kr >= 0 && kc >= 0 && kr < H && kc < W) through_door = door[static_cast<std::size_t>(kr * W + kc)];
                    }
                    const bool out = (label[n] && label[n] != l) || through_door;
                    if (!out) continue;
                    if (!runs.empty() && runs.back().line == a && runs.back().to == t) {
                        runs.back().to = t + 1;
                        runs.back().door = runs.back().door || through_door;
                    } else {
                        runs.push_back({a, t, t + 1, through_door});
                    }
                }
            }
            for (const Run& run : runs) {
                if ((run.to - run.from) * kCell < 80) continue;  // furniture gaps, not passages
                Opening o;
                o.side = d;
                o.from = run.from * kCell;
                o.to = run.to * kCell;
                o.edge = (d == kNorth || d == kWest) ? run.line * kCell : (run.line + 1) * kCell;
                o.door = run.door;
                room.openings.push_back(o);
            }
        }
        // How closed the room stands on its map.
        {
            const int c0b = b.c0 - 6, r0b = b.r0 - 6, cw = b.c1 - b.c0 + 13, rh = b.r1 - b.r0 + 13;
            std::vector<std::uint8_t> tall(static_cast<std::size_t>(cw * rh)), low(tall.size());
            for (const LevelItem& i : items) {
                const ShapeInfo* s = info(i.shape);
                if (!s || !s->is(ShapeInfo::kSolid) || i.z >= z0 + kHead || i.z + s->footpad_z * 8 < z0 + 16) continue;
                if (i.z + s->footpad_z * 8 < z0 + 64) {
                    const int lc1 = i.x / kCell - c0b, lr1 = i.y / kCell - r0b;
                    for (int r = std::max(0, lr1 - s->footpad_y + 1); r <= std::min(rh - 1, lr1); ++r) {
                        for (int c = std::max(0, lc1 - s->footpad_x + 1); c <= std::min(cw - 1, lc1); ++c) {
                            low[static_cast<std::size_t>(r * cw + c)] = 1;
                        }
                    }
                    continue;
                }
                const int ic1 = i.x / kCell - c0b, ir1 = i.y / kCell - r0b;
                for (int r = std::max(0, ir1 - s->footpad_y + 1); r <= std::min(rh - 1, ir1); ++r) {
                    for (int c = std::max(0, ic1 - s->footpad_x + 1); c <= std::min(cw - 1, ic1); ++c) {
                        tall[static_cast<std::size_t>(r * cw + c)] = 1;
                    }
                }
            }
            for (int r = b.r0; r <= b.r1; ++r) {
                for (int c = b.c0; c <= b.c1; ++c) {
                    if (!in_room(r, c)) continue;
                    for (int d = 0; d < 4; ++d) {
                        if (in_room(r + dr[d], c + dc[d])) continue;
                        // Outwards, up to 96 units: a wall, a door or another
                        // room closes it; the room again: furniture within it.
                        int verdict = 0;  // 1 closed, 2 within, 0 open
                        bool railing = false;
                        for (int k = 1; k <= 6 && !verdict; ++k) {
                            const int nr = r + dr[d] * k, nc = c + dc[d] * k;
                            if (nr < 0 || nc < 0 || nr >= H || nc >= W) break;
                            const std::size_t n = static_cast<std::size_t>(nr * W + nc);
                            const bool in_box = nr >= r0b && nc >= c0b && nr < r0b + rh && nc < c0b + cw;
                            if (in_box && low[static_cast<std::size_t>((nr - r0b) * cw + (nc - c0b))]) railing = true;
                            if (label[n] == l) verdict = 2;
                            // Another room right there, nothing between:
                            // one space that the cutting parted, open.
                            else if (label[n] && k == 1) break;
                            else if (label[n] || door[n] ||
                                     (in_box && tall[static_cast<std::size_t>((nr - r0b) * cw + (nc - c0b))])) {
                                verdict = 1;
                            }
                        }
                        if (verdict == 2) continue;
                        ++room.border;
                        if (verdict == 0) ++room.open_border;
                        if (verdict == 0 && railing) ++room.low_border;
                    }
                }
            }
        }
        // The room's inside: its cells, and the cells of its box that its
        // walls enclose with them (under its furniture: a stack of crates,
        // a row of barrels, deeper than a cell from where the player walks),
        // not another room's.
        auto in_box = [&](int r, int c) { return r >= b.r0 && c >= b.c0 && r <= b.r1 && c <= b.c1; };
        auto inside_at = [&](int r, int c) -> std::uint8_t& {
            return room.cells[static_cast<std::size_t>((r - b.r0) * room.cols + (c - b.c0))];
        };
        std::vector<std::uint8_t> enclosed(room.cells.size());
        {
            std::deque<std::pair<int, int>> q;
            for (int r = b.r0; r <= b.r1; ++r) {
                for (int c = b.c0; c <= b.c1; ++c) {
                    if (inside_at(r, c)) q.push_back({r, c});
                }
            }
            while (!q.empty()) {
                const auto [r, c] = q.front();
                q.pop_front();
                for (int d = 0; d < 4; ++d) {
                    const int nr = r + dr[d], nc = c + dc[d];
                    if (!in_box(nr, nc)) continue;
                    const std::size_t n = static_cast<std::size_t>(nr * W + nc);
                    std::uint8_t& e = enclosed[static_cast<std::size_t>((nr - b.r0) * room.cols + (nc - b.c0))];
                    if (e || inside_at(nr, nc) || label[n] || walls[n] || door[n] || !grid.support[n]) continue;
                    e = 1;
                    q.push_back({nr, nc});
                }
            }
        }
        auto within = [&](int r, int c) {
            if (!in_box(r, c)) return false;
            const std::size_t k = static_cast<std::size_t>((r - b.r0) * room.cols + (c - b.c0));
            return room.cells[k] || enclosed[k];
        };
        // The outline: the inside, dilated then eroded by kClose cells.
        {
            constexpr int kClose = 3;
            const int mc = room.cols + 2 * kClose, mr = room.rows + 2 * kClose;
            std::vector<std::uint8_t> grown(static_cast<std::size_t>(mc * mr));
            for (int r = 0; r < mr; ++r) {
                for (int c = 0; c < mc; ++c) {
                    bool any = false;
                    for (int a = -kClose; a <= kClose && !any; ++a) {
                        for (int e = -kClose; e <= kClose && !any; ++e) any = within(b.r0 + r - kClose + a, b.c0 + c - kClose + e);
                    }
                    grown[static_cast<std::size_t>(r * mc + c)] = any;
                }
            }
            room.outline.assign(room.cells.size(), 0);
            for (int r = 0; r < room.rows; ++r) {
                for (int c = 0; c < room.cols; ++c) {
                    bool all = true;
                    for (int a = -kClose; a <= kClose && all; ++a) {
                        for (int e = -kClose; e <= kClose && all; ++e) {
                            const int r2 = r + kClose + a, c2 = c + kClose + e;
                            all = r2 >= 0 && c2 >= 0 && r2 < mr && c2 < mc && grown[static_cast<std::size_t>(r2 * mc + c2)];
                        }
                    }
                    const std::size_t k = static_cast<std::size_t>(r * room.cols + c);
                    room.outline[k] = all || room.cells[k] || enclosed[k];
                }
            }
        }
        // Square: a rectangle, or an L (the rectangle less its largest empty
        // corner), never a run of steps (where the cutting parted an open
        // space on a slant). The cells it adds are bare floor of the room.
        {
            const int R = room.rows, C = room.cols;
            auto marked = [&](int r, int c) { return room.outline[static_cast<std::size_t>(r * C + c)] != 0; };
            int best = 0, best_corner = -1, best_h = 0, best_w = 0;
            for (int corner = 0; corner < 4; ++corner) {
                const bool top = corner < 2, left = corner % 2 == 0;
                int width = C;  // the empty run from the corner, over the rows so far
                for (int height = 1; height <= R && width > 0; ++height) {
                    const int r = top ? height - 1 : R - height;
                    int run = 0;
                    while (run < width && !marked(r, left ? run : C - 1 - run)) ++run;
                    width = run;
                    if (width * height > best) best = width * height, best_corner = corner, best_h = height, best_w = width;
                }
            }
            // A notch of 64 x 64 units at least makes an L; a smaller one is filled.
            const bool notch = best_corner >= 0 && best_h >= 4 && best_w >= 4;
            int added = 0, total = 0;
            for (int r = 0; r < R; ++r) {
                for (int c = 0; c < C; ++c) {
                    bool in = true;
                    if (notch) {
                        const bool top = best_corner < 2, left = best_corner % 2 == 0;
                        const bool rows = top ? r < best_h : r >= R - best_h;
                        const bool cols = left ? c < best_w : c >= C - best_w;
                        in = !(rows && cols);
                    }
                    std::uint8_t& o = room.outline[static_cast<std::size_t>(r * C + c)];
                    total += in;
                    added += in && !o;
                    o = in;
                }
            }
            room.squared = total ? 100 * added / total : 0;
        }
        auto outlined = [&](int r, int c) {
            return in_box(r, c) && room.outline[static_cast<std::size_t>((r - b.r0) * room.cols + (c - b.c0))];
        };
        // Each item of the storey near the room, and whether it goes with it.
        struct Candidate {
            std::size_t item = 0;
            bool take = false;      // by its own footprint
            bool furniture = false; // a thing standing in the room, of a heap or a row
            bool close = false;     // all of its footprint within a cell of the room's inside
            std::int32_t x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
        };
        std::vector<Candidate> cand;
        auto borders = [&](int r, int c) {
            for (int a = -1; a <= 1; ++a) {
                for (int e = -1; e <= 1; ++e) {
                    if (within(r + a, c + e)) return true;
                }
            }
            return false;
        };
        for (std::size_t k = 0; k < items.size(); ++k) {
            const LevelItem& i = items[k];
            const ShapeInfo* s = info(i.shape);
            // The storey's items, and what bears its floor from lower down.
            const bool support = s && s->is(ShapeInfo::kSolid) && s->footpad_z <= 2 &&
                                 i.z + s->footpad_z * 8 >= z0 - kStep && i.z + s->footpad_z * 8 <= z0 + kStep;
            if ((i.z < z0 - 8 && !support) || i.z >= z0 + 96) continue;
            const int c1 = i.x / kCell, r1 = i.y / kCell;
            const int c0 = c1 - std::max(1, s ? int{s->footpad_x} : 1) + 1;
            const int r0 = r1 - std::max(1, s ? int{s->footpad_y} : 1) + 1;
            if (c1 < b.c0 - 2 || r1 < b.r0 - 2 || c0 > b.c1 + 2 || r0 > b.r1 + 2) continue;
            // By footprint, against the room's inside: a floor goes with
            // every room it bears (tiles are 256 units, often held by a
            // neighbour), and what stands within the room; a wall, with the
            // room it borders for half its length at least (not one that
            // runs on past the room's corner, over the void or another
            // room); what hangs on a wall (thin), with the room it borders.
            // Not what stands past an opening, in the next room.
            int axis = 0, length = 0;
            const bool wall = s && (wall_kind(*s, axis, length) || is_door_shape(i.shape));
            const bool thin = s && (s->footpad_x <= 1 || s->footpad_y <= 1);
            // What hangs on a wall shows on its face that the camera sees
            // (east or south): it goes with the room on that side, not with
            // the one behind the wall (a fake lift door of the next room).
            auto faces = [&](int r, int c) {
                if (!s) return false;
                if (s->footpad_x <= 1 && s->footpad_y > 1) return within(r, c + 1);
                if (s->footpad_y <= 1 && s->footpad_x > 1) return within(r + 1, c);
                return within(r, c + 1) || within(r + 1, c) || within(r + 1, c + 1);
            };
            Candidate cd;
            cd.item = k;
            int near = 0, cells = 0;
            for (int r = r0; r <= r1; ++r) {
                for (int c = c0; c <= c1; ++c) {
                    ++cells, near += borders(r, c);
                    if (support ? outlined(r, c) : within(r, c) || (!wall && thin && faces(r, c))) cd.take = true;
                }
            }
            if (wall) cd.take = near * 2 >= cells;
            cd.close = near == cells;
            // A flat piece that is no floor tile (a hazard stripe, a hatch, a
            // grating) lies within the room or stays: past its wall, it
            // would hang over the void.
            if (support && !is_floor_tile(*s)) {
                bool laid = true;  // on a floor tile all over
                for (int r = r0; r <= r1 && laid; ++r) {
                    for (int c = c0; c <= c1 && laid; ++c) {
                        laid = r >= 0 && c >= 0 && r < H && c < W && tiled[static_cast<std::size_t>(r * W + c)];
                    }
                }
                if (laid) cd.take = cd.close && cd.take;
            }
            cd.furniture = s && !wall && !support && s->footpad_z > 0 && i.shape != kShapeMonster &&
                           i.shape != kShapeTriggerEgg && i.shape != kShapeTeleportEgg && i.shape != kShapeMonsterEgg;
            if (s) {
                cd.x0 = i.x - s->footpad_x * 16, cd.x1 = i.x, cd.y0 = i.y - s->footpad_y * 16, cd.y1 = i.y;
                cd.z0 = i.z, cd.z1 = i.z + s->footpad_z * 8;
            }
            cand.push_back(cd);
        }
        // Heaps and rows (things that touch: crates stacked, barrels side by
        // side) go whole or stay whole: with the room when it holds them
        // (all within a cell of its inside), else left behind, never cut.
        {
            std::vector<std::size_t> parent(cand.size());
            for (std::size_t a = 0; a < cand.size(); ++a) parent[a] = a;
            std::function<std::size_t(std::size_t)> root = [&](std::size_t a) {
                return parent[a] == a ? a : parent[a] = root(parent[a]);
            };
            for (std::size_t a = 0; a < cand.size(); ++a) {
                if (!cand[a].furniture) continue;
                for (std::size_t e = a + 1; e < cand.size(); ++e) {
                    if (!cand[e].furniture) continue;
                    const Candidate &p = cand[a], &q = cand[e];
                    if (p.x0 <= q.x1 && q.x0 <= p.x1 && p.y0 <= q.y1 && q.y0 <= p.y1 && p.z0 <= q.z1 && q.z0 <= p.z1) {
                        parent[root(a)] = root(e);
                    }
                }
            }
            std::map<std::size_t, std::pair<bool, bool>> group;  // root -> (one taken, all close)
            for (std::size_t a = 0; a < cand.size(); ++a) {
                if (!cand[a].furniture) continue;
                auto [it, fresh] = group.try_emplace(root(a), false, true);
                it->second.first |= cand[a].take;
                it->second.second &= cand[a].close;
            }
            for (std::size_t a = 0; a < cand.size(); ++a) {
                if (!cand[a].furniture) continue;
                const auto& [taken, close] = group[root(a)];
                cand[a].take = taken && close;
            }
        }
        // The floor under what goes with the room goes too.
        for (Candidate& f : cand) {
            if (f.take || f.furniture || f.z1 > z0 + kStep || f.z1 < z0 - kStep) continue;
            const ShapeInfo* s = info(items[f.item].shape);
            if (!s || !s->is(ShapeInfo::kSolid) || s->footpad_z > 2) continue;
            for (const Candidate& o : cand) {
                if (o.take && o.furniture && o.x0 < f.x1 && f.x0 < o.x1 && o.y0 < f.y1 && f.y0 < o.y1 &&
                    o.z0 <= z0 + kStep) {
                    f.take = true;
                    break;
                }
            }
        }
        for (const Candidate& cd : cand) {
            if (!cd.take) continue;
            const LevelItem& i = items[cd.item];
            const ShapeInfo* s = info(i.shape);
            // A floor tile that runs past the room (where its neighbour was)
            // would hang over the void: when the tile has a quarter of the
            // same look (concrete 0x14 / 0x16, green tiles 0xCC / 0xCE), it
            // is laid again as its quarters within the room.
            if (s && is_floor_tile(*s)) {
                std::uint16_t quarter = 0;
                for (const auto& [whole, part] : kFloorQuarters) {
                    if (i.shape == whole) quarter = part;
                }
                const int c1 = i.x / kCell, r1 = i.y / kCell;
                bool past = false;
                for (int r = r1 - 15; r <= r1 && !past; ++r) {
                    for (int c = c1 - 15; c <= c1 && !past; ++c) past = !borders(r, c);
                }
                if (quarter && past) {
                    for (int a = 0; a < 2; ++a) {
                        for (int e = 0; e < 2; ++e) {
                            bool inner = false;
                            for (int r = r1 - 8 * e - 7; r <= r1 - 8 * e && !inner; ++r) {
                                for (int c = c1 - 8 * a - 7; c <= c1 - 8 * a && !inner; ++c) inner = outlined(r, c);
                            }
                            if (!inner) continue;
                            LevelItem q = i;
                            q.shape = quarter;
                            q.x = i.x - 128 * a;
                            q.y = i.y - 128 * e;
                            room.items.push_back(q);
                        }
                    }
                    continue;
                }
            }
            room.items.push_back(i);
            if (i.shape == kShapeMonster && i.frame == 0) ++room.npcs;
            // Furniture: what stands in the room (not its walls, nor what
            // hangs on them: posters, signs, fake doors).
            const bool thin = s && (s->footpad_x <= 1 || s->footpad_y <= 1);
            const bool hung = thin && i.z >= z0 + 16 && s->footpad_z <= 6;
            if (cd.furniture && !hung && i.z >= z0 - 8) ++room.furniture;
            const int t = tag_of(name(i.shape));
            if (t >= 0) ++room.tags[static_cast<std::size_t>(t)];
        }
        // Doors that can be cut: a straight wall piece, 128 or 256 units long,
        // right along the room's border, with six cells (96 units) of the
        // room behind it all along: the way in stays clear of furniture. A mission that
        // takes one puts its own door there.
        for (const LevelItem& i : room.items) {
            const ShapeInfo* s = info(i.shape);
            int axis = 0, length = 0;
            if (!s || !wall_kind(*s, axis, length) || i.z < z0 - 8 || i.z > z0 + 8) continue;
            const int units = axis == kAlongX ? s->footpad_x : s->footpad_y;
            if (units != 8 && units != 16) continue;
            const int c1 = i.x / kCell, r1 = i.y / kCell;
            const int c0 = c1 - s->footpad_x + 1, r0 = r1 - s->footpad_y + 1;
            for (int d = 0; d < 4; ++d) {
                const bool along_x = d == kNorth || d == kSouth;
                if (along_x != (axis == kAlongX)) continue;
                bool clear = true;
                for (int t = along_x ? c0 : r0; t <= (along_x ? c1 : r1) && clear; ++t) {
                    for (int k = 1; k <= 6 && clear; ++k) {
                        // From the wall's cell, k cells into the room.
                        const int r = along_x ? r1 - dr[d] * k : t, c = along_x ? t : c1 - dc[d] * k;
                        clear = in_room(r, c);
                    }
                }
                if (!clear) continue;
                Opening o;
                o.side = d;
                o.from = (along_x ? c0 : r0) * kCell;
                o.to = ((along_x ? c1 : r1) + 1) * kCell;
                o.edge = d == kNorth ? (r1 + 1) * kCell : d == kSouth ? r1 * kCell : d == kWest ? (c1 + 1) * kCell : c1 * kCell;
                o.cut = true;
                const bool known = std::any_of(room.openings.begin(), room.openings.end(), [&](const Opening& e) {
                    return e.side == o.side && e.edge == o.edge && e.from < o.to && o.from < e.to;
                });
                if (!known) room.openings.push_back(o);
            }
        }
        if (room.openings.empty()) continue;
        // The floor taken that runs past the room on the side the camera
        // sees (south, east): past the black frame of its near walls, it
        // would lie over the void.
        for (const LevelItem& i : room.items) {
            const ShapeInfo* s = info(i.shape);
            if (!s || !s->is(ShapeInfo::kSolid) || s->footpad_z > 2 || !s->footpad_x || !s->footpad_y) continue;
            const std::int32_t top = i.z + s->footpad_z * 8;
            if (top < z0 - kStep || top > z0 + kStep) continue;
            const int c1 = i.x / kCell, r1 = i.y / kCell;
            for (int r = r1 - s->footpad_y + 1; r <= r1; ++r) {
                for (int c = c1 - s->footpad_x + 1; c <= c1; ++c) {
                    if (outlined(r, c) || outlined(r - 1, c) || outlined(r, c - 1) || outlined(r - 1, c - 1)) continue;
                    bool seen = false;  // the room lies north or west of it
                    for (int t = 2; t <= 16 && !seen; ++t) seen = outlined(r - t, c) || outlined(r, c - t);
                    room.overhang += seen;
                }
            }
        }
        // The outline's cells that no floor taken bears (where the room was
        // open on the storey below, or its floor belonged to a neighbour).
        {
            std::vector<std::uint8_t> floored(room.cells.size());
            for (const LevelItem& i : room.items) {
                const ShapeInfo* s = info(i.shape);
                if (!s || !s->is(ShapeInfo::kSolid) || s->footpad_z > 2) continue;
                const std::int32_t top = i.z + s->footpad_z * 8;
                if (top < z0 - kStep || top > z0 + kStep) continue;
                const int c1 = i.x / kCell, r1 = i.y / kCell;
                for (int r = r1 - std::max(1, int{s->footpad_y}) + 1; r <= r1; ++r) {
                    for (int c = c1 - std::max(1, int{s->footpad_x}) + 1; c <= c1; ++c) {
                        if (in_box(r, c)) floored[static_cast<std::size_t>((r - b.r0) * room.cols + (c - b.c0))] = 1;
                    }
                }
            }
            for (std::size_t k = 0; k < floored.size(); ++k) room.unfloored += room.outline[k] && !floored[k];
        }
        // Links (quality low) of scripted objects that stay within the room.
        for (const LevelItem& i : room.items) {
            const std::uint8_t link = i.quality_low();
            if (!link || i.shape >= data.scripted.size() || !data.scripted[i.shape]) continue;
            if (std::find(room.local_links.begin(), room.local_links.end(), link) != room.local_links.end()) continue;
            int inside = 0, total = 0;
            for (const LevelItem& o : items) {
                if (o.quality_low() == link && o.shape < data.scripted.size() && data.scripted[o.shape]) ++total;
            }
            for (const LevelItem& o : room.items) {
                if (o.quality_low() == link && o.shape < data.scripted.size() && data.scripted[o.shape]) ++inside;
            }
            if (inside == total) room.local_links.push_back(link);
        }
        rooms.push_back(std::move(room));
    }
    return rooms;
}

}  // namespace crusader::editor::gen
