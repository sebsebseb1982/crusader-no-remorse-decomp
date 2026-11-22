#include "engine/world/barrel_leaks.hpp"

#include "core/dtable.hpp"
#include "engine/world/casings.hpp"
#include "engine/world/damage_marks.hpp"

#include <algorithm>
#include <cmath>

namespace crusader {

namespace {

using Solid = BarrelLeaks::Solid;

constexpr std::uint16_t kItemExploded = 0x0040;  // SPRITE_1150_0891 ran (item_damage.cpp)
constexpr double kPi = 3.14159265358979;
constexpr double kGravity = 0.25;     // usecode units a tick per tick, the jet's fall
constexpr double kJet = 0.65;         // jet speed, units a tick, per square root of the head
constexpr double kDrain = 0.006;      // level fall a tick per hole, per square root of the head
constexpr double kFull = 0.9;         // a whole barrel's level, of its height
constexpr double kThickness = 0.45;   // a pool's depth: its area is the fuel run out over it
constexpr double kMaxLand = 220, kMaxFoot = 90, kMaxLeft = 160;  // lobe radii, usecode units
constexpr double kWarp = 26;          // the most a pool's edge is pushed in or out, usecode units
constexpr double kDribble = 0.5;      // below this jet speed, the fuel runs down the side
constexpr double kSpread = 2.5;       // the fire's spread over a pool, units a tick
constexpr double kDropGravity = 0.3;  // the burning drops' fall, units a tick per tick
constexpr std::uint32_t kDropLife = 150;
constexpr float kFlameHeight = 64;    // pixels, the tallest flames
constexpr int kMirrorReach = 120;     // pixels above a pool it may mirror
constexpr int kHeight = 2;            // a floor this close to a pool's level takes it
constexpr int kPad = 256;             // largest footpad reach of an anchor

// The leaks' own numbers (splitmix32), never the game's generators.
// @orig none: port-only barrel leaks
std::uint32_t mix(std::uint32_t v) {
    v += 0x9E3779B9u;
    v = (v ^ (v >> 16)) * 0x85EBCA6Bu;
    v = (v ^ (v >> 13)) * 0xC2B2AE35u;
    return v ^ (v >> 16);
}

std::uint32_t mix(std::uint32_t seed, int a, int b) {
    return mix(seed ^ mix(static_cast<std::uint32_t>(a) * 0x27D4EB2Fu ^ static_cast<std::uint32_t>(b)));
}

double unit(std::uint32_t h) { return (h & 0xFFFF) / 65535.0; }

// Value noise, 0..1, features of one unit, from a table of 256 values.
// @orig none: port-only barrel leaks
float fast_noise(float x, float y, std::uint32_t seed) {
    static const auto table = [] {
        std::array<float, 256> t{};
        std::array<std::uint8_t, 256> perm{};
        for (int i = 0; i < 256; ++i) {
            t[i] = static_cast<float>(unit(mix(static_cast<std::uint32_t>(i) * 0x9E3779B9u + 1)));
            perm[i] = static_cast<std::uint8_t>(mix(static_cast<std::uint32_t>(i) + 77) & 0xFF);
        }
        return std::pair{t, perm};
    }();
    const float fx = std::floor(x), fy = std::floor(y);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const auto at = [&](int i, int j) {
        return table.first[table.second[(table.second[(i + static_cast<int>(seed)) & 0xFF] + j) & 0xFF]];
    };
    const float a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

int screen_x(double x, double y) { return static_cast<int>(std::floor((x - y) / 2)); }
int screen_y(double x, double y, double z) { return static_cast<int>(std::floor((x + y) / 4 - z)); }

bool upright(std::uint16_t shape) { return shape == 0xAA; }

// A fire's strength `age` ticks into its `life`: 0 out .. 1 at its
// height; it takes at once, then dies down for the last 40 %.
// @orig none: port-only barrel leaks
float curve(float age, float life) {
    if (life <= 0 || age >= life) return 0;
    const float ramp = std::min(1.0f, 0.35f + age / 20);
    const float u = age / life;
    const float tail = u > 0.6f ? std::max(0.0f, 1 - (u - 0.6f) / 0.4f) : 1.0f;
    return ramp * (0.35f + 0.65f * tail);
}

// The pool's bounds, usecode units.
struct Bounds {
    double x0 = 0, y0 = 0, x1 = -1, y1 = -1;
};

// @orig none: port-only barrel leaks
Bounds bounds_of(const BarrelLeaks::Pool& p) {
    // The lobes add up: at a distance D from all of them the field is at
    // most the sum of r^2 / D^2, ragged up to 1.2 times, so nothing lies
    // past sqrt(1.2 * sum) from the lobes (plus the warp).
    double sum = 0;
    for (const BarrelLeaks::Lobe& l : p.lobes) sum += l.r * l.r;
    const double reach = std::sqrt(1.2 * sum) + 2 + kWarp;
    Bounds b;
    bool any = false;
    for (const BarrelLeaks::Lobe& l : p.lobes) {
        if (l.r <= 0) continue;
        if (!any) b = {l.x - reach, l.y - reach, l.x + reach, l.y + reach};
        b.x0 = std::min(b.x0, l.x - reach);
        b.y0 = std::min(b.y0, l.y - reach);
        b.x1 = std::max(b.x1, l.x + reach);
        b.y1 = std::max(b.y1, l.y + reach);
        any = true;
    }
    return b;
}

// Where pool `p` is drawn: the box of its fuel once its field is built.
// @orig none: port-only barrel leaks
Bounds wet_bounds(const BarrelLeaks::Pool& p) {
    if (p.field_grid.empty()) return bounds_of(p);
    return {p.wet_x0, p.wet_y0, p.wet_x1, p.wet_y1};
}

double depth_of(double field) { return std::clamp((field - 1) / 2, 0.0, 1.0); }

// @orig none: port-only barrel leaks
std::vector<Solid> solids_around(const World& world, std::int32_t x0, std::int32_t y0, std::int32_t x1,
                                 std::int32_t y1) {
    std::vector<Solid> out;
    for (ObjId id : world.rect_search(x0 - kPad, y0 - kPad, x1 + kPad, y1 + kPad)) {
        const Item* item = world.get(id);
        if (!item || is_npc_id(item->id) || item->parent != 0 || item->off_map || item->sprite_only ||
            leaks_fuel(item->shape) || (item->flags & (kItemInvisible | kItemContained | kItemEthereal)))
            continue;
        const ShapeInfo* info = world.shape_info(item->shape);
        if (!info || info->is(ShapeInfo::kEditor) || info->is(ShapeInfo::kTranslucent)) continue;
        const bool mirrored = item->flags & kItemFlipped;
        out.push_back({item->id, item->x - (mirrored ? info->footpad_y : info->footpad_x) * 16, item->x,
                       item->y - (mirrored ? info->footpad_x : info->footpad_y) * 16, item->y, item->z,
                       item->z + info->footpad_z * 8});
    }
    return out;
}

// The top of the floor under (x, y): the highest scenery whose box holds it
// with its top from `below` under z up to just over it. False when none.
// @orig none: port-only barrel leaks
bool floor_under(const std::vector<Solid>& solids, double x, double y, double z, std::int32_t& top, int below = 16) {
    bool found = false;
    for (const Solid& s : solids) {
        if (x < s.x_left || x > s.x || y < s.y_far || y > s.y || s.z_top > z + kHeight || s.z_top < z - below) continue;
        if (!found || s.z_top > top) top = s.z_top;
        found = true;
    }
    return found;
}

// A pool at level z lies at (x, y): a floor there at that level, nothing
// standing on it (the barrel that leaked aside).
// @orig none: port-only barrel leaks
bool on_floor(const std::vector<Solid>& solids, double x, double y, std::int32_t z) {
    const auto ix = static_cast<std::int32_t>(std::lround(x)), iy = static_cast<std::int32_t>(std::lround(y));
    bool floor = false;
    for (const Solid& s : solids) {
        if (ix < s.x_left || ix > s.x || iy < s.y_far || iy > s.y) continue;
        if (std::abs(s.z_top - z) <= kHeight) floor = true;
        else if (s.z_top > z + kHeight && s.z <= z + 16) return false;  // standing there
    }
    return floor;
}

// The top of the floor under barrel `b` where it stands (its own bottom
// when none), as far down as it lies: a barrel standing on another (the
// barrels are no floor) spills down its side to the floor under both, one
// in the air (thrown) is over the floor it falls to.
// @orig none: port-only barrel leaks
std::int32_t floor_of(const World& world, const BarrelLeaks::Barrel& b) {
    const auto cx = static_cast<std::int32_t>(b.x - b.length_x / 2);
    const auto cy = static_cast<std::int32_t>(b.y - b.length_y / 2);
    std::int32_t top = 0;
    return floor_under(solids_around(world, cx, cy, cx, cy), cx, cy, b.z, top, 256) ? top : b.z;
}

// A fuel barrel as it stands, full.
// @orig none: port-only barrel leaks
BarrelLeaks::Barrel make_barrel(const World& world, const Item& item, const ShapeInfo& info) {
    BarrelLeaks::Barrel b;
    b.id = item.id;
    b.shape = item.shape;
    b.frame = item.frame;
    b.x = item.x;
    b.y = item.y;
    b.z = item.z;
    const bool mirrored = item.flags & kItemFlipped;
    b.length_x = (mirrored ? info.footpad_y : info.footpad_x) * 16.0;
    b.length_y = (mirrored ? info.footpad_x : info.footpad_y) * 16.0;
    b.height = info.footpad_z * 8.0;
    b.radius = upright(b.shape) ? std::min(b.length_x, b.length_y) / 2 : b.height / 2;
    b.level = b.height * kFull;
    b.floor = floor_of(world, b);
    return b;
}

// Fingers round lobe `anchor` of `p`, in directions about `towards`
// (radians) give or take `spread`.
// @orig none: port-only barrel leaks
void add_fingers(BarrelLeaks::Pool& p, int anchor, int count, double towards, double spread, std::uint32_t seed) {
    for (int f = 0; f < count; ++f) {
        const std::uint32_t h = mix(seed, f, 9);
        const double angle = towards + (unit(h) - 0.5) * 2 * spread;
        const double length = 0.7 + 0.55 * unit(h >> 8);
        BarrelLeaks::Lobe l;
        l.anchor = anchor;
        l.ox = std::cos(angle) * length;
        l.oy = std::sin(angle) * length;
        l.frac = 0.28 + 0.27 * unit(mix(h));
        p.lobes.push_back(l);
    }
}

// The fingers follow their lobes.
// @orig none: port-only barrel leaks
void follow_fingers(BarrelLeaks::Pool& p) {
    for (BarrelLeaks::Lobe& l : p.lobes) {
        if (l.anchor < 0 || static_cast<std::size_t>(l.anchor) >= p.lobes.size()) continue;
        const BarrelLeaks::Lobe& a = p.lobes[static_cast<std::size_t>(l.anchor)];
        l.r = l.frac * a.r;
        l.x = a.x + l.ox * a.r;
        l.y = a.y + l.oy * a.r;
    }
}

}  // namespace

// @orig none: port-only barrel leaks
bool is_fuel_barrel(std::uint16_t shape) { return shape == 0xAA || (shape >= 0x151 && shape <= 0x155); }

// @orig none: port-only barrel leaks
bool is_rolling_barrel(std::uint16_t shape) { return shape == 0x586 || shape == 0x58B; }

// @orig none: port-only barrel leaks
bool leaks_fuel(std::uint16_t shape) { return is_fuel_barrel(shape) || is_rolling_barrel(shape); }

// @orig none: port-only barrel leaks
int barrel_bullet_damage(std::uint16_t shape, int damage, int fire_type) {
    const bool bullet = mark_kind(fire_type) == MarkKind::Bullet || ejection_of(fire_type) != Ejection::None;
    if (!is_fuel_barrel(shape) || !bullet) return damage;
    return std::max(1, damage * 3 / 5);
}

// @orig none: port-only barrel leaks
double BarrelLeaks::field(const Pool& p, double x, double y) {
    double extent = 0;
    for (const Lobe& l : p.lobes) extent = std::max(extent, l.r);
    // The edge pushed in and out by broad noise, so that the pool is no
    // round blob, then ragged on a finer scale.
    const double warp = std::min(kWarp, 0.3 * extent);
    const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
    const double ux = 2 * fast_noise(fx / 40, fy / 40, p.seed ^ 0x51u) - 1;
    const double uy = 2 * fast_noise(fx / 40 + 13.7f, fy / 40 + 7.1f, p.seed ^ 0x52u) - 1;
    double s = 0;
    for (const Lobe& l : p.lobes) {
        if (l.r <= 0) continue;
        const double w = l.trail ? std::min(warp, 0.3 * l.r) : warp;
        const double dx = x + w * ux - l.x, dy = y + w * uy - l.y;
        s += l.r * l.r / (dx * dx + dy * dy + 1);
    }
    return s * (0.8 + 0.28 * fast_noise(fx / 9, fy / 9, p.seed) + 0.12 * fast_noise(fx / 3.5f, fy / 3.5f, p.seed ^ 3));
}

// The pool's field on kGrid cells (cell corners), for drawing: the edge
// interpolated between them stays smooth.
// @orig none: port-only barrel leaks
void BarrelLeaks::build_field(Pool& p, std::uint32_t tick) {
    p.field_tick = tick;
    p.field_dirty = false;
    const Bounds bb = bounds_of(p);
    if (bb.x1 < bb.x0) {
        p.field_grid.clear();
        return;
    }
    p.field_x = std::floor(bb.x0 / kGrid) * kGrid;
    p.field_y = std::floor(bb.y0 / kGrid) * kGrid;
    p.field_w = static_cast<int>(std::ceil((bb.x1 - p.field_x) / kGrid)) + 2;
    p.field_h = static_cast<int>(std::ceil((bb.y1 - p.field_y) / kGrid)) + 2;
    p.field_grid.resize(static_cast<std::size_t>(p.field_w) * p.field_h);
    // field_at reaches 1 only in the cells round a corner of 1 or more.
    int i0 = p.field_w, j0 = p.field_h, i1 = -1, j1 = -1;
    for (int j = 0; j < p.field_h; ++j) {
        for (int i = 0; i < p.field_w; ++i) {
            const float s = static_cast<float>(field(p, p.field_x + i * kGrid, p.field_y + j * kGrid));
            p.field_grid[static_cast<std::size_t>(j) * p.field_w + i] = s;
            if (s < 1) continue;
            i0 = std::min(i0, i);
            j0 = std::min(j0, j);
            i1 = std::max(i1, i);
            j1 = std::max(j1, j);
        }
    }
    p.wet_x0 = p.field_x + (i0 - 1) * kGrid;
    p.wet_y0 = p.field_y + (j0 - 1) * kGrid;
    p.wet_x1 = i1 < 0 ? p.wet_x0 - 1 : p.field_x + (i1 + 1) * kGrid;
    p.wet_y1 = j1 < 0 ? p.wet_y0 - 1 : p.field_y + (j1 + 1) * kGrid;
}

// @orig none: port-only barrel leaks
double BarrelLeaks::field_at(const Pool& p, double x, double y) {
    if (p.field_grid.empty()) return field(p, x, y);
    const double gx = (x - p.field_x) / kGrid, gy = (y - p.field_y) / kGrid;
    const int i = static_cast<int>(std::floor(gx)), j = static_cast<int>(std::floor(gy));
    if (i < 0 || j < 0 || i + 1 >= p.field_w || j + 1 >= p.field_h) return 0;
    const double tx = gx - i, ty = gy - j;
    const float* row = &p.field_grid[static_cast<std::size_t>(j) * p.field_w + i];
    const double top = row[0] + (row[1] - row[0]) * tx;
    const double bottom = row[p.field_w] + (row[p.field_w + 1] - row[p.field_w]) * tx;
    return top + (bottom - top) * ty;
}

const BarrelLeaks::Pool* BarrelLeaks::pool(std::uint32_t serial) const {
    for (const Pool& p : pools_)
        if (p.serial == serial) return &p;
    return nullptr;
}

BarrelLeaks::Pool* BarrelLeaks::pool_of(std::uint32_t serial) {
    for (Pool& p : pools_)
        if (p.serial == serial) return &p;
    return nullptr;
}

// A new, empty pool at level z (past kMaxPools, the oldest burnt stain
// goes, else the oldest that does not burn). The other pools may move.
// @orig none: port-only barrel leaks
BarrelLeaks::Pool& BarrelLeaks::new_pool(std::int32_t z) {
    if (pools_.size() >= kMaxPools) {
        // A burnt stain first, then fuel that does not burn.
        auto old = std::find_if(pools_.begin(), pools_.end(), [](const Pool& q) { return q.burnt; });
        if (old == pools_.end())
            old = std::find_if(pools_.begin(), pools_.end(), [](const Pool& q) { return !q.burning; });
        pools_.erase(old != pools_.end() ? old : pools_.begin());
    }
    Pool fresh;
    fresh.serial = next_serial_++;
    fresh.z = z;
    next_seed_ = mix(next_seed_);
    fresh.seed = next_seed_;
    pools_.push_back(fresh);
    return pools_.back();
}

bool BarrelLeaks::on_fire(ObjId id) const {
    return std::any_of(burning_.begin(), burning_.end(), [&](const Burning& b) { return b.id == id; });
}

// @orig none: port-only barrel leaks
void BarrelLeaks::shot(const World& world, ObjId hit, World::Point at, int fire_type, std::uint32_t now) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    const MarkKind kind = mark_kind(fire_type);
    const bool bullet = kind == MarkKind::Bullet || ejection_of(fire_type) != Ejection::None;
    if (kind == MarkKind::Scorch || (kind == MarkKind::Burn && !bullet)) {
        // A blast within reach of a pool, an energy shot on one: it catches.
        const dtable::FireType* ft = dtable::fire_type(fire_type);
        const double reach = kind == MarkKind::Scorch ? (ft ? ft->splash() : 0) * 16.0 + 8 : 0;
        for (Pool& p : pools_) {
            if (p.burning || p.burnt || std::abs(at.z - p.z) > (kind == MarkKind::Scorch ? 48 : 16)) continue;
            bool near = field_at(p, at.x, at.y) >= 1;
            for (const Lobe& l : p.lobes) {
                const double dx = at.x - l.x, dy = at.y - l.y;
                near = near || (reach > 0 && l.r > 0 && std::sqrt(dx * dx + dy * dy) <= reach + l.r);
            }
            if (near) ignite(world, p, at.x, at.y, now);
        }
        if (kind == MarkKind::Scorch) return;
    }
    if (!bullet) return;
    const Item* item = world.get(hit);
    if (!item || !leaks_fuel(item->shape) || (item->flags & kItemExploded)) return;
    const ShapeInfo* info = world.shape_info(item->shape);
    if (!info) return;
    auto found = std::find_if(barrels_.begin(), barrels_.end(),
                              [&](const Barrel& b) { return b.id == hit && b.shape == item->shape; });
    if (found == barrels_.end()) {
        if (barrels_.size() >= kMaxBarrels) barrels_.erase(barrels_.begin());
        barrels_.push_back(make_barrel(world, *item, *info));
        found = barrels_.end() - 1;
    }
    Barrel& b = *found;
    if (b.holes.size() >= kMaxHoles || b.height <= 2 || b.length_x <= 0 || b.length_y <= 0) return;
    const double cx = b.x - b.length_x / 2, cy = b.y - b.length_y / 2;
    Hole h;
    double hx = 0, hy = 0;
    if (upright(b.shape)) {
        // On the round side, across from where the shot came.
        double ux = at.x - cx, uy = at.y - cy;
        const double len = std::sqrt(ux * ux + uy * uy);
        if (len < 1) ux = uy = std::sqrt(0.5);
        else ux /= len, uy /= len;
        h.ux = ux;
        h.uy = uy;
        hx = cx + ux * b.radius;
        hy = cy + uy * b.radius;
        h.seen = ux + uy > 0.1;
    } else {
        hx = std::clamp(static_cast<double>(at.x), b.x - b.length_x, static_cast<double>(b.x));
        hy = std::clamp(static_cast<double>(at.y), b.y - b.length_y, static_cast<double>(b.y));
        double ux = hx - cx, uy = hy - cy;
        const double len = std::sqrt(ux * ux + uy * uy);
        if (len < 1) ux = uy = std::sqrt(0.5);
        else ux /= len, uy /= len;
        h.ux = ux;
        h.uy = uy;
        h.seen = hx >= b.x - 4 || hy >= b.y - 4;
    }
    h.x = hx - b.x;
    h.y = hy - b.y;
    // A shot on the top makes a hole above the fuel: it lets nothing out.
    h.height = at.z >= b.z + b.height - 1 ? b.height : std::clamp(static_cast<double>(at.z - b.z), 1.0, b.height - 1);
    h.made = now;
    next_seed_ = mix(next_seed_);
    h.seed = next_seed_;
    b.holes.push_back(h);
}

// @orig none: port-only barrel leaks
void BarrelLeaks::exploded(const World& world, ObjId id, std::uint32_t now) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    const Item* item = world.get(id);
    if (!item || !leaks_fuel(item->shape)) return;
    const ShapeInfo* info = world.shape_info(item->shape);
    if (!info) return;
    Barrel b;
    auto found = std::find_if(barrels_.begin(), barrels_.end(),
                              [&](const Barrel& t) { return t.id == id && t.shape == item->shape; });
    if (found != barrels_.end()) {
        b = *found;
        barrels_.erase(found);
    } else {
        b = make_barrel(world, *item, *info);
    }
    blow_up(world, b, now);
}

// The barrel blew up: of what was left in it, most burns in the blast, some
// (kLeft) runs out where it stood (spreading out over kSpreadTicks) and its
// pool catches fire from there, some (kThrown) flies all round in burning
// drops, which splash where they land.
// @orig none: port-only barrel leaks
void BarrelLeaks::blow_up(const World& world, const Barrel& b, std::uint32_t tick) {
    const double cx = b.x - b.length_x / 2, cy = b.y - b.length_y / 2;
    const double area = b.length_x * b.length_y / kPi;
    // Its pool, unless it rolled down to another level since: then that one
    // is part of its trail.
    std::vector<std::uint32_t> trail = b.trail_pools;
    Pool* p = pool_of(b.pool);
    if (p && !p->burnt && std::abs(p->z - b.floor) > kHeight) {
        trail.push_back(p->serial);
        p = nullptr;
    }
    if (!p || p->burnt) p = &new_pool(b.floor);
    const std::uint32_t seed = mix(p->seed, static_cast<int>(b.id), static_cast<int>(tick));
    if (b.level > 1) {
        const int anchor = static_cast<int>(p->lobes.size());
        const double to = std::min(kMaxLeft, 0.6 * std::sqrt(area * b.level * kLeft / kThickness));
        p->lobes.push_back({cx, cy, to});
        add_fingers(*p, anchor, 4, 0, kPi, seed);
        follow_fingers(*p);
        // The fire laid over all it will cover (its front never overtakes
        // the fuel spreading out), the pool drawn small at first.
        ignite(world, *p, cx, cy, tick);
        spreads_.push_back({p->serial, static_cast<std::size_t>(anchor), to, tick});
        spread(world, tick);
    } else {
        ignite(world, *p, cx, cy, tick);
    }
    // Its trail at other levels catches too, from its end nearest the barrel.
    for (std::uint32_t serial : trail) {
        Pool* q = pool_of(serial);
        if (!q || q->burning || q->burnt || q->lobes.empty()) continue;
        const Lobe* near = &q->lobes.front();
        for (const Lobe& l : q->lobes) {
            if ((l.x - cx) * (l.x - cx) + (l.y - cy) * (l.y - cy) <
                (near->x - cx) * (near->x - cx) + (near->y - cy) * (near->y - cy))
                near = &l;
        }
        ignite(world, *q, near->x, near->y, tick);
    }
    // The drops: more the fuller the barrel, all round (a turn shared out
    // among them, give or take), sharing what it throws by their size.
    if (b.height <= 0) return;
    const auto solids = std::make_shared<const std::vector<Solid>>(
        solids_around(world, static_cast<std::int32_t>(cx) - 320, static_cast<std::int32_t>(cy) - 320,
                      static_cast<std::int32_t>(cx) + 320, static_cast<std::int32_t>(cy) + 320));
    const int count = 12 + static_cast<int>(20 * std::clamp(b.level / b.height, 0.0, 1.0));
    double weights = 0;
    for (int k = 0; k < count; ++k) {
        const double size = 0.5 + 0.9 * unit(mix(mix(seed, k, 31), 2, 2));
        weights += size * size;
    }
    const double thrown = b.level > 1 ? area * b.level * kThrown : 0;
    const double turn = unit(mix(seed, 7, 7)) * 2 * kPi;
    for (int k = 0; k < count; ++k) {
        const std::uint32_t h = mix(seed, k, 31);
        const double angle = turn + (k + 0.8 * (unit(h) - 0.5)) * 2 * kPi / count;
        const double speed = 1.2 + 4.4 * unit(h >> 8);
        Drop d;
        d.x = cx;
        d.y = cy;
        d.z = b.z + b.height * 0.6;
        d.vx = std::cos(angle) * speed;
        d.vy = std::sin(angle) * speed;
        d.vz = 1.5 + 4.5 * unit(mix(h, 1, 1));
        d.born = tick;
        d.seed = mix(h);
        d.size = static_cast<float>(0.5 + 0.9 * unit(mix(h, 2, 2)));
        d.fuel = weights > 0 ? thrown * d.size * d.size / weights : 0;
        d.solids = solids;
        if (drops_.size() >= kMaxDrops) drops_.erase(drops_.begin());
        drops_.push_back(d);
    }
}

// @orig none: port-only barrel leaks
void BarrelLeaks::advance(const World& world, std::uint32_t now) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    if (started_ && now < now_) clear();  // a load: the clock went back
    if (!started_) {
        started_ = true;
        now_ = now;
    }
    std::uint32_t from = now_;
    if (now - from > 600) from = now - 600;
    for (std::uint32_t t = from + 1; t <= now && t != 0; ++t) step(world, t);
    now_ = now;
}

// @orig none: port-only barrel leaks
std::vector<ObjId> BarrelLeaks::take_burns() {
    std::vector<ObjId> out;
    out.swap(burns_);
    return out;
}

// @orig none: port-only barrel leaks
void BarrelLeaks::step(const World& world, std::uint32_t tick) {
    for (std::size_t i = 0; i < barrels_.size();) {
        Barrel& b = barrels_[i];
        const Item* item = world.get(b.id);
        if (!item || item->shape != b.shape || (item->flags & kItemExploded)) {
            // Gone without the game saying it blew up: as if it had.
            const Barrel gone = b;
            barrels_.erase(barrels_.begin() + static_cast<std::ptrdiff_t>(i));
            blow_up(world, gone, tick);
            continue;
        }
        // Thrown: it leaves its pool and lays a trail from where it was.
        const bool slid = item->x != b.x || item->y != b.y;
        const bool moved = slid || item->z != b.z;
        if (slid && !b.rolling) {
            b.rolling = true;
            b.trail_x = b.x - b.length_x / 2;
            b.trail_y = b.y - b.length_y / 2;
            for (Hole& h : b.holes) h.land_lobe = h.foot_lobe = -1, h.spilled = 0;
        }
        if (slid) b.moved_at = tick;
        b.x = item->x;
        b.y = item->y;
        b.z = item->z;
        b.frame = item->frame;
        if (moved) b.floor = floor_of(world, b);
        if (b.rolling && tick - b.moved_at >= kRest) {
            // At rest: the jets make a pool where it lies.
            b.rolling = false;
            for (Hole& h : b.holes)
                if (h.started && b.level > h.height) place_jet(b, h);
        }
        for (Hole& h : b.holes) {
            h.flow = 0;
            if (!h.started && tick - h.made >= kLeakDelay) {
                h.started = true;
                if (b.level > h.height && !b.rolling) place_jet(b, h);
            }
            if (!h.started) continue;
            // Rolling, a lying barrel turns every hole under the fuel.
            const double head = b.level - (b.rolling && !upright(b.shape) ? 0 : h.height);
            if (head <= 0) continue;
            const double out = std::min(head, kDrain * std::sqrt(head));
            b.level -= out;
            if (b.rolling) h.flow = out;
            else h.spilled += out;
        }
        if (b.rolling) lay_trail(b);
        grow(b);
        ++i;
    }
    spread(world, tick);
    step_drops(world, tick);
    for (Pool& p : pools_) {
        if (!p.burning) continue;
        if (tick - p.lit_at >= p.burn_ticks) {
            p.burning = false;
            p.burnt = true;
            continue;
        }
        catch_fire(world, p, tick);
        // A burning pool lights the pools it touches, once its fire has come
        // where they touch (a trail in several pools burns on from one to
        // the next).
        if ((tick - p.lit_at) % 8 != 0 || tick - p.lit_at < 16) continue;
        for (Pool& q : pools_) {
            if (&q == &p || q.burning || q.burnt || std::abs(q.z - p.z) > 4) continue;
            for (const Lobe& a : p.lobes) {
                for (const Lobe& c : q.lobes) {
                    if (q.burning) break;
                    const double dx = a.x - c.x, dy = a.y - c.y;
                    // Two drops of a trail touch across a gap of its dribble.
                    const double gap = a.trail && c.trail ? 2 * kTrailStep : 0;
                    if (a.r > 0 && c.r > 0 && std::sqrt(dx * dx + dy * dy) < a.r + c.r + gap &&
                        reached(p, a.x, a.y, tick) > 0)
                        ignite(world, q, c.x, c.y, tick);
                }
            }
        }
    }
    step_heating(world, tick);
    // The fires on walls whose wall went.
    std::erase_if(wall_fires_, [&](const WallFire& w) {
        const Item* host = world.get(w.host);
        return !host || host->shape != w.shape || host->frame != w.frame || host->x != w.ax || host->y != w.ay ||
               host->z != w.az;
    });
    // The people on fire burn, every kBurnEvery ticks, until they are out.
    for (std::size_t i = 0; i < burning_.size();) {
        Burning& b = burning_[i];
        const Item* item = world.get(b.id);
        if (!item || world.is_dead(b.id) || tick >= b.until) {
            burning_.erase(burning_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (tick >= b.next) {
            burns_.push_back(b.id);
            b.next = tick + kBurnEvery;
        }
        ++i;
    }
}

// The burning drops fly on: one that meets someone sets them alight, one
// that meets a floor, the top of something or a wall the camera sees
// splashes there, burning.
// @orig none: port-only barrel leaks
void BarrelLeaks::step_drops(const World& world, std::uint32_t tick) {
    for (std::size_t i = 0; i < drops_.size();) {
        Drop d = drops_[i];
        if (tick - d.born > kDropLife || !d.solids) {
            drops_.erase(drops_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const double px = d.x, py = d.y, pz = d.z;
        d.vz -= kDropGravity;
        d.x += d.vx;
        d.y += d.vy;
        d.z += d.vz;
        drops_[i] = d;
        bool ended = false;
        const auto ix = static_cast<std::int32_t>(d.x), iy = static_cast<std::int32_t>(d.y);
        for (ObjId id : world.rect_search(ix - 8, iy - 8, ix + 64, iy + 64)) {
            if (!is_npc_id(id) || world.is_dead(id)) continue;
            const Item* npc = world.get(id);
            const ShapeInfo* info = npc ? world.shape_info(npc->shape) : nullptr;
            if (!info || npc->off_map || npc->parent != 0) continue;
            if (d.x < npc->x - info->footpad_x * 16 || d.x > npc->x || d.y < npc->y - info->footpad_y * 16 ||
                d.y > npc->y || d.z < npc->z || d.z > npc->z + info->footpad_z * 8)
                continue;
            auto found = std::find_if(burning_.begin(), burning_.end(), [&](const Burning& b) { return b.id == id; });
            if (found == burning_.end()) burning_.push_back({id, tick + 2 * kAfterBurn, tick + 2});
            else found->until = std::max(found->until, tick + 2 * kAfterBurn);
            ended = true;
            break;
        }
        for (const Solid& s : *d.solids) {
            if (ended) break;
            if (s.z_top - s.z < 8 || d.x < s.x_left || d.x > s.x || d.y < s.y_far || d.y > s.y || d.z < s.z ||
                d.z > s.z_top)
                continue;
            ended = true;
            if (pz >= s.z_top - 1) splash_floor(world, d, d.x, d.y, s.z_top, tick);
            else if (px > s.x) splash_wall(world, d, s, 0, s.x, d.y, d.z, tick);
            else if (py > s.y) splash_wall(world, d, s, 1, d.x, s.y, d.z, tick);
        }
        std::int32_t top = 0;
        if (!ended && floor_under(*d.solids, d.x, d.y, pz, top, 64) && d.z <= top) {
            splash_floor(world, d, d.x, d.y, top, tick);
            ended = true;
        }
        if (ended) drops_.erase(drops_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// A drop lands on a floor: a pool there catches; elsewhere what it carries
// splashes in a little pool of its own, burning: a blot, drawn out ahead
// along its way the faster it flew, droplets round it.
// @orig none: port-only barrel leaks
void BarrelLeaks::splash_floor(const World& world, const Drop& d, double x, double y, std::int32_t z,
                               std::uint32_t tick) {
    for (Pool& p : pools_) {
        if (p.burnt || std::abs(p.z - z) > kHeight || field_at(p, x, y) < 1) continue;
        if (!p.burning) ignite(world, p, x, y, tick);
        return;
    }
    Pool& p = new_pool(z);
    const double r = d.fuel > 0 ? std::clamp(0.6 * std::sqrt(d.fuel / kThickness), 5.0, 34.0) : 5 + 9.0 * d.size;
    p.lobes.push_back({x, y, r});
    const double run = std::hypot(d.vx, d.vy);
    if (run > 0.5) {
        const double ux = d.vx / run, uy = d.vy / run;
        const int streak = std::min(3, 1 + static_cast<int>(run / 1.8));
        double along = 0, size = r;
        for (int k = 0; k < streak; ++k) {
            along += 0.85 * size;
            size *= 0.68;
            const double sway = (unit(mix(d.seed, k, 3)) - 0.5) * 0.5 * size;
            p.lobes.push_back({x + ux * along - uy * sway, y + uy * along + ux * sway, size});
        }
        add_fingers(p, 0, 3, std::atan2(uy, ux), 1.2, d.seed);
    } else {
        add_fingers(p, 0, 2, unit(d.seed) * 2 * kPi, kPi, d.seed);
    }
    follow_fingers(p);
    ignite(world, p, x, y, tick);
    p.burn_ticks = std::min<std::uint32_t>(p.burn_ticks, 140 + static_cast<std::uint32_t>(unit(d.seed >> 4) * 160));
}

// A drop hits a wall: it burns there a while, then leaves a stain.
// @orig none: port-only barrel leaks
void BarrelLeaks::splash_wall(const World& world, const Drop& d, const Solid& s, int face, double x, double y,
                              double z, std::uint32_t tick) {
    const Item* host = world.get(s.id);
    if (!host) return;
    if (wall_fires_.size() >= kMaxWallFires) wall_fires_.erase(wall_fires_.begin());
    WallFire w;
    w.host = s.id;
    w.shape = host->shape;
    w.frame = host->frame;
    w.ax = host->x;
    w.ay = host->y;
    w.az = host->z;
    w.face = face;
    w.x = x;
    w.y = y;
    w.z = z;
    w.r = static_cast<float>(8 + 10 * d.size);
    w.lit_at = tick;
    w.burn_ticks = 150 + static_cast<std::uint32_t>(unit(d.seed >> 3) * 150);
    w.seed = d.seed;
    wall_fires_.push_back(w);
}

// Whoever stands in the fire of `p` catches fire; a fuel barrel standing
// in it heats up.
// @orig none: port-only barrel leaks
void BarrelLeaks::catch_fire(const World& world, const Pool& p, std::uint32_t tick) {
    // The fire's cells, and the feet round them.
    if (p.fuel.empty()) return;
    const auto x0 = static_cast<std::int32_t>(p.grid_x) - 8, y0 = static_cast<std::int32_t>(p.grid_y) - 8;
    const auto x1 = static_cast<std::int32_t>(p.grid_x + p.grid_w * kGrid) + 8;
    const auto y1 = static_cast<std::int32_t>(p.grid_y + p.grid_h * kGrid) + 8;
    for (ObjId id : world.rect_search(x0, y0, x1 + kPad, y1 + kPad)) {
        const bool npc = is_npc_id(id);
        const Item* item = world.get(id);
        if (!item || item->off_map || item->parent != 0) continue;
        const bool barrel = !npc && is_fuel_barrel(item->shape) && !(item->flags & kItemExploded);
        if ((!npc && !barrel) || (npc && world.is_dead(id))) continue;
        if (item->z < p.z - kHeight || item->z > p.z + 16) continue;
        const ShapeInfo* info = world.shape_info(item->shape);
        const double half_x = (info ? info->footpad_x : 0) * 8.0, half_y = (info ? info->footpad_y : 0) * 8.0;
        const double cx = item->x - half_x, cy = item->y - half_y;
        // The feet: the middle and four points around it. A barrel's foot:
        // the floor round it (none lies under it), at its sides and corners.
        double hottest = fire_at(p, cx, cy, tick);
        if (barrel) {
            static constexpr int kSides[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            for (const auto& a : kSides) {
                const double out = a[0] && a[1] ? 2 : 4;
                hottest = std::max(hottest, fire_at(p, cx + a[0] * (half_x + out), cy + a[1] * (half_y + out), tick));
            }
            if (hottest >= 0.25) heat_barrel(*item, tick);
            continue;
        }
        static constexpr int kAround[4][2] = {{6, 0}, {-6, 0}, {0, 6}, {0, -6}};
        for (const auto& a : kAround) hottest = std::max(hottest, fire_at(p, cx + a[0], cy + a[1], tick));
        if (hottest < 0.25) continue;
        auto found = std::find_if(burning_.begin(), burning_.end(), [&](const Burning& b) { return b.id == id; });
        if (found == burning_.end()) burning_.push_back({id, tick + kAfterBurn, tick + 4});
        else found->until = std::max(found->until, tick + kAfterBurn);
    }
}

// Fuel barrel `item` stands in a fire at game tick `tick`.
// @orig none: port-only barrel leaks
void BarrelLeaks::heat_barrel(const Item& item, std::uint32_t tick) {
    auto found = std::find_if(heating_.begin(), heating_.end(),
                              [&](const Heating& h) { return h.id == item.id && h.shape == item.shape; });
    if (found == heating_.end()) {
        if (heating_.size() >= kMaxBarrels) heating_.erase(heating_.begin());
        Heating h;
        h.id = item.id;
        h.shape = item.shape;
        h.fuse = static_cast<float>(kCookMin + unit(mix(next_seed_, static_cast<int>(item.id), 11)) * kCookMore);
        heating_.push_back(h);
        found = heating_.end() - 1;
    }
    found->seen = tick;
}

// The barrels in a fire heat up (one tick of heat a tick, however many
// fires), those out of it cool off at half that pace; one hot enough blows
// up.
// @orig none: port-only barrel leaks
void BarrelLeaks::step_heating(const World& world, std::uint32_t tick) {
    for (std::size_t i = 0; i < heating_.size();) {
        Heating& h = heating_[i];
        const Item* item = world.get(h.id);
        bool gone = !item || item->shape != h.shape || (item->flags & kItemExploded);
        if (!gone) {
            h.heat += h.seen == tick ? 1.0f : -0.5f;
            if (h.heat >= h.fuse) {
                cook_offs_.push_back(h.id);
                gone = true;
            }
            gone = gone || h.heat <= 0;
        }
        if (gone) heating_.erase(heating_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// @orig none: port-only barrel leaks
std::vector<ObjId> BarrelLeaks::take_cook_offs() {
    std::vector<ObjId> out;
    out.swap(cook_offs_);
    return out;
}

// The pool barrel `b` spills into: its own, or a new one at the level of
// its floor when it has none, its own burnt or at another level (the
// barrel moved), or (for a trail) its own burns or holds kMaxTrail drops
// already (the trail goes on in a new pool that touches it: it catches from
// it). A new pool leaves the holes without lobes.
// @orig none: port-only barrel leaks
BarrelLeaks::Pool& BarrelLeaks::pool_for(Barrel& b, bool trail) {
    Pool* p = pool_of(b.pool);
    const bool level = p && std::abs(p->z - b.floor) <= kHeight;
    bool full = false;
    if (trail && p) {
        std::size_t laid = 0;
        for (const Lobe& l : p->lobes) laid += l.trail;
        full = laid >= kMaxTrail;
    }
    if (p && !p->burnt && level && !(trail && (p->burning || full))) return *p;
    if (p && !p->burnt && !p->burning && !level) {
        // Its trail goes on at another level: that one catches with it too.
        if (b.trail_pools.size() >= 8) b.trail_pools.erase(b.trail_pools.begin());
        b.trail_pools.push_back(p->serial);
    }
    Pool& fresh = new_pool(b.floor);
    b.pool = fresh.serial;
    for (Hole& h : b.holes) h.land_lobe = h.foot_lobe = -1, h.spilled = 0;
    return fresh;
}

// The lobes of hole `h` in the barrel's pool: where its jet lands now, at
// the foot below it, and fingers running on from where it lands.
// @orig none: port-only barrel leaks
void BarrelLeaks::place_jet(Barrel& b, Hole& h) {
    Pool& p = pool_for(b, false);
    const double v = kJet * std::sqrt(b.level - h.height);
    const double fall = std::sqrt(2 * std::max(0.0, h.height + b.z - b.floor) / kGravity);
    const double reach = v >= kDribble ? v * fall : 1;
    const double hx = b.x + h.x, hy = b.y + h.y;
    h.spilled = 0;
    h.land_lobe = static_cast<int>(p.lobes.size());
    p.lobes.push_back({hx + h.ux * (reach + 2), hy + h.uy * (reach + 2), 0});
    h.foot_lobe = static_cast<int>(p.lobes.size());
    p.lobes.push_back({hx + h.ux * 3, hy + h.uy * 3, 0});
    add_fingers(p, h.land_lobe, 3, std::atan2(h.uy, h.ux), 1.7, h.seed);
}

// A rolling barrel: each hole that let fuel out this tick drops it every
// kTrailStep along its way since the trail's end, wider the more it lets
// out: an upright barrel at the foot of the side under the hole (where the
// fuel runs down to), a lying one under itself, where the hole lies along
// its length (the hole turns round with it). Off the floor (falling) or dry,
// it lays none.
// @orig none: port-only barrel leaks
void BarrelLeaks::lay_trail(Barrel& b) {
    const double cx = b.x - b.length_x / 2, cy = b.y - b.length_y / 2;
    const double dx = cx - b.trail_x, dy = cy - b.trail_y;
    const double len = std::sqrt(dx * dx + dy * dy);
    const bool flows = std::any_of(b.holes.begin(), b.holes.end(), [](const Hole& h) { return h.flow > 0; });
    if (!flows || b.z > b.floor + kHeight) {
        b.trail_x = cx;
        b.trail_y = cy;
        return;
    }
    if (len < kTrailStep) return;
    const int steps = static_cast<int>(len / kTrailStep);
    const double turn = 2 * kPi * std::max(b.radius, 4.0);
    for (int k = 1; k <= steps; ++k) {
        const double s = b.trail_way + k * kTrailStep;
        const double ax = b.trail_x + dx * (k * kTrailStep / len), ay = b.trail_y + dy * (k * kTrailStep / len);
        for (const Hole& h : b.holes) {
            if (h.flow <= 0) continue;
            double ox = b.x + h.x + h.ux * 3 - cx, oy = b.y + h.y + h.uy * 3 - cy;
            if (!upright(b.shape)) {
                ox = b.length_x > b.length_y ? b.x + h.x - cx : 0;
                oy = b.length_y > b.length_x ? b.y + h.y - cy : 0;
            }
            // The drops add up along the way: some 2 r^2 / kTrailStep wide.
            // Uneven: the barrel wobbles, so the trail wanders from side to
            // side (up to 14 units, over some 120), and the drops are wider
            // or narrower. A lying barrel brings the hole round under itself
            // about once a turn: a splash there, a broken dribble while the
            // hole is up.
            const double r = std::clamp(2.5 + 50 * h.flow, 3.0, 4.5);
            const std::uint32_t n = mix(h.seed, static_cast<int>(s), 17);
            const double side = 14 * (2 * fast_noise(static_cast<float>(s / 120), 0.5f, h.seed) - 1) +
                                (unit(n >> 16) - 0.5) * 3;
            double size = 0.75 + 0.5 * unit(n);
            bool splash = false;
            if (!upright(b.shape)) {
                const double wobble = 0.3 * fast_noise(static_cast<float>(s / 150), 3.5f, h.seed ^ 0x5Au);
                const double down = std::cos(2 * kPi * (s / turn + unit(h.seed >> 8) + wobble));
                size *= 0.5 + 0.9 * std::max(0.0, down);
                splash = down > 0.97;
            }
            Pool& p = pool_for(b, true);
            Lobe l;
            l.x = ax + ox - dy / len * side;
            l.y = ay + oy + dx / len * side;
            l.r = r * size;
            l.trail = true;
            p.lobes.push_back(l);
            if (splash) {
                // Thrown out a little ahead and aside.
                const double off = (unit(n >> 8) - 0.5) * 10;
                l.x += dx / len * 3 - dy / len * off;
                l.y += dy / len * 3 + dx / len * off;
                l.r = r * (1.5 + 0.7 * unit(n >> 4));
                p.lobes.push_back(l);
            }
            p.field_dirty = true;
        }
    }
    b.trail_x += dx * (steps * kTrailStep / len);
    b.trail_y += dy * (steps * kTrailStep / len);
    b.trail_way += steps * kTrailStep;
}

// The lobes of a barrel's pool, from what ran out of each hole.
// The pools of the barrels blown up spread out, fast then slower, over the
// fire laid when they caught: only what is drawn of them grows.
// @orig none: port-only barrel leaks
void BarrelLeaks::spread(const World&, std::uint32_t tick) {
    for (std::size_t i = 0; i < spreads_.size();) {
        const Spread& s = spreads_[i];
        Pool* p = pool_of(s.pool);
        if (!p || p->burnt || s.lobe >= p->lobes.size()) {
            spreads_.erase(spreads_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // Drawn anew every few ticks: a big pool costs.
        const double u = std::min(1.0, static_cast<double>(tick - s.from) / kSpreadTicks);
        if ((tick - s.from) % 3 != 0 && u < 1) {
            ++i;
            continue;
        }
        p->lobes[s.lobe].r = s.to * (0.25 + 0.75 * (1 - (1 - u) * (1 - u)));
        follow_fingers(*p);
        build_field(*p, tick);
        if (u >= 1) spreads_.erase(spreads_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// @orig none: port-only barrel leaks
void BarrelLeaks::grow(Barrel& b) {
    Pool* p = pool_of(b.pool);
    if (!p) return;
    const double area = b.length_x * b.length_y / kPi;  // its cross-section, as a disc's radius squared
    for (const Hole& h : b.holes) {
        if (h.land_lobe < 0 || h.foot_lobe < 0 || static_cast<std::size_t>(h.foot_lobe) >= p->lobes.size()) continue;
        const double r = std::min(kMaxLand, 0.8 * std::sqrt(area * h.spilled / kThickness));
        p->lobes[static_cast<std::size_t>(h.land_lobe)].r = r;
        p->lobes[static_cast<std::size_t>(h.foot_lobe)].r = std::min(kMaxFoot, 0.55 * r);
    }
    follow_fingers(*p);
    p->field_dirty = true;
}

// The pool catches fire at (x, y); one already burning takes in the fuel
// added since.
// @orig none: port-only barrel leaks
void BarrelLeaks::ignite(const World& world, Pool& p, double x, double y, std::uint32_t tick) {
    if (p.burnt) return;
    double extent = 0;
    for (const Lobe& l : p.lobes) extent = std::max(extent, l.r);
    if (extent < 2) return;
    if (!p.burning) {
        p.burning = true;
        p.lit_at = tick;
        p.fire_x = x;
        p.fire_y = y;
        p.burn_ticks = static_cast<std::uint32_t>(std::clamp(240.0 + 4 * extent, 240.0, 900.0));
    }
    // A trail's thin streak of fuel burns lower than a pool.
    std::size_t drops = 0, lobes = 0;
    for (const Lobe& l : p.lobes) {
        if (l.r <= 0) continue;
        ++lobes;
        drops += l.trail;
    }
    p.flames = lobes ? 1.0f - 0.55f * static_cast<float>(drops) / static_cast<float>(lobes) : 1.0f;
    // The fuel, cell by cell, where it lies on the floor; the patches that
    // hold some.
    build_field(p, tick);
    const Bounds bb = wet_bounds(p);
    if (bb.x1 < bb.x0) {
        p.fuel.clear();
        p.patches.clear();
        return;
    }
    p.grid_x = std::floor(bb.x0 / kGrid) * kGrid;
    p.grid_y = std::floor(bb.y0 / kGrid) * kGrid;
    p.grid_w = static_cast<int>(std::ceil((bb.x1 - p.grid_x) / kGrid)) + 1;
    p.grid_h = static_cast<int>(std::ceil((bb.y1 - p.grid_y) / kGrid)) + 1;
    p.fuel.assign(static_cast<std::size_t>(p.grid_w) * p.grid_h, -1.0f);
    const auto solids = solids_around(world, static_cast<std::int32_t>(bb.x0), static_cast<std::int32_t>(bb.y0),
                                      static_cast<std::int32_t>(bb.x1), static_cast<std::int32_t>(bb.y1));
    p.patches.clear();
    for (int pj = 0; pj < p.grid_h; pj += kPatch) {
        for (int pi = 0; pi < p.grid_w; pi += kPatch) {
            bool any = false;
            for (int j = pj; j < std::min(pj + kPatch, p.grid_h); ++j) {
                for (int i = pi; i < std::min(pi + kPatch, p.grid_w); ++i) {
                    const double cx = p.grid_x + (i + 0.5) * kGrid, cy = p.grid_y + (j + 0.5) * kGrid;
                    const double s = field_at(p, cx, cy);
                    if (s < 1 || !on_floor(solids, cx, cy, p.z)) continue;
                    p.fuel[static_cast<std::size_t>(j) * p.grid_w + i] = static_cast<float>(depth_of(s));
                    any = true;
                }
            }
            if (any) p.patches.emplace_back(pi, pj);
        }
    }
    // Where the fire comes when, at the cells' corners: reached interpolates.
    p.arrive.clear();
    for (int j = 0; j <= p.grid_h; ++j)
        for (int i = 0; i <= p.grid_w; ++i)
            p.arrive.push_back(static_cast<float>(arrival(p, p.grid_x + i * kGrid, p.grid_y + j * kGrid)));
}

// @orig none: port-only barrel leaks
float BarrelLeaks::fire_strength(const Pool& p, std::uint32_t tick) const {
    if (!p.burning) return 0;
    return curve(static_cast<float>(tick - p.lit_at), static_cast<float>(p.burn_ticks));
}

// How far past (x, y) the fire has come (units; not yet there when <= 0):
// it spreads unevenly, faster one way than another.
// @orig none: port-only barrel leaks
double BarrelLeaks::reached(const Pool& p, double x, double y, std::uint32_t tick) const {
    const double gone = static_cast<double>(tick - p.lit_at) * kSpread;
    const double gx = (x - p.grid_x) / kGrid, gy = (y - p.grid_y) / kGrid;
    const int i = static_cast<int>(std::floor(gx)), j = static_cast<int>(std::floor(gy));
    if (p.arrive.empty() || i < 0 || j < 0 || i >= p.grid_w || j >= p.grid_h) return gone - arrival(p, x, y);
    const double tx = gx - i, ty = gy - j;
    const float* row = &p.arrive[static_cast<std::size_t>(j) * (p.grid_w + 1) + i];
    const double top = row[0] + (row[1] - row[0]) * tx;
    const double bottom = row[p.grid_w + 1] + (row[p.grid_w + 2] - row[p.grid_w + 1]) * tx;
    return gone - (top + (bottom - top) * ty);
}

// How far the fire has to come from where it caught to reach (x, y): its
// distance, longer one way than another.
// @orig none: port-only barrel leaks
double BarrelLeaks::arrival(const Pool& p, double x, double y) {
    const double from = std::sqrt((x - p.fire_x) * (x - p.fire_x) + (y - p.fire_y) * (y - p.fire_y));
    return from * (0.75 + 0.5 * fast_noise(static_cast<float>(x / 22), static_cast<float>(y / 22), p.seed ^ 0x77u));
}

// Reached by the fire, not yet burnt out there: the edge goes out first.
// @orig none: port-only barrel leaks
double BarrelLeaks::fire_at(const Pool& p, double x, double y, std::uint32_t tick) const {
    if (!p.burning || p.fuel.empty()) return 0;
    const int i = static_cast<int>(std::floor((x - p.grid_x) / kGrid));
    const int j = static_cast<int>(std::floor((y - p.grid_y) / kGrid));
    if (i < 0 || j < 0 || i >= p.grid_w || j >= p.grid_h) return 0;
    if (p.fuel[static_cast<std::size_t>(j) * p.grid_w + i] < 0) return 0;
    return fire_from(p, x, y, tick, reached(p, x, y, tick));
}

// @orig none: port-only barrel leaks
double BarrelLeaks::fire_from(const Pool& p, double x, double y, std::uint32_t tick, double come) const {
    if (come <= 0 || !p.burning || p.fuel.empty()) return 0;
    const int i = static_cast<int>(std::floor((x - p.grid_x) / kGrid));
    const int j = static_cast<int>(std::floor((y - p.grid_y) / kGrid));
    if (i < 0 || j < 0 || i >= p.grid_w || j >= p.grid_h) return 0;
    const float fuel = p.fuel[static_cast<std::size_t>(j) * p.grid_w + i];
    if (fuel < 0) return 0;
    const double age = tick - p.lit_at;
    const double dying = std::max(0.0, (age / p.burn_ticks - 0.55) / 0.45);
    if (fuel < dying) return 0;
    return (0.5 + 0.4 * fuel) * std::min(1.0, come / 12) * std::min(1.0, (fuel - dying) * 8 + 0.25) *
           fire_strength(p, tick);
}

// @orig none: port-only barrel leaks
void BarrelLeaks::prepare(const World& world, const Palette& game_palette, const std::vector<DrawItem>& list,
                          int view_left, int view_top, int width, int height) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    view_left_ = view_left;
    view_top_ = view_top;
    glowing_.clear();
    reflected_ = 0;
    ++frame_;
    const std::size_t pixels = static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0));
    if (flames_.size() != pixels) {
        flames_.assign(pixels, Flame{});
        for (auto* v : {&floor_frame_, &pool_frame_}) v->assign(pixels, 0);
        for (auto* v : {&floor_colour_, &pool_colour_}) v->assign(pixels, 0);
    }
    bool same = tone_ready_;
    for (int c = 0; same && c < 256; ++c) {
        if (c >= 1 && c <= 14) continue;
        const Rgb &a = game_palette.colors[c], &b = tone_palette_.colors[c];
        same = a.r == b.r && a.g == b.g && a.b == b.b;
    }
    if (!same) {
        // The fuel, dark and a little see-through at its edge; what it
        // mirrors, darkened and browned (more when far); its jet; the
        // holes; the pool glowing under the fire; the burnt stain; the
        // fire, white at its heart, yellow, orange, red, then a see-through
        // dark red and a wisp of smoke at its tips.
        tone_[kRim] = tint_table(game_palette, {52, 36, 16}, 55);
        tone_[kBody] = tint_table(game_palette, {30, 21, 10}, 80);
        tone_[kDeep] = tint_table(game_palette, {18, 12, 6}, 88);
        tone_[kSheen] = tint_table(game_palette, {70, 60, 48}, 75);
        tone_[kMirror] = tint_table(game_palette, {24, 16, 8}, 62);
        tone_[kMirrorFaint] = tint_table(game_palette, {24, 16, 8}, 76);
        tone_[kStream] = tint_table(game_palette, {92, 62, 26}, 78);
        tone_[kStreamLight] = tint_table(game_palette, {196, 160, 90}, 72);
        tone_[kHole] = shade_table(game_palette, 12);
        tone_[kWet] = tint_table(game_palette, {34, 22, 10}, 45);
        tone_[kHot] = tint_table(game_palette, {150, 50, 12}, 70);
        tone_[kAsh] = shade_table(game_palette, 58);
        tone_[kAshDark] = shade_table(game_palette, 38);
        tone_[kSoot] = shade_table(game_palette, 18);
        tone_[kFlameWhite] = tint_table(game_palette, {255, 248, 204}, 100);
        tone_[kFlameYellow] = tint_table(game_palette, {255, 214, 84}, 100);
        tone_[kFlameOrange] = tint_table(game_palette, {250, 150, 40}, 100);
        tone_[kFlameDeep] = tint_table(game_palette, {228, 94, 22}, 92);
        tone_[kFlameRed] = tint_table(game_palette, {176, 42, 10}, 75);
        tone_[kFlameDark] = tint_table(game_palette, {110, 24, 6}, 50);
        tone_[kFlameSmoke] = tint_table(game_palette, {36, 28, 24}, 28);
        tone_palette_ = game_palette;
        tone_ready_ = true;
    }
    for (Pool& p : pools_) {
        if (p.field_grid.empty() || (p.field_dirty && now_ - p.field_tick >= 8)) build_field(p, now_);
    }
    // Each fire on a wall right after the item that shows its middle: the
    // last drawn there whose box is near the point hit (the wall hit may be
    // hidden behind a door or a panel drawn over it).
    walls_after_.clear();
    walls_shown_ = 0;
    for (std::size_t k = 0; k < wall_fires_.size(); ++k) {
        const WallFire& w = wall_fires_[k];
        const int cx = screen_x(w.x, w.y) - view_left_, cy = screen_y(w.x, w.y, w.z) - view_top_;
        if (cx < 0 || cy < 0 || cx >= width || cy >= height) continue;
        for (std::size_t i = list.size(); i-- > 0;) {
            const DrawItem& d = list[i];
            if (cx < d.left || cx >= d.right || cy < d.top || cy >= d.bottom || !opaque(d, cx, cy)) continue;
            constexpr double kNear = 24;
            const bool near = w.x >= d.x_left / 2.0 - kNear && w.x <= d.x / 2.0 + kNear && w.y >= d.y_far / 2.0 - kNear &&
                              w.y <= d.y / 2.0 + kNear && w.z >= d.z - kNear && w.z <= d.z_top + kNear;
            if (near && !d.item->sprite_only && !is_npc_id(d.item->id)) {
                walls_after_[i].push_back(k);
                ++walls_shown_;
            }
            break;
        }
    }
    // What the pools may mirror: their screen boxes, raised.
    mirror_areas_.clear();
    for (const Pool& p : pools_) {
        if (p.burnt) continue;
        const Bounds bb = wet_bounds(p);
        if (bb.x1 < bb.x0) continue;
        Rect r{std::max(screen_x(bb.x0, bb.y1) - view_left_, 0), std::max(screen_y(bb.x0, bb.y0, p.z) - view_top_ - kMirrorReach, 0),
               std::min(screen_x(bb.x1, bb.y0) - view_left_ + 1, width), std::min(screen_y(bb.x1, bb.y1, p.z) - view_top_ + 1, height)};
        if (r.left < r.right && r.top < r.bottom) mirror_areas_.push_back(r);
    }
    // Each patch of fire right after the last item it must be drawn over (a
    // box of its own in the game's draw order), before the items in front.
    fire_after_.clear();
    fire_first_.clear();
    for (std::size_t pi = 0; pi < pools_.size(); ++pi) {
        const Pool& p = pools_[pi];
        if (!p.burning) continue;
        for (std::size_t k = 0; k < p.patches.size(); ++k) {
            const double x0 = p.grid_x + p.patches[k].first * kGrid, y0 = p.grid_y + p.patches[k].second * kGrid;
            const double x1 = x0 + kPatch * kGrid, y1 = y0 + kPatch * kGrid;
            DrawItem d{};
            d.x = world_from_usecode(static_cast<std::int32_t>(x1));
            d.y = world_from_usecode(static_cast<std::int32_t>(y1));
            d.x_left = world_from_usecode(static_cast<std::int32_t>(x0));
            d.y_far = world_from_usecode(static_cast<std::int32_t>(y0));
            d.z = p.z;
            d.z_top = p.z + 40;
            d.left = screen_x(x0, y1) - view_left_ - 2;
            d.right = screen_x(x1, y0) - view_left_ + 2;
            d.top = screen_y(x0, y0, p.z) - view_top_ - static_cast<int>(kFlameHeight) - 2;
            d.bottom = screen_y(x1, y1, p.z) - view_top_ + 2;
            if (d.right < 0 || d.bottom < 0 || d.left > width || d.top > height) continue;
            std::size_t after = list.size();
            for (std::size_t i = list.size(); i-- > 0;) {
                if (draw_before(list[i], d).value_or(false)) {
                    after = i;
                    break;
                }
            }
            if (after == list.size()) fire_first_.emplace_back(pi, k);
            else fire_after_[after].emplace_back(pi, k);
        }
    }
}

// @orig none: port-only barrel leaks
void BarrelLeaks::put(Surface8& screen, int x, int y, Tone tone, bool glows) {
    if (x < 0 || y < 0 || x >= screen.width || y >= screen.height) return;
    std::uint8_t& p = screen.row(y)[x];
    p = tone_[tone][p];
    if (glows) glowing_.emplace_back(static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(screen.width) + x, p);
}

// @orig none: port-only barrel leaks
const std::vector<std::uint8_t>& BarrelLeaks::mask_of(const ShapeFrame& f) const {
    auto [it, made] = masks_.try_emplace(&f);
    std::vector<std::uint8_t>& mask = it->second;
    if (made) {
        mask.assign(static_cast<std::size_t>(f.width) * f.height, 0);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) mask[static_cast<std::size_t>(y) * f.width + x] = f.pixel(x, y) >= 0;
    }
    return mask;
}

// @orig none: port-only barrel leaks
bool BarrelLeaks::opaque(const DrawItem& d, int sx, int sy) const {
    const ShapeFrame& f = *d.frame;
    const std::vector<std::uint8_t>& mask = mask_of(f);
    const int fy = sy - d.top;
    const int fx = d.mirrored ? d.right - 1 - sx : sx - d.left;
    if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) return false;
    return mask[static_cast<std::size_t>(fy) * f.width + fx];
}

// @orig none: port-only barrel leaks
void BarrelLeaks::draw_first(Surface8& screen) {
    if (!tone_ready_) return;
    for (const auto& [pi, k] : fire_first_) draw_patch(screen, pools_[pi], pools_[pi].patches[k]);
}

// @orig none: port-only barrel leaks
void BarrelLeaks::draw(Surface8& screen, const DrawItem& d, std::size_t index) {
    if (!tone_ready_) return;
    const Item& item = *d.item;
    if (leaks_fuel(item.shape)) {
        for (const Barrel& b : barrels_)
            if (b.id == item.id && b.shape == item.shape) draw_barrel(screen, d, b);
        for (const Heating& h : heating_)
            if (h.id == item.id && h.shape == item.shape) draw_heating(screen, d, h);
    } else if (is_npc_id(item.id)) {
        for (const Burning& b : burning_)
            if (b.id == item.id) draw_person(screen, d, b);
    } else if (!item.sprite_only && !(d.shape_flags & ShapeInfo::kTranslucent)) {
        bool level = false;
        for (const Pool& p : pools_) {
            if (std::abs(d.z_top - p.z) > kHeight) continue;
            draw_pool(screen, d, p);
            level = true;
        }
        if (level) mark_floor(screen, d);
    }
    if (const auto found = walls_after_.find(index); found != walls_after_.end()) {
        for (std::size_t k : found->second) draw_wall_fire(screen, d, wall_fires_[k]);
    }
    if (const auto found = fire_after_.find(index); found != fire_after_.end()) {
        for (const auto& [pi, k] : found->second) draw_patch(screen, pools_[pi], pools_[pi].patches[k]);
    }
}

// The pool over the part of floor `d` it covers. Its plain fuel is
// remembered: it may mirror what stands behind it (reflect).
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_pool(Surface8& screen, const DrawItem& d, const Pool& p) {
    const Bounds bb = wet_bounds(p);
    if (bb.x1 < bb.x0) return;
    // The floor's box, usecode units, against the pool's.
    const double hx0 = d.x_left / 2.0, hx1 = d.x / 2.0, hy0 = d.y_far / 2.0, hy1 = d.y / 2.0;
    if (hx1 < bb.x0 || hx0 > bb.x1 || hy1 < bb.y0 || hy0 > bb.y1) return;
    const double x0 = std::max(hx0, bb.x0), x1 = std::min(hx1, bb.x1);
    const double y0 = std::max(hy0, bb.y0), y1 = std::min(hy1, bb.y1);
    const int left = std::max({d.left, screen_x(x0, y1) - view_left_ - 1, 0});
    const int right = std::min({d.right, screen_x(x1, y0) - view_left_ + 2, screen.width});
    const int top = std::max({d.top, screen_y(x0, y0, p.z) - view_top_ - 1, 0});
    const int bottom = std::min({d.bottom, screen_y(x1, y1, p.z) - view_top_ + 2, screen.height});
    const std::uint32_t tick = now_;
    const std::vector<std::uint8_t>& mask = mask_of(*d.frame);
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            // The point of the floor's top seen at this pixel.
            const double a = 2.0 * (px + view_left_ + 0.5), b = 4.0 * (py + view_top_ + 0.5 + p.z);
            const double x = (a + b) / 2, y = (b - a) / 2;
            // A little past the box: the seam between two floors may fall
            // just outside both.
            if (x < hx0 - 2 || x > hx1 + 2 || y < hy0 - 2 || y > hy1 + 2) continue;
            const int mx = d.mirrored ? d.right - 1 - px : px - d.left, my = py - d.top;
            if (!mask[static_cast<std::size_t>(my) * d.frame->width + mx]) continue;
            const double s = field_at(p, x, y);
            if (s < 1) continue;
            const double depth = depth_of(s);
            const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
            if (p.burnt) {
                put(screen, px, py, fast_noise(fx / 4, fy / 4, p.seed ^ 7) + depth * 0.4 > 0.75 ? kAshDark : kAsh);
                continue;
            }
            if (const double come = p.burning ? reached(p, x, y, tick) : 0; come > 0) {
                // Glowing under the fire, burnt where it went out.
                if (fire_from(p, x, y, tick, come) > 0) put(screen, px, py, kHot, true);
                else put(screen, px, py, fast_noise(fx / 4, fy / 4, p.seed ^ 7) > 0.6 ? kAshDark : kAsh);
                continue;
            }
            Tone tone = depth < 0.08 ? kRim : depth > 0.6 ? kDeep : kBody;
            const bool sheen = depth > 0.2 && fast_noise(fx / 16 + tick * 0.004f, fy / 9, p.seed ^ 1) > 0.84f;
            if (sheen) tone = kSheen;
            put(screen, px, py, tone);
            if (depth >= 0.08 && !sheen) {
                const std::size_t j = static_cast<std::size_t>(py) * screen.width + px;
                if (j < pool_frame_.size()) {
                    pool_frame_[j] = frame_;
                    pool_colour_[j] = screen.row(py)[px];
                }
            }
        }
    }
}

// The pixels of floor `d`, at a pool's level, in the pools' mirror areas.
// @orig none: port-only barrel leaks
void BarrelLeaks::mark_floor(const Surface8& screen, const DrawItem& d) {
    const std::vector<std::uint8_t>& mask = mask_of(*d.frame);
    for (const Rect& r : mirror_areas_) {
        for (int py = std::max(r.top, d.top); py < std::min(r.bottom, d.bottom); ++py) {
            for (int px = std::max(r.left, d.left); px < std::min(r.right, d.right); ++px) {
                const int mx = d.mirrored ? d.right - 1 - px : px - d.left;
                if (!mask[static_cast<std::size_t>(py - d.top) * d.frame->width + mx]) continue;
                const std::size_t j = static_cast<std::size_t>(py) * screen.width + px;
                if (j >= floor_frame_.size()) continue;
                floor_frame_[j] = frame_;
                floor_colour_[j] = screen.row(py)[px];
            }
        }
    }
}

// The holes of a barrel, the fuel running down under them and out of them.
// A lying barrel rolling turns them round: none drawn till it stops.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_barrel(Surface8& screen, const DrawItem& d, const Barrel& b) {
    if (b.rolling && !upright(b.shape)) return;
    const Item& item = *d.item;
    const std::uint32_t tick = now_;
    const auto at = [&](double x, double y, double z, int& sx, int& sy) {
        sx = d.sx + screen_x(x, y) - screen_x(item.x, item.y);
        sy = d.sy + screen_y(x, y, z) - screen_y(item.x, item.y, item.z);
    };
    for (const Hole& h : b.holes) {
        if (!h.seen) continue;
        const double hx = b.x + h.x, hy = b.y + h.y, hz = b.z + h.height;
        int sx = 0, sy = 0;
        at(hx, hy, hz, sx, sy);
        if (h.started && h.height < b.height) {
            // Wet down the side to the floor.
            for (int y = sy + 1; y <= sy + static_cast<int>(h.height); ++y)
                if (opaque(d, sx, y)) put(screen, sx, y, kWet);
        }
        const double head = b.level - h.height;
        if (!h.started || head <= 0) continue;
        const double v = kJet * std::sqrt(head);
        if (v >= kDribble) {
            // A jet out of the side, falling to the floor; light flecks run
            // along it.
            int last_x = 1 << 20, last_y = 1 << 20;
            const double land = std::sqrt(2 * (hz - b.floor) / kGravity);
            for (double t = 0; t <= land; t += 0.2) {
                const double z = hz - 0.5 * kGravity * t * t;
                if (z < b.floor) break;
                int jx = 0, jy = 0;
                at(hx + h.ux * v * t, hy + h.uy * v * t, z, jx, jy);
                if (jx == last_x && jy == last_y) continue;
                last_x = jx;
                last_y = jy;
                const int fleck = static_cast<int>(std::floor(t * 1.5 - tick * 0.6));
                const Tone tone = (fleck & 3) == 0 ? kStreamLight : kStream;
                put(screen, jx, jy, tone);
                if (v > 1.6 && t < land * 0.4) put(screen, jx + 1, jy, kStream);
            }
            // A splash where it lands.
            int lx = 0, ly = 0;
            at(hx + h.ux * v * land, hy + h.uy * v * land, b.floor, lx, ly);
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t r = mix(h.seed, static_cast<int>(tick / 3), k);
                put(screen, lx + static_cast<int>(r % 5) - 2, ly - static_cast<int>((r >> 4) % 2), kStreamLight);
            }
        } else {
            // A dribble: drops running down the side, slower the lower the level.
            const std::uint32_t period = 24 + static_cast<std::uint32_t>((kDribble - v) * 80);
            const double t = static_cast<double>((tick + h.seed) % period);
            const double fall = 0.05 * t * t;
            if (fall < h.height) {
                const int y = sy + 1 + static_cast<int>(fall);
                put(screen, sx, y, kStreamLight);
                put(screen, sx, y - 1, kStream);
            }
        }
    }
    // The holes over their jets.
    for (const Hole& h : b.holes) {
        if (!h.seen) continue;
        int sx = 0, sy = 0;
        at(b.x + h.x, b.y + h.y, b.z + h.height, sx, sy);
        if (opaque(d, sx, sy)) put(screen, sx, sy, kHole);
        if (opaque(d, sx + 1, sy)) put(screen, sx + 1, sy, kHole);
    }
}

// Flames rise from each burning row of the column, losing heat as they go
// up (1 / height a pixel), broken up by noise rising faster than them, so
// that tongues lick up and part from the fire; the hotter, the whiter.
// Where fire was already drawn this frame, the hotter stays.
// @orig none: port-only barrel leaks
void BarrelLeaks::fire_column(Surface8& screen, int px, int bottom, const std::vector<float>& base, float height,
                              std::uint32_t seed) {
    if (px < 0 || px >= screen.width || height < 2) return;
    const float t = static_cast<float>(now_);
    const float decay = 1.0f / height;
    const float x = static_cast<float>(px + view_left_);
    float m = 0;
    const int n = static_cast<int>(base.size());
    for (int k = 0;; ++k) {
        const float b = k < n ? base[static_cast<std::size_t>(k)] : 0.0f;
        m = std::max(b, m - decay);
        if (m <= 0) {
            if (k >= n) break;
            continue;
        }
        const int py = bottom - k;
        if (py < 0) break;
        if (py >= screen.height) continue;
        const std::size_t j = static_cast<std::size_t>(py) * static_cast<std::size_t>(screen.width) + px;
        std::uint8_t& pixel = screen.row(py)[px];
        if (j >= flames_.size()) continue;
        Flame& f = flames_[j];
        const bool drawn = f.heat_frame == frame_ && f.colour == pixel;
        if (drawn && f.heat >= m * 1.47f) continue;  // as hot as it can get here
        // The noise here, once a frame for a seed (the patches of a pool
        // overlap).
        if (f.noise_frame != frame_ || f.seed != seed) {
            const float y = static_cast<float>(py + view_top_);
            const float sway = 1.3f * std::sin(y * 0.19f + t * 0.08f + x * 0.05f);
            // Streaks taller than wide, rising faster than the flames.
            const float n1 = fast_noise((x + sway) / 3.2f, (y + t * 1.9f) / 11.0f, seed);
            const float n2 = fast_noise(x / 1.7f, (y + t * 3.1f) / 5.0f, seed + 101);
            f.noise_frame = frame_;
            f.seed = seed;
            f.noise = 0.02f + 1.45f * (0.6f * n1 + 0.4f * n2);
        }
        const float v = m * f.noise;
        if (v < 0.1f || (drawn && f.heat >= v)) continue;
        const Tone tone = v > 1.0f    ? kFlameWhite
                          : v > 0.8f  ? kFlameYellow
                          : v > 0.62f ? kFlameOrange
                          : v > 0.46f ? kFlameDeep
                          : v > 0.32f ? kFlameRed
                          : v > 0.2f  ? kFlameDark
                                      : kFlameSmoke;
        put(screen, px, py, tone, v > 0.32f);
        f.heat_frame = frame_;
        f.heat = v;
        f.colour = pixel;
    }
}

// A patch of a burning pool: for each column of its screen, how strongly
// each row of its floor burns, then the flames off them.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_patch(Surface8& screen, const Pool& p, std::pair<int, int> patch) {
    const float strength = fire_strength(p, now_);
    if (strength <= 0) return;
    const double x0 = p.grid_x + patch.first * kGrid, y0 = p.grid_y + patch.second * kGrid;
    const double x1 = x0 + kPatch * kGrid, y1 = y0 + kPatch * kGrid;
    const int left = screen_x(x0, y1) - view_left_, right = screen_x(x1, y0) - view_left_;
    const int top = screen_y(x0, y0, p.z) - view_top_, bottom = screen_y(x1, y1, p.z) - view_top_;
    const float t = static_cast<float>(now_);
    for (int px = std::max(left, 0); px <= std::min(right, screen.width - 1); ++px) {
        column_.assign(static_cast<std::size_t>(bottom - top + 1), 0.0f);
        bool any = false;
        for (int k = 0; k <= bottom - top; ++k) {
            const int py = bottom - k;
            const double a = 2.0 * (px + view_left_ + 0.5), b = 4.0 * (py + view_top_ + 0.5 + p.z);
            const double x = (a + b) / 2, y = (b - a) / 2;
            if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
            const float f = static_cast<float>(fire_at(p, x, y, now_));
            column_[static_cast<std::size_t>(k)] = f;
            any = any || f > 0;
        }
        if (!any) continue;
        // Taller here, lower there, slowly changing.
        const float tall = 0.45f + 0.9f * fast_noise((px + view_left_) / 9.0f + t * 0.012f, t * 0.006f, p.seed);
        fire_column(screen, px, bottom, column_, kFlameHeight * p.flames * strength * tall, p.seed);
    }
}

// Fuel burning on a wall: a ragged patch of the face, burnt black, on the
// item `d` that shows it, with flames rising off it up the wall, until it
// goes out.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_wall_fire(Surface8& screen, const DrawItem& d, const WallFire& w) {
    const float age = static_cast<float>(now_ - w.lit_at);
    const float strength = curve(age, static_cast<float>(w.burn_ticks));
    const int cx = screen_x(w.x, w.y) - view_left_, cy = screen_y(w.x, w.y, w.z) - view_top_;
    const float r = w.r;
    const int reach = static_cast<int>(r) + 2;
    const int bottom = cy + reach;
    for (int px = cx - reach / 2 - 2; px <= cx + reach / 2 + 2; ++px) {
        if (px < 0 || px >= screen.width) continue;
        column_.assign(static_cast<std::size_t>(2 * reach + 1), 0.0f);
        bool any = false;
        for (int k = 0; k <= 2 * reach; ++k) {
            const int py = bottom - k;
            if (py < 0 || py >= screen.height || !opaque(d, px, py)) continue;
            // Along the face (u) and up it (v), usecode units.
            const float dx = static_cast<float>(px - cx), dy = static_cast<float>(py - cy);
            const float u = w.face == 0 ? -2 * dx : 2 * dx;
            const float v = (w.face == 0 ? -dx / 2 : dx / 2) - dy;
            const float e = (u / r) * (u / r) + (v / (0.8f * r)) * (v / (0.8f * r));
            const float edge = 0.6f + 0.55f * fast_noise(u / 4 + 7.3f, v / 4, w.seed);
            if (e > edge * edge) continue;
            const float rel = e / (edge * edge);
            put(screen, px, py, rel < 0.35f ? kSoot : rel < 0.75f ? kAshDark : kAsh);
            if (strength <= 0) continue;
            column_[static_cast<std::size_t>(k)] = strength * (0.9f - 0.4f * e / (edge * edge));
            any = true;
        }
        if (any) fire_column(screen, px, bottom, column_, 28 * strength, w.seed);
    }
}

// Someone on fire: flames over their body, here and there, rising off it;
// weaker once out of the fire, until they go out.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_person(Surface8& screen, const DrawItem& d, const Burning& b) {
    const Item& item = *d.item;
    if (item.shape == 0x576 || item.shape == 0x58F) return;  // the game's own burning and burnt
    const float left = b.until > now_ ? static_cast<float>(b.until - now_) / kAfterBurn : 0.0f;
    const float strength = 0.35f + 0.65f * std::min(1.0f, left * 1.5f);
    body_fire(screen, d, strength, 16 * strength, 1, mix(item.id));
}

// A barrel heating up in a fire: past a third of its fuse, flames climb its
// sides, higher and stronger as it nears blowing up.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_heating(Surface8& screen, const DrawItem& d, const Heating& h) {
    const float u = std::clamp((h.heat / h.fuse - 0.33f) / 0.67f, 0.0f, 1.0f);
    if (u <= 0) return;
    const float strength = 0.3f + 0.7f * u;
    body_fire(screen, d, strength, 6 + 18 * u, 0.3f + 0.7f * u, mix(d.item->id, 0, 17));
}

// @orig none: port-only barrel leaks
void BarrelLeaks::body_fire(Surface8& screen, const DrawItem& d, float strength, float height, float reach,
                            std::uint32_t seed) {
    const float t = static_cast<float>(now_);
    const int rows = d.bottom - d.top;
    for (int px = std::max(d.left, 0); px < std::min(d.right, screen.width); ++px) {
        column_.assign(static_cast<std::size_t>(rows), 0.0f);
        bool any = false;
        for (int k = 0; k < rows; ++k) {
            const int py = d.bottom - 1 - k;
            if (k > rows * reach) break;
            if (!opaque(d, px, py)) continue;
            const float patchy = fast_noise((px + view_left_) / 3.0f, (py + view_top_ + t * 1.2f) / 4.0f, seed);
            if (patchy < 0.45f) continue;
            const float low = k < rows * 0.6f ? 0.85f : 0.6f;
            column_[static_cast<std::size_t>(k)] = low * strength;
            any = true;
        }
        if (any) fire_column(screen, px, d.bottom - 1, column_, height, seed);
    }
}

// The drops in flight, over everything (they fly for a moment only), each a
// burning head with a tail along the way it came; then what the pools
// mirror.
// @orig none: port-only barrel leaks
void BarrelLeaks::draw_last(Surface8& screen) {
    if (!tone_ready_) return;
    static constexpr Tone kTail[] = {kFlameWhite, kFlameYellow, kFlameYellow, kFlameOrange, kFlameOrange,
                                     kFlameDeep,  kFlameRed,    kFlameDark,   kFlameSmoke};
    constexpr int kSteps = static_cast<int>(sizeof kTail / sizeof kTail[0]);
    for (const Drop& d : drops_) {
        // The tail first, the head over it: a blob of 2 x 2 pixels, 3 x 3
        // for a big drop, with a white heart.
        for (int k = kSteps - 1; k >= 0; --k) {
            const double back = k * 0.6;
            const double x = d.x - d.vx * back, y = d.y - d.vy * back;
            const double z = d.z - d.vz * back - 0.5 * kDropGravity * back * back;
            const int sx = screen_x(x, y) - view_left_, sy = screen_y(x, y, z) - view_top_;
            const int width = k == 0 ? (d.size > 0.9f ? 3 : 2) : k < 4 ? 2 : 1;
            for (int dy = 0; dy < width; ++dy)
                for (int dx = 0; dx < width; ++dx) put(screen, sx + dx - width / 2, sy + dy - width / 2, kTail[k], true);
            if (k == 0) put(screen, sx, sy, kFlameWhite, true);
        }
    }
    reflect(screen);
}

// What stands behind a pool shows in it, upside down: in this view a
// mirror in the floor shows the scene above each of its pixels, mirrored
// about the line where the floor behind it ends. Column by column, the
// floor run above each pool pixel gives that line; the pixel as far above
// it shows there, darkened and browned (more when far), a little wavy.
// @orig none: port-only barrel leaks
void BarrelLeaks::reflect(Surface8& screen) {
    const float t = static_cast<float>(now_);
    for (const Rect& r : mirror_areas_) {
        for (int px = r.left; px < r.right; ++px) {
            int run = 0;
            for (int py = r.top; py < r.bottom; ++py) {
                const std::size_t j = static_cast<std::size_t>(py) * screen.width + px;
                if (j >= floor_frame_.size()) break;
                std::uint8_t& pixel = screen.row(py)[px];
                const bool floor = floor_frame_[j] == frame_ && floor_colour_[j] == pixel;
                run = floor ? run + 1 : 0;
                if (!floor || pool_frame_[j] != frame_ || pool_colour_[j] != pixel) continue;
                if (run >= py - r.top + 1) continue;  // floor all the way up: nothing to mirror
                const int sample_y = py - 2 * run + 1;
                const int wave = fast_noise(px * 0.35f, py * 0.6f + t * 0.05f, 0x3D) > 0.62f ? 1 : 0;
                const int sample_x = std::min(px + wave, screen.width - 1);
                if (sample_y < 0) continue;
                const std::uint8_t mirrored = screen.row(sample_y)[sample_x];
                pixel = tone_[run > 45 ? kMirrorFaint : kMirror][mirrored];
                pool_colour_[j] = pixel;
                floor_colour_[j] = pixel;
                ++reflected_;
            }
        }
    }
}

// @orig none: port-only barrel leaks
std::vector<PointLight> BarrelLeaks::lights(const World& world) const {
    std::vector<PointLight> out;
    const float t = static_cast<float>(now_);
    for (const Pool& p : pools_) {
        const float strength = fire_strength(p, now_);
        if (strength <= 0) continue;
        double sx = 0, sy = 0, sw = 0, extent = 0;
        for (const Lobe& l : p.lobes) {
            sx += l.x * l.r * l.r;
            sy += l.y * l.r * l.r;
            sw += l.r * l.r;
            extent = std::max(extent, l.r);
        }
        if (sw <= 0) continue;
        const float flicker = 0.85f + 0.1f * std::sin(t * 0.5f + p.serial) + 0.05f * std::sin(t * 1.3f + p.serial * 3);
        PointLight l;
        l.pos = {static_cast<float>(sx / sw * 2), static_cast<float>(sy / sw * 2), (p.z + 22) * kLightZScale};
        const float k = (extent > 40 ? 1.6f : 1.0f) * strength * flicker;
        l.color = Vec3{1.0f * k, 0.52f * k, 0.18f * k};
        l.radius = static_cast<float>(extent > 40 ? 420 + 4 * extent : 260 + 4 * extent);
        out.push_back(l);
    }
    int walls = 0;
    for (const WallFire& w : wall_fires_) {
        const float strength = curve(static_cast<float>(now_ - w.lit_at), static_cast<float>(w.burn_ticks));
        if (strength <= 0 || ++walls > 16) continue;
        const float flicker = 0.8f + 0.2f * std::sin(t * 0.8f + w.seed % 7);
        PointLight l;
        // A little off the face, towards the camera.
        l.pos = {static_cast<float>(w.x * 2 + (w.face == 0 ? 12 : 0)), static_cast<float>(w.y * 2 + (w.face == 1 ? 12 : 0)),
                 static_cast<float>((w.z + 6) * kLightZScale)};
        l.color = Vec3{0.8f * strength * flicker, 0.4f * strength * flicker, 0.13f * strength * flicker};
        l.radius = 220;
        l.owner = w.host;
        out.push_back(l);
    }
    for (const Burning& b : burning_) {
        const Item* item = world.get(b.id);
        if (!item) continue;
        const float flicker = 0.8f + 0.2f * std::sin(t * 0.7f + b.id);
        PointLight l;
        l.pos = {float(world_from_usecode(item->x)) - 16, float(world_from_usecode(item->y)) - 16,
                 (item->z + 24) * kLightZScale};
        l.color = Vec3{1.0f * flicker, 0.5f * flicker, 0.16f * flicker};
        l.radius = 300;
        l.owner = b.id;
        out.push_back(l);
    }
    for (const Heating& h : heating_) {
        const Item* item = world.get(h.id);
        const float u = std::clamp((h.heat / h.fuse - 0.33f) / 0.67f, 0.0f, 1.0f);
        if (!item || u <= 0) continue;
        const float glow = u * (0.8f + 0.2f * std::sin(t * 0.9f + h.id));
        PointLight l;
        l.pos = {float(world_from_usecode(item->x)) - 16, float(world_from_usecode(item->y)) - 16,
                 (item->z + 24) * kLightZScale};
        l.color = Vec3{0.9f * glow, 0.45f * glow, 0.14f * glow};
        l.radius = 240;
        l.owner = h.id;
        out.push_back(l);
    }
    return out;
}

// @orig none: port-only barrel leaks
std::vector<Vec3> BarrelLeaks::smoke_since(std::uint32_t since) const {
    std::vector<Vec3> out;
    if (now_ - since > 60) since = now_ - 60;
    // Each puff costs the lighting all the pixels it covers: a big pool
    // smokes every 7 ticks, a drop's little one every 21, a fire on a wall
    // every 39.
    for (const Pool& p : pools_) {
        if (!p.burning || p.fuel.empty()) continue;
        double extent = 0;
        for (const Lobe& l : p.lobes) extent = std::max(extent, l.r);
        const std::uint32_t every = extent > 40 ? 7 : 21;
        for (std::uint32_t t = since + 1; t <= now_ && t != 0; ++t) {
            if ((t + p.serial) % every != 0 || fire_strength(p, t) < 0.4f) continue;
            // A cell picked at random: smoke if it burns.
            const std::uint32_t h = mix(p.seed, static_cast<int>(t), 5);
            const int i = static_cast<int>(h % static_cast<std::uint32_t>(p.grid_w));
            const int j = static_cast<int>((h >> 12) % static_cast<std::uint32_t>(p.grid_h));
            const double x = p.grid_x + (i + 0.5) * kGrid, y = p.grid_y + (j + 0.5) * kGrid;
            if (fire_at(p, x, y, t) < 0.3) continue;
            out.push_back({static_cast<float>(x * 2), static_cast<float>(y * 2), (p.z + 20) * kLightZScale});
        }
    }
    for (const WallFire& w : wall_fires_) {
        for (std::uint32_t t = since + 1; t <= now_ && t != 0; ++t) {
            if ((t + w.seed) % 39 != 0 || curve(static_cast<float>(t - w.lit_at), static_cast<float>(w.burn_ticks)) < 0.4f)
                continue;
            out.push_back({static_cast<float>(w.x * 2), static_cast<float>(w.y * 2),
                           static_cast<float>((w.z + 10) * kLightZScale)});
        }
    }
    return out;
}

// @orig none: port-only barrel leaks
void BarrelLeaks::clear() {
    barrels_.clear();
    pools_.clear();
    drops_.clear();
    wall_fires_.clear();
    burning_.clear();
    heating_.clear();
    cook_offs_.clear();
    spreads_.clear();
    burns_.clear();
    fire_after_.clear();
    fire_first_.clear();
    walls_after_.clear();
    mirror_areas_.clear();
    glowing_.clear();
    masks_.clear();
    started_ = false;
}

}  // namespace crusader
