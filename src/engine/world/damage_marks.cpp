#include "engine/world/damage_marks.hpp"

#include "core/dtable.hpp"
#include "core/typeflags.hpp"
#include "engine/world/projectile.hpp"

#include <algorithm>
#include <cmath>

namespace crusader {

namespace {

constexpr int kShadePercent[DamageMarks::kShades] = {72, 52, 34, 16};  // light .. black
constexpr int kFaceSlack = 4;      // usecode units a shot may stop short of a face
constexpr int kSearchPad = 256;    // largest footpad reach of an anchor, usecode units
constexpr std::size_t kMaxScorchHosts = 24;

// splitmix32: the marks' own numbers, never the game's generators.
// @orig none: port-only impact marks
std::uint32_t mix(std::uint32_t v) {
    v += 0x9E3779B9u;
    v = (v ^ (v >> 16)) * 0x85EBCA6Bu;
    v = (v ^ (v >> 13)) * 0xC2B2AE35u;
    return v ^ (v >> 16);
}

std::uint32_t mix(std::uint32_t seed, int a, int b) {
    return mix(seed ^ mix(static_cast<std::uint32_t>(a) * 0x27D4EB2Fu ^ static_cast<std::uint32_t>(b)));
}

struct Box {
    std::int32_t x_left, x, y_far, y, z, z_top;
};

// The item's box, in usecode units (see render.hpp; footpads x/y are 16
// usecode units, z 8, swapped when mirrored).
// @orig none: port-only impact marks
Box box_of(const Item& item, const ShapeInfo* info) {
    const bool mirrored = item.flags & kItemFlipped;
    const int fx = info ? info->footpad_x : 0, fy = info ? info->footpad_y : 0, fz = info ? info->footpad_z : 0;
    return {item.x - (mirrored ? fy : fx) * 16, item.x, item.y - (mirrored ? fx : fy) * 16, item.y, item.z,
            item.z + fz * 8};
}

// A shot ending at `p` hit a face seen from the camera (+x, +y or the top).
bool faces_viewer(const Box& b, const World::Point& p) {
    return p.x >= b.x - kFaceSlack || p.y >= b.y - kFaceSlack || p.z >= b.z_top - kFaceSlack;
}

World::Point nearest_in(const Box& b, const World::Point& p) {
    return {std::clamp(p.x, b.x_left, b.x), std::clamp(p.y, b.y_far, b.y), std::clamp(p.z, b.z, b.z_top)};
}

// The point of the face of `b` that `p` stands out of, straight across: the
// same point for every item of a floor or of a wall face, so that one
// explosion leaves one scorch over all of them. `gap` gets how far `p` is
// from that face, `top` whether it is the top face. Inside the box: `p`.
// @orig none: port-only impact marks
World::Point across_face(const Box& b, const World::Point& p, double& gap, bool& top) {
    const std::int32_t out_x = p.x - b.x, out_y = p.y - b.y, out_z = p.z - b.z_top;
    World::Point s = p;
    gap = 0;
    top = false;
    if (out_z > 0 && out_z >= out_x && out_z >= out_y) {
        s.z = b.z_top;
        gap = out_z;
        top = true;
    } else if (out_x > 0 && out_x >= out_y) {
        s.x = b.x;
        gap = out_x;
    } else if (out_y > 0) {
        s.y = b.y;
        gap = out_y;
    }
    return s;
}

double distance(const World::Point& a, const World::Point& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Scenery that can take a mark: a drawn, solid-looking item on the map, not
// an NPC, a shot or a translucent shape.
// @orig none: port-only impact marks
bool can_host(const World& world, const Item& item) {
    if (is_npc_id(item.id) || item.parent != 0 || item.off_map || item.sprite_only) return false;
    if (item.flags & (kItemInvisible | kItemContained | kItemEthereal)) return false;
    if (item.shape == kProjectileShape) return false;
    const ShapeInfo* info = world.shape_info(item.shape);
    return info && !info->is(ShapeInfo::kEditor) && !info->is(ShapeInfo::kTranslucent) &&
           !info->is(ShapeInfo::kNpc);
}

bool same_host(const DamageMarks::Mark& m, const Item& item) {
    return item.shape == m.shape && item.frame == m.frame && item.x == m.x && item.y == m.y && item.z == m.z &&
           (item.flags & kItemFlipped) == m.flags;
}

int screen_x(std::int32_t x, std::int32_t y) { return (x - y) >> 1; }
int screen_y(std::int32_t x, std::int32_t y, std::int32_t z) { return ((x + y) >> 2) - z; }

}  // namespace

// @orig none: port-only impact marks
MarkKind mark_kind(int fire_type) {
    const dtable::FireType* ft = dtable::fire_type(fire_type);
    if (ft && ft->splash() > 0) return MarkKind::Scorch;
    if (fire_type == 1 || fire_type == 2 || fire_type == 0xB || fire_type == 0xD) return MarkKind::Bullet;
    return MarkKind::Burn;
}

// @orig none: port-only impact marks
std::array<std::uint8_t, 256> shade_table(const Palette& palette, int percent) {
    std::array<std::uint8_t, 256> table{};
    for (int c = 0; c < 256; ++c) {
        if (c >= 1 && c <= 14) {
            table[c] = static_cast<std::uint8_t>(c);
            continue;
        }
        const Rgb& src = palette.colors[c];
        const int r = src.r * percent / 100, g = src.g * percent / 100, b = src.b * percent / 100;
        int best = 0;
        long best_d = -1;
        for (int k = 0; k < 255; ++k) {
            if (k >= 1 && k <= 14) continue;
            const Rgb& o = palette.colors[k];
            const long dr = o.r - r, dg = o.g - g, db = o.b - b;
            const long d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
            if (best_d < 0 || d < best_d) {
                best_d = d;
                best = k;
            }
        }
        table[c] = static_cast<std::uint8_t>(best);
    }
    return table;
}

// @orig none: port-only impact marks
std::vector<MarkPixel> mark_pattern(MarkKind kind, int radius, bool flat, std::uint32_t seed) {
    std::vector<MarkPixel> out;
    const auto put = [&](int x, int y, int shade) {
        out.push_back({static_cast<std::int8_t>(x), static_cast<std::int8_t>(y), static_cast<std::uint8_t>(shade)});
    };
    if (kind == MarkKind::Bullet) {
        // A black hole of two pixels (one high on a floor), a dark rim
        // around it with a gap or two, and a few chips further out.
        put(0, 0, 3);
        put(1, 0, 3);
        if (!flat) {
            put(0, 1, 3);
            put(1, 1, 2);
        }
        const int below = flat ? 1 : 2;
        static constexpr int kRim[][2] = {{-1, 0}, {2, 0}, {0, -1}, {1, -1}, {-1, 1}, {2, 1}};
        int bit = 0;
        for (const auto& r : kRim) {
            const int y = r[1] > 0 ? below : r[1];
            if ((seed >> bit++) & 3) put(r[0], y, 1 + ((seed >> (bit + 8)) & 1));
        }
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t h = mix(seed, k, 1);
            const int x = static_cast<int>(h % 7) - 3, y = static_cast<int>((h >> 4) % 5) - 2;
            if (std::abs(x) >= 2 || std::abs(y) >= 2) put(x, flat ? y / 2 : y, 0);
        }
        return out;
    }
    // A dark centre fading out with some noise on every pixel; a scorch has
    // a ragged edge (16 spokes of random length).
    constexpr double kPi = 3.14159265358979;
    radius = std::clamp(radius, 1, 100);
    const int rx = radius, ry = flat ? std::max(1, radius / 2) : radius;
    const bool scorch = kind == MarkKind::Scorch;
    for (int y = -ry; y <= ry; ++y) {
        for (int x = -rx; x <= rx; ++x) {
            const double nx = static_cast<double>(x) / rx, ny = static_cast<double>(y) / ry;
            const double nd = std::sqrt(nx * nx + ny * ny);
            if (nd > 1.0) continue;
            double edge = 1.0;
            if (scorch) {
                const double turn = (std::atan2(ny, nx) + kPi) / (2 * kPi) * 16;
                const int spoke = static_cast<int>(turn) & 15;
                const double frac = turn - std::floor(turn);
                const double a = (mix(seed, spoke, 0) & 255) / 255.0;
                const double b = (mix(seed, (spoke + 1) & 15, 0) & 255) / 255.0;
                edge = 0.6 + 0.4 * (a + (b - a) * frac);
                if (nd > edge) continue;
            }
            const double noise = (mix(seed, x, y) & 255) / 255.0;
            const double v = (1.0 - nd / edge) + (noise - 0.5) * (scorch ? 0.45 : 0.3);
            if (v > (scorch ? 0.7 : 0.8)) put(x, y, 3);
            else if (v > 0.55) put(x, y, 2);
            else if (v > 0.25) put(x, y, 1);
            else if (v > 0.05) put(x, y, 0);
        }
    }
    return out;
}

// @orig none: port-only impact marks
void DamageMarks::record(const World& world, ObjId hit, World::Point at, int fire_type) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    const MarkKind kind = mark_kind(fire_type);
    if (kind == MarkKind::Scorch) {
        // The scorch has a radius of splash x 3 pixels (10..32) on what it
        // hits, smaller on a face further away, up to 2 x that many units.
        const dtable::FireType* ft = dtable::fire_type(fire_type);
        const int radius = std::clamp((ft ? ft->splash() : 0) * 3, 10, 32);
        const double reach = radius * 2.0;
        std::vector<std::pair<double, ObjId>> hosts;
        const int pad = static_cast<int>(reach) + kSearchPad;
        for (ObjId id : world.rect_search(at.x - pad, at.y - pad, at.x + pad, at.y + pad)) {
            const Item* item = world.get(id);
            if (!item || !can_host(world, *item)) continue;
            const Box b = box_of(*item, world.shape_info(item->shape));
            if (!faces_viewer(b, at)) continue;
            const double d = distance(at, nearest_in(b, at));
            if (d <= reach) hosts.emplace_back(d, id);
        }
        std::sort(hosts.begin(), hosts.end());
        if (hosts.size() > kMaxScorchHosts) hosts.resize(kMaxScorchHosts);
        const std::uint32_t seed = next_seed_;
        for (const auto& [d, id] : hosts) {
            const Item& item = *world.get(id);
            double gap = 0;
            bool top = false;
            const World::Point s = across_face(box_of(item, world.shape_info(item.shape)), at, gap, top);
            const int r = static_cast<int>(radius - gap / 4.0);
            if (r < 4) continue;
            next_seed_ = seed;  // one explosion, one pattern on every host
            add(item, s, kind, r, top);
        }
        next_seed_ = mix(seed);
        return;
    }
    const int radius = kind == MarkKind::Burn ? 3 : 1;
    if (hit != 0) {
        const Item* item = world.get(hit);
        if (!item || !can_host(world, *item)) return;
        const Box b = box_of(*item, world.shape_info(item->shape));
        if (!faces_viewer(b, at)) return;
        const World::Point s = nearest_in(b, at);
        add(*item, s, kind, radius, s.z == b.z_top && at.z >= b.z_top && b.z_top - b.z <= 8);
        return;
    }
    // On nothing: the floor right under the end of the shot, if any.
    const Item* floor = nullptr;
    std::int32_t floor_top = 0;
    for (ObjId id : world.rect_search(at.x - kSearchPad, at.y - kSearchPad, at.x + kSearchPad, at.y + kSearchPad)) {
        const Item* item = world.get(id);
        if (!item || !can_host(world, *item)) continue;
        const Box b = box_of(*item, world.shape_info(item->shape));
        if (at.x < b.x_left || at.x > b.x || at.y < b.y_far || at.y > b.y) continue;
        if (b.z_top > at.z + kFaceSlack || b.z_top < at.z - 16) continue;
        if (!floor || b.z_top > floor_top) {
            floor = item;
            floor_top = b.z_top;
        }
    }
    if (floor) add(*floor, {at.x, at.y, floor_top}, kind, radius, true);
}

// @orig none: port-only impact marks
void DamageMarks::add(const Item& host, World::Point surface, MarkKind kind, int radius, bool flat) {
    Mark m;
    m.host = host.id;
    m.shape = host.shape;
    m.frame = host.frame;
    m.flags = host.flags & kItemFlipped;
    m.x = host.x;
    m.y = host.y;
    m.z = host.z;
    m.dx = static_cast<std::int16_t>(screen_x(surface.x, surface.y) - screen_x(host.x, host.y));
    m.dy = static_cast<std::int16_t>(screen_y(surface.x, surface.y, surface.z) - screen_y(host.x, host.y, host.z));
    m.kind = kind;
    m.radius = static_cast<std::uint8_t>(radius);
    m.flat = flat;
    m.seed = mix(next_seed_);
    next_seed_ = m.seed;
    m.pixels = std::make_shared<const std::vector<MarkPixel>>(mark_pattern(kind, radius, m.flat, m.seed));
    marks_.push_back(m);
    while (marks_.size() > kMaxMarks) marks_.pop_front();
}

// @orig none: port-only impact marks
void DamageMarks::prepare(const World& world, const Palette& game_palette) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    std::erase_if(marks_, [&](const Mark& m) {
        const Item* item = world.get(m.host);
        return !item || !same_host(m, *item) || !can_host(world, *item);
    });
    by_host_.clear();
    for (std::uint32_t i = 0; i < marks_.size(); ++i) by_host_[marks_[i].host].push_back(i);

    // Colours 1..14 cycle every tick and are never used by the tables.
    bool same = shade_ready_;
    for (int c = 0; same && c < 256; ++c) {
        if (c >= 1 && c <= 14) continue;
        const Rgb &a = game_palette.colors[c], &b = shade_palette_.colors[c];
        same = a.r == b.r && a.g == b.g && a.b == b.b;
    }
    if (!same) {
        for (int s = 0; s < kShades; ++s) shade_[s] = shade_table(game_palette, kShadePercent[s]);
        shade_palette_ = game_palette;
        shade_ready_ = true;
    }
}

// @orig none: port-only impact marks
bool DamageMarks::opaque(const DrawItem& d, int sx, int sy) const {
    const ShapeFrame& f = *d.frame;
    auto [it, made] = masks_.try_emplace(d.frame);
    std::vector<std::uint8_t>& mask = it->second;
    if (made) {
        mask.assign(static_cast<std::size_t>(f.width) * f.height, 0);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) mask[static_cast<std::size_t>(y) * f.width + x] = f.pixel(x, y) >= 0;
    }
    const int top = d.sy - f.yoff;
    const int left = d.mirrored ? d.sx + f.xoff - (f.width - 1) : d.sx - f.xoff;
    const int fy = sy - top;
    const int fx = d.mirrored ? f.width - 1 - (sx - left) : sx - left;
    if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) return false;
    return mask[static_cast<std::size_t>(fy) * f.width + fx];
}

// @orig none: port-only impact marks
void DamageMarks::draw(Surface8& screen, const DrawItem& d) const {
    if (!shade_ready_) return;
    const auto found = by_host_.find(d.item->id);
    if (found == by_host_.end()) return;
    for (std::uint32_t index : found->second) {
        const Mark& m = marks_[index];
        if (!same_host(m, *d.item) || !m.pixels) continue;
        const int cx = d.sx + m.dx, cy = d.sy + m.dy;
        for (const MarkPixel& px : *m.pixels) {
            const int x = cx + px.dx, y = cy + px.dy;
            if (x < 0 || y < 0 || x >= screen.width || y >= screen.height || !opaque(d, x, y)) continue;
            std::uint8_t& p = screen.row(y)[x];
            p = shade_[px.shade][p];
        }
    }
}

// @orig none: port-only impact marks
void DamageMarks::clear() {
    marks_.clear();
    by_host_.clear();
    masks_.clear();
}

}  // namespace crusader
