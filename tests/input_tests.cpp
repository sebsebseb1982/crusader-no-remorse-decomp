// SDL event -> KeyEvent / MouseEvent translation, using synthetic events
// (SDL itself is not initialised).

#define SDL_MAIN_HANDLED
#include "engine/input/mouse_queue.hpp"
#include "platform/sdl_gamepad.hpp"
#include "platform/sdl_input.hpp"

#include <SDL.h>

#include <cstdio>

using namespace crusader;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

SDL_Event key(Uint32 type, SDL_Scancode sc, SDL_Keycode sym, Uint16 mod = KMOD_NONE) {
    SDL_Event ev{};
    ev.type = type;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = mod;
    return ev;
}

void test_scancodes() {
    bool ext = true;
    CHECK(pc_scancode(SDL_SCANCODE_ESCAPE, &ext) == 0x01 && !ext);
    CHECK(pc_scancode(SDL_SCANCODE_A) == 0x1E);
    CHECK(pc_scancode(SDL_SCANCODE_SPACE) == 0x39);
    CHECK(pc_scancode(SDL_SCANCODE_F10) == 0x44);
    CHECK(pc_scancode(SDL_SCANCODE_UP, &ext) == 0x48 && ext);
    CHECK(pc_scancode(SDL_SCANCODE_KP_8, &ext) == 0x48 && !ext);
    CHECK(pc_scancode(SDL_SCANCODE_RCTRL, &ext) == 0x1D && ext);
    CHECK(pc_scancode(SDL_SCANCODE_APPLICATION) == 0);
}

void test_keys() {
    auto k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a));
    CHECK(k && k->pressed && k->scancode == 0x1E && k->ascii == 'a');

    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a, KMOD_LSHIFT));
    CHECK(k && k->ascii == 'A' && k->shift() && (k->flags & kLeftShift));

    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a, KMOD_CAPS | KMOD_RSHIFT));
    CHECK(k && k->ascii == 'a');  // caps + shift cancel out

    k = translate_key(key(SDL_KEYUP, SDL_SCANCODE_A, SDLK_a));
    CHECK(k && !k->pressed && k->ascii == 0);

    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_LEFT, SDLK_LEFT, KMOD_RCTRL));
    CHECK(k && k->extended && k->scancode == 0x4B && k->ascii == 0 && (k->flags & kRightCtrl));

    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN));
    CHECK(k && k->ascii == '\r');

    // Keypad (11D0:0013 tables): digits with Num Lock, Shift or Caps Lock
    // only; * - + always; '.' none.
    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_KP_7, SDLK_KP_7, KMOD_NUM));
    CHECK(k && k->scancode == 0x47 && k->ascii == '7');
    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_KP_0, SDLK_KP_0, KMOD_LSHIFT));
    CHECK(k && k->ascii == '0');
    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_KP_8, SDLK_KP_8));
    CHECK(k && k->ascii == 0);
    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_KP_MINUS, SDLK_KP_MINUS));
    CHECK(k && k->ascii == '-');
    k = translate_key(key(SDL_KEYDOWN, SDL_SCANCODE_KP_PERIOD, SDLK_KP_PERIOD, KMOD_NUM));
    CHECK(k && k->ascii == 0);

    SDL_Event other{};
    other.type = SDL_MOUSEMOTION;
    CHECK(!translate_key(other));
}

void test_mouse() {
    SDL_Event ev{};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = 320;
    ev.motion.y = 240;
    ev.motion.xrel = -3;
    ev.motion.yrel = 4;
    auto m = translate_mouse(ev);
    CHECK(m && m->kind == MouseEvent::Kind::Move && m->x == 320 && m->y == 240 && m->dx == -3 && m->dy == 4);

    ev = SDL_Event{};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_RIGHT;
    ev.button.x = 10;
    ev.button.y = 20;
    m = translate_mouse(ev);
    CHECK(m && m->kind == MouseEvent::Kind::ButtonDown && m->button == 2 && m->x == 10);
}

// MOUSECTL.C: clicks held back 13 ticks, double clicks, both buttons, motion.
void test_mouse_queue() {
    MouseQueue q;
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) q.tick();
    };
    // A click: nothing for 12 ticks, then the press and its release.
    q.press(false, 10, 20, 1);
    q.release(false, 11, 21, 0);
    ticks(12);
    CHECK(!q.next());
    ticks(1);
    auto e = q.next();
    CHECK(e && e->type == MouseInput::kPress && e->code == MouseInput::kLeftDown && e->kind() == MouseInput::kLeft &&
          e->x == 10 && e->buttons == 1);
    e = q.next();
    CHECK(e && e->type == MouseInput::kRelease && e->code == MouseInput::kLeftUp && e->x == 11);
    CHECK(!q.next());

    // A double click: the second press at once, the first click lost.
    q.press(true, 0, 0, 2);
    q.release(true, 0, 0, 0);
    ticks(5);
    q.press(true, 0, 0, 2);
    e = q.next();
    CHECK(e && e->type == MouseInput::kDoubleClick && e->code == MouseInput::kRightDown &&
          e->kind() == MouseInput::kRight);
    CHECK(!q.next());
    ticks(20);
    CHECK(!q.next());
    q.release(true, 0, 0, 0);  // nothing held back: at once
    e = q.next();
    CHECK(e && e->type == MouseInput::kRelease && e->code == MouseInput::kRightUp);

    // Both buttons: one press, both dropped.
    q.press(false, 0, 0, 1);
    q.press(true, 0, 0, 3);
    e = q.next();
    CHECK(e && e->type == MouseInput::kPress && e->code == MouseInput::kBothDown && e->kind() == MouseInput::kBoth);
    ticks(13);
    CHECK(!q.next());

    // Motion: only the latest, after the queued events.
    q.move(1, 1, 0);
    q.move(5, 6, 0);
    q.release(false, 7, 7, 0);
    e = q.next();
    CHECK(e && e->type == MouseInput::kRelease);
    e = q.next();
    CHECK(e && e->type == MouseInput::kMotion && e->kind() == MouseInput::kMotionKind && e->x == 5 && e->y == 6);
    CHECK(!q.next());

    // Ten queued events at most.
    for (int i = 0; i < 12; ++i) q.release(false, i, 0, 0);
    int n = 0;
    while (q.next()) ++n;
    CHECK(n == 10);
}

SDL_Event button(Uint32 type, SDL_GameControllerButton b) {
    SDL_Event ev{};
    ev.type = type;
    ev.cbutton.button = static_cast<Uint8>(b);
    return ev;
}

SDL_Event axis(SDL_GameControllerAxis a, int v) {
    SDL_Event ev{};
    ev.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.axis = static_cast<Uint8>(a);
    ev.caxis.value = static_cast<Sint16>(v);
    return ev;
}

// The settings the controller had before the author's own became the defaults.
PadSettings legacy_settings() {
    PadSettings p;
    p.buttons = PadSettings::default_buttons();
    const int old[] = {kPadFire, kPadCrouch, kPadRollLeft, kPadRollRight, kPadPrevWeapon, kPadNextWeapon,
                       kPadPrevItem, kPadNextItem, kPadRun, kPadUseItem, kPadMenu, kPadConfirm};
    for (int i = 0; i < kPadFirstDirection; ++i) p.buttons[i] = old[i];
    for (int i = kPadFirstDirection; i < kPadButtonCount; ++i) p.buttons[i] = kPadDefaultRole;
    return p;
}

void test_gamepad() {
    Gamepad pad;
    const PadSettings legacy = legacy_settings();
    pad.set_settings(&legacy);
    CHECK(!pad.feed(key(SDL_KEYDOWN, SDL_SCANCODE_A, SDLK_a)));

    // A fires (Space), or confirms (Enter) in a menu; the release matches the press.
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    pad.set_menu(true);
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    auto keys = pad.take_keys();
    CHECK(keys.size() == 4);
    CHECK(keys[0].scancode == 0x39 && keys[0].pressed && keys[0].ascii == ' ');
    CHECK(keys[1].scancode == 0x39 && !keys[1].pressed);
    CHECK(keys[2].scancode == 0x1C && keys[2].pressed && keys[2].ascii == '\r');
    CHECK(keys[3].scancode == 0x1C && !keys[3].pressed);
    CHECK(pad.take_keys().empty());
    pad.set_menu(false);

    // D-pad and left stick share the arrow; the stick has hysteresis.
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_UP));
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, -30000));  // already up: no second press
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_UP));
    keys = pad.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x48 && keys[0].extended && keys[0].pressed);
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, -12000));  // between the thresholds: still held
    CHECK(pad.take_keys().empty());
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, -5000));
    keys = pad.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x48 && !keys[0].pressed);
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 12000));  // below the on threshold
    CHECK(pad.take_keys().empty());

    // Combinations: B kneels (Ctrl + down): the modifier goes first, comes off last,
    // and the BIOS flags follow it for the keys pressed meanwhile.
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B));
    keys = pad.take_keys();
    CHECK(keys.size() == 5);
    CHECK(keys[0].scancode == 0x1D && keys[0].pressed && keys[0].ctrl());
    CHECK(keys[1].scancode == 0x50 && keys[1].extended && keys[1].pressed && keys[1].ctrl());
    CHECK(keys[2].scancode == 0x4B && keys[2].ctrl());
    CHECK(keys[3].scancode == 0x50 && !keys[3].pressed);
    CHECK(keys[4].scancode == 0x1D && !keys[4].pressed && !keys[4].ctrl());
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    pad.take_keys();

    // The other moves: X rolls left (Ctrl + left), L3 runs (Shift + up), step aside is Alt.
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_X));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_X));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_LEFTSTICK));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_LEFTSTICK));
    keys = pad.take_keys();
    CHECK(keys.size() == 8);
    CHECK(keys[0].scancode == 0x1D && keys[1].scancode == 0x4B && keys[1].extended && keys[1].ctrl());
    CHECK(keys[4].scancode == 0x2A && keys[5].scancode == 0x48 && keys[5].shift() && !keys[5].ctrl());
    PadSettings aside = legacy;
    aside.buttons[kPadY] = kPadStepRight;
    pad.set_settings(&aside);
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_Y));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_Y));
    keys = pad.take_keys();
    CHECK(keys.size() == 4 && keys[0].scancode == 0x38 && keys[1].scancode == 0x4D && keys[1].extended && keys[1].alt());
    pad.set_settings(&legacy);

    int dx = 0, dy = 0;

    // Triggers act as I / O with hysteresis.
    pad.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 20000));
    pad.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 12000));
    keys = pad.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x18 && keys[0].pressed);
    pad.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0));
    keys = pad.take_keys();
    CHECK(keys.size() == 1 && !keys[0].pressed);

    // Settings: A is remapped (to the medkit), except in a menu where it stays Enter.
    PadSettings custom = legacy;
    custom.buttons[kPadA] = kPadMedkit;
    custom.buttons[kPadRightBumper] = kPadNone;
    custom.aim = 5;
    custom.dead_zone = 0;
    pad.set_settings(&custom);
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));  // none: nothing sent
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x32 && keys[0].ascii == 'm' && !keys[1].pressed);
    pad.set_menu(true);
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x1C);
    pad.set_menu(false);
    // The release matches the press even when the setting changes while held.
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    custom.buttons[kPadA] = kPadFire;
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[1].scancode == 0x32 && !keys[1].pressed);
    // A faster stick with no dead zone: 4000 already moves; full tilt 1250 px/s.
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 4000));
    pad.mouse_motion(1.0, &dx, &dy);
    CHECK(dx > 0);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    pad.mouse_motion(0.1, &dx, &dy);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767));
    pad.mouse_motion(0.1, &dx, &dy);
    CHECK(dx >= 124 && dx <= 125);
    pad.set_settings(&legacy);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    pad.mouse_motion(0.1, &dx, &dy);

    // Directions: with the defaults, left stick right is the right arrow and the d-pad too.
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 30000));
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 0));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x4D && keys[0].extended && keys[0].pressed && !keys[1].pressed);
    // Left stick right bound to "step right" (Alt + right): no arrow, the combination, with hysteresis.
    PadSettings dirs = legacy;
    dirs.assign(kPadLeftStickRight, kPadStepRight);
    dirs.assign(kPadRightStickUp, kPadNextItem);
    pad.set_settings(&dirs);
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 30000));
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 12000));
    CHECK(pad.take_keys().size() == 2);  // Alt, right: nothing more while it stays past the lower threshold
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 0));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x4D && !keys[0].pressed && keys[1].scancode == 0x38 && !keys[1].pressed);
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));  // d-pad keeps the arrow
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    keys = pad.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x4D && !keys[0].alt());
    // Right stick up bound to "next item": it fires O and no longer moves the mouse upward.
    int mx = 0, my = 0;
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, -32767));
    keys = pad.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x18 && keys[0].pressed);
    pad.mouse_motion(0.1, &mx, &my);
    CHECK(my == 0);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, 32767));  // down is still free for the mouse
    pad.take_keys();
    pad.mouse_motion(0.1, &mx, &my);
    CHECK(my > 60);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, 0));
    pad.take_keys();
    // Unbinding a direction gives its role back; a button's binding goes to "none".
    dirs.unbind(kPadStepRight);
    CHECK(dirs.buttons[kPadLeftStickRight] == kPadDefaultRole);
    dirs.unbind(kPadFire);
    CHECK(dirs.buttons[kPadA] == kPadNone);
    // Capture of a direction: no key, no arrow.
    pad.set_settings(&legacy);
    pad.set_capture(true);
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, -30000));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 30000));
    CHECK(pad.take_keys().empty());
    auto got = pad.take_captured();
    CHECK(got.size() == 3 && got[0] == kPadLeftStickUp && got[1] == kPadDpadLeft && got[2] == kPadRightStickRight);
    pad.set_capture(false);
    pad.take_keys();
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, 0));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    pad.take_keys();
    pad.mouse_motion(0.1, &mx, &my);

    // Capture: a press is kept for the controller screen, no key is sent, the release neither.
    pad.set_capture(true);
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    pad.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 30000));
    pad.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0));
    pad.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_START));
    pad.set_capture(false);
    pad.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_START));
    CHECK(pad.take_keys().empty());
    const auto captured = pad.take_captured();
    CHECK(captured.size() == 3 && captured[0] == kPadLeftBumper && captured[1] == kPadLeftTrigger && captured[2] == kPadStart);
    CHECK(pad.take_captured().empty());

    // Right stick: dead zone, then motion proportional to time.
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 3000));
    pad.mouse_motion(1.0, &dx, &dy);
    CHECK(dx == 0 && dy == 0);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767));
    pad.mouse_motion(1.0 / 60, &dx, &dy);
    CHECK(dx >= 11 && dx <= 12 && dy == 0);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, -32767));
    pad.mouse_motion(0.1, &dx, &dy);
    CHECK(dx < -60 && dy == 0);

    // The defaults are the author's settings.
    Gamepad fresh;
    fresh.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A));
    fresh.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A));
    keys = fresh.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x1F && keys[0].ascii == 's');
    fresh.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 30000));  // RT fires
    fresh.feed(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0));
    keys = fresh.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x39);
    fresh.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_UP));  // use item, no arrow
    fresh.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_UP));
    keys = fresh.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x16 && !keys[0].extended);
    fresh.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767));  // turn right: the right arrow, no mouse
    keys = fresh.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x4D && keys[0].extended && keys[0].pressed && !keys[0].ctrl());
    fresh.mouse_motion(0.1, &dx, &dy);
    CHECK(dx == 0);
    fresh.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    fresh.take_keys();
    // In menus the d-pad is the arrows and the stick moves the mouse, whatever they are bound to.
    fresh.set_menu(true);
    fresh.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    fresh.feed(button(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    keys = fresh.take_keys();
    CHECK(keys.size() == 2 && keys[0].scancode == 0x4D && keys[0].extended);
    fresh.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 32767));
    CHECK(fresh.take_keys().empty());
    fresh.mouse_motion(0.1, &dx, &dy);
    CHECK(dx > 60);
    fresh.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    // A direction held while a menu opens turns into an arrow, and back.
    fresh.set_menu(false);
    fresh.feed(button(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    fresh.take_keys();
    fresh.set_menu(true);
    keys = fresh.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x4B && keys[0].pressed);
    fresh.set_menu(false);
    keys = fresh.take_keys();
    CHECK(keys.size() == 1 && keys[0].scancode == 0x4B && !keys[0].pressed);
}

// Directional movement: the left stick keeps the keypad's absolute screen
// directions with the weapon out, and strafes (Alt + arrow, relative to the
// aim) only while the right stick aims.
void test_gamepad_directional_weapon_out() {
    Gamepad pad;
    pad.set_directional(true);
    pad.set_directional_aim(true);
    pad.set_weapon_out(true);

    // Not aiming: up is the keypad's 8, as without the weapon.
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, -30000));
    auto keys = pad.take_keys();
    CHECK(keys.size() == 1);
    CHECK(keys[0].scancode == 0x48 && !keys[0].extended && keys[0].pressed && !keys[0].alt());

    // Aiming east: up is to the left of the aim, a side step (Alt + left).
    // The modifier changes, so the keypad key goes up before Alt goes down.
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 30000));
    keys = pad.take_keys();
    CHECK(keys.size() == 3);
    CHECK(keys[0].scancode == 0x48 && !keys[0].extended && !keys[0].pressed);
    CHECK(keys[1].scancode == 0x38 && keys[1].pressed && keys[1].alt());
    CHECK(keys[2].scancode == 0x4B && keys[2].extended && keys[2].pressed && keys[2].alt());

    // Aiming north (via north-east, still a left step): up is forward (Alt +
    // up); the new key goes down first.
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, -30000));
    CHECK(pad.take_keys().empty());
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTX, 0));
    keys = pad.take_keys();
    CHECK(keys.size() == 2);
    CHECK(keys[0].scancode == 0x48 && keys[0].extended && keys[0].pressed && keys[0].alt());
    CHECK(keys[1].scancode == 0x4B && keys[1].extended && !keys[1].pressed && keys[1].alt());

    // Down while aiming north: backward (Alt + down).
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, 30000));
    keys = pad.take_keys();
    CHECK(keys.size() == 2);
    CHECK(keys[0].scancode == 0x50 && keys[0].extended && keys[0].pressed && keys[0].alt());
    CHECK(keys[1].scancode == 0x48 && !keys[1].pressed);

    // Right while aiming north: a side step to the right.
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTY, 0));
    pad.take_keys();
    pad.feed(axis(SDL_CONTROLLER_AXIS_LEFTX, 30000));
    keys = pad.take_keys();
    CHECK(keys.size() == 2);
    CHECK(keys[0].scancode == 0x38 && keys[0].pressed && keys[0].alt());
    CHECK(keys[1].scancode == 0x4D && keys[1].extended && keys[1].pressed && keys[1].alt());

    // Letting go of the aim: back to the keypad's 6, Alt released first.
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, 0));
    keys = pad.take_keys();
    CHECK(keys.size() == 3);
    CHECK(keys[0].scancode == 0x4D && keys[0].extended && !keys[0].pressed && keys[0].alt());
    CHECK(keys[1].scancode == 0x38 && !keys[1].pressed && !keys[1].alt());
    CHECK(keys[2].scancode == 0x4D && !keys[2].extended && keys[2].pressed && !keys[2].alt());

    // Aiming with the weapon holstered keeps the keypad moves.
    pad.set_weapon_out(false);
    pad.feed(axis(SDL_CONTROLLER_AXIS_RIGHTY, -30000));
    CHECK(pad.take_keys().empty());

    // The weapon comes out while aiming: the strafe starts.
    pad.set_weapon_out(true);
    keys = pad.take_keys();
    CHECK(keys.size() == 3);
    CHECK(keys[0].scancode == 0x4D && !keys[0].extended && !keys[0].pressed);
    CHECK(keys[1].scancode == 0x38 && keys[1].pressed && keys[1].alt());
    CHECK(keys[2].scancode == 0x4D && keys[2].extended && keys[2].pressed && keys[2].alt());

    // Capturing an input (the controller screen) lets go of the move key too.
    pad.set_capture(true);
    keys = pad.take_keys();
    CHECK(keys.size() == 2);
    CHECK(keys[0].scancode == 0x4D && !keys[0].pressed && keys[0].alt());
    CHECK(keys[1].scancode == 0x38 && !keys[1].pressed && !keys[1].alt());
}

}  // namespace

int main() {
    SDL_SetMainReady();
    test_scancodes();
    test_keys();
    test_mouse();
    test_mouse_queue();
    test_gamepad();
    test_gamepad_directional_weapon_out();
    std::printf(failures ? "%d failure(s)\n" : "all input checks passed\n", failures);
    return failures ? 1 : 0;
}
