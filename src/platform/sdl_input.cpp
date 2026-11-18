#include "platform/sdl_input.hpp"

#include <SDL.h>

namespace crusader {

namespace {

struct Mapping {
    SDL_Scancode sdl;
    std::uint8_t pc;
    bool extended;
};

// IBM PC/AT keyboard, scancode set 1.
constexpr Mapping kMappings[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, false},
    {SDL_SCANCODE_1, 0x02, false}, {SDL_SCANCODE_2, 0x03, false}, {SDL_SCANCODE_3, 0x04, false},
    {SDL_SCANCODE_4, 0x05, false}, {SDL_SCANCODE_5, 0x06, false}, {SDL_SCANCODE_6, 0x07, false},
    {SDL_SCANCODE_7, 0x08, false}, {SDL_SCANCODE_8, 0x09, false}, {SDL_SCANCODE_9, 0x0A, false},
    {SDL_SCANCODE_0, 0x0B, false}, {SDL_SCANCODE_MINUS, 0x0C, false}, {SDL_SCANCODE_EQUALS, 0x0D, false},
    {SDL_SCANCODE_BACKSPACE, 0x0E, false}, {SDL_SCANCODE_TAB, 0x0F, false},
    {SDL_SCANCODE_Q, 0x10, false}, {SDL_SCANCODE_W, 0x11, false}, {SDL_SCANCODE_E, 0x12, false},
    {SDL_SCANCODE_R, 0x13, false}, {SDL_SCANCODE_T, 0x14, false}, {SDL_SCANCODE_Y, 0x15, false},
    {SDL_SCANCODE_U, 0x16, false}, {SDL_SCANCODE_I, 0x17, false}, {SDL_SCANCODE_O, 0x18, false},
    {SDL_SCANCODE_P, 0x19, false}, {SDL_SCANCODE_LEFTBRACKET, 0x1A, false},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1B, false}, {SDL_SCANCODE_RETURN, 0x1C, false},
    {SDL_SCANCODE_LCTRL, 0x1D, false},
    {SDL_SCANCODE_A, 0x1E, false}, {SDL_SCANCODE_S, 0x1F, false}, {SDL_SCANCODE_D, 0x20, false},
    {SDL_SCANCODE_F, 0x21, false}, {SDL_SCANCODE_G, 0x22, false}, {SDL_SCANCODE_H, 0x23, false},
    {SDL_SCANCODE_J, 0x24, false}, {SDL_SCANCODE_K, 0x25, false}, {SDL_SCANCODE_L, 0x26, false},
    {SDL_SCANCODE_SEMICOLON, 0x27, false}, {SDL_SCANCODE_APOSTROPHE, 0x28, false},
    {SDL_SCANCODE_GRAVE, 0x29, false}, {SDL_SCANCODE_LSHIFT, 0x2A, false},
    {SDL_SCANCODE_BACKSLASH, 0x2B, false}, {SDL_SCANCODE_NONUSBACKSLASH, 0x56, false},
    {SDL_SCANCODE_Z, 0x2C, false}, {SDL_SCANCODE_X, 0x2D, false}, {SDL_SCANCODE_C, 0x2E, false},
    {SDL_SCANCODE_V, 0x2F, false}, {SDL_SCANCODE_B, 0x30, false}, {SDL_SCANCODE_N, 0x31, false},
    {SDL_SCANCODE_M, 0x32, false}, {SDL_SCANCODE_COMMA, 0x33, false}, {SDL_SCANCODE_PERIOD, 0x34, false},
    {SDL_SCANCODE_SLASH, 0x35, false}, {SDL_SCANCODE_RSHIFT, 0x36, false},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37, false}, {SDL_SCANCODE_LALT, 0x38, false},
    {SDL_SCANCODE_SPACE, 0x39, false}, {SDL_SCANCODE_CAPSLOCK, 0x3A, false},
    {SDL_SCANCODE_F1, 0x3B, false}, {SDL_SCANCODE_F2, 0x3C, false}, {SDL_SCANCODE_F3, 0x3D, false},
    {SDL_SCANCODE_F4, 0x3E, false}, {SDL_SCANCODE_F5, 0x3F, false}, {SDL_SCANCODE_F6, 0x40, false},
    {SDL_SCANCODE_F7, 0x41, false}, {SDL_SCANCODE_F8, 0x42, false}, {SDL_SCANCODE_F9, 0x43, false},
    {SDL_SCANCODE_F10, 0x44, false}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x45, false},
    {SDL_SCANCODE_SCROLLLOCK, 0x46, false},
    {SDL_SCANCODE_KP_7, 0x47, false}, {SDL_SCANCODE_KP_8, 0x48, false}, {SDL_SCANCODE_KP_9, 0x49, false},
    {SDL_SCANCODE_KP_MINUS, 0x4A, false}, {SDL_SCANCODE_KP_4, 0x4B, false}, {SDL_SCANCODE_KP_5, 0x4C, false},
    {SDL_SCANCODE_KP_6, 0x4D, false}, {SDL_SCANCODE_KP_PLUS, 0x4E, false}, {SDL_SCANCODE_KP_1, 0x4F, false},
    {SDL_SCANCODE_KP_2, 0x50, false}, {SDL_SCANCODE_KP_3, 0x51, false}, {SDL_SCANCODE_KP_0, 0x52, false},
    {SDL_SCANCODE_KP_PERIOD, 0x53, false}, {SDL_SCANCODE_F11, 0x57, false}, {SDL_SCANCODE_F12, 0x58, false},
    // E0-prefixed keys share the set-1 code of their keypad/left twin.
    {SDL_SCANCODE_KP_ENTER, 0x1C, true}, {SDL_SCANCODE_RCTRL, 0x1D, true},
    {SDL_SCANCODE_KP_DIVIDE, 0x35, true}, {SDL_SCANCODE_RALT, 0x38, true},
    {SDL_SCANCODE_HOME, 0x47, true}, {SDL_SCANCODE_UP, 0x48, true}, {SDL_SCANCODE_PAGEUP, 0x49, true},
    {SDL_SCANCODE_LEFT, 0x4B, true}, {SDL_SCANCODE_RIGHT, 0x4D, true}, {SDL_SCANCODE_END, 0x4F, true},
    {SDL_SCANCODE_DOWN, 0x50, true}, {SDL_SCANCODE_PAGEDOWN, 0x51, true},
    {SDL_SCANCODE_INSERT, 0x52, true}, {SDL_SCANCODE_DELETE, 0x53, true},
};

std::uint16_t bios_flags(Uint16 mod) {
    std::uint16_t f = 0;
    if (mod & KMOD_RSHIFT) f |= kRightShift;
    if (mod & KMOD_LSHIFT) f |= kLeftShift;
    if (mod & KMOD_NUM) f |= kNumLock;
    if (mod & KMOD_CAPS) f |= kCapsLock;
    if (mod & KMOD_LCTRL) f |= kLeftCtrl;
    if (mod & KMOD_LALT) f |= kLeftAlt;
    if (mod & KMOD_RCTRL) f |= kRightCtrl;
    if (mod & KMOD_RALT) f |= kRightAlt;
    if (mod & KMOD_CTRL) f |= kCtrl;
    if (mod & KMOD_ALT) f |= kAlt;
    return f;
}

// Keypad characters as in the original's tables (key_event_to_ascii,
// 11D0:0013, DS:2F4C..31E3): * - + always; the digits with Num Lock, Shift or
// Caps Lock, none otherwise (the keys move then); '.' (0x53) none.
// @orig 11D0:0013
std::uint8_t keypad_ascii(SDL_Keycode key, std::uint16_t flags) {
    switch (key) {
    case SDLK_KP_MULTIPLY: return '*';
    case SDLK_KP_MINUS: return '-';
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_DIVIDE: return '/';
    default: break;
    }
    if (!(flags & (kNumLock | kCapsLock | kLeftShift | kRightShift))) return 0;
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9) return static_cast<std::uint8_t>('1' + (key - SDLK_KP_1));
    if (key == SDLK_KP_0) return '0';
    return 0;
}

// Character for the key in the user's layout. Text entry in the game needs
// printable ASCII, plus the umlauts of a German keyboard (the German data's
// save names, load_screen.hpp), given as small code page 850 letters (SDL
// names those keys by their Unicode letter); other accents and dead keys are
// out of scope here.
std::uint8_t ascii_for(SDL_Keycode key, std::uint16_t flags) {
    if (std::uint8_t c = keypad_ascii(key, flags)) return c;
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) return '\r';
    if (key == SDLK_BACKSPACE) return 8;
    if (key == SDLK_TAB) return '\t';
    if (key == SDLK_ESCAPE) return 27;
    if (key == 0xE4) return 0x84;  // a umlaut
    if (key == 0xF6) return 0x94;  // o umlaut
    if (key == 0xFC) return 0x81;  // u umlaut
    if (key < 32 || key > 126) return 0;
    char c = static_cast<char>(key);
    const bool shift = flags & (kLeftShift | kRightShift);
    const bool caps = flags & kCapsLock;
    if (c >= 'a' && c <= 'z' && (shift != caps)) {
        c = static_cast<char>(c - 'a' + 'A');
    }
    return static_cast<std::uint8_t>(c);
}

}  // namespace

std::uint8_t pc_scancode(int sdl_scancode, bool* extended) {
    for (const Mapping& m : kMappings) {
        if (m.sdl == sdl_scancode) {
            if (extended) *extended = m.extended;
            return m.pc;
        }
    }
    if (extended) *extended = false;
    return 0;
}

std::optional<KeyEvent> translate_key(const SDL_Event& ev) {
    if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP) {
        return std::nullopt;
    }
    KeyEvent k;
    k.scancode = pc_scancode(ev.key.keysym.scancode, &k.extended);
    if (k.scancode == 0) {
        return std::nullopt;
    }
    k.pressed = ev.type == SDL_KEYDOWN;
    k.flags = bios_flags(ev.key.keysym.mod);
    k.ascii = k.pressed ? ascii_for(ev.key.keysym.sym, k.flags) : 0;
    return k;
}

std::optional<MouseEvent> translate_mouse(const SDL_Event& ev) {
    // With SDL_RenderSetLogicalSize (SdlPresenter), SDL already reports
    // positions in game screen coordinates.
    MouseEvent m;
    switch (ev.type) {
        case SDL_MOUSEMOTION:
            m.kind = MouseEvent::Kind::Move;
            m.x = ev.motion.x;
            m.y = ev.motion.y;
            m.dx = ev.motion.xrel;
            m.dy = ev.motion.yrel;
            return m;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            m.kind = ev.type == SDL_MOUSEBUTTONDOWN ? MouseEvent::Kind::ButtonDown : MouseEvent::Kind::ButtonUp;
            m.x = ev.button.x;
            m.y = ev.button.y;
            m.button = ev.button.button == SDL_BUTTON_LEFT    ? 1
                       : ev.button.button == SDL_BUTTON_RIGHT ? 2
                       : ev.button.button == SDL_BUTTON_MIDDLE ? 3
                                                               : 0;
            return m;
        default:
            return std::nullopt;
    }
}

}  // namespace crusader
