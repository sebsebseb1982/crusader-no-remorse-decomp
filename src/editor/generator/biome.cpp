#include "editor/generator/biome.hpp"

#include "editor/item_semantics.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace crusader::editor::gen {

namespace {

int length_index(int units) {
    for (int i = 0; i < 5; ++i) {
        if (kWallLengths[i] == units) return i;
    }
    return -1;
}

struct GlobPieces {
    std::set<Piece> floors;
    std::set<std::pair<int, Piece>> walls;  // (wall_key, piece)
};

int wall_key(int side, int axis, int length) { return side * 16 + axis * 8 + length; }

}  // namespace

bool is_floor_tile(const ShapeInfo& info) {
    return info.footpad_x == 16 && info.footpad_y == 16 && info.footpad_z == 0 && info.is(ShapeInfo::kSolid) &&
           info.is(ShapeInfo::kLand) && info.family == 0;
}

bool wall_kind(const ShapeInfo& info, int& axis, int& length) {
    if (info.footpad_z != 12 || !info.is(ShapeInfo::kSolid) || info.family != 0) return false;
    if (info.footpad_x == 1 && (length = length_index(info.footpad_y)) >= 0) {
        axis = kAlongY;
        return true;
    }
    if (info.footpad_y == 1 && (length = length_index(info.footpad_x)) >= 0) {
        axis = kAlongX;
        return true;
    }
    return false;
}

std::string Biome::name() const {
    char text[96];
    std::snprintf(text, sizeof text, "floor 0x%03X, walls 0x%03X / 0x%03X", floor.shape,
                  walls[kBack][kAlongX][4].shape, walls[kBack][kAlongY][4].shape);
    std::string s = text;
    if (!maps.empty()) {
        s += " (maps";
        for (std::size_t i = 0; i < maps.size() && i < 4; ++i) s += " " + std::to_string(maps[i]);
        s += ")";
    }
    return s;
}

bool Biome::can_build(int side, int axis, int units) const {
    return units == 0 || !wall_run(side, axis, units).empty();
}

std::vector<Piece> Biome::wall_run(int side, int axis, int units) const {
    std::vector<Piece> run;
    for (int i = 4; i >= 0 && units > 0; --i) {
        const Piece& p = walls[side][axis][i];
        if (!p.valid()) continue;
        while (units >= kWallLengths[i]) {
            run.push_back(p);
            units -= kWallLengths[i];
        }
    }
    if (units != 0) run.clear();
    return run;
}

std::vector<Biome> learn_biomes(const BiomeSource& source, std::size_t max_biomes, int min_globs) {
    const auto& shapes = source.shapes;
    auto info = [&](std::uint16_t shape) -> const ShapeInfo* {
        return shape < shapes.size() ? &shapes[shape] : nullptr;
    };

    // Maps that place each glob.
    std::map<std::size_t, std::set<int>> glob_maps;
    for (std::size_t m = 0; m < source.maps.size(); ++m) {
        for (const LevelItem& i : source.maps[m].items) {
            const ShapeInfo* si = info(i.shape);
            if (si && si->family == kFamGlobEgg && i.quality != 0) glob_maps[i.quality].insert(static_cast<int>(m));
        }
    }

    // The floors and walls of each glob that a map uses.
    std::vector<std::pair<std::size_t, GlobPieces>> globs;
    for (const auto& [g, maps] : glob_maps) {
        if (g >= source.globs.size()) continue;
        GlobPieces gp;
        for (const GlobPiece& p : source.globs[g].pieces) {
            const ShapeInfo* si = info(p.shape);
            if (!si) continue;
            int axis = 0, length = 0;
            if (p.shape < source.scripted.size() && source.scripted[p.shape]) continue;
            if (is_floor_tile(*si)) gp.floors.insert({p.shape, p.frame});
            else if (wall_kind(*si, axis, length)) {
                // The side: the edge of the cell it stands on (across it).
                auto side = [&](int a) { return (a == kAlongX ? p.y : p.x) < 128 ? kBack : kFront; };
                gp.walls.insert({wall_key(side(axis), axis, length), {p.shape, p.frame}});
                // A post stands for both directions.
                if (length == 0) gp.walls.insert({wall_key(side(1 - axis), 1 - axis, length), {p.shape, p.frame}});
            }
        }
        if (!gp.floors.empty() || !gp.walls.empty()) globs.emplace_back(g, std::move(gp));
    }

    // Counts: floor with wall, wall with wall (same glob).
    using WallId = std::pair<int, Piece>;
    std::map<std::pair<Piece, WallId>, int> floor_wall;
    std::map<std::pair<WallId, WallId>, int> wall_wall;
    std::map<Piece, int> floor_count;
    // Frames of each wall shape on the near side of the cells.
    std::map<std::uint16_t, std::map<std::uint8_t, int>> front_frames;
    for (const auto& [g, gp] : globs) {
        for (const WallId& w : gp.walls) {
            if (w.first >= 16) ++front_frames[w.second.shape][w.second.frame];
        }
        for (const Piece& f : gp.floors) {
            ++floor_count[f];
            for (const WallId& w : gp.walls) ++floor_wall[{f, w}];
        }
        for (const WallId& a : gp.walls) {
            for (const WallId& b : gp.walls) {
                if (!(a == b)) ++wall_wall[{a, b}];
            }
        }
    }

    auto best_with_floor = [&](const Piece& f, int key) {
        Piece best;
        int count = 0;
        for (auto it = floor_wall.lower_bound({f, {key, Piece{}}}); it != floor_wall.end(); ++it) {
            if (!(it->first.first == f) || it->first.second.first != key) break;
            if (it->second > count) count = it->second, best = it->first.second.second;
        }
        return std::make_pair(best, count);
    };
    auto best_with_wall = [&](const WallId& w, int key) {
        Piece best;
        int count = 0;
        for (auto it = wall_wall.lower_bound({w, {key, Piece{}}}); it != wall_wall.end(); ++it) {
            if (!(it->first.first == w) || it->first.second.first != key) break;
            if (it->second > count) count = it->second, best = it->first.second.second;
        }
        return std::make_pair(best, count);
    };

    // One biome per floor shape and pair of long walls; its floor frames merge.
    struct Key {
        std::uint16_t floor;
        Piece wx, wy;
        auto operator<=>(const Key&) const = default;
    };
    std::map<Key, Biome> by_key;
    for (const auto& [f, n] : floor_count) {
        const auto [wx, cx] = best_with_floor(f, wall_key(kBack, kAlongX, 4));
        const auto [wy, cy] = best_with_floor(f, wall_key(kBack, kAlongY, 4));
        if (!wx.valid() || !wy.valid()) continue;
        Biome& b = by_key[{f.shape, wx, wy}];
        b.floor_variants.push_back(f);
        b.globs += std::min(cx, cy);
        if (b.walls[kBack][kAlongX][4].valid()) continue;
        for (int axis = 0; axis < 2; ++axis) {
            const WallId back{wall_key(kBack, axis, 4), axis == kAlongX ? wx : wy};
            b.walls[kBack][axis][4] = back.second;
            for (int len = 0; len < 4; ++len) {
                Piece p = best_with_wall(back, wall_key(kBack, axis, len)).first;
                if (!p.valid()) p = best_with_floor(f, wall_key(kBack, axis, len)).first;
                b.walls[kBack][axis][len] = p;
            }
            // The near side: the same shapes, in their black frame used
            // there, else in the frame they have most there.
            for (int len = 0; len < 5; ++len) {
                Piece p = b.walls[kBack][axis][len];
                const auto it = front_frames.find(p.shape);
                if (p.valid() && it != front_frames.end()) {
                    int best = 0;
                    bool black = false;
                    for (const auto& [frame, uses] : it->second) {
                        const bool is_black = source.black && source.black({p.shape, frame});
                        if ((is_black && !black) || (is_black == black && uses > best)) {
                            best = uses, p.frame = frame, black = is_black;
                        }
                    }
                }
                b.walls[kFront][axis][len] = p;
            }
        }
    }

    std::vector<Biome> out;
    for (auto& [key, b] : by_key) {
        if (b.globs < min_globs || !b.walls[kBack][kAlongX][0].valid() || !b.walls[kBack][kAlongY][0].valid()) {
            continue;
        }
        std::sort(b.floor_variants.begin(), b.floor_variants.end(),
                  [&](const Piece& a, const Piece& c) { return floor_count[a] > floor_count[c]; });
        b.floor = b.floor_variants.front();
        // Maps whose globs hold the floor shape and the long walls.
        std::map<int, int> maps;
        for (const auto& [g, gp] : globs) {
            const bool floor = std::any_of(gp.floors.begin(), gp.floors.end(),
                                           [&](const Piece& p) { return p.shape == key.floor; });
            if (!floor || !gp.walls.count({wall_key(kBack, kAlongX, 4), key.wx})) continue;
            for (int m : glob_maps[g]) ++maps[m];
        }
        std::vector<std::pair<int, int>> ranked(maps.begin(), maps.end());
        std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& c) { return a.second > c.second; });
        for (const auto& [m, n] : ranked) b.maps.push_back(m);
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Biome& a, const Biome& b) { return a.globs > b.globs; });
    if (out.size() > max_biomes) out.resize(max_biomes);
    return out;
}

}  // namespace crusader::editor::gen
