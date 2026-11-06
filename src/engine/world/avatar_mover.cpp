#include "engine/world/avatar_mover.hpp"

#include "engine/world/actor_anim.hpp"
#include "engine/world/aim_mode.hpp"

#include <algorithm>
#include <initializer_list>

namespace crusader {

namespace {

constexpr int kNone = 0x40;  // "no action" in the original

// Scancodes (PC set 1).
enum : std::uint8_t {
    kKeySlash = 0x35,  // keypad / with E0
    kKeyHome = 0x47,
    kKeyUp = 0x48,
    kKeyPgUp = 0x49,
    kKeyLeft = 0x4B,
    kKeyCenter = 0x4C,  // keypad 5
    kKeyRight = 0x4D,
    kKeyEnd = 0x4F,
    kKeyDown = 0x50,
    kKeyPgDn = 0x51,
    kKeyIns = 0x52,
    kKeyDel = 0x53,
};

bool in(std::uint16_t v, std::initializer_list<std::uint16_t> set) {
    return std::find(set.begin(), set.end(), v) != set.end();
}

}  // namespace

// @orig 1138:0070
bool stance_not_moving(const World& world, ObjId id) {
    const auto r = world.npc_record(id);
    const std::uint16_t last =
        r.empty() ? 0 : static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
    return !in(last, {1, 0, 0x3A, 3, 0x30, 0x31, 0xB, 0x10, 0x37}) && !in(last, {0x26, 0x22, 0x23});
}

// @orig 1138:0F8D
bool action_redirectable(GameState::MouseTurn& mouse, std::uint32_t now, std::uint16_t a) {
    // A walk or a run turned by the mouse: at most once every
    // 60 / (speed * 3 + 3) ticks.
    if (mouse.active && in(a, {1, 3, 0x30, 0x31})) {
        if (mouse.redirected_at != 0 && now < mouse.redirected_at + 60u / (mouse.speed * 3 + 3)) return false;
        mouse.redirected_at = now;
        return true;
    }
    return in(a, {1, 3, 0x26, 4, 0x25, 0x36, 0x32, 0x2E, 0x2F, 8, 0x2A, 0xD, 0x2B, 10, 0xF, 0x30, 0x31, 0x22,
                  0x23, 0x24, 0x2C, 2, 0x2D, 7, 0xC, 0x27, 0x19});
}

std::uint16_t AvatarMover::last_anim() const {
    auto r = world_.npc_record(actor());
    return r.empty() ? 0 : static_cast<std::uint16_t>(r[npc::kLastAnim] | r[npc::kLastAnim + 1] << 8);
}

bool AvatarMover::kneeling() const {
    auto r = world_.npc_record(actor());
    return !r.empty() && (r[npc::kFlags13] & 0x04);
}

// @orig 1138:12CF
bool AvatarMover::handle_key(const GameKeyEvent& e) {
    const ObjId a_id = actor();
    if (a_id == 0 || world_.is_dead(a_id) || !world_.get(a_id)) return false;

    const std::uint16_t last = last_anim();
    const int dir = world_.npc_direction(a_id) & 0xF;
    // Classes of the last animation (NPC.C 21A0, 21D0, 1F74; CRUSADER.C 0014, 0070).
    const bool running = in(last, {3, 0x30, 0x31, 0x38});
    const bool run_start = in(last, {0x26, 0x22, 0x23});
    const bool side_step = in(last, {0x1C, 0x1D});
    const bool upright = in(last, {0, 1, 0x3A, 0x37, 0xB, 0x10, 0x20, 0x3B});
    const bool not_moving = stance_not_moving(world_, a_id);
    // The weapon itself (Actor::getActiveWeapon): its slot may be left empty.
    const bool has_weapon = world_.active_weapon(a_id) != 0;
    const int run = has_weapon ? 0x30 : 3;
    const bool shift = e.flags & (kLeftShift | kRightShift);
    const bool walk_only = (e.flags & kCapsLock) ? shift : !shift;  // local_1f
    const bool num_lock = e.flags & kNumLock;
    const bool alt = e.flags & (kLeftAlt | kRightAlt);
    const bool ctrl = e.flags & (kLeftCtrl | kRightCtrl);
    const bool map_40 = world_.get(a_id)->map_number == 40;  // NPC.C 0122
    const std::uint8_t sc = e.scancode;
    const bool kneel = kneeling();

    int action = kNone;
    if (!e.pressed) {
        // Release: when no movement key is left down, stand.
        const bool any_down = keyboard_.down(kKeyUp, true) || keyboard_.down(kKeyDown, true) ||
                              keyboard_.down(kKeyLeft, true) || keyboard_.down(kKeyRight, true) ||
                              keyboard_.down(kKeyUp, false) || keyboard_.down(kKeyDown, false) ||
                              keyboard_.down(kKeyLeft, false) || keyboard_.down(kKeyRight, false) ||
                              keyboard_.down(kKeyHome, true) || keyboard_.down(kKeyPgUp, true) ||
                              keyboard_.down(kKeyEnd, true) || keyboard_.down(kKeyPgDn, true) ||
                              keyboard_.down(kKeySlash, true);
        if (any_down || !in(sc, {kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyHome, kKeyPgUp, kKeyEnd, kKeyPgDn,
                                 kKeySlash})) {
            return false;
        }
        if (map_40) action = 0;
        else if (kneel) action = 0x2E;
        else if (running || run_start) action = 0x27;
        else if (!side_step) action = !upright && (a_id == 1 || (last != 2 && last != 0x2D)) ? 4 : 0;
        if (action == kNone) return false;
        do_anim(kernel_, world_, anims_, a_id, static_cast<std::uint16_t>(action), kDirCurrent);
        return true;
    }

    // Pressed or repeated: target direction.
    int d = dir;
    bool absolute = false;  // local_6: the key gives a screen direction
    bool special = false;   // local_5
    auto keypad = [&](int screen_dir) {
        absolute = true;
        d = num_lock ? dir : screen_dir;
    };
    switch (sc) {
        case kKeySlash:
            if (!e.extended) return false;
            absolute = true;
            break;
        case kKeyHome:
            if (!e.extended) keypad(0xC);
            else special = true;
            break;
        case kKeyUp:
            if (!e.extended) keypad(0xE);
            break;
        case kKeyPgUp:
            if (!e.extended) keypad(0);
            else special = true;
            break;
        case kKeyLeft:
            if (!e.extended) {
                absolute = true;
                d = num_lock ? kDirTurnLeft : 10;
            } else {
                d = kDirTurnLeft;
            }
            break;
        case kKeyCenter:
            break;
        case kKeyRight:
            if (!e.extended) {
                absolute = true;
                d = num_lock ? kDirTurnRight : 2;
            } else {
                d = kDirTurnRight;
            }
            break;
        case kKeyEnd:
            if (!e.extended) keypad(8);
            else special = true;
            break;
        case kKeyDown:
            if (!e.extended) keypad(6);
            break;
        case kKeyPgDn:
            if (!e.extended) keypad(4);
            else special = true;
            break;
        case kKeyIns:
        case kKeyDel:
            if (!e.extended) return false;
            special = true;
            break;
        default:
            return false;
    }

    const bool busy = actor_busy(kernel_, a_id);
    int second = kNone;
    bool stance_turn = false;  // local_7: the turn ends the aiming mode until it is over
    auto turn_crosshair_here = [&] { turn_crosshair(kernel_, world_, anims_, sc, walk_only, d, action); };
    // Quarter turn for absolute keypad moves with Ctrl / Alt.
    auto quarter = [&](int towards_left, int towards_right) {
        const int diff = d < dir ? dir - d : dir + 16 - d;
        if (16 - diff < diff) {
            action = towards_left;
            d = (d - 4) & 0xF;
        } else {
            action = towards_right;
            d = (d + 4) & 0xF;
        }
    };
    if (!map_40) {
        if (!alt && !ctrl) {
            if (sc == kKeyCenter) {
                if (e.count == 1) action = kneel ? 0x29 : 0x28;
            } else if (last == 1 || run_start || running) {
                action = !walk_only ? run : (sc == kKeySlash ? run : 1);
            } else if (!special) {
                if (e.extended && sc == kKeyDown) {
                    action = 2;
                } else if (!num_lock || !absolute) {
                    if (dir == d) {
                        action = sc == kKeySlash ? 0x24 : (!walk_only ? 0x22 : 1);
                    } else if (!e.extended && !world_.state.aim.no_stand && (sc == kKeyLeft || sc == kKeyRight)) {
                        if (!upright) {
                            if (!not_moving) {
                                action = !walk_only ? 0x22 : 1;
                            } else if (last != 7 && last != 0xC) {
                                action = 4;
                                stance_turn = true;
                            }
                        } else {
                            action = 0;
                        }
                    } else if (!upright) {
                        if (!absolute || num_lock) {
                            turn_crosshair_here();
                        } else if (last != 7 && last != 0xC) {
                            action = 4;
                            stance_turn = true;
                        }
                    } else if (!busy && (absolute || ((sc == kKeyLeft || sc == kKeyRight) && e.extended))) {
                        action = 0;
                    }
                } else {
                    switch (sc) {
                        case kKeyLeft:
                        case kKeyRight:
                            if (upright) action = 0;
                            else turn_crosshair_here();
                            break;
                        case kKeyUp: action = !walk_only ? 0x22 : 1; break;
                        case kKeySlash: action = 0x24; break;
                        case kKeyHome: action = 0x1C; break;
                        case kKeyPgUp: action = 0x1D; break;
                        case kKeyEnd: action = kneel ? 0x17 : 0x3D; break;
                        case kKeyDown: action = 2; break;
                        case kKeyPgDn: action = kneel ? 0x18 : 0x3E; break;
                        default: break;
                    }
                }
            } else {
                switch (sc) {
                    case kKeyHome: action = kneel ? 0x29 : 0x24; break;
                    case kKeyPgUp: action = kneel ? 0x3E : 0x18; break;
                    case kKeyEnd: action = kneel ? 0x29 : 2; break;
                    case kKeyPgDn: action = kneel ? 0x29 : 0x1D; break;
                    case kKeyIns: action = kneel ? 0x3D : 0x17; break;
                    case kKeyDel: action = kneel ? 0x29 : 0x1C; break;
                    default: break;
                }
            }
        } else if (!alt) {  // Ctrl
            if (!absolute || num_lock) {
                d = dir;
                switch (sc) {
                    case kKeyUp:
                        if (kneel) action = 0x29;
                        else if (!upright) {
                            action = 0xB;
                            if (a_id == 1) second = 0x3A;
                        } else if (a_id == 1) {
                            action = 0x3A;
                        }
                        break;
                    case kKeyLeft: action = kneel ? 0x17 : 0x3D; break;
                    case kKeyCenter:
                    case kKeyDown:
                        if (e.count == 1) action = kneel ? 0x29 : 0x28;
                        break;
                    case kKeyRight: action = kneel ? 0x18 : 0x3E; break;
                    default: break;
                }
            } else if (in(sc, {kKeyHome, kKeyUp, kKeyPgUp, kKeyLeft, kKeyRight, kKeyEnd, kKeyDown, kKeyPgDn})) {
                quarter(0x18, 0x17);
            }
        } else {  // Alt
            if (!absolute || num_lock) {
                d = dir;
                switch (sc) {
                    case kKeyUp: action = 0x24; break;
                    case kKeyLeft: action = 0x1C; break;
                    case kKeyCenter:
                        if (e.count == 1) action = kneel ? 0x29 : 0x28;
                        break;
                    case kKeyRight: action = 0x1D; break;
                    case kKeyDown: action = 2; break;
                    default: break;
                }
            } else if (in(sc, {kKeyHome, kKeyUp, kKeyPgUp, kKeyLeft, kKeyRight, kKeyEnd, kKeyDown, kKeyPgDn})) {
                quarter(0x1D, 0x1C);
            }
        }
    } else if (last == 1 || run_start || running) {
        action = !walk_only ? run : 1;
    } else {
        action = dir == d ? (!walk_only ? 0x26 : 1) : 0;
    }
    if (action == kNone) return false;

    // Turn codes become directions for the chosen action (16 or 8 directions).
    if (d == kDirTurnRight || d == kDirTurnLeft) {
        const AnimAction* act = anims_.action(world_.get(a_id)->shape, static_cast<std::uint16_t>(action));
        const int step = act && act->sixteen_directions() ? 1 : 2;
        d = (dir + (d == kDirTurnRight ? step : -step)) & 0xF;
    }

    if (!busy) {
        const Pid pid = do_anim(kernel_, world_, anims_, a_id, static_cast<std::uint16_t>(action), d);
        if (stance_turn && pid != 0) {
            leave_aim_mode(kernel_, world_);
            if (auto* anim = dynamic_cast<ActorAnimProcess*>(kernel_.get(pid))) anim->mark_turning();
        }
        if (second != kNone) do_anim(kernel_, world_, anims_, a_id, static_cast<std::uint16_t>(second), d);
        return true;
    }
    // Busy: turn the running animation when it allows it.
    if (dir == d) return false;
    if (e.count == 1 || (last != 1 && !running && !run_start)) {
        ActorAnimProcess* current = current_anim(kernel_, a_id);
        if (current && action_redirectable(world_.state.mouse_turn, kernel_.ticks(), current->action())) {
            current->set_direction(d);
            return true;
        }
    }
    return false;
}

}  // namespace crusader
