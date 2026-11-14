#include "platform/real_shift.hpp"

#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace crusader {

namespace {

constexpr std::uint16_t kVkShift = 0x10, kVkLShift = 0xA0, kVkRShift = 0xA1;
constexpr std::uint16_t kRiKeyBreak = 1, kRiKeyE0 = 2;
constexpr std::uint16_t kScLeftShift = 0x2A, kScRightShift = 0x36;

}  // namespace

std::optional<RealShift::Change> RealShift::raw_key(std::uint16_t vkey, std::uint16_t make_code, std::uint16_t flags) {
    if (vkey != kVkShift && vkey != kVkLShift && vkey != kVkRShift) return std::nullopt;
    if (flags & kRiKeyE0) return std::nullopt;  // the keyboard's own fake Shift (grey keys)
    bool right;
    if (make_code == kScLeftShift) right = false;
    else if (make_code == kScRightShift) right = true;
    else if (make_code == 0) right = vkey == kVkRShift;  // sent by virtual key only (no scancode)
    else return std::nullopt;  // Windows' fake Shift around a keypad key: its scancode
    Change c;
    c.right = right;
    c.pressed = !(flags & kRiKeyBreak);
    c.repeat = c.pressed && held_[right];
    if (!c.pressed && !held_[right]) return std::nullopt;  // nothing to let go of
    held_[right] = c.pressed;
    return c;
}

std::uint16_t RealShift::apply(std::uint16_t sdl_mod) const {
    std::uint16_t mod = sdl_mod & ~KMOD_SHIFT;
    if (held_[0]) mod |= KMOD_LSHIFT;
    if (held_[1]) mod |= KMOD_RSHIFT;
    return mod;
}

std::vector<RealShift::Change> RealShift::release_all() {
    std::vector<Change> out;
    for (int side = 0; side < 2; ++side) {
        if (!held_[side]) continue;
        held_[side] = false;
        Change c;
        c.right = side == 1;
        out.push_back(c);
    }
    return out;
}

#ifdef _WIN32

namespace {

RealShift g_shift;
bool g_pushing = false;  // our own events go through the filter

void push(const RealShift::Change& c) {
    SDL_Event e{};
    e.type = c.pressed ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = c.pressed ? SDL_PRESSED : SDL_RELEASED;
    e.key.repeat = c.repeat ? 1 : 0;
    if (SDL_Window* focus = SDL_GetKeyboardFocus()) e.key.windowID = SDL_GetWindowID(focus);
    e.key.keysym.scancode = c.right ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
    e.key.keysym.sym = c.right ? SDLK_RSHIFT : SDLK_LSHIFT;
    e.key.keysym.mod = g_shift.apply(static_cast<std::uint16_t>(SDL_GetModState()));
    g_pushing = true;
    SDL_PushEvent(&e);
    g_pushing = false;
}

// Runs for each message SDL takes from the queue, before it is dispatched:
// the Raw Input report of a key comes before the key's own message.
void SDLCALL on_message(void*, void*, unsigned int message, Uint64, Sint64 lparam) {
    if (message != WM_INPUT) return;
    RAWINPUT raw;
    UINT size = sizeof raw;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        raw.header.dwType != RIM_TYPEKEYBOARD) {
        return;
    }
    const RAWKEYBOARD& k = raw.data.keyboard;
    if (auto c = g_shift.raw_key(k.VKey, k.MakeCode, k.Flags)) push(*c);
}

int SDLCALL filter(void*, SDL_Event* e) {
    if (g_pushing) return 1;
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
        const SDL_Scancode sc = e->key.keysym.scancode;
        if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT) return 0;  // ours instead
        e->key.keysym.mod = g_shift.apply(e->key.keysym.mod);
    } else if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        // No more reports until the window has the keyboard again.
        for (const auto& c : g_shift.release_all()) push(c);
    }
    return 1;
}

}  // namespace

void install_real_shift() {
    if (SDL_GetEventFilter(nullptr, nullptr)) return;  // someone else's: left alone
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;  // generic desktop
    device.usUsage = 0x06;      // keyboard
    device.dwFlags = 0;         // the legacy key messages stay
    device.hwndTarget = nullptr;  // the window with the keyboard focus
    if (!RegisterRawInputDevices(&device, 1, sizeof device)) return;
    SDL_SetWindowsMessageHook(on_message, nullptr);
    SDL_SetEventFilter(filter, nullptr);
}

#else

void install_real_shift() {}

#endif

}  // namespace crusader
