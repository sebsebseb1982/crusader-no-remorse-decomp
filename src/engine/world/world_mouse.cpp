#include "engine/world/world_mouse.hpp"

#include "engine/world/actor_anim.hpp"
#include "engine/world/attack.hpp"
#include "engine/world/avatar_mover.hpp"
#include "engine/world/collision.hpp"
#include "engine/world/gravity.hpp"
#include "engine/world/item_selection.hpp"

#include <cstdlib>

namespace crusader {

namespace {

// Scancodes (PC set 1).
constexpr std::uint8_t kKeySpace = 0x39;
constexpr std::uint8_t kKeyUp = 0x48;
constexpr std::uint8_t kKeyLeft = 0x4B;
constexpr std::uint8_t kKeyRight = 0x4D;

constexpr std::uint16_t kActionDraw = 7;
constexpr std::uint16_t kActionHolster = 0xB;
constexpr std::uint16_t kActionStandUp = 0x29;

bool kneeling(const World& world, ObjId id) {  // Actor::I_isKneeling
    const auto r = world.npc_record(id);
    return !r.empty() && (r[npc::kFlags13] & 0x04);
}

}  // namespace

// The player in stasis while controlled (10A8:00C1 on DS:7E0E, DS:7E10).
bool WorldMouse::player_held() const {
    return world_.state.controlled_npc == 1 && world_.state.avatar_in_stasis;
}

// @orig 13E8:2A13 13E8:2B74 13E8:2DAD
bool WorldMouse::event(const MouseInput& e) {
    switch (e.kind()) {
    case MouseInput::kMotionKind: return motion(e);
    case MouseInput::kLeft: left(e); return false;
    case MouseInput::kRight: right(e); return false;
    default: return false;  // both buttons: +0x24 does nothing in the view
    }
}

// @orig 13E8:2A13
bool WorldMouse::motion(const MouseInput& e) {
    // The mouse goes back to the centre, out of the item selection and of
    // the hack mover.
    const bool hack_mover = world_.state.cheats.hack_mover;
    const bool warp = !hack_mover && !world_.state.selection.active;
    const int dx = warp ? kCentreX - e.x : 0;
    if (world_.state.avatar_in_stasis) return warp;
    GameState::MouseTurn& mouse = world_.state.mouse_turn;
    const int speed = mouse.speed;
    mouse.rate = stance_not_moving(world_, world_.state.controlled_npc) ? std::abs(dx) >> (3 - speed)
                                                                        : std::abs(dx) >> (7 - 2 * speed);
    if (mouse.rate != 0) {
        mouse.active = true;
        send_arrow(dx > 0 ? kKeyLeft : kKeyRight, true, keyboard_.flags());
        mouse.active = false;
    }
    if (walking_ || firing_) idle();
    else if (dragging_ && hack_mover) drag(e.x, e.y);  // 13E8:338D
    return warp;
}

// @orig 13E8:2B74
void WorldMouse::left(const MouseInput& e) {
    if (world_.state.selection.active) {
        if (e.type == MouseInput::kPress) selection_click(world_);
        return;
    }
    if (player_held()) return;
    if (e.type != MouseInput::kRelease) {
        if (!world_.state.cheats.hack_mover) {
            firing_ = true;
            idle();
        } else if ((e.buttons & 3) != 0 && !walking_ && !firing_ && !dragging_ && item_at(e.x, e.y) != 0) {
            grab(e.x, e.y);
        }
    } else if (dragging_) {
        drop();
    } else if (firing_) {
        send_fire(false);
        firing_ = false;
    }
}

// The hack mover's press: 13E8:3253, DEBUGUMP_13b0_0524, 13E8:32F2.
// @orig 13E8:3253 13E8:32F2 13B0:0524
void WorldMouse::grab(int x, int y) {
    const ObjId id = item_at(x, y);
    const Item* item = world_.get(id);
    if (!item) return;
    // The display list entry's place on screen (DLIST 11A0:095D), less the scroll.
    const int ix = static_cast<std::uint16_t>(item->x) >> 1, iy = static_cast<std::uint16_t>(item->y) >> 1;
    const auto [left, top] = origin();
    mover_ = Mover{};
    mover_.item = id;
    mover_.grab_x = x - (ix - iy - left);
    mover_.grab_y = y - ((ix >> 1) + (iy >> 1) - (item->z & 0xFF) - top);
    leave_surface(kernel_, world_, id);  // ITEM.C 2970
    dragging_ = true;
    drag(x, y);  // the view's motion method (+0x10), the mover on
}

// DEBUGUMP_13b0_01ee, "existing item" mode.
// @orig 13E8:338D 13B0:01EE
void WorldMouse::drag(int x, int y) {
    const Item* item = world_.get(mover_.item);
    if (!item) return;
    const std::uint16_t shape = item->shape;
    world_.push(mover_.item);
    const World::Point p = target(x, y);
    if (position_free(world_, shape, p)) {
        world_.pop_to_coords(p.x, p.y, p.z);
        mover_.last_x = x;
        mover_.last_y = y;
    } else {
        world_.pop();
    }
}

// The release: 13E8:334E then DEBUGUMP_13b0_0587.
// @orig 13E8:334E 13B0:0587
void WorldMouse::drop() {
    dragging_ = false;
    const ObjId id = mover_.item;
    mover_.item = 0;
    const Item* item = world_.get(id);
    if (!item) return;
    const std::uint8_t family = world_.family_of(item->shape);
    if ((family == 2 || family == 9) && mover_.last_x >= 0 && mover_.last_y >= 0) {
        const std::uint16_t shape = item->shape;
        world_.push(id);
        const Item* other = world_.get(item_at(mover_.last_x, mover_.last_y));
        if (other && other->shape == shape) {
            // Drawn, so on the map: the original's contained case (popped
            // into the other's container) does not arise.
            const World::Point at = world_.location(*other);
            world_.pop_to_coords(at.x, at.y, at.z);
            merge_stacks(world_, id, other->id);
        } else {
            world_.pop();
        }
    }
    // Game running (DS:08AF): an item out of any container falls.
    if (const Item* moved = world_.get(id); moved && !(moved->flags & kItemContained)) fall(kernel_, world_, id);
}

// @orig 13B0:0837 1188:0467
World::Point WorldMouse::target(int x, int y) const {
    const auto [left, top] = origin();
    // ANIMCACH 1188:0467: the map point of a place on screen, at z 0.
    const auto at_z0 = [&](int sx, int sy, int& wx, int& wy) {
        sx += left;
        sy += top;
        wx = sx + (sy << 1);
        wy = (sy << 1) - sx;
    };
    int wx = 0, wy = 0, wz = 0;
    ObjId hit = 0;
    bool inside = false;
    under(x, y, wx, wy, wz, &hit, &inside);
    at_z0(x - mover_.grab_x, y - mover_.grab_y, wx, wy);
    wx += wz * 2;
    wy += wz * 2;
    if (hit != 0 && !inside) {
        // Over an item, beyond its box: from the held item's far corner.
        const Item* held = world_.get(mover_.item);
        const ShapeInfo* info = held ? world_.shape_info(held->shape) : nullptr;
        const int a = info ? info->footpad_x * 8 : 0, b = info ? info->footpad_y * 8 : 0;
        under(x - mover_.grab_x - a + b, y - mover_.grab_y - ((a + b) >> 2), wx, wy, wz, nullptr, nullptr);
        wx += a * 2;
        wy += b * 2;
    }
    return World::Point{static_cast<std::uint16_t>(wx), static_cast<std::uint16_t>(wy), wz};
}

// @orig 1188:049E
void WorldMouse::under(int x, int y, int& wx, int& wy, int& wz, ObjId* hit, bool* inside) const {
    const auto [left, top] = origin();
    const int sx = x + left, sy = y + top;
    wx = sx + (sy << 1);
    wy = (sy << 1) - sx;
    wz = 0;
    const Item* item = world_.get(item_at(x, y));
    if (!item) return;
    // On its top: the shape's height (8 per footpad unit; 1 for a flat item
    // that is not fixed), the z kept in a byte, at most 250.
    const ShapeInfo* info = world_.shape_info(item->shape);
    int height = info ? info->footpad_z * 8 : 0;
    if (height == 0 && !(info && info->is(ShapeInfo::kFixed))) height = 1;
    wz = ((item->z & 0xFF) + height) & 0xFF;
    if (wz > 250) wz = 250;
    wx += wz << 1;
    wy += wz << 1;
    const int ix = static_cast<std::uint16_t>(item->x), iy = static_cast<std::uint16_t>(item->y);
    if (hit) *hit = item->id;
    if (inside) *inside = ix >= wx && iy >= wy;
    if (ix > wx && iy > wy) return;
    // Beyond its box: down its front face, as far as its top allows.
    const int d = wx - ix > wy - iy ? wx - ix : wy - iy;
    if (wz < (d >> 1)) return;
    wx -= d;
    wy -= d;
    wz -= d >> 1;
}

// @orig 13E8:2DAD
void WorldMouse::right(const MouseInput& e) {
    if (world_.state.selection.active) return;  // view flags 0x180
    const ObjId npc = world_.state.controlled_npc;
    if (e.type == MouseInput::kRelease) {
        if (!walking_) return;
        walking_ = false;
        send_arrow(kKeyUp, false, keyboard_.flags());
        return;
    }
    if (player_held()) return;
    if (e.type == MouseInput::kPress) {
        walking_ = true;
        if (stance_not_moving(world_, npc) && (e.buttons & 3) != 0) {
            if (kneeling(world_, npc)) do_anim(kernel_, world_, anims_, npc, kActionStandUp, kDirCurrent, 0);
            do_anim(kernel_, world_, anims_, npc, kActionHolster, kDirCurrent, 1);
        }
        // The original then compares the NPC's direction with itself and
        // turns it when they differ: never.
        idle();
        return;
    }
    // A double click.
    if (actor_busy(kernel_, npc)) return;
    if (stance_not_moving(world_, npc)) {
        if (kneeling(world_, npc)) do_anim(kernel_, world_, anims_, npc, kActionStandUp, kDirCurrent, 0);
        do_anim(kernel_, world_, anims_, npc, kActionHolster, kDirCurrent, 1);
    } else {
        do_anim(kernel_, world_, anims_, npc, kActionDraw, kDirCurrent, 0);
    }
}

// @orig 13E8:307F
void WorldMouse::idle() {
    if (player_held()) return;
    // The item selection's part of this method is selection_tick.
    if (world_.state.selection.active) return;
    const ObjId npc = world_.state.controlled_npc;
    if (walking_ && !actor_busy(kernel_, npc) &&
        turn_to(kernel_, world_, anims_, npc, world_.npc_direction(npc), false, false) == 0) {
        send_arrow(kKeyUp, true, stance_not_moving(world_, npc) ? kLeftAlt : keyboard_.flags());
    }
    if (firing_) send_fire(true);
}

// The key events built by the view: E0 arrows, count 1, time 0.
// @orig 13E8:2A13 13E8:2DAD 13E8:307F; the key events they build
void WorldMouse::send_arrow(std::uint8_t scancode, bool pressed, std::uint16_t flags) {
    GameKeyEvent k;
    k.scancode = scancode;
    k.flags = flags;
    k.pressed = pressed;
    k.extended = true;
    k.count = 1;
    send_(k);
}

// @orig 1150:05E8
void WorldMouse::send_fire(bool pressed) {
    if (actor_busy(kernel_, world_.state.controlled_npc)) return;  // DS:285A
    GameKeyEvent k;
    k.ascii = ' ';
    k.scancode = kKeySpace;
    k.pressed = pressed;
    k.count = 1;
    send_(k);
}

}  // namespace crusader
