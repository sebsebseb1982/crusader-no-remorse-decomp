#include "engine/world/collision.hpp"

#include "engine/world/inventory.hpp"

#include <algorithm>
#include <cstdlib>

namespace crusader {

namespace {

struct Box {
    std::int32_t w = 0, d = 0, h = 0;  // x extent, y extent, height
};

// @orig 10E8:032E
Box box_of(const ShapeInfo* info, bool mirrored) {
    Box b;
    if (!info) return b;
    b.w = info->footpad_x * 16;
    b.d = info->footpad_y * 16;
    b.h = info->footpad_z * 8;
    if (b.h == 0 && !info->is(ShapeInfo::kFixed)) b.h = 1;
    if (mirrored) std::swap(b.w, b.d);
    return b;
}

constexpr std::int32_t kMaxZ = 250;

// State shared by the per-item test (the original's sweep object).
struct Sweep {
    const World& world;
    Box mover;
    std::uint8_t mask;          // low byte of the mover's mask
    bool mover_damaging;
    std::uint16_t mode = 6;     // bit 2 cleared by kSweepSupportOnly
    bool need_ground;
    bool any_ground_below = false;  // the original's +0x11: makes need_ground fail
    std::int32_t gap_allowed;
    World::Point start, end;
    std::int32_t dx, dy, dz;
    std::int32_t best_dx, best_dy, best_dz;
    SweepResult result;
    ObjId goal = 0;  // +0x2D

    // @orig 10E8:0228
    void add_hit(ObjId item, std::int32_t hx, std::int32_t hy, std::int32_t hz, bool blocking) {
        // ITEMFIND.C 0228: a blocking contact closer on any axis becomes the blocker.
        if (blocking && (std::abs(hx) < std::abs(best_dx) || std::abs(hy) < std::abs(best_dy) ||
                         std::abs(hz) < std::abs(best_dz))) {
            if (goal != 0 && item == goal) {
                result.goal_hit = true;
                if (result.blocker == 0) result.goal_first = true;
            } else {
                result.goal_first = false;
            }
            result.blocker = item;
            best_dx = hx;
            best_dy = hy;
            best_dz = hz;
        }
        result.hits.push_back({item, hx, hy, hz, blocking});
    }

    // ITEMFIND.C 032E.
    // @orig 10E8:032E
    void test(const Item& c) {
        const ShapeInfo* info = world.shape_info(c.shape);
        const Box cb = box_of(info, c.flags & 0x0020);
        const std::int32_t cx = static_cast<std::uint16_t>(c.x), cy = static_cast<std::uint16_t>(c.y);
        const std::int32_t cz = static_cast<std::uint8_t>(c.z);

        // Support under the end point.
        if (need_ground) {
            const std::int32_t gap = end.z - (cz + cb.h);
            if (gap >= 0 && gap <= gap_allowed && end.x - mover.w + 1 <= cx && end.y - mover.d + 1 <= cy &&
                cx - cb.w + 1 <= end.x && cy - cb.d + 1 <= end.y) {
                if (!(mode & 4)) {
                    if (info && info->is(ShapeInfo::kSolid)) {
                        result.supported = true;
                        gap_allowed = gap;
                        result.ground_gap = gap;
                        if (gap == 0) add_hit(c.id, 0, 0, 0, false);
                    }
                } else {
                    any_ground_below = true;
                }
            }
        }

        const std::uint16_t cmask = collision_mask(info);
        const bool blocking = (cmask & mask & 0xFF) != 0;
        const bool damaging = (info && info->is(ShapeInfo::kDamaging)) || mover_damaging;
        if (!blocking && !damaging) return;

        // Distances from the start box to each face of the item's box.
        const std::int64_t to_low_x = (cx - cb.w + 1) - start.x;
        const std::int64_t to_low_y = (cy - cb.d + 1) - start.y;
        const std::int64_t to_high_x = (cx + mover.w - 1) - start.x;
        const std::int64_t to_high_y = (cy + mover.d - 1) - start.y;
        const std::int64_t to_top = (cz + cb.h - 1) - start.z;
        const std::int64_t to_bottom = (cz - mover.h + 1) - start.z;

        // At time t / s along the move, is the other coordinate within [lo, hi]?
        auto within = [](std::int64_t lo, std::int64_t hi, std::int64_t v, std::int64_t t, std::int64_t s) {
            // lo <= v * t / s <= hi with s the (signed) axis speed.
            if (s > 0) return lo * s <= v * t && v * t <= hi * s;
            return hi * s <= v * t && v * t <= lo * s;
        };
        auto touch = [&](std::int64_t t, std::int64_t s, int axis) {
            if (blocking) {
                result.axes |= static_cast<std::uint16_t>(axis);
                ok = false;
            }
            const std::int64_t back = s > 0 ? t - 1 : t + 1;
            const std::int64_t h[3] = {axis == 1 ? back : dx * back / s, axis == 2 ? back : dy * back / s,
                                       axis == 4 ? back : dz * back / s};
            add_hit(c.id, static_cast<std::int32_t>(h[0]), static_cast<std::int32_t>(h[1]),
                    static_cast<std::int32_t>(h[2]), blocking);
        };
        // Faces in the original's order: +x, +y, +z, -x, -y, -z.
        if (dx != 0 && to_low_x <= dx && to_low_x > 0 && within(to_low_y, to_high_y, dy, to_low_x, dx) &&
            within(to_bottom, to_top, dz, to_low_x, dx)) {
            return touch(to_low_x, dx, 1);
        }
        if (dy != 0 && to_low_y <= dy && to_low_y > 0 && within(to_low_x, to_high_x, dx, to_low_y, dy) &&
            within(to_bottom, to_top, dz, to_low_y, dy)) {
            return touch(to_low_y, dy, 2);
        }
        if (dz != 0 && to_bottom <= dz && to_bottom > 0 && within(to_low_x, to_high_x, dx, to_bottom, dz) &&
            within(to_low_y, to_high_y, dy, to_bottom, dz)) {
            return touch(to_bottom, dz, 4);
        }
        if (dx != 0 && dx <= to_high_x && to_high_x < 0 && within(to_low_y, to_high_y, dy, to_high_x, dx) &&
            within(to_bottom, to_top, dz, to_high_x, dx)) {
            return touch(to_high_x, dx, 1);
        }
        if (dy != 0 && dy <= to_high_y && to_high_y < 0 && within(to_low_x, to_high_x, dx, to_high_y, dy) &&
            within(to_bottom, to_top, dz, to_high_y, dy)) {
            return touch(to_high_y, dy, 2);
        }
        if (dz != 0 && dz <= to_top && to_top < 0 && within(to_low_x, to_high_x, dx, to_top, dz) &&
            within(to_low_y, to_high_y, dy, to_top, dz)) {
            return touch(to_top, dz, 4);
        }
    }

    bool ok = true;
};

}  // namespace

// @orig 1018:0000
std::uint16_t collision_mask(const ShapeInfo* info) {
    std::uint16_t m = 0x8000;
    if (info && info->is(ShapeInfo::kSolid)) m |= 1;
    if (info && info->is(ShapeInfo::kDamaging)) m |= 0x20;
    return m;
}

// @orig 10E8:0B39
SweepResult sweep(const World& world, std::uint16_t shape, ObjId mover, const World::Point& start,
                  const World::Point& end, std::uint8_t flags, std::uint16_t mask, ObjId goal) {
    const ShapeInfo* info = world.shape_info(shape);
    const Item* m = world.get(mover);
    Sweep s{world,
            box_of(info, m && (m->flags & 0x0020)),
            static_cast<std::uint8_t>(mask ? mask : collision_mask(info) & 0x7FFF),
            info && info->is(ShapeInfo::kDamaging),
            static_cast<std::uint16_t>((flags & kSweepSupportOnly) ? 2 : 6),
            !(flags & kSweepNoGround),
            false,
            (flags & kSweepGroundSlack) ? 9 : 0,
            start,
            end,
            end.x - start.x,
            end.y - start.y,
            end.z - start.z,
            0,
            0,
            0,
            {}};
    s.goal = goal;
    s.best_dx = s.dx;
    s.best_dy = s.dy;
    s.best_dz = s.dz;
    s.result.end = end;
    s.result.delta = {s.dx, s.dy, s.dz};
    s.result.done = s.result.delta;
    if (start.z > kMaxZ) return s.result;

    // Out of the map: clipped, not ok.
    if (end.x < 0) {
        s.result.end.x = s.dx < 0 ? 0 : 0x7FFF;
        s.result.axes |= 1;
        s.result.off_map = true;
        s.ok = false;
    }
    if (end.y < 0) {
        s.result.end.y = s.dy < 0 ? 0 : 0x7FFF;
        s.result.axes |= 2;
        s.result.off_map = true;
        s.ok = false;
    }
    if (end.z < 0 || end.z > kMaxZ) {
        s.result.end.z = end.z < 0 ? 0 : kMaxZ;
        s.result.axes |= 4;
        s.ok = false;
    }

    // Both footprints must lie in the fast area (ITEMCACH.C 1BF5 on the four corners).
    const FastArea& area = world.fast_area();
    auto footprint_in = [&](const World::Point& p) {
        return area.contains(p.x - s.mover.w + 1, p.y - s.mover.d + 1) && area.contains(p.x, p.y - s.mover.d + 1) &&
               area.contains(p.x - s.mover.w + 1, p.y) && area.contains(p.x, p.y);
    };
    if (!footprint_in(start)) {
        s.result.end = start;
        return s.result;
    }
    if (!footprint_in(end)) {
        s.result.end = start;
        s.result.axes |= 3;
        s.result.off_map = true;
        return s.result;
    }

    // Candidates: items whose x/y lie in the swept rectangle widened by the
    // box and by 0x100 (the largest footprints).
    auto lo = [](std::int32_t v) { return v < 0 ? 0 : v; };
    auto hi = [](std::int32_t v) { return static_cast<std::int16_t>(v) < 0 ? 0x7FFF : v; };
    const std::int32_t x0 = lo(std::min(start.x, end.x) - s.mover.w);
    const std::int32_t y0 = lo(std::min(start.y, end.y) - s.mover.d);
    const std::int32_t x1 = hi(std::max(start.x, end.x) + 0x100);
    const std::int32_t y1 = hi(std::max(start.y, end.y) + 0x100);
    for (int gy = y0 >> World::kCellShift; gy <= (y1 >> World::kCellShift); ++gy) {
        for (int gx = x0 >> World::kCellShift; gx <= (x1 >> World::kCellShift); ++gx) {
            for (ObjId id : world.cell(gx, gy)) {
                if (id == mover) continue;
                const Item& c = *world.get(id);
                if (c.sprite_only) continue;
                const std::int32_t cx = static_cast<std::uint16_t>(c.x), cy = static_cast<std::uint16_t>(c.y);
                if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
                s.test(c);
            }
        }
    }

    if (s.result.blocker != 0) {
        s.result.end = {start.x + s.best_dx, start.y + s.best_dy, std::clamp(start.z + s.best_dz, 0, kMaxZ)};
        s.result.done = {s.best_dx, s.best_dy, s.best_dz};
    }
    s.result.ok = s.ok && (!s.need_ground || (s.result.supported && !s.any_ground_below));
    return s.result;
}

// @orig 10E8:00D9 1018:00FD
bool box_free(const World& world, std::int32_t w, std::int32_t d, std::int32_t h, std::uint16_t mask,
              const World::Point& at, ObjId* first_solid, ObjId ignore) {
    if (first_solid) *first_solid = 0;
    const std::int32_t x0 = at.x - w + 1, y0 = at.y - d + 1, x1 = at.x, y1 = at.y;
    if (static_cast<std::int16_t>(x0) < 0 || static_cast<std::int16_t>(y0) < 0 || static_cast<std::int16_t>(x1) < 0 ||
        static_cast<std::int16_t>(y1) < 0) {
        return false;
    }
    const std::int32_t z0 = at.z & 0xFF, z1 = std::min<std::int32_t>(z0 + h - 1, 0xFF);
    if (z0 == 0 && z1 == 0xFF) return true;
    std::uint16_t found = 0;
    for (int gy = y0 >> World::kCellShift; gy <= ((y1 + 0x100) >> World::kCellShift); ++gy) {
        for (int gx = x0 >> World::kCellShift; gx <= ((x1 + 0x100) >> World::kCellShift); ++gx) {
            for (ObjId id : world.cell(gx, gy)) {
                const Item& c = *world.get(id);
                if (c.sprite_only || id == ignore) continue;
                const std::int32_t cx = static_cast<std::uint16_t>(c.x), cy = static_cast<std::uint16_t>(c.y);
                const std::int32_t cz = c.z & 0xFF;
                if (cx < x0 || cx > x1 + 0x100 || cy < y0 || cy > y1 + 0x100 || cz > z1) continue;
                const ShapeInfo* info = world.shape_info(c.shape);
                const Box cb = box_of(info, false);
                if (cx - cb.w + 1 > x1 || cy - cb.d + 1 > y1 || z0 > cz + cb.h - 1) continue;
                found |= collision_mask(info) & 0x7FFF;
                if (first_solid && *first_solid == 0 && (found & 1)) *first_solid = id;
            }
        }
    }
    return (mask & found) == 0;
}

bool position_free(const World& world, std::uint16_t shape, const World::Point& at, ObjId* first_solid,
                   ObjId ignore) {
    const ShapeInfo* info = world.shape_info(shape);
    const Box b = box_of(info, false);
    return box_free(world, b.w, b.d, b.h, collision_mask(info), at, first_solid, ignore);
}

// @orig 10A8:104E
ObjId legal_create_at(World& world, std::uint16_t shape, std::uint8_t frame, const World::Point& at) {
    if (shape > 0x7FF || !position_free(world, shape, at)) return 0;
    const ObjId id = world.create(shape, frame);
    if (id != 0) world.pop_to_coords(at.x, at.y, at.z);
    return id;
}

SweepResult try_move(const World& world, ObjId mover, const World::Point& end, std::uint8_t flags) {
    const Item* m = world.get(mover);
    if (!m) return {};
    return try_move(world, mover, World::Point{m->x, m->y, m->z}, end, flags);
}

// @orig 10E8:162F
SweepResult try_move(const World& world, ObjId mover, const World::Point& start, const World::Point& end,
                     std::uint8_t flags, ObjId goal) {
    const Item* m = world.get(mover);
    if (!m) return {};
    if (start.z != end.z) return sweep(world, m->shape, mover, start, end, flags, 0, goal);

    // Level move: the ground may be up to 9 below; follow it down, straight
    // down from the end point (the second sweep starts at param_4). From the
    // start, a step down would cross the step being left.
    SweepResult r = sweep(world, m->shape, mover, start, end, flags | kSweepGroundSlack, 0, goal);
    if (r.ok) {
        if (r.ground_gap != 0) {
            const World::Point lower{end.x, end.y, end.z - r.ground_gap};
            SweepResult down = sweep(world, m->shape, mover, end, lower, flags);
            if (down.ok) return down;
        }
        return r;
    }
    if (!(flags & kSweepStepUp)) return r;
    // Not onto the goal when it blocks first.
    if (goal != 0 && r.goal_first) return r;
    // Blocked: try climbing 8, then 9 units. At 9 the ground below is not
    // followed down. Refused, the last sweep tried is returned.
    for (std::int32_t step : {8, 9}) {
        const World::Point raised{start.x, start.y, start.z + step};
        r = sweep(world, m->shape, mover, start, raised, kSweepNoGround, 0, goal);
        if (!r.ok) continue;
        r = sweep(world, m->shape, mover, raised, {end.x, end.y, end.z + step}, flags | kSweepGroundSlack);
        if (r.ok) {
            r.end.z = end.z + step - (step == 8 ? r.ground_gap : 0);
            return r;
        }
        if (step == 9) return r;
    }
    return r;
}

namespace {

// SHAPHAND_1088_0000: a moving stack meets one of the same shape (families 2
// and 9; 9 needs the same frame too). Their quantities go to the mover, up
// to 666, and the other keeps the rest or goes.
// @orig 1088:0000
void merge_stacks(World& world, ObjId mover, ObjId other) {
    const Item* a = world.get(mover);
    const Item* b = world.get(other);
    if (!a || !b || a->shape != b->shape) return;
    const std::uint8_t family = world.family_of(a->shape);
    if (family != 2 && family != 9) return;
    if (family == 9 && a->frame != b->frame) return;
    constexpr int kMaxStack = 666;
    const int total = a->quality + b->quality;
    if (total <= kMaxStack) {
        set_quality_and_combine(world, mover, static_cast<std::uint16_t>(total));
        world.destroy(other);
    } else {
        set_quality_and_combine(world, mover, kMaxStack);
        set_quality_and_combine(world, other, static_cast<std::uint16_t>(total - kMaxStack));
    }
}

// The two contact events (REFSTORE.C 0379, processes 0x20B and 0x20C).
// @orig 10F8:0379 10A8:2718
void send_contact(World& world, ObjId mover, ObjId other, std::uint16_t force) {
    // ITEM.C 2718: event 5 ("hit") of the mover, with the other's id and the force.
    if (world.hooks().start_event_args && world.get(mover)) {
        const std::uint8_t args[4] = {static_cast<std::uint8_t>(other), static_cast<std::uint8_t>(other >> 8),
                                      static_cast<std::uint8_t>(force), static_cast<std::uint8_t>(force >> 8)};
        world.hooks().start_event_args(mover, 5, args);
    }
    if (world.hooks().got_hit && world.get(other)) world.hooks().got_hit(other, mover, force);
}

}  // namespace

// @orig 10E8:123A
void finish_move(World& world, ObjId mover, const SweepResult& r) {
    if (mover == 0) return;
    world.move_item(mover, r.end.x, r.end.y, r.end.z);
    // Touched before the stop, on one axis at least.
    auto touched = [&r](const SweepHit& h) {
        return std::abs(h.dx) <= std::abs(r.done.x) || std::abs(h.dy) <= std::abs(r.done.y) ||
               std::abs(h.dz) <= std::abs(r.done.z);
    };
    if (r.blocker != 0) {
        const Item* m = world.get(mover);
        const Item* b = world.get(r.blocker);
        const std::uint8_t family = m ? world.family_of(m->shape) : 0;
        if (m && b && (family == 2 || family == 9) && m->shape == b->shape) {
            merge_stacks(world, mover, r.blocker);
        } else {
            const std::int32_t ax = std::abs(r.delta.x >> 2), ay = std::abs(r.delta.y >> 2), az = std::abs(r.delta.z);
            const auto force = static_cast<std::uint16_t>((ax + ay + az + std::max({ax, ay, az})) >> 1);
            send_contact(world, mover, r.blocker, force);
        }
    }
    for (auto it = r.hits.rbegin(); it != r.hits.rend(); ++it) {
        if (it->item != r.blocker && touched(*it)) send_contact(world, mover, it->item, 0);
    }
}

}  // namespace crusader
