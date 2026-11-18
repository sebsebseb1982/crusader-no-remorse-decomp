#include "engine/world/damage_marks.hpp"

#include "core/dtable.hpp"
#include "core/typeflags.hpp"
#include "engine/world/projectile.hpp"

#include <algorithm>
#include <cmath>

namespace crusader {

namespace {

constexpr int kDarkShades = 4;
constexpr int kShadePercent[kDarkShades] = {72, 52, 34, 16};  // light .. black
// Blood: the colour mixed with a red, light to dark.
constexpr Rgb kBloodTarget[3] = {{170, 24, 16}, {130, 6, 4}, {70, 0, 0}};
constexpr int kBloodPercent[3] = {45, 68, 85};
// Casings: bright brass, brass, dark brass, a shell's red and dark red, in
// place of the colour underneath.
constexpr Rgb kCasingColour[5] = {{250, 214, 120}, {200, 150, 60}, {128, 88, 30}, {196, 40, 30}, {116, 18, 14}};
constexpr int kFaceSlack = 4;      // usecode units a shot may stop short of a face
constexpr int kSearchPad = 256;    // largest footpad reach of an anchor, usecode units
constexpr std::size_t kMaxScorchHosts = 24;
constexpr double kPi = 3.14159265358979;

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

// The screen direction of a level world direction (ux, uy): on a wall as
// seen, on a floor in the ground's own proportions (y not halved).
double screen_angle(double ux, double uy, bool flat) {
    return std::atan2((ux + uy) / (flat ? 2.0 : 4.0), (ux - uy) / 2.0);
}

// The floor right under `at`: the highest scenery whose box holds (x, y)
// and whose top is at most 16 below z (or kFaceSlack above it).
// @orig none: port-only impact marks
const Item* floor_under(const World& world, const World::Point& at, std::int32_t& top) {
    const Item* floor = nullptr;
    for (ObjId id : world.rect_search(at.x - kSearchPad, at.y - kSearchPad, at.x + kSearchPad, at.y + kSearchPad)) {
        const Item* item = world.get(id);
        if (!item || !can_host(world, *item)) continue;
        const Box b = box_of(*item, world.shape_info(item->shape));
        if (at.x < b.x_left || at.x > b.x || at.y < b.y_far || at.y > b.y) continue;
        if (b.z_top > at.z + kFaceSlack || b.z_top < at.z - 16) continue;
        if (!floor || b.z_top > top) {
            floor = item;
            top = b.z_top;
        }
    }
    return floor;
}

// The colour nearest to (r, g, b) in the palette, colours 1..14 and 0xFF left out.
// @orig none: port-only impact marks
std::uint8_t nearest_colour(const Palette& palette, int r, int g, int b) {
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
    return static_cast<std::uint8_t>(best);
}

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
        table[c] = nearest_colour(palette, src.r * percent / 100, src.g * percent / 100, src.b * percent / 100);
    }
    return table;
}

// @orig none: port-only blood marks
std::array<std::uint8_t, 256> tint_table(const Palette& palette, Rgb target, int percent) {
    std::array<std::uint8_t, 256> table{};
    const auto mixed = [&](int from, int to) { return (from * (100 - percent) + to * percent) / 100; };
    for (int c = 0; c < 256; ++c) {
        if (c >= 1 && c <= 14) {
            table[c] = static_cast<std::uint8_t>(c);
            continue;
        }
        const Rgb& src = palette.colors[c];
        table[c] = nearest_colour(palette, mixed(src.r, target.r), mixed(src.g, target.g), mixed(src.b, target.b));
    }
    return table;
}

// @orig none: port-only impact marks
std::vector<MarkPixel> mark_pattern(MarkKind kind, int radius, bool flat, std::uint32_t seed, double angle) {
    std::vector<MarkPixel> out;
    const auto put = [&](int x, int y, int shade) {
        if (x < -127 || x > 127 || y < -127 || y > 127) return;
        out.push_back({static_cast<std::int8_t>(x), static_cast<std::int8_t>(y), static_cast<std::uint8_t>(shade)});
    };
    if (is_casing(kind)) {
        // Lying flat one of four ways: a casing two pixels long (bright at
        // its mouth), a shell three (red, its brass head at the end), a
        // pixel of shadow under the last.
        static constexpr int kLie[4][2] = {{1, 0}, {1, 1}, {1, -1}, {-1, 1}};
        const int ox = kLie[seed & 3][0], oy = kLie[seed & 3][1];
        if (kind == MarkKind::Shell) {
            put(0, 0, DamageMarks::kShellDark);
            put(ox, oy, DamageMarks::kShellRed);
            put(2 * ox, 2 * oy, DamageMarks::kBrass);
            put(2 * ox, 2 * oy + 1, 1);
        } else {
            put(0, 0, DamageMarks::kBrassLight);
            put(ox, oy, DamageMarks::kBrassDark);
            put(ox, oy + 1, 1);
        }
        return out;
    }
    if (is_blood(kind)) {
        // Worked out on the wall, or on the ground at its own proportions
        // (y halved on screen when `flat`); u along `angle`, v across.
        const int r = std::clamp(radius, 1, 20);
        const double ca = std::cos(angle), sa = std::sin(angle);
        const auto unit = [](std::uint32_t h) { return (h & 0xFFFF) / 65535.0; };
        const auto place = [&](double gx, double gy, int shade) {
            put(static_cast<int>(std::lround(gx)), static_cast<int>(std::lround(flat ? gy / 2 : gy)), shade);
        };
        // A ragged blob around ground point (cx, cy), `along` x `across`,
        // darker inside (`dark` darkens it all).
        const auto blob = [&](double cx, double cy, double along, double across, double ragged, double dark,
                              int salt) {
            const int reach = static_cast<int>(std::ceil(std::max(along, across))) + 1;
            const int scx = static_cast<int>(std::lround(cx)), scy = static_cast<int>(std::lround(flat ? cy / 2 : cy));
            const int ys = flat ? reach / 2 + 1 : reach;
            for (int sy = scy - ys; sy <= scy + ys; ++sy) {
                for (int sx = scx - reach; sx <= scx + reach; ++sx) {
                    const double gx = sx - cx, gy = (flat ? sy * 2.0 : sy) - cy;
                    const double u = gx * ca + gy * sa, v = -gx * sa + gy * ca;
                    const double nd = std::sqrt((u / along) * (u / along) + (v / across) * (v / across));
                    const double turn = (std::atan2(v, u) + kPi) / (2 * kPi) * 12;
                    const int spoke = static_cast<int>(turn) % 12;
                    const double frac = turn - std::floor(turn);
                    const double a = unit(mix(seed, spoke, salt)), b = unit(mix(seed, (spoke + 1) % 12, salt));
                    const double edge = 1.0 - ragged + ragged * (a + (b - a) * frac);
                    if (nd > edge) continue;
                    const double noise = unit(mix(seed, sx * 131 + salt, sy));
                    const double value = (1.0 - nd / edge) + dark + (noise - 0.5) * 0.3;
                    put(sx, sy, value > 0.5 ? 6 : value > 0.2 ? 5 : 4);
                }
            }
        };
        if (kind == MarkKind::BloodDrop) {
            // A drop stretched the way it flew, a few smaller ones beyond.
            blob(0, 0, r * 1.5, r * 0.9 + 0.3, 0.2, 0.15, 1);
            const int beyond = 1 + static_cast<int>(seed % 3);
            for (int k = 0; k < beyond; ++k) {
                const double dist = r * 1.8 + 1.5 + k * 1.7;
                const double side = (unit(mix(seed, k, 2)) - 0.5) * 2.0;
                place(ca * dist - sa * side, sa * dist + ca * side, 5);
            }
            return out;
        }
        if (kind == MarkKind::BloodPool) {
            // A pool, dark, with drops around its edge.
            blob(0, 0, r, r * 0.8, 0.35, 0.35, 3);
            for (int k = 0; k < r; ++k) {
                const double turn = unit(mix(seed, k, 4)) * 2 * kPi;
                const double dist = r * (1.05 + 0.55 * unit(mix(seed, k, 5)));
                place(std::cos(turn) * dist, std::sin(turn) * dist, (k & 1) ? 5 : 6);
            }
            return out;
        }
        // A spray: a splash, droplets thrown on in a cone (the further, the
        // smaller, some with a tail), and on a wall drips running down.
        blob(0, 0, r * 1.1, r * 0.85, 0.45, 0.1, 6);
        const int droplets = 4 * r + 4;
        for (int k = 0; k < droplets; ++k) {
            const double turn = angle + (unit(mix(seed, k, 7)) - 0.5) * 1.4;
            const double dist = r * (0.9 + 2.2 * unit(mix(seed, k, 8)));
            const double gx = std::cos(turn) * dist, gy = std::sin(turn) * dist;
            if ((mix(seed, k, 9) & 7) == 0 && dist < r * 2) {
                blob(gx, gy, 1.3, 1.0, 0.2, 0.2, 10 + k);
                continue;
            }
            place(gx, gy, dist > 2 * r ? 5 : 6);
            if (dist > 1.6 * r) place(gx - std::cos(turn), gy - std::sin(turn), 4);
        }
        if (!flat) {
            const int drips = 2 + r / 3;
            for (int k = 0; k < drips; ++k) {
                const std::uint32_t h = mix(seed, k, 11);
                const int x = static_cast<int>(std::lround((unit(h) - 0.5) * r * 1.2));
                const int top = r / 2;
                const int length = 2 + static_cast<int>((h >> 16) % static_cast<std::uint32_t>(r + 4));
                for (int y = 0; y < length; ++y) put(x, top + y, y < length - 2 ? 6 : 5);
                put(x, top + length, 5);
                put(x + 1, top + length - 1, 4);
            }
        }
        return out;
    }
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
    std::int32_t floor_top = 0;
    if (const Item* floor = floor_under(world, at, floor_top)) add(*floor, {at.x, at.y, floor_top}, kind, radius, true);
}

// @orig none: port-only blood marks
int DamageMarks::roll(int n) {
    next_seed_ = mix(next_seed_);
    return n > 0 ? static_cast<int>(next_seed_ % static_cast<std::uint32_t>(n)) : 0;
}

// @orig none: port-only blood marks
bool DamageMarks::drop(const World& world, World::Point at, MarkKind kind, int radius, double ux, double uy) {
    std::int32_t top = 0;
    const Item* floor = floor_under(world, at, top);
    if (!floor) return false;
    add(*floor, {at.x, at.y, top}, kind, radius, true, screen_angle(ux, uy, true));
    return true;
}

// @orig none: port-only blood marks
bool DamageMarks::spray_wall(const World& world, World::Point from, double ux, double uy, int reach, int radius) {
    // Scenery standing at that height (16 units high at least: no floor).
    std::vector<std::pair<const Item*, Box>> walls;
    const int pad = reach + kSearchPad;
    for (ObjId id : world.rect_search(from.x - pad, from.y - pad, from.x + pad, from.y + pad)) {
        const Item* item = world.get(id);
        if (!item || !can_host(world, *item)) continue;
        const Box b = box_of(*item, world.shape_info(item->shape));
        if (b.z_top - b.z >= 16 && from.z >= b.z && from.z <= b.z_top) walls.emplace_back(item, b);
    }
    World::Point before = from;
    for (int step = 6; step <= reach; step += 6) {
        const World::Point p{from.x + static_cast<std::int32_t>(std::lround(ux * step)),
                             from.y + static_cast<std::int32_t>(std::lround(uy * step)), from.z};
        for (const auto& [item, b] : walls) {
            if (p.x < b.x_left || p.x > b.x || p.y < b.y_far || p.y > b.y) continue;
            if (!faces_viewer(b, before)) return false;  // its back: hidden
            add(*item, nearest_in(b, before), MarkKind::BloodSpray, radius, false, screen_angle(ux, uy, false));
            return true;
        }
        before = p;
    }
    return false;
}

// @orig none: port-only blood marks
void DamageMarks::bleed(const World& world, const Wound& wound, bool gore) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    if (!wound.bleeds || wound.dealt <= 0) return;
    const MarkKind shot = mark_kind(wound.fire_type);
    if (shot == MarkKind::Burn) return;  // energy weapons burn, freeze or melt
    const Item* npc = world.get(wound.npc);
    if (!npc) return;
    const Box b = box_of(*npc, world.shape_info(npc->shape));
    const World::Point centre{(b.x_left + b.x) / 2, (b.y_far + b.y) / 2, npc->z};
    const std::int32_t chest = npc->z + std::min((b.z_top - b.z) * 5 / 8, 40);
    // Away from the shooter or the blast; anywhere when it stood right there.
    double ux = centre.x - wound.from_x, uy = centre.y - wound.from_y;
    const double length = std::sqrt(ux * ux + uy * uy);
    if (length < 1) {
        const double turn = roll(628) / 100.0;
        ux = std::cos(turn);
        uy = std::sin(turn);
    } else {
        ux /= length;
        uy /= length;
    }
    const auto turned = [&](double degrees, double& tx, double& ty) {
        const double t = degrees * kPi / 180;
        tx = ux * std::cos(t) - uy * std::sin(t);
        ty = ux * std::sin(t) + uy * std::cos(t);
    };
    // Pieces and drops thrown on the floor in a fan of +/- `spread` degrees,
    // up to `far` units; sprays on the walls met by rays across the fan.
    const auto fan = [&](int pieces, int spread, int far, int spray_radius, int drop_radius) {
        for (int k = 0; k < pieces; ++k) {
            double tx = 0, ty = 0;
            turned(roll(2 * spread + 1) - spread, tx, ty);
            const int dist = 12 + roll(far - 11);
            const World::Point at{centre.x + static_cast<std::int32_t>(std::lround(tx * dist)),
                                  centre.y + static_cast<std::int32_t>(std::lround(ty * dist)), centre.z};
            if (k % 2) drop(world, at, MarkKind::BloodSpray, spray_radius + roll(5), tx, ty);
            else drop(world, at, MarkKind::BloodDrop, drop_radius + roll(4), tx, ty);
        }
    };
    const auto walls = [&](int spread, int step, int reach, int radius) {
        for (int degrees = -spread; degrees <= spread; degrees += step) {
            double tx = 0, ty = 0;
            turned(degrees + roll(11) - 5, tx, ty);
            spray_wall(world, {centre.x, centre.y, chest}, tx, ty, reach, radius + roll(6));
        }
    };
    const bool blast = shot == MarkKind::Scorch;
    if (blast && wound.killed) {
        // Torn apart: a pool under the body, pieces and drops thrown in a
        // fan away from the blast, sprays on the walls around.
        drop(world, centre, MarkKind::BloodPool, gore ? 15 + roll(6) : 12 + roll(6), ux, uy);
        if (gore) {
            fan(32 + roll(13), 100, 200, 5, 2);
            walls(90, 15, 200, 9);
        } else {
            fan(18 + roll(9), 80, 150, 4, 2);
            walls(75, 25, 150, 7);
        }
        return;
    }
    // A bullet (or a blast that did not kill): a spray on the wall behind,
    // always when it killed, most of the time otherwise (always in gore
    // mode), and drops on the floor around, more for a harder hit. A kill
    // leaves a pool under the body; in gore mode also a second and a third
    // spray to the sides and pieces thrown behind.
    const int strength = std::clamp(wound.dealt, 1, 60);
    if (gore || wound.killed || roll(100) < 75) {
        const int radius = gore ? std::min(6 + strength / 5 + (wound.killed ? 3 : 0), 15)
                                : std::min(4 + strength / 7 + (wound.killed ? 2 : 0), 11);
        const int reach = gore ? std::min(80 + strength * 3, 200) : std::min(48 + strength * 2, 150);
        spray_wall(world, {centre.x, centre.y, chest}, ux, uy, reach, radius);
        if (gore && wound.killed) {
            for (int degrees : {-25, 25}) {
                double tx = 0, ty = 0;
                turned(degrees, tx, ty);
                spray_wall(world, {centre.x, centre.y, chest}, tx, ty, reach, radius - 2);
            }
        }
    }
    const int drops = gore ? std::min(4 + strength / 6 + (wound.killed ? 4 : 0), 14)
                           : std::min(2 + strength / 10 + (wound.killed ? 2 : 0), 7);
    for (int k = 0; k < drops; ++k) {
        const int along = 6 + roll((gore ? 40 : 24) + strength * (gore ? 3 : 2));
        const int aside = blast ? roll(41) - 20 : roll(gore ? 25 : 17) - (gore ? 12 : 8);
        const World::Point at{centre.x + static_cast<std::int32_t>(std::lround(ux * along - uy * aside)),
                              centre.y + static_cast<std::int32_t>(std::lround(uy * along + ux * aside)), centre.z};
        drop(world, at, MarkKind::BloodDrop, (gore || wound.killed ? 2 : 1) + roll(gore ? 4 : 3), ux, uy);
    }
    if (wound.killed) {
        drop(world, centre, MarkKind::BloodPool, gore ? 9 + roll(6) : 5 + roll(4), ux, uy);
        if (gore) fan(6 + roll(5), 40, 120, 4, 2);
    }
}

// @orig none: port-only casings
bool DamageMarks::lay_casing(const World& world, World::Point at, bool shell, std::uint32_t seed) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    std::int32_t top = 0;
    const Item* floor = floor_under(world, at, top);
    if (!floor) return false;
    // The oldest casing goes past kMaxCasings, the other marks stay.
    const auto casings = std::count_if(marks_.begin(), marks_.end(), [](const Mark& m) { return is_casing(m.kind); });
    if (static_cast<std::size_t>(casings) >= kMaxCasings) {
        marks_.erase(std::find_if(marks_.begin(), marks_.end(), [](const Mark& m) { return is_casing(m.kind); }));
    }
    next_seed_ = seed;
    add(*floor, {at.x, at.y, top}, shell ? MarkKind::Shell : MarkKind::Casing, 1, true);
    return true;
}

// @orig none: port-only impact marks
void DamageMarks::add(const Item& host, World::Point surface, MarkKind kind, int radius, bool flat, double angle) {
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
    m.pixels = std::make_shared<const std::vector<MarkPixel>>(mark_pattern(kind, radius, m.flat, m.seed, angle));
    marks_.push_back(m);
    while (marks_.size() > kMaxMarks) marks_.pop_front();
}

// @orig none: port-only impact marks
void DamageMarks::prepare(const World& world, const Palette& game_palette, bool impacts, bool blood, bool casings) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    std::erase_if(marks_, [&](const Mark& m) {
        if (!(is_blood(m.kind) ? blood : is_casing(m.kind) ? casings : impacts)) return true;
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
        for (int s = 0; s < kDarkShades; ++s) shade_[s] = shade_table(game_palette, kShadePercent[s]);
        for (int s = 0; s < 3; ++s) shade_[kDarkShades + s] = tint_table(game_palette, kBloodTarget[s], kBloodPercent[s]);
        for (int s = 0; s < 5; ++s) shade_[kBrassLight + s] = tint_table(game_palette, kCasingColour[s], 100);
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
