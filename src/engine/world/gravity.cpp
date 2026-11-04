#include "engine/world/gravity.hpp"

#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/actor_anim.hpp"
#include "engine/world/collision.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace crusader {

namespace {

struct Footprint {
    int w, d, h;
};

// @orig 1040:0072
Footprint footprint(const ShapeInfo* info) {
    if (!info) return {0, 0, 0};
    int h = info->footpad_z * 8;
    if (h == 0 && !info->is(ShapeInfo::kFixed)) h = 1;
    return {info->footpad_x * 16, info->footpad_y * 16, h};
}

constexpr std::size_t kStartZ = 0x11;  // NPC record: height when the fall began
constexpr std::uint8_t kFalling = 0x02;  // NPC record +0x59

}  // namespace

// @orig 1040:0072
std::uint8_t support_mask(const World& world, std::uint16_t shape, std::int32_t x, std::int32_t y, std::int32_t z,
                          bool slack, ObjId ignore) {
    if (z == 0) return 0x1F;
    const ShapeInfo* info = world.shape_info(shape);
    const Footprint f = footprint(info);
    const std::uint16_t mask = collision_mask(info);
    const int x0 = x - f.w + 1, y0 = y - f.d + 1;
    const int x_lo_mid = (x0 + x) >> 1, x_hi_mid = (x0 + x + 1) >> 1;
    const int y_lo_mid = (y0 + y) >> 1, y_hi_mid = (y0 + y + 1) >> 1;

    std::uint8_t result = 0;
    bool exact = false;
    // Candidates whose x / y lie from the near corner to 0x1E0 beyond.
    const int sx0 = std::max(x0, 0), sy0 = std::max(y0, 0), sx1 = x + 0x1E0, sy1 = y + 0x1E0;
    for (int gy = sy0 >> World::kCellShift; gy <= (sy1 >> World::kCellShift); ++gy) {
        for (int gx = sx0 >> World::kCellShift; gx <= (sx1 >> World::kCellShift); ++gx) {
            for (ObjId id : world.cell(gx, gy)) {
                if (id == ignore) continue;
                const Item& c = *world.get(id);
                const int cx = static_cast<std::uint16_t>(c.x), cy = static_cast<std::uint16_t>(c.y);
                const int cz = static_cast<std::uint8_t>(c.z);
                if (cx < x0 || cy < y0 || cx > sx1 || cy > sy1 || cz > z) continue;
                const ShapeInfo* ci = world.shape_info(c.shape);
                const Footprint cf = footprint(ci);
                const int top = cz + cf.h;
                if (cx - cf.w + 1 > x || cy - cf.d + 1 > y) continue;
                if (top != z && !(slack && top <= z && z - 9 <= top)) continue;
                if (!(mask & collision_mask(ci) & 0x7FFF)) continue;
                if (top == z) exact = true;
                if (cx - cf.w + 1 <= x_lo_mid) {
                    if (cy - cf.d + 1 <= y_lo_mid) result |= 8;
                    if (y_hi_mid <= cy) result |= 4;
                }
                if (x_hi_mid <= cx) {
                    if (cy - cf.d + 1 <= y_lo_mid) result |= 1;
                    if (y_hi_mid <= cy) result |= 2;
                }
            }
        }
    }
    if (((result & 8) && (result & 2)) || ((result & 4) && (result & 1))) result |= 0x10;
    if (slack && !exact) result = 0;
    return result;
}

// @orig 1040:0395
GravityProcess::GravityProcess(World& world, ObjId item, int vx_, int vy_, int vz_, int gravity_)
    : vx(vx_), vy(vy_), vz(vz_), gravity(gravity_), world_(world) {
    set_owner(item);
    set_type(kType);
    if (Item* i = world_.get(item)) i->flags &= static_cast<std::uint16_t>(~kItemBouncing);
    if (is_npc_id(item)) {
        if (auto r = world_.npc_record(item); !r.empty()) {
            r[npc::kAnimFlags] |= kFalling;
            if (const Item* i = world_.get(item)) r[kStartZ] = static_cast<std::uint8_t>(i->z);
        }
    }
}

// @orig 1040:06D2
void GravityProcess::run() {
    const ObjId id = owner();
    Item* item = world_.get(id);
    if (!item) {
        terminate();
        return;
    }
    if ((item->z & 0xFF) > 250) {
        // 11D8:0D0E: every process of the item fails, this one included.
        kernel().fail_processes(id, Kernel::kAnyType, 1);
        return;
    }
    if (item->flags & kItemContained) {
        terminate();
        return;
    }
    // Out of the fast area the fall waits, without sleeping, for the item to
    // come back (the process followed a target otherwise, see gravity.hpp).
    if (!(item->flags & kItemFastArea)) return;

    // DS:3A88: a slice that comes late makes a step of 2 to 4 times as far.
    const std::uint32_t late = kernel().slice_ticks();
    const int k = late >= 4 ? 4 : late >= 3 ? 3 : late >= 2 ? 2 : 1;
    const bool controlled = id == world_.state.controlled_npc;  // DS:2852
    const bool npc = is_npc_id(id);
    const bool low_friction = item->flags & kItemLowFriction;
    auto quarter = [](int v) { return (v < 0 ? v + 1 : v) >> 2; };
    auto rnd = [this](int n) {
        return static_cast<int>(random_range(world_.state.random_state, static_cast<std::uint16_t>(n)));
    };

    // 1040:0899: an item that starts falling (or bounces) leaves what rests
    // on it; moving sideways, it also leaves what it rests on.
    if (vz == 0 || (item->flags & (kItemLowFriction | kItemBouncing))) {
        if (vx == 0 && vy == 0) drop_items_on_top(kernel(), world_, id);
        else leave_surface(kernel(), world_, id);
        item = world_.get(id);
        if (!item) {
            terminate();
            return;
        }
    }
    item->flags &= static_cast<std::uint16_t>(~kItemBouncing);
    // The move uses vz from before gravity.
    const World::Point from{item->x, item->y, item->z};
    const World::Point to{item->x + vx * k, item->y + vy * k, (item->z & 0xFF) + vz * k};
    vz -= gravity * k;
    SweepResult r = sweep(world_, item->shape, id, from, to, kSweepNoGround);
    std::uint16_t axes = 0;
    bool landed = false;
    if (!r.ok) {
        axes = r.axes;
        if (axes) item->flags |= kItemBouncing;
        if (axes & 4) {
            // Landed (or hit a ceiling). A low-friction item goes on sideways
            // at the height where it stopped.
            if (low_friction) {
                r = sweep(world_, item->shape, id, from, {to.x, to.y, r.end.z}, kSweepNoGround);
                axes = r.axes | 4;
            }
            landed = true;
        }
    }
    // With the contact events: what it lands on or bumps into gets event 6
    // ("gotHit") with the force of the fall (ITEMFIND.C 123A).
    finish_move(world_, id, r);
    if (terminated()) return;
    item = world_.get(id);
    if (!item) {
        terminate();
        return;
    }
    if (r.off_map) vx = vy = 0;
    if (axes == 0) {
        if (--steps_left_ == 0) terminate();
        else sleep(4 * k);
        return;
    }

    if (!rolling_) {
        if (low_friction) {
            // A random nudge, one unit towards 0 or away from it.
            const int n = rnd(0x14);
            if (n < 4) vx = vx < 0 ? vx + 1 : vx > 0 ? vx - 1 : 0;
            else if (n < 8) vy = vy < 0 ? vy + 1 : vy > 0 ? vy - 1 : 0;
            else if (n == 8) ++vx;
            else if (n == 9) --vx;
            else if (n == 10) ++vy;
            else if (n == 11) --vy;
            if (!(item->flags & kItemBouncing)) axes &= ~3;
        }
        // Friction: a quarter of the horizontal speed is left.
        if (!low_friction || (axes & 3)) {
            vx = quarter(vx);
            vy = quarter(vy);
        }
    }
    if (!(axes & 4)) vz = quarter(vz);
    // Against a wall, an NPC loses its speed, an item (or a rolling NPC) rebounds.
    if (axes & 1) vx = !rolling_ && npc ? 0 : -vx;
    if (axes & 2) vy = !rolling_ && npc ? 0 : -vy;

    bool stands = false;
    if (axes & 4) {
        vz += gravity;
        if (controlled && vz <= 0) {
            // 1040:0000: the controlled NPC, half over a ledge, is pushed off
            // it, away from the side that holds it.
            const std::uint8_t m = support_mask(world_, item->shape, item->x, item->y, item->z, false, id);
            if ((m & 9) && !(m & 6)) vy += 4;
            if ((m & 3) && !(m & 0xC)) vx -= 4;
            if ((m & 6) && !(m & 9)) vy -= 4;
            if ((m & 0xC) && !(m & 3)) vx += 4;
            stands = m & 0x10;
        }
        if (!(item->flags & (kItemLowFriction | kItemBouncing))) {
            terminate();
            return;
        }
        if (!stands) {
            if (!controlled) {
                terminate();
                return;
            }
            // Not standing: it slides on, at least 4 units a step on each
            // axis under 16 (one at rest only has one chance in two to go
            // towards low coordinates, as written in the original).
            for (int* v : {&vx, &vy}) {
                if (std::abs(*v) >= 0x10) continue;
                if (*v > 0) *v += 4;
                else if (*v < 0) *v -= 4;
                else if (rnd(2) == 0) *v -= 4;
            }
            vz = 0;
            rolling_ = true;
        } else {
            if (landed && npc) {
                terminate();
                return;
            }
            // A rebound, a quarter as fast.
            vz = vz < 0 ? (-vz) >> 2 : -(vz >> 2);
            rolling_ = false;
            if (!low_friction) {
                if (vz < gravity) {
                    terminate();
                    return;
                }
            } else {
                if (!controlled) {
                    terminate();
                    return;
                }
                if (vz < gravity) {
                    vz = 0;
                } else {
                    vx += rnd(vz);
                    vx -= rnd(vz);
                    vy += rnd(vz);
                    vy -= rnd(vz);
                }
                if (std::abs(vx) < 4 && std::abs(vy) < 4 && vz < gravity) {
                    terminate();
                    return;
                }
            }
        }
    }
    // 1040:1131: after a rebound that does not go up, a blocked way down ends it.
    if (stands && static_cast<std::int16_t>(vz * gravity) <= 0) {
        const SweepResult down = sweep(world_, item->shape, id, {item->x, item->y, item->z},
                                       {item->x, item->y, static_cast<std::uint8_t>(item->z + vz)}, kSweepNoGround);
        if (!down.ok) {
            terminate();
            return;
        }
    }
    if (--steps_left_ == 0) {
        terminate();
        return;
    }
    sleep(4 * k);
}

// @orig 1040:04BC
void GravityProcess::on_terminate() {
    // 1040:04BC: the NPC no longer falls.
    const ObjId id = owner();
    const Item* item = world_.get(id);
    if (!is_npc_id(id) || !item || (item->z & 0xFF) >= 0xFB) return;
    auto r = world_.npc_record(id);
    if (r.empty()) return;
    r[npc::kAnimFlags] &= static_cast<std::uint8_t>(~kFalling);
    if (failed() || id != 1 || world_.is_dead(id)) return;
    const int steps = (r[kStartZ] - (item->z & 0xFF)) / 8;
    const int last = r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8;
    const bool running = last == 3 || last == 0x30 || last == 0x31 || last == 0x38;  // 10F0:21A0
    if (last == 0x3A || last == 0x3D || last == 0x3E || last == 0x17 || last == 0x18 || running || steps < 3) return;
    if (world_.hooks().hard_landing) world_.hooks().hard_landing(id);
}

// @orig 10A8:0020
ReleaseProcess::ReleaseProcess(World& world, ObjId item) : world_(world) {
    set_owner(item);
    set_type(kType);
}

// @orig 10A8:0078
void ReleaseProcess::run() {
    if (world_.get(owner()) && world_.hooks().start_event) world_.hooks().start_event(owner(), 9);
    terminate();
}

// @orig 10A8:2970
void leave_surface(Kernel& kernel, World& world, ObjId mover) {
    static constexpr std::uint8_t kAll[] = {'$'};
    std::vector<std::uint8_t> sides;
    const std::vector<ObjId> found = world.surface_search(mover, 3, kAll, &sides);
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (sides[i] == 1) fall(kernel, world, found[i]);
        else if (!kernel.find_process(found[i], ReleaseProcess::kType)) kernel.spawn<ReleaseProcess>(world, found[i]);
    }
}

// @orig 1040:06D2; branche 08D8
void drop_items_on_top(Kernel& kernel, World& world, ObjId item) {
    static constexpr std::uint8_t kAll[] = {'$'};
    for (ObjId id : world.surface_search(item, 1, kAll)) fall(kernel, world, id);
}

// @orig 1040:11FD
void drop_out_of_fast_area(Kernel& kernel, World& world, ObjId item) {
    Process* p = kernel.find_process(item, GravityProcess::kType);
    if (!p) return;
    if (const Item* i = world.get(item)) {
        const SweepResult r = sweep(world, i->shape, item, {i->x, i->y, i->z}, {i->x, i->y, 0}, kSweepNoGround);
        finish_move(world, item, r);
    }
    if (!p->terminated()) p->terminate();
}

// @orig 1040:12A6
Pid hurl(Kernel& kernel, World& world, ObjId item, int vx, int vy, int vz, int gravity) {
    GravityProcess* existing = nullptr;
    kernel.for_each_process([&](const Process& p) {
        auto* g = dynamic_cast<const GravityProcess*>(&p);
        if (g && g->owner() == item && !existing) existing = const_cast<GravityProcess*>(g);
    });
    if (existing) {
        existing->vx += vx;
        existing->vy += vy;
        existing->vz += vz;
        return existing->pid();
    }
    auto& g = kernel.spawn<GravityProcess>(world, item, vx, vy, vz, gravity);
    g.sleep(20);
    return g.pid();
}

// @orig 1040:1334
Pid fall(Kernel& kernel, World& world, ObjId item) {
    const Item* i = world.get(item);
    const ShapeInfo* info = i ? world.shape_info(i->shape) : nullptr;
    if (!info || info->is(ShapeInfo::kFixed) || info->weight == 0) return 0;
    return hurl(kernel, world, item, 0, 0, 0, 2);
}

// @orig 1048:00A6
ThrowPath throw_path(const World::Point& from, const World::Point& to, int speed, int gravity) {
    // 16-bit arithmetic, as in the original.
    const auto s16 = [](int v) { return static_cast<int>(static_cast<std::int16_t>(v)); };
    const auto abs16 = [&](int v) { return s16(v < 0 ? -v : v); };  // |-0x8000| stays -0x8000
    ThrowPath t;
    speed = s16(speed);
    t.gravity = std::max(0, s16(gravity));
    t.max_range = t.gravity == 0 ? 3000 : s16(speed * (s16(speed * 2) / t.gravity));
    const int dx = s16(to.x - from.x), dy = s16(to.y - from.y);
    const int dz = (to.z & 0xFF) - (from.z & 0xFF);
    const int ax = abs16(dx), ay = abs16(dy);
    t.range = s16(std::max(ax, ay) + ax + ay) >> 1;
    if (speed == 0) return t;  // the original divides by zero
    int steps = s16(t.range + (speed >> 1)) / speed;
    const int quarter = speed >> 2;
    // Height to climb so that gravity brings the item down after n steps.
    const auto climb = [&](int n) { return s16(dz + (s16(s16(t.gravity * n) * (n - 1)) >> 1)) / n; };
    if (steps == 0) {
        t.vz = dz < 0 ? -quarter : quarter;
    } else {
        t.vz = climb(steps);
        if (t.vz > quarter) {
            // Too steep: more steps.
            if (t.gravity != 0 && quarter / t.gravity > steps) steps = quarter / t.gravity;
            else if (speed > 3 && dz / quarter > steps) steps = dz / quarter;
            if (steps == 0) return t;  // only after a 16-bit overflow; the original divides by zero
            t.vz = climb(steps);
        }
        t.vx = s16(dx + (steps >> 1)) / steps;
        t.vy = s16(dy + (steps >> 1)) / steps;
    }
    t.in_reach = t.vz <= quarter && t.range <= t.max_range;
    return t;
}

// @orig 10A8:38D1 1048:0290
void shoot(Kernel& kernel, World& world, ObjId item, const World::Point& target, int speed, int gravity) {
    const Item* i = world.get(item);
    if (!i) return;
    const ThrowPath t = throw_path(world.location(*i), target, speed, gravity);
    hurl(kernel, world, item, t.vx, t.vy, t.vz, t.gravity);
}

}  // namespace crusader
