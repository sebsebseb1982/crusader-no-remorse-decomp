#include "engine/world/glass_debris.hpp"

#include "core/dtable.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/projectile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace crusader {

namespace {

constexpr double kGravity = 0.34;     // units a tick, a tick
constexpr double kDrag = 0.992;       // of the speed kept each tick in the air
constexpr double kBounce = 0.32;      // of the speed down kept up
constexpr double kSlideBrake = 0.16;  // units a tick taken off a sliding piece each tick
constexpr double kSlideDrag = 0.88;   // of the speed kept each tick on the floor
constexpr double kMaxSpeed = 9.0;     // units a tick along the floor
constexpr int kChunkW = 7, kChunkH = 5;  // pixels a piece covers, about
constexpr int kMaxCatchUp = 240;          // ticks moved at most in one advance
constexpr int kMaxFlight = 600;           // a piece still moving after that is laid down

// splitmix32, as the marks': never the game's generators.
// @orig none: port-only glass debris
std::uint32_t mix(std::uint32_t v) {
    v += 0x9E3779B9u;
    v = (v ^ (v >> 16)) * 0x85EBCA6Bu;
    v = (v ^ (v >> 13)) * 0xC2B2AE35u;
    return v ^ (v >> 16);
}

struct Footprint {
    int w = 0, d = 0, h = 0;
};

// @orig none: port-only glass debris
Footprint footprint(const World& world, const Item& item) {
    const ShapeInfo* info = world.shape_info(item.shape);
    if (!info) return {};
    Footprint f{info->footpad_x * 16, info->footpad_y * 16, info->footpad_z * 8};
    if (item.flags & kItemFlipped) std::swap(f.w, f.d);
    return f;
}

// Scenery a piece may lie on: drawn, on the map, not an NPC, a shot, a pile
// of glass or a see-through shape.
// @orig none: port-only glass debris
bool can_bear(const World& world, const Item& item) {
    if (is_npc_id(item.id) || item.parent != 0 || item.off_map || item.sprite_only) return false;
    if (item.flags & (kItemInvisible | kItemContained | kItemEthereal)) return false;
    if (item.shape == kProjectileShape || is_glass_pile(item.shape)) return false;
    const ShapeInfo* info = world.shape_info(item.shape);
    return info && !info->is(ShapeInfo::kEditor) && !info->is(ShapeInfo::kTranslucent) &&
           !info->is(ShapeInfo::kNpc);
}

// The floor under (x, y): the highest scenery whose box holds the point and
// whose top lies between z - `below` and z + 4; its top in `top`.
// @orig none: port-only glass debris
const Item* floor_under(const World& world, double x, double y, double z, int below, std::int32_t& top) {
    const auto ix = static_cast<std::int32_t>(std::lround(x)), iy = static_cast<std::int32_t>(std::lround(y));
    const Item* floor = nullptr;
    for (ObjId id : world.rect_search(ix - 256, iy - 256, ix + 256, iy + 256)) {
        const Item* item = world.get(id);
        if (!item || !can_bear(world, *item)) continue;
        const Footprint f = footprint(world, *item);
        if (ix <= item->x - f.w || ix > item->x || iy <= item->y - f.d || iy > item->y) continue;
        const std::int32_t t = (item->z & 0xFF) + f.h;
        if (t > z + 4 || t < z - below) continue;
        if (!floor || t > top) {
            floor = item;
            top = t;
        }
    }
    return floor;
}

// Something solid at (x, y, z).
// @orig none: port-only glass debris
bool solid_at(const World& world, double x, double y, double z) {
    if (z < 0) return true;
    const World::Point p{static_cast<std::int32_t>(std::lround(x)), static_cast<std::int32_t>(std::lround(y)),
                         static_cast<std::int32_t>(std::lround(z))};
    return !box_free(world, 1, 1, 1, 1, p);
}

}  // namespace

// @orig none: port-only glass debris
bool is_breakable_glass(std::uint16_t shape) {
    return shape == 0x27B || shape == 0x2F3 || shape == 0x541 || shape == 0x542;
}

// @orig none: port-only glass debris
bool is_glass_pile(std::uint16_t shape) { return shape == 0x27A || shape == 0x349; }

// @orig none: port-only glass debris
double GlassDebris::unit() {
    next_seed_ = mix(next_seed_);
    return (next_seed_ & 0xFFFF) / 65535.0;
}

// @orig none: port-only glass debris
std::size_t GlassDebris::chunk_count(std::uint16_t shape) const {
    return static_cast<std::size_t>(
        std::count_if(chunks_.begin(), chunks_.end(), [&](const Chunk& c) { return c.shape == shape; }));
}

// The sprite's opaque pixels go to the nearest of seeds strewn one to a cell
// of about kChunkW x kChunkH pixels: irregular pieces, each a frame of its
// own anchored at its middle.
// @orig none: port-only glass debris
void GlassDebris::add_pile_art(std::uint16_t shape, const ShapeFrame& frame) {
    if (chunk_count(shape) != 0 || frame.width <= 0 || frame.height <= 0) return;
    struct Px {
        int x, y;
        std::uint8_t c;
    };
    std::vector<Px> px;
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
            if (const int c = frame.pixel(x, y); c >= 0) px.push_back({x, y, static_cast<std::uint8_t>(c)});
    if (px.empty()) return;
    std::uint32_t seed = mix(0x91A55u + shape);
    const auto rnd = [&](int n) {
        seed = mix(seed);
        return static_cast<int>(seed % static_cast<std::uint32_t>(n));
    };
    std::vector<std::pair<double, double>> seeds;
    for (int cy = 0; cy < frame.height; cy += kChunkH) {
        for (int cx = 0; cx < frame.width; cx += kChunkW) {
            std::vector<const Px*> in;
            for (const Px& p : px)
                if (p.x >= cx && p.x < cx + kChunkW && p.y >= cy && p.y < cy + kChunkH) in.push_back(&p);
            if (in.size() < 3) continue;
            const Px* s = in[static_cast<std::size_t>(rnd(static_cast<int>(in.size())))];
            seeds.emplace_back(s->x, s->y);
        }
    }
    if (seeds.empty()) seeds.emplace_back(px[0].x, px[0].y);
    std::vector<std::vector<const Px*>> groups(seeds.size());
    for (const Px& p : px) {
        std::size_t best = 0;
        double best_d = std::numeric_limits<double>::max();
        for (std::size_t k = 0; k < seeds.size(); ++k) {
            const double dx = p.x - seeds[k].first, dy = (p.y - seeds[k].second) * 1.3;
            const double d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        groups[best].push_back(&p);
    }
    for (const auto& g : groups) {
        if (g.empty()) continue;
        int x0 = frame.width, y0 = frame.height, x1 = -1, y1 = -1;
        double mx = 0, my = 0;
        for (const Px* p : g) {
            x0 = std::min(x0, p->x);
            y0 = std::min(y0, p->y);
            x1 = std::max(x1, p->x);
            y1 = std::max(y1, p->y);
            mx += p->x;
            my += p->y;
        }
        mx /= static_cast<double>(g.size());
        my /= static_cast<double>(g.size());
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        std::vector<int> grid(static_cast<std::size_t>(w) * h, -1);
        for (const Px* p : g) grid[static_cast<std::size_t>(p->y - y0) * w + (p->x - x0)] = p->c;
        Chunk& ch = chunks_.emplace_back();
        ch.shape = shape;
        ch.pixels = static_cast<int>(g.size());
        // Uncompressed runs: (transparent, length, colours...) per line, and
        // a last transparent run to the width.
        std::vector<std::uint32_t> lines;
        for (int y = 0; y < h; ++y) {
            lines.push_back(static_cast<std::uint32_t>(ch.data.size()));
            int x = 0;
            while (x < w) {
                int skip = 0;
                while (x + skip < w && grid[static_cast<std::size_t>(y) * w + x + skip] < 0) ++skip;
                ch.data.push_back(static_cast<std::uint8_t>(skip));
                x += skip;
                if (x >= w) break;
                int len = 0;
                while (x + len < w && grid[static_cast<std::size_t>(y) * w + x + len] >= 0) ++len;
                ch.data.push_back(static_cast<std::uint8_t>(len));
                for (int k = 0; k < len; ++k)
                    ch.data.push_back(static_cast<std::uint8_t>(grid[static_cast<std::size_t>(y) * w + x + k]));
                x += len;
                if (x >= w) ch.data.push_back(0);
            }
        }
        const int ax = static_cast<int>(std::lround(mx)), ay = static_cast<int>(std::lround(my));
        ch.frame.width = w;
        ch.frame.height = h;
        ch.frame.xoff = ax - x0;
        ch.frame.yoff = ay - y0;
        ch.frame.compressed = false;
        ch.frame.rle = std::span<const std::uint8_t>(ch.data);
        ch.frame.lines = std::move(lines);
        // On the floor plane of the pile: screen (dx, dy) from its anchor.
        const double dx = ax - frame.xoff, dy = ay - frame.yoff;
        ch.ox = dx + 2 * dy;
        ch.oy = 2 * dy - dx;
    }
}

// @orig none: port-only glass debris
void GlassDebris::clear() {
    pieces_.clear();
    shattered_.clear();
    watches_.clear();
    tick_ = 0;
    last_blast_tick_ = 0xFFFFFFFFu;
}

// @orig none: port-only glass debris
void GlassDebris::new_map(const World& world) {
    if (world.current_map() == map_) return;
    clear();
    map_ = world.current_map();
}

// @orig none: port-only glass debris
bool GlassDebris::is_shattered(ObjId pile) const {
    return std::any_of(shattered_.begin(), shattered_.end(), [&](const Shattered& s) { return s.pile == pile; });
}

// @orig none: port-only glass debris
bool GlassDebris::shatter(const World& world, ObjId id) {
    if (is_shattered(id)) return true;
    const Item* pile = world.get(id);
    if (!pile || pile->parent != 0 || pile->off_map || chunk_count(pile->shape) == 0) return false;
    const bool flipped = pile->flags & kItemFlipped;
    for (const Chunk& c : chunks_) {
        if (c.shape != pile->shape) continue;
        Piece p;
        p.chunk = &c;
        p.pile = id;
        // A mirrored pile mirrors its pieces' places across x = y.
        p.x = pile->x + (flipped ? c.oy : c.ox);
        p.y = pile->y + (flipped ? c.ox : c.oy);
        p.z = pile->z & 0xFF;
        p.mirrored = flipped;
        p.resting = true;
        p.ground = static_cast<std::int32_t>(p.z);
        p.ground_known = true;
        p.gx = p.x;
        p.gy = p.y;
        next_seed_ = mix(next_seed_);
        p.spin = 3 + static_cast<int>(next_seed_ % 6);
        p.proxy.shape = pile->shape;
        pieces_.push_back(std::move(p));
    }
    shattered_.push_back({id, pile->shape, true});
    return true;
}

// @orig none: port-only glass debris
void GlassDebris::throw_piece(Piece& p, double vx, double vy, double vz) {
    const double h = std::hypot(vx, vy);
    if (h > kMaxSpeed) {
        vx *= kMaxSpeed / h;
        vy *= kMaxSpeed / h;
    }
    p.vx = vx;
    p.vy = vy;
    p.vz = vz;
    p.resting = false;
    p.age = 0;
    for (Shattered& s : shattered_)
        if (s.pile == p.pile) s.settled = false;
}

// @orig none: port-only glass debris
void GlassDebris::hit(const World& world, const ItemHit& h, std::uint32_t now) {
    new_map(world);
    const Item* item = world.get(h.item);
    if (!item || item->parent != 0 || item->off_map) return;
    const dtable::FireType* ft = dtable::fire_type(h.fire_type);
    const bool explosive = ft && ft->splash() > 0;
    const double reach = std::max(96, ft ? ft->splash() * 16 : 0);
    const World::Point from{h.from_x, h.from_y, h.from_z}, to{h.to_x, h.to_y, h.to_z};
    if (h.blast) {
        // Once per blast: the pieces in its reach, and the piles it reaches.
        if (now != last_blast_tick_ || from.x != last_blast_.x || from.y != last_blast_.y ||
            from.z != last_blast_.z) {
            last_blast_ = from;
            last_blast_tick_ = now;
            for (ObjId id : world.rect_search(from.x - static_cast<int>(reach) - 128, from.y - static_cast<int>(reach) - 128,
                                              from.x + static_cast<int>(reach) + 128, from.y + static_cast<int>(reach) + 128)) {
                const Item* p = world.get(id);
                if (p && is_glass_pile(p->shape)) shatter(world, id);
            }
            blow(world, from, reach, now);
        }
    } else if (is_glass_pile(item->shape)) {
        shatter(world, h.item);
        if (explosive) blow(world, to, reach, now);
        else kick(world, h.item, from, to);
        return;
    }
    if (!is_breakable_glass(item->shape)) return;
    if (std::any_of(watches_.begin(), watches_.end(), [&](const Watch& w) { return w.wall == h.item; })) return;
    const Footprint f = footprint(world, *item);
    const double cx = item->x - f.w / 2.0, cy = item->y - f.d / 2.0;
    // A shot: the way it went. A blast: away from it, the harder the nearer.
    const double dx = h.blast ? cx - from.x : to.x - from.x, dy = h.blast ? cy - from.y : to.y - from.y;
    const double d = std::hypot(dx, dy);
    Watch w;
    w.wall = h.item;
    w.shape = item->shape;
    if (d > 1) {
        w.ux = dx / d;
        w.uy = dy / d;
    } else {
        const double a = unit() * 2 * 3.14159265358979;
        w.ux = std::cos(a);
        w.uy = std::sin(a);
    }
    w.strength = h.blast ? 1.2 + 0.9 * std::clamp(1.0 - d / (reach + 128), 0.0, 1.0) : explosive ? 2.0 : 1.0;
    w.tick = now;
    for (ObjId near : world.rect_search(item->x - 256, item->y - 256, item->x + 256, item->y + 256)) {
        const Item* p = world.get(near);
        if (p && is_glass_pile(p->shape)) w.piles_before.push_back(near);
    }
    watches_.push_back(std::move(w));
}

// The pieces within `reach` of a blast at `at` fly up and away from it.
// @orig none: port-only glass debris
void GlassDebris::blow(const World& world, const World::Point& at, double reach, std::uint32_t now) {
    (void)world;
    (void)now;
    for (Piece& p : pieces_) {
        const double dx = p.x - at.x, dy = p.y - at.y, d = std::hypot(dx, dy);
        if (d >= reach || std::abs(p.z - at.z) > 64) continue;
        const double k = 1.0 - d / reach;
        double ux = 0, uy = 0;
        if (d > 1) {
            ux = dx / d;
            uy = dy / d;
        } else {
            const double a = unit() * 2 * 3.14159265358979;
            ux = std::cos(a);
            uy = std::sin(a);
        }
        // Lighter pieces further.
        const double light = std::clamp(30.0 / std::max(8, p.chunk->pixels), 0.7, 1.5);
        const double speed = (1.5 + 7.0 * k) * (0.6 + 0.7 * unit()) * light;
        const double side = (unit() - 0.5) * 0.6;
        throw_piece(p, (ux - uy * side) * speed, (uy + ux * side) * speed, 1.2 + (2.0 + 4.0 * k) * unit());
    }
}

// A shot that hits a pile kicks the pieces round where it hit.
// @orig none: port-only glass debris
void GlassDebris::kick(const World& world, ObjId pile, const World::Point& from, const World::Point& to) {
    (void)world;
    double ux = to.x - from.x, uy = to.y - from.y;
    const double l = std::hypot(ux, uy);
    if (l < 1) return;
    ux /= l;
    uy /= l;
    for (Piece& p : pieces_) {
        if (p.pile != pile) continue;
        const double d = std::hypot(p.x - to.x, p.y - to.y);
        if (d > 28) continue;
        const double speed = (1.0 - d / 28) * (1.0 + 2.5 * unit());
        const double side = (unit() - 0.5) * 1.2;
        throw_piece(p, (ux - uy * side) * speed, (uy + ux * side) * speed, 0.6 + 1.6 * unit());
    }
}

// The piles the wall just laid: their pieces burst out of the pane.
// @orig none: port-only glass debris
void GlassDebris::break_wall(World& world, const Watch& w) {
    const Item* wall = world.get(w.wall);
    if (!wall) return;
    const Footprint f = footprint(world, *wall);
    const double cx = wall->x - f.w / 2.0, cy = wall->y - f.d / 2.0;
    const double z = wall->z & 0xFF;
    // Along the pane: its longer side.
    const bool along_x = f.w >= f.d;
    const double length = along_x ? f.w : f.d;
    for (ObjId near : world.rect_search(wall->x - 256, wall->y - 256, wall->x + 256, wall->y + 256)) {
        const Item* pile = world.get(near);
        if (!pile || !is_glass_pile(pile->shape)) continue;
        if (std::find(w.piles_before.begin(), w.piles_before.end(), near) != w.piles_before.end()) continue;
        // Its own: in front of it or behind it, not a neighbour's that broke
        // at the same time.
        if (is_shattered(near)) continue;
        const Footprint pf = footprint(world, *pile);
        const double along = along_x ? pile->x - pf.w / 2.0 - cx : pile->y - pf.d / 2.0 - cy;
        if (std::abs(along) > length / 2 + 8) continue;
        if (!shatter(world, near)) continue;
        for (Piece& p : pieces_) {
            if (p.pile != near) continue;
            // Somewhere in the pane, low rather than high.
            const double t = unit() - 0.5, up = unit();
            p.x = cx + (along_x ? t * length : (unit() - 0.5) * 6);
            p.y = cy + (along_x ? (unit() - 0.5) * 6 : t * length);
            p.z = z + 2 + 54 * up * up;
            p.ground_known = false;
            const double light = std::clamp(30.0 / std::max(8, p.chunk->pixels), 0.7, 1.4);
            const double speed = w.strength * (0.5 + 3.4 * unit()) * light;
            const double side = (unit() - 0.5) * 0.9;
            throw_piece(p, (w.ux - w.uy * side) * speed, (w.uy + w.ux * side) * speed,
                        (0.2 + 1.8 * unit()) * std::min(1.6, w.strength));
        }
    }
}

// The floor under the piece, at most 200 below it; false over nothing.
// @orig none: port-only glass debris
bool GlassDebris::find_ground(const World& world, Piece& p) {
    std::int32_t top = 0;
    const Item* floor = floor_under(world, p.x, p.y, p.z, 200, top);
    p.gx = p.x;
    p.gy = p.y;
    p.ground_known = floor != nullptr;
    if (floor) p.ground = top;
    return floor != nullptr;
}

// One tick of a piece that moves.
// @orig none: port-only glass debris
void GlassDebris::step(const World& world, Piece& p) {
    ++p.age;
    if (!p.ground_known || std::abs(p.x - p.gx) + std::abs(p.y - p.gy) > 6) {
        if (!find_ground(world, p)) {
            // Over nothing: it is lost below.
            p.ground = -1000;
            p.ground_known = true;
        }
    }
    const bool airborne = p.z > p.ground + 0.01 || p.vz > 0;
    if (airborne) {
        p.vz -= kGravity;
        p.vx *= kDrag;
        p.vy *= kDrag;
        if (p.spin > 0 && p.age % p.spin == 0) p.mirrored = !p.mirrored;
    } else {
        // Sliding on the floor.
        const double v = std::hypot(p.vx, p.vy);
        const double left = std::max(0.0, v * kSlideDrag - kSlideBrake);
        if (left < 0.05) {
            p.vx = p.vy = 0;
            p.resting = true;
            return;
        }
        p.vx *= left / v;
        p.vy *= left / v;
    }
    const double nx = p.x + p.vx, ny = p.y + p.vy;
    const double nz = std::max(p.z + p.vz, static_cast<double>(p.ground));
    const double probe = std::max(nz, static_cast<double>(p.ground) + 2);
    // Something solid in the way: it bounces back off it.
    if (solid_at(world, nx, ny, probe)) {
        const bool block_x = solid_at(world, nx, p.y, probe), block_y = solid_at(world, p.x, ny, probe);
        if (block_x || !block_y) p.vx = -p.vx * 0.3;
        if (block_y || !block_x) p.vy = -p.vy * 0.3;
    } else {
        p.x = nx;
        p.y = ny;
    }
    if (p.z + p.vz <= p.ground && airborne) {
        // Down on the floor: a bounce, or it slides on.
        p.z = p.ground;
        if (p.vz < -1.0) {
            p.vz = -p.vz * kBounce * (0.6 + 0.8 * unit());
            p.vx *= 0.6;
            p.vy *= 0.6;
            p.mirrored = !p.mirrored;
        } else {
            p.vz = 0;
        }
    } else {
        p.z = nz;
    }
    if (p.z < -200 || p.age > kMaxFlight) {
        p.z = std::max(p.z, static_cast<double>(p.ground));
        p.vx = p.vy = p.vz = 0;
        p.resting = true;
    }
}

// The piles whose pieces all came to rest move to their middle.
// @orig none: port-only glass debris
void GlassDebris::settle(World& world) {
    for (Shattered& s : shattered_) {
        if (s.settled) continue;
        double sx = 0, sy = 0, sz = 0;
        int n = 0;
        bool moving = false;
        for (const Piece& p : pieces_) {
            if (p.pile != s.pile) continue;
            moving |= !p.resting;
            sx += p.x;
            sy += p.y;
            sz += p.z;
            ++n;
        }
        if (moving) continue;
        s.settled = true;
        const Item* pile = world.get(s.pile);
        if (!pile || n == 0 || pile->shape != s.shape) continue;
        const Footprint f = footprint(world, *pile);
        const auto x = static_cast<std::int32_t>(std::lround(sx / n + f.w / 2.0));
        const auto y = static_cast<std::int32_t>(std::lround(sy / n + f.d / 2.0));
        const auto z = std::clamp(static_cast<std::int32_t>(std::lround(sz / n)) + 1, 0, 250);
        if (x != pile->x || y != pile->y || z != (pile->z & 0xFF)) world.move_item(s.pile, x, y, z);
    }
}

// @orig none: port-only glass debris
void GlassDebris::advance(World& world, std::uint32_t now) {
    new_map(world);
    if (now < tick_) clear();  // time went back: a load
    if (tick_ == 0 || now - tick_ > kMaxCatchUp) tick_ = now > 0 ? now - 1 : 0;
    while (tick_ < now) {
        ++tick_;
        // The walls hit that broke throw their pieces.
        std::erase_if(watches_, [&](const Watch& w) {
            const Item* wall = world.get(w.wall);
            if (!wall) return true;
            if (wall->shape != w.shape) {
                break_wall(world, w);
                return true;
            }
            return tick_ - w.tick > static_cast<std::uint32_t>(kWatchTicks);
        });
        for (Piece& p : pieces_)
            if (!p.resting) step(world, p);
    }
    settle(world);
    // A pile gone (destroyed, or changed) takes its pieces along.
    std::erase_if(shattered_, [&](const Shattered& s) {
        const Item* pile = world.get(s.pile);
        if (pile && pile->shape == s.shape && pile->parent == 0 && !pile->off_map) return false;
        std::erase_if(pieces_, [&](const Piece& p) { return p.pile == s.pile; });
        return true;
    });
    // Too many pieces: the oldest pile at rest shows as a pile again.
    while (pieces_.size() > kMaxPieces) {
        const auto it = std::find_if(shattered_.begin(), shattered_.end(), [](const Shattered& s) { return s.settled; });
        if (it == shattered_.end()) break;
        const ObjId pile = it->pile;
        std::erase_if(pieces_, [&](const Piece& p) { return p.pile == pile; });
        shattered_.erase(it);
    }
}

// @orig none: port-only glass debris
void GlassDebris::extras(DisplayExtras& out) const {
    for (const Shattered& s : shattered_) out.hidden.push_back(s.pile);
    std::sort(out.hidden.begin(), out.hidden.end());
    for (const Piece& p : pieces_) {
        // A small box round the piece, in world units (2 x usecode).
        Item& proxy = p.proxy;
        proxy.x = static_cast<std::int32_t>(std::lround(p.x));
        proxy.y = static_cast<std::int32_t>(std::lround(p.y));
        proxy.z = static_cast<std::int32_t>(std::lround(p.z));
        ExtraSprite e;
        e.proxy = &p.proxy;
        e.frame = &p.chunk->frame;
        e.mirrored = p.mirrored;
        e.x = static_cast<std::int32_t>(std::lround(p.x * 2));
        e.y = static_cast<std::int32_t>(std::lround(p.y * 2));
        e.z = static_cast<std::int32_t>(std::lround(p.z));
        e.x_left = e.x - 8;
        e.y_far = e.y - 8;
        e.z_top = e.z + 1;
        out.sprites.push_back(e);
    }
}

}  // namespace crusader
