#include "platform/sdl_gamepad.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace crusader {

namespace {

constexpr int kStickOn = 16000;   // left stick: digital threshold, with hysteresis
constexpr int kStickOff = 10000;
constexpr int kTriggerOn = 16000;
constexpr int kTriggerOff = 8000;

// CRUSADER_PAD_DEBUG=1 prints the controller and every raw / mapped input to
// stdout, to diagnose a device whose SDL mapping is off (the PortMaster log).
bool debug_enabled() {
    static const bool on = std::getenv("CRUSADER_PAD_DEBUG") != nullptr;
    return on;
}

constexpr std::uint8_t kArrows[4] = {0x48, 0x50, 0x4B, 0x4D};  // up, down, left, right (E0)

// Keypad keys (not extended), by 45 degree sector counter-clockwise from east:
// right, up-right (PgUp), up, up-left (Home), left, down-left (End), down,
// down-right (PgDn). AvatarMover::handle_key reads them as screen directions.
constexpr std::uint8_t kMoveKeys[8] = {0x4D, 0x49, 0x48, 0x47, 0x4B, 0x4F, 0x50, 0x51};
constexpr double kMoveHold = 30.0;  // degrees from the held sector's axis before the stick changes it

// While aiming, the stick's direction relative to the aim picks one of the
// original's Alt moves, which keep the aim: forward, left, backward, right
// (indices in kArrows), counter-clockwise from the aim.
constexpr int kStrafeArrows[4] = {0, 2, 1, 3};
constexpr double kStrafeHold = 60.0;

// How long a stick's rebound lasts after a release (milliseconds): a push the
// other way within it is not a move.
constexpr std::uint32_t kSnapbackMs = 80;
// The turn (degrees) past which the stick is taken to have crossed the centre.
constexpr double kReversal = 120.0;

double angle_between(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// The sector of `angle` among `count` equal ones (0 centred on angle 0), kept
// on `held` while the angle stays within `hold` degrees of its axis.
int sector_of(double angle, int count, int held, double hold) {
    const double width = 360.0 / count;
    int want = ((static_cast<int>(std::lround(angle / width)) % count) + count) % count;
    if (held >= 0) {
        double off = std::fmod(std::fabs(angle - held * width), 360.0);
        if (off > 180.0) off = 360.0 - off;
        if (off < hold) want = held;
    }
    return want;
}

}  // namespace

Gamepad::~Gamepad() {
    if (controller_) SDL_GameControllerClose(reinterpret_cast<SDL_GameController*>(controller_));
}

void Gamepad::init() {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
    for (int i = 0; i < SDL_NumJoysticks() && !controller_; ++i) open(i);
}

void Gamepad::rumble(std::uint16_t low_frequency, std::uint16_t high_frequency, std::uint32_t duration_ms) {
    if (controller_) {
        SDL_GameControllerRumble(reinterpret_cast<SDL_GameController*>(controller_), low_frequency, high_frequency,
                                 duration_ms);
    }
}

void Gamepad::open(int device_index) {
    if (controller_ || !SDL_IsGameController(device_index)) return;
    SDL_GameController* c = SDL_GameControllerOpen(device_index);
    if (!c) return;
    controller_ = reinterpret_cast<_SDL_GameController*>(c);
    instance_ = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    if (debug_enabled()) {
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device_index), guid, sizeof guid);
        char* mapping = SDL_GameControllerMapping(c);
        std::printf("[pad] opened #%d \"%s\" guid=%s\n[pad] mapping: %s\n", device_index, SDL_GameControllerName(c), guid,
                    mapping ? mapping : "(none)");
        SDL_free(mapping);
        std::fflush(stdout);
    }
}

bool Gamepad::quit_requested() const {
    return held_[SDL_CONTROLLER_BUTTON_BACK] && held_[SDL_CONTROLLER_BUTTON_START];
}

void Gamepad::release_all() {
    for (int s = kPadFirstDirection; s < kPadButtonCount; ++s) {
        if (active_[s]) direction(s, false);
    }
    trigger(0, false);
    trigger(1, false);
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) button(b, false);
    right_x_ = right_y_ = 0;
    left_x_ = left_y_ = 0;
    reversing_ = false;  // let go now, without waiting for the stick
    update_move();
    update_aim();
}

bool Gamepad::feed(const SDL_Event& ev) {
    if (debug_enabled()) {
        switch (ev.type) {
            case SDL_JOYAXISMOTION:
                if (std::abs(ev.jaxis.value) > 4000) std::printf("[pad] raw axis %d = %d\n", ev.jaxis.axis, ev.jaxis.value);
                break;
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                std::printf("[pad] raw button %d %s\n", ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN ? "down" : "up");
                break;
            case SDL_JOYHATMOTION: std::printf("[pad] raw hat %d = %d\n", ev.jhat.hat, ev.jhat.value); break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                std::printf("[pad] mapped %s %s\n", SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(ev.cbutton.button)),
                            ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(ev.caxis.value) > 4000)
                    std::printf("[pad] mapped %s = %d\n", SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(ev.caxis.axis)), ev.caxis.value);
                break;
            default: break;
        }
        std::fflush(stdout);
    }
    switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open(ev.cdevice.which);
            return true;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (controller_ && ev.cdevice.which == instance_) {
                release_all();
                SDL_GameControllerClose(reinterpret_cast<SDL_GameController*>(controller_));
                controller_ = nullptr;
                instance_ = -1;
            }
            return true;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            if (controller_ && ev.cbutton.which != instance_) return true;
            button(ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN);
            return true;
        case SDL_CONTROLLERAXISMOTION:
            if (controller_ && ev.caxis.which != instance_) return true;
            axis(ev.caxis.axis, ev.caxis.value);
            return true;
        default:
            return false;
    }
}

void Gamepad::press_key(std::uint8_t scancode, bool extended, bool pressed, std::uint8_t ascii) {
    // The BIOS shift flags follow the modifier keys, as the keyboard ISR keeps them.
    std::uint16_t mod = 0;
    if (!extended) {
        if (scancode == kScCtrl) mod = kLeftCtrl | kCtrl;
        else if (scancode == kScAlt) mod = kLeftAlt | kAlt;
        else if (scancode == kScShift) mod = kLeftShift;
    }
    if (pressed) flags_ |= mod;
    else flags_ = static_cast<std::uint16_t>(flags_ & ~mod);

    KeyEvent k;
    k.scancode = scancode;
    k.extended = extended;
    k.pressed = pressed;
    k.flags = flags_;
    k.ascii = pressed ? ascii : 0;
    keys_.push_back(k);
}

// The d-pad and left stick directions left on their role are the arrow keys
// (an arrow is down while either is). Held back while an input is captured.
void Gamepad::refresh_arrows() {
    auto role = [&](int slot) { return active_[slot] && has_role(slot); };
    for (int d = 0; d < 4; ++d) {
        const bool down = !capture_ && (role(kPadDpadUp + d) || role(kPadLeftStickUp + d));
        if (down == arrow_down_[d]) continue;
        arrow_down_[d] = down;
        press_key(kArrows[d], true, down);
    }
}

// A direction input crossed its threshold: its action, if it has one, goes
// down or up like a button's.
void Gamepad::direction(int slot, bool on) {
    active_[slot] = on;
    refresh_arrows();
    press_slot(slot, on);
}

// In menus the directions always act as the arrows and the mouse, so that
// no setting makes them unusable.
bool Gamepad::has_role(int slot) const { return menu_ || settings_->buttons[slot] == kPadDefaultRole; }

void Gamepad::set_menu(bool menu) {
    if (menu_ == menu) return;
    menu_ = menu;
    refresh_arrows();
    sync_left_stick();
    sync_right_stick();
}

void Gamepad::set_capture(bool on) {
    if (capture_ == on) return;
    capture_ = on;
    refresh_arrows();
    sync_left_stick();
    sync_right_stick();
}

void Gamepad::set_directional(bool on) {
    if (directional_ == on) return;
    directional_ = on;
    sync_left_stick();
}

void Gamepad::set_weapon_out(bool on) {
    if (weapon_out_ == on) return;
    weapon_out_ = on;
    if (directional_active()) update_move();
}

void Gamepad::set_directional_aim(bool on) {
    if (directional_aim_ == on) return;
    directional_aim_ = on;
    sync_right_stick();
}

bool Gamepad::aim_stick(double* deg) const {
    if (!aim_directional_active() || !aim_on_) return false;
    *deg = aim_deg_;
    return true;
}

// The right stick is either four digital directions (turns, mouse) or one aim
// direction; when the mode changes, the old one lets go.
void Gamepad::sync_right_stick() {
    if (aim_directional_active()) {
        for (int s = kPadRightStickUp; s <= kPadRightStickRight; ++s) {
            if (active_[s]) direction(s, false);
        }
        update_aim();
    } else {
        update_aim();
        stick(kPadRightStickUp, kLeft, kRight, right_x_);
        stick(kPadRightStickUp, kUp, kDown, right_y_);
    }
}

void Gamepad::update_aim() {
    if (!aim_directional_active()) {
        aim_on_ = false;
    } else {
        const double len = std::hypot(static_cast<double>(right_x_), static_cast<double>(right_y_));
        aim_on_ = len > (aim_on_ ? kStickOff : kStickOn);
        if (aim_on_) aim_deg_ = std::atan2(-static_cast<double>(right_y_), static_cast<double>(right_x_)) * 180.0 / 3.14159265358979;
    }
    if (directional_active()) update_move();  // the strafe follows the aim
}

// The left stick is either four arrow-like directions or one keypad move; when
// the mode changes, whatever the old one holds goes up and the new one reads the stick.
void Gamepad::sync_left_stick() {
    if (directional_active()) {
        for (int s = kPadLeftStickUp; s <= kPadLeftStickRight; ++s) {
            if (active_[s]) direction(s, false);
        }
        update_move();
    } else {
        update_move();  // the stick is back to the digital directions: no move key stays down
        stick(kPadLeftStickUp, kLeft, kRight, left_x_);
        stick(kPadLeftStickUp, kUp, kDown, left_y_);
    }
}

// The key of the stick's 8-way sector, down while the stick is past its
// threshold. A new sector goes down before the old one goes up, so the
// avatar turns on the spot instead of stopping in between. While aiming with
// the weapon out, the stick strafes instead: an Alt move relative to the aim.
//
// A released stick springs back past the centre: within a frame it may go
// from one side to the other, and it rings there for a few milliseconds. A
// turn of more than kReversal degrees between two frames is that rebound, or
// a quick turn back: the held key stays down for kSnapbackMs, then the stick
// decides. Still pushed firmly the other way, it is a turn back: the new key
// goes down before the old one goes up, as a player turning on the keypad
// does, and the avatar turns without stopping (a release followed by a press
// would play the run's stop, 0x27, first). Otherwise the key goes up. A push
// that way within kSnapbackMs of a release is ignored, and only a firm push
// (past kStickOn) changes the sector of a held key.
void Gamepad::update_move() {
    const double len = std::hypot(static_cast<double>(left_x_), static_cast<double>(left_y_));
    const double angle = std::atan2(-static_cast<double>(left_y_), static_cast<double>(left_x_)) * 180.0 / 3.14159265358979;
    const bool held = move_scancode_ != 0;
    bool off = !directional_active() || len <= (held ? kStickOff : kStickOn);
    if (released_ && now_ms_ - released_ms_ >= kSnapbackMs) released_ = false;
    const bool reversed = !off && angle_between(angle, stick_deg_) > kReversal;
    if (!directional_active()) {
        reversing_ = false;
    } else if (reversing_) {
        if (now_ms_ - reversal_ms_ < kSnapbackMs) {
            if (off || reversed) return;  // still undecided: the old key stays down
            reversing_ = false;           // back towards the held sector
        } else {
            reversing_ = false;
            if (reversed && len <= kStickOn) {
                off = true;  // a rebound still ringing
            } else if (reversed) {
                move_dir_ = strafe_dir_ = -1;  // the new sector, not the held one
                stick_deg_ = angle;
            }
        }
    } else if (held && reversed) {
        reversing_ = true;
        reversal_ms_ = now_ms_;
        return;
    }
    if (!off && released_ && angle_between(angle, stick_deg_) > kReversal) off = true;
    if (off) {
        if (held) {
            released_ = true;
            released_ms_ = now_ms_;
        }
        move_dir_ = strafe_dir_ = -1;
        set_move_key(0, false, false);
        return;
    }
    stick_deg_ = angle;
    const bool weak = held && len <= kStickOn;  // keeps the held sector
    if (weapon_out_ && aim_on_) {
        move_dir_ = -1;
        if (!weak || strafe_dir_ < 0) strafe_dir_ = sector_of(angle - aim_deg_, 4, strafe_dir_, kStrafeHold);
        set_move_key(kArrows[kStrafeArrows[strafe_dir_]], true, true);
    } else {
        strafe_dir_ = -1;
        if (!weak || move_dir_ < 0) move_dir_ = sector_of(angle, 8, move_dir_, kMoveHold);
        set_move_key(kMoveKeys[move_dir_], false, false);
    }
}

void Gamepad::set_move_key(int scancode, bool extended, bool alt) {
    if (scancode == move_scancode_ && extended == move_extended_ && alt == move_alt_) return;
    if (alt != move_alt_) {
        // The modifier changes: let go of the old key before Alt, then press
        // Alt before the new key, so each key carries the right flags.
        if (move_scancode_ != 0) press_key(static_cast<std::uint8_t>(move_scancode_), move_extended_, false);
        if (move_alt_) press_key(kScAlt, false, false);
        if (alt) press_key(kScAlt, false, true);
        if (scancode != 0) press_key(static_cast<std::uint8_t>(scancode), extended, true);
    } else {
        if (scancode != 0) press_key(static_cast<std::uint8_t>(scancode), extended, true);
        if (move_scancode_ != 0) press_key(static_cast<std::uint8_t>(move_scancode_), move_extended_, false);
    }
    move_scancode_ = scancode;
    move_extended_ = extended;
    move_alt_ = alt;
}

void Gamepad::trigger(int index, bool on) {
    if (triggers_[index] == on) return;
    triggers_[index] = on;
    press_slot(index == 0 ? kPadLeftTrigger : kPadRightTrigger, on);
}

// A configurable button went down or up. The action is the one set for it,
// except in menus where A, B and Start are fixed (Enter, Esc, Esc), so that
// a bad setting never locks the player out.
void Gamepad::press_slot(int slot, bool pressed) {
    if (pressed && capture_) {
        captured_.push_back(slot);
        return;
    }
    if (pressed) {
        if (sent_[slot]) return;
        int action = settings_->buttons[slot];
        if (menu_) {
            if (slot == kPadA) action = kPadConfirm;
            else if (slot == kPadB || slot == kPadStart) action = kPadMenu;
        }
        if (menu_ && slot >= kPadFirstDirection) return;
        if (action < 0 || action >= kPadActionCount) return;  // the default role, or nothing
        // Run is Shift held with the move key (stick or d-pad), never Shift + up: that Up would be
        // released with the stick and stop the run while the button is still held.
        if (action == kPadRun) action = kPadShift;
        const PadAction& a = kPadActions[action];
        if (a.scancode == 0) return;
        sent_[slot] = &a;
        if (a.modifier) press_key(a.modifier, false, true);
        press_key(a.scancode, a.extended, true, a.ascii);
    } else if (sent_[slot]) {
        // The key sent at the press is the one released, even if a menu opened meanwhile.
        const PadAction& a = *sent_[slot];
        press_key(a.scancode, a.extended, false);
        if (a.modifier) press_key(a.modifier, false, false);
        sent_[slot] = nullptr;
    }
}

void Gamepad::button(int b, bool pressed) {
    if (b < 0 || b >= 32 || held_[b] == pressed) return;
    held_[b] = pressed;
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: direction(kPadDpadUp, pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: direction(kPadDpadDown, pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: direction(kPadDpadLeft, pressed); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: direction(kPadDpadRight, pressed); break;
        case SDL_CONTROLLER_BUTTON_A: press_slot(kPadA, pressed); break;
        case SDL_CONTROLLER_BUTTON_B: press_slot(kPadB, pressed); break;
        case SDL_CONTROLLER_BUTTON_X: press_slot(kPadX, pressed); break;
        case SDL_CONTROLLER_BUTTON_Y: press_slot(kPadY, pressed); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: press_slot(kPadLeftBumper, pressed); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: press_slot(kPadRightBumper, pressed); break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: press_slot(kPadLeftStick, pressed); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: press_slot(kPadRightStick, pressed); break;
        case SDL_CONTROLLER_BUTTON_START: press_slot(kPadStart, pressed); break;
        case SDL_CONTROLLER_BUTTON_BACK: press_slot(kPadBack, pressed); break;
        default: break;
    }
}

// The two directions of a stick axis, on past kStickOn and off below kStickOff.
void Gamepad::stick(int first_slot, Dir negative, Dir positive, int value) {
    const bool n = active_[first_slot + negative], p = active_[first_slot + positive];
    const bool now_n = value < 0 && (n ? value < -kStickOff : value < -kStickOn);
    const bool now_p = value > 0 && (p ? value > kStickOff : value > kStickOn);
    if (now_n != n) direction(first_slot + negative, now_n);
    if (now_p != p) direction(first_slot + positive, now_p);
}

void Gamepad::axis(int a, int value) {
    switch (a) {
        // A stick read as one direction waits for update(), when both its
        // axes are in; read as four directions, each axis stands alone.
        case SDL_CONTROLLER_AXIS_LEFTX:
            left_x_ = value;
            if (directional_active()) left_moved_ = true;
            else stick(kPadLeftStickUp, kLeft, kRight, value);
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            left_y_ = value;
            if (directional_active()) left_moved_ = true;
            else stick(kPadLeftStickUp, kUp, kDown, value);
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX:
            right_x_ = value;
            if (aim_directional_active()) right_moved_ = true;
            else stick(kPadRightStickUp, kLeft, kRight, value);
            break;
        case SDL_CONTROLLER_AXIS_RIGHTY:
            right_y_ = value;
            if (aim_directional_active()) right_moved_ = true;
            else stick(kPadRightStickUp, kUp, kDown, value);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            trigger(0, triggers_[0] ? value > kTriggerOff : value > kTriggerOn);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            trigger(1, triggers_[1] ? value > kTriggerOff : value > kTriggerOn);
            break;
        default: break;
    }
}

std::vector<int> Gamepad::take_captured() {
    std::vector<int> out;
    out.swap(captured_);
    return out;
}

void Gamepad::update(std::uint32_t now_ms) {
    now_ms_ = now_ms;
    // update_aim() updates the move too (the strafe follows the aim). After a
    // release, and during a turn back, the move is read every frame: a push held
    // back as a rebound is pressed once the rebound is over, and a turn back
    // is decided once its wait is over.
    if (right_moved_) update_aim();
    else if ((left_moved_ || released_ || reversing_) && directional_active()) update_move();
    left_moved_ = right_moved_ = false;
}

std::vector<KeyEvent> Gamepad::take_keys() {
    update(now_ms_);
    std::vector<KeyEvent> out;
    out.swap(keys_);
    return out;
}

void Gamepad::mouse_motion(double seconds, int* dx, int* dy) {
    if (aim_directional_active()) {
        *dx = *dy = 0;
        rem_x_ = rem_y_ = 0;
        return;
    }
    // Dead zone on the stick's length, then a quadratic response: fine aim
    // near the centre, full speed at the edge.
    const double dead_zone = PadSettings::kDeadZones[std::clamp(settings_->dead_zone, 0, PadSettings::kDeadZoneLevels - 1)];
    const double max_speed = PadSettings::kAimSpeeds[std::clamp(settings_->aim, 1, PadSettings::kAimLevels) - 1];
    auto speed = [&](int v, int other) {
        const double len = std::hypot(static_cast<double>(v), static_cast<double>(other));
        if (len <= dead_zone) return 0.0;
        const double t = std::min(1.0, (len - dead_zone) / (32767.0 - dead_zone));
        return (v / len) * t * t * max_speed;
    };
    // A direction bound to an action no longer moves the mouse.
    auto moves = [&](int slot) { return has_role(slot); };
    double sx = speed(right_x_, right_y_), sy = speed(right_y_, right_x_);
    if ((sx < 0 && !moves(kPadRightStickLeft)) || (sx > 0 && !moves(kPadRightStickRight))) sx = 0;
    if ((sy < 0 && !moves(kPadRightStickUp)) || (sy > 0 && !moves(kPadRightStickDown))) sy = 0;
    rem_x_ += sx * seconds;
    rem_y_ += sy * seconds;
    *dx = static_cast<int>(rem_x_);
    *dy = static_cast<int>(rem_y_);
    rem_x_ -= *dx;
    rem_y_ -= *dy;
    if (right_x_ == 0 && right_y_ == 0) rem_x_ = rem_y_ = 0;
}

}  // namespace crusader
