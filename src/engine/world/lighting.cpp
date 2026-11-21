#include "engine/world/lighting.hpp"

#include "engine/world/projectile.hpp"
#include "engine/world/sprite_process.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace crusader {

namespace {

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 1};
}
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Camera basis of the isometric view, in lighting space.
const Vec3 kToViewer{0.6124f, 0.6124f, 0.5f};
const Vec3 kScreenRight{0.7071f, -0.7071f, 0.0f};
const Vec3 kScreenUp{-0.3536f, -0.3536f, 0.8660f};
// Screen pixels per lighting-space unit across the view: (x - y) / 4 = k (x - y) cos 45.
constexpr float kPixelScale = 0.35355f;
// Along a pixel's view ray, lighting-space point = A + B s, with s = x + y:
// A = (2 sx, -2 sx, -sy kLightZScale), B = (1/2, 1/2, kLightZScale / 8).
const Vec3 kRayStep{0.5f, 0.5f, kLightZScale / 8};
const float kRayStepLength = std::sqrt(dot(kRayStep, kRayStep));

constexpr int kTile = 16;
constexpr int kSmokeSteps = 8;
constexpr std::size_t kMaxPuffs = 160;
// A lamp's halo (halo_over): its light times this at its centre, and how far
// behind a surface the light may be and still glow over it (a lamp's wall).
constexpr float kHaloGain = 0.1f;
constexpr float kHaloDepth = 16;

// Colour-cycled palette entries (palette_cycle.hpp): blinking lamps,
// screens, force fields, lasers.
bool glowing(int c) { return c >= 1 && c <= 14; }

// Characters, drawn as billboards: the NPC table (ids 1..255, the Silencer
// included, whose shape has no NPC typeflag) and NPC shapes created later.
bool is_actor(const Item& item, const ShapeInfo* info) {
    return is_npc_id(item.id) || (info && info->is(ShapeInfo::kNpc));
}

// A rocket in flight: the projectile's frames 0x11..0x20, by direction, for
// fire types 3, 9 (guided) and 10 (projectile.cpp, projectile_frame).
// @orig none: port lighting
bool is_rocket(const Item& item) {
    return item.shape == kProjectileShape && item.frame >= 0x11 && item.frame <= 0x20;
}

// Liquid surfaces (SHAPES.FLX; no TYPEFLAG bit marks them): the pools 0x150
// and 0x59E (16 x 16), their strips 0x227, 0x228, 0x5A2, thin edges 0x55A,
// 0x56D, 0x56E, ripples and bubbles 0x1D6, 0x539, 0x54C, 0x580, and the
// bubbling green puddle G_POOL 0x15D (12 x 9, frames 0..3 animated).
// @orig none: port lighting (liquids)
bool is_liquid(std::uint16_t shape) {
    switch (shape) {
    case 0x150: case 0x59E: case 0x227: case 0x228: case 0x5A2: case 0x55A: case 0x56D: case 0x56E:
    case 0x1D6: case 0x539: case 0x54C: case 0x580: case 0x15D:
        return true;
    default:
        return false;
    }
}
// The pools and their strips give off steam.
bool steams(std::uint16_t shape) {
    return shape == 0x150 || shape == 0x59E || shape == 0x227 || shape == 0x228 || shape == 0x5A2 || shape == 0x15D;
}

// The liquids' radioactive green, and how much of their own colour they glow
// with (times liquid_glow), so that they show but stay a background.
const Vec3 kLiquidTint{0.45f, 1.0f, 0.35f};
constexpr float kLiquidGlow = 1.4f;
// Their light on what is around them: one by 1024 x 1024 cell of liquid.
constexpr float kLiquidCell = 1024;
constexpr float kLiquidLight = 2.0f;
// The ambient light near them, at most this much lower (liquid_dim).
constexpr float kLiquidDim = 0.75f;
constexpr std::size_t kMaxSteam = 128;

// Light fixtures (SHAPES.FLX; no TYPEFLAG bit marks them): the frames where
// they are whole give light, their broken frames none, so that a lamp shot
// goes out; a shape may have one entry per light colour. `bulb`: the light
// is at the sprite's brightest pixels (the bulb), else in the middle of the
// box (screens, whose frame is bright too). `pulse`: the alarm lights, whose
// lit frames are those of an alert (set_alert, world_intrinsics.hpp) and
// whose red is colour-cycled (entries 8 and 10, two ramps half a cycle apart:
// palette_cycle.hpp): the light stays at the middle of those pixels and is as
// bright as they are now, so that it pulses with the sprite. `halo`: the glow round the bulb
// (PointLight::halo), lamps only: a screen's light comes from its whole face.
// `face`: a machine lit from inside behind its glass (vending machines, a
// drinks fridge), its glass on the box's +x (1) or +y (2) face: the light
// sits just in front of the glass and shines out of it, a spot, so the wall
// behind the machine stays dark. `levels`: the light's strength by lit
// frame (from frame 0), for a light that flickers with the sprite's
// animation (TYPEFLAG, item_anim.cpp), a vending machine's neon; null: whole.
// `flicker`: a flame's light wavers (kFlame), an electric arc's jumps from
// one strength to another a few times a second (kSpark); each item its own.
// `glows`: the sprite itself is a light (an arc): drawn as it is, not
// shaded, as a flash.
struct Fixture {
    enum Flicker : std::uint8_t { kSteady, kFlame, kSpark };
    std::uint16_t shape;
    std::uint32_t lit_frames;  // bit n: frame n gives light
    Vec3 colour;
    float strength, radius;
    bool bulb;
    bool pulse = false;
    float halo = 0;
    float halo_gain = 1;
    std::uint8_t face = 0;
    const float* levels = nullptr;
    Flicker flicker = kSteady;
    bool glows = false;
};
Fixture flickering(Fixture f, Fixture::Flicker how) {
    f.flicker = how;
    return f;
}
const Vec3 kWarmLight{1.0f, 0.86f, 0.66f};
const Vec3 kTubeLight{0.85f, 0.95f, 1.0f};
const Vec3 kScreenLight{0.55f, 0.75f, 1.0f};
const Vec3 kRedLight{1.0f, 0.38f, 0.25f};
const Vec3 kAlarmLight{1.0f, 0.15f, 0.08f};
const Vec3 kVendingLight{0.75f, 0.88f, 1.0f};
const Vec3 kTerminalOrange{1.0f, 0.6f, 0.25f};
const Vec3 kTeleportLight{0.45f, 0.65f, 1.0f};
const Vec3 kArcLight{0.45f, 0.7f, 1.0f};
const Vec3 kFieldLight{0.6f, 0.55f, 1.0f};
const Vec3 kFireLight{1.0f, 0.55f, 0.2f};
constexpr float kArcHalo = 96;
constexpr float kArcHaloGain = 2.5f;
// How far in front of its glass a lit machine's light is, how high up its
// box (0 floor, 1 top), and its cone round the face's normal.
constexpr float kFaceOut = 20;
constexpr float kFaceHeight = 0.55f;
constexpr float kFaceCosInner = 0.35f, kFaceCosOuter = -0.15f;
// A vending machine's neon (frames 0..2): bright, dimmer, off.
const float kVendingLevels[] = {1.0f, 0.65f, 0.2f};
const Fixture kFixtures[] = {
    {0x2A0, 0x0F, kWarmLight, 1.3f, 620, true, false, 44},  // wall lamp, a dome (4..7 broken)
    {0x269, 0x01, kWarmLight, 1.3f, 650, true, false, 48},  // ceiling lamp
    {0x4EB, 0x03, kTubeLight, 1.2f, 620, true, false, 44},  // strip lights (2, 3 broken)
    {0x519, 0x03, kTubeLight, 1.2f, 620, true, false, 44},
    {0x3B7, 0x0F, kWarmLight, 0.9f, 380, true, false, 30},  // desk lamps (frames 0..3)
    {0x435, 0x30, kWarmLight, 1.1f, 520, true, false, 40},  // a globe on a stand (frames 4, 5)
    {0x102, 0x01, kScreenLight, 0.45f, 300, false},  // monitors (1: back, 2, 3 broken)
    {0x165, 0x01, kScreenLight, 0.45f, 300, false},
    {0x130, 0x01, Vec3{1.0f, 0.75f, 0.35f}, 0.6f, 420, false},  // wall screens with the E (1 broken)
    {0x1A3, 0x01, Vec3{1.0f, 0.75f, 0.35f}, 0.6f, 420, false},
    {0x0BC, 0x09, kTubeLight, 2.6f, 800, true, false, 30, 0.6f},  // wall lamps: a floodlight (frames 0, 3),
    {0x0BC, 0x36, kRedLight, 1.4f, 520, true, false, 40},   // red caged lamps (1, 4) and lanterns (2, 5); 6..11 broken
    {0x021, 0x02, kAlarmLight, 1.2f, 460, true, true, 40},  // ALERT signs (0 off, 1 on, 2 broken)
    {0x049, 0x02, kAlarmLight, 1.2f, 460, true, true, 40},
    {0x174, 0x02, kAlarmLight, 1.4f, 520, true, true, 44},  // alarm lights (0 off, 1 on, 2 broken)
    {0x271, 0x02, kAlarmLight, 1.4f, 520, true, true, 44},
    {0x477, 0x0C, kAlarmLight, 0.8f, 320, true, true, 30},  // alarm box: 2, 3 its frames 0, 1 on
    // Vending machines (PEPSIEW +x, PEPSINS +y; frames 0..2, their neon
    // flickering; shot: replaced by 0x39F, 0x3A0) and a drinks fridge (+x;
    // 1 broken, DAMAGE.FLX).
    {0x094, 0x07, kVendingLight, 0.9f, 400, false, false, 60, 0.3f, 1, kVendingLevels},
    {0x09A, 0x07, kVendingLight, 0.9f, 400, false, false, 60, 0.3f, 2, kVendingLevels},
    {0x40B, 0x01, kTubeLight, 0.8f, 360, false, false, 56, 0.3f, 1},
    // Lamps on desks (swan necks 0x405, 0x406: 4..7 broken; the mushroom
    // 0x414: 3..5 broken), standing lamps 0x407 (3..5 broken), globes on a
    // wall bracket 0x3D8 (4..11 broken).
    {0x405, 0x0F, kWarmLight, 0.9f, 380, true, false, 28},
    {0x406, 0x0F, kWarmLight, 0.9f, 380, true, false, 28},
    {0x414, 0x07, kWarmLight, 0.9f, 380, true, false, 30},
    {0x407, 0x07, kWarmLight, 1.1f, 520, true, false, 40},
    {0x3D8, 0x0F, kWarmLight, 1.0f, 460, true, false, 36},
    // Wall terminals: an orange screen (frame 0) or a blue one (1); 2, 3
    // off, 4, 5 broken.
    {0x3AB, 0x01, kTerminalOrange, 0.45f, 300, false},
    {0x3AB, 0x02, kScreenLight, 0.45f, 300, false},
    {0x474, 0x01, kTerminalOrange, 0.45f, 300, false},
    {0x474, 0x02, kScreenLight, 0.45f, 300, false},
    // The blue teleporter pad (RTELEPAD), a faint glow from its pool.
    {0x188, 0x03, kTeleportLight, 0.5f, 340, false},
    // Force fields (EWALLEW, EWALLNS: translucent, no colour-cycled pixels).
    {0x329, 0x7F, kFieldLight, 0.5f, 360, false},
    {0x32A, 0x7F, kFieldLight, 0.5f, 360, false},
    // Electric arcs between two transformers (SHOCKEW, SHOCKNS: 0..5, 6
    // none; TRANSFOR's usecode destroys them when it breaks), and the arcs
    // that the shockers on the walls throw at whoever touches them (WSHOCKNS
    // 0x44D and WSHOCKEW 0x44E create SHOCKNS1 0x459 and SHOCKEW1 0x45A, the
    // same frames). Drawn as lights, with a wide halo.
    {0x1C2, 0x3F, kArcLight, 2.8f, 700, false, false, kArcHalo, kArcHaloGain, 0, nullptr, Fixture::kSpark, true},
    {0x1C9, 0x3F, kArcLight, 2.8f, 700, false, false, kArcHalo, kArcHaloGain, 0, nullptr, Fixture::kSpark, true},
    {0x459, 0x3F, kArcLight, 2.8f, 700, false, false, kArcHalo, kArcHaloGain, 0, nullptr, Fixture::kSpark, true},
    {0x45A, 0x3F, kArcLight, 2.8f, 700, false, false, kArcHalo, kArcHaloGain, 0, nullptr, Fixture::kSpark, true},
    // Fires (items, not sprites): what burns (class ITEM, Item::I_create:
    // 0x085, 0x44A, 0x456, 0x591..0x593), the jets of the flame traps
    // (FLAMEBOX: 0x43A..0x43C, 0x50A, 0x4CB, 0x518) and the columns of
    // NOSTRIL (0x515, 0x516). Every frame burns.
    flickering({0x085, 0x1F, kFireLight, 1.3f, 520, true, false, 30}, Fixture::kFlame),
    flickering({0x44A, 0x1F, kFireLight, 1.3f, 520, true, false, 30}, Fixture::kFlame),
    flickering({0x456, 0x1F, kFireLight, 1.3f, 520, true, false, 30}, Fixture::kFlame),
    flickering({0x591, 0x3FF, kFireLight, 1.3f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x592, 0x3FF, kFireLight, 1.1f, 460, true, false, 26}, Fixture::kFlame),
    flickering({0x593, 0x3FF, kFireLight, 1.3f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x43A, 0x3F, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x43B, 0x0FFFFFFF, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x43C, 0x1F, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x50A, 0x0FFFFFFF, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x4CB, 0x1F, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x518, 0x3F, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x515, 0x3FFFFF, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
    flickering({0x516, 0xFFFFF, kFireLight, 1.4f, 560, true, false, 30}, Fixture::kFlame),
};

// The fixtures' light against the ambient light (LightingOptions::ambient):
// tuned dark, they would hardly show in the game's bright default (85 %).
float fixture_boost(float ambient) { return 1 + 1.2f * std::clamp(ambient, 0.0f, 1.0f); }

// Broken machines (SHAPES.FLX, their frames once shot, as the usecode or
// DAMAGE.FLX leave them): they smoulder, a thin wisp rising from them for
// good. Electronics only: a lamp has none, nor a desk.
// Not only broken things: a fire gives off its smoke (as long as it burns:
// the fixtures that burn, kFixtures), a glowing core its heat, a pierced pipe
// or a blown tank its steam.
enum class Plume : std::uint8_t {
    kWisp,   // a thin wisp from the top
    kMotor,  // a fan: a thicker smoke from its hub, in the middle of its face
    kFire,   // a fire: thick dark smoke rising fast
    kHeat,   // a glowing core: a wide, faint haze over its surface
    kSteam,  // a leak: small white puffs, quickly gone
};
// `mended`: a leak whose frame less this is the same thing whole (a pierced
// pipe): the plume comes out of the hole, the pixels that differ.
struct Smoulder {
    std::uint16_t shape;
    std::uint32_t frames;  // bit n: frame n gives off smoke
    Plume plume = Plume::kWisp;
    std::uint8_t mended = 0;
};
const Smoulder kSmoulders[] = {
    // Screens and computers.
    {0x102, 0x0C}, {0x165, 0x0C},  // monitors
    {0x130, 0x02}, {0x1A3, 0x02},  // wall screens with the E
    {0x370, 0x02},                 // a small screen
    {0x3DE, 0xF0}, {0x3DF, 0xF0}, {0x3E0, 0xF0},  // small monitors and sets
    {0x131, 0x0C}, {0x13A, 0x0C}, {0x182, 0x0C},  // computers
    {0x0E8, 0x0C}, {0x185, 0x0C}, {0x3D9, 0x38}, {0x446, 0x38},  // electronic boxes
    {0x257, 0x08}, {0x258, 0x08},  // BIGCOMNS, BIGCOMEW
    {0x1B0, 0x08}, {0x1B1, 0x08}, {0x1B2, 0x02}, {0x1B3, 0x02},  // consoles
    {0x1EF, 0x30}, {0x1F8, 0x30},  // control desks
    {0x25B, 0x60}, {0x25C, 0x60}, {0x25D, 0x02}, {0x25E, 0x02},
    {0x26A, 0x02}, {0x26B, 0x02},  // racks
    {0x3BF, 0xF0}, {0x3C0, 0xF0},  // cabinets
    {0x431, 0xFFFF00},             // server cabinets (8..23)
    {0x409, 0xF0},                 // arcade cabinets
    {0x2ED, 0x02}, {0x2EE, 0x02},  // computer cabinets
    {0x37B, 0x02}, {0x37C, 0x02}, {0x380, 0x02}, {0x381, 0x02},  // consoles
    {0x3AB, 0x30}, {0x474, 0x30},  // wall terminals
    {0x3D4, 0x0A},                 // black cabinets (1, 3)
    {0x3E1, 0xF0},                 // beige consoles
    {0x3E4, 0x2A}, {0x3E5, 0x2A},  // computers (1, 3, 5)
    {0x433, 0x0C}, {0x434, 0x0C},  // copiers (COPY_EW, COPY_NS)
    {0x39F, 0x01}, {0x3A0, 0x01},  // vending machines, broken (PEPSIEW, PEPSINS replaced)
    {0x40B, 0x02},                 // drinks fridge
    // Fans: shot, they are replaced by these shapes, frame 0.
    {0x3CE, 0x01, Plume::kMotor}, {0x3D3, 0x01, Plume::kMotor},  // RFANBADE, RFANBADN (RFANEW, RFANNS)
    {0x32C, 0x03, Plume::kMotor}, {0x32D, 0x03, Plume::kMotor},  // MFAN_EW, MFAN_NS broken
    {0x3E7, 0x0C}, {0x3E8, 0x0C},  // wall vents (WVENTNS, WVENTEW)
    // Power: chargers, generators, transformers.
    {0x136, 0x30}, {0x158, 0x30},  // chargers (RECHAGNS, RECHAGEW: both parts)
    {0x3C1, 0x3C},                 // GENERATR
    {0x3C2, 0x0C}, {0x3C3, 0x0C},  // generator cabinets
    {0x1BF, 0x02},                 // TRANSFOR
    {0x073, 0x02}, {0x0C8, 0x02}, {0x0FF, 0x02}, {0x21C, 0x02}, {0x2A5, 0x02},  // power units
    {0x4F6, 0x0C}, {0x55C, 0x0C},
    {0x176, 0xF0},                 // power cubes
    {0x454, 0xF0}, {0x455, 0xF0},  // heaters
    {0x1F1, 0x38}, {0x1FA, 0x38}, {0x1F0, 0x02}, {0x1F9, 0x02},  // heating conduits
    {0x1B6, 0x20}, {0x1B8, 0x200}, {0x1D7, 0x200},  // cameras (CAMERACO, CAMERANS, CAMERAEW)
    // Fires: what burns, the flame traps' jets, NOSTRIL's columns.
    {0x085, 0x1F, Plume::kFire}, {0x44A, 0x1F, Plume::kFire}, {0x456, 0x1F, Plume::kFire},
    {0x591, 0x3FF, Plume::kFire}, {0x592, 0x3FF, Plume::kFire}, {0x593, 0x3FF, Plume::kFire},
    {0x43A, 0x3F, Plume::kFire}, {0x43B, 0x0FFFFFFF, Plume::kFire}, {0x43C, 0x1F, Plume::kFire},
    {0x50A, 0x0FFFFFFF, Plume::kFire}, {0x4CB, 0x1F, Plume::kFire}, {0x518, 0x3F, Plume::kFire},
    {0x515, 0x3FFFFF, Plume::kFire}, {0x516, 0xFFFFF, Plume::kFire},
    // Gas tanks (0x520, 0x521: 2, 3 blown open) leak.
    {0x520, 0x0C, Plume::kSteam}, {0x521, 0x0C, Plume::kSteam},
    // Pierced pipes leak steam, some from the start (placed so on the maps),
    // others once shot: straight pipes (1 pierced) and joints (4..7).
    {0x02E, 0x02, Plume::kSteam, 1}, {0x02F, 0x02, Plume::kSteam, 1}, {0x03D, 0x02, Plume::kSteam, 1},
    {0x03E, 0x02, Plume::kSteam, 1}, {0x03F, 0x02, Plume::kSteam, 1}, {0x04C, 0x02, Plume::kSteam, 1},
    {0x04E, 0x02, Plume::kSteam, 1}, {0x050, 0x02, Plume::kSteam, 1}, {0x051, 0x02, Plume::kSteam, 1},
    {0x052, 0x02, Plume::kSteam, 1}, {0x053, 0x02, Plume::kSteam, 1}, {0x054, 0x02, Plume::kSteam, 1},
    {0x03C, 0xF0, Plume::kSteam, 4}, {0x041, 0xF0, Plume::kSteam, 4}, {0x056, 0xF0, Plume::kSteam, 4},
    {0x057, 0xF0, Plume::kSteam, 4},
};
constexpr std::size_t kMaxSmoulder = 96;

// The game's own jets of steam (STEAMBOX 0x500, both games): STEAM1 0x4FA
// and STEAM2 0x511, opening (0x4F9, 0x4FD) and shutting (0x4FB, 0x512), and
// the marker 0x3A9 of a shut one, which a valve opens again. Each stands in
// front of the hole of a pierced pipe, its box 64 to 100 units off the pipe:
// that pipe's steam is the jet's sprite, shut with its valve, not a leak.
// @orig none: port lighting (smoke)
bool is_steam_jet(std::uint16_t shape) {
    return shape == 0x3A9 || shape == 0x4F9 || shape == 0x4FA || shape == 0x4FB || shape == 0x4FD || shape == 0x511 ||
           shape == 0x512;
}
constexpr float kJetReach = 128;  // from the jet's box to the hole, x and y

// @orig none: port lighting (smoke)
const Smoulder* smoulders(std::uint16_t shape, std::uint32_t frame) {
    for (const Smoulder& s : kSmoulders)
        if (s.shape == shape && frame < 32 && (s.frames >> frame & 1)) return &s;
    return nullptr;
}

// @orig none: port lighting (light fixtures)
const Fixture* fixture(std::uint16_t shape, std::uint32_t frame) {
    for (const Fixture& f : kFixtures) {
        if (f.shape == shape && frame < 32 && (f.lit_frames >> frame & 1)) return &f;
    }
    return nullptr;
}

// Explosion and flash shapes (sprite_process.hpp, list DS:29C2).
bool is_blast_shape(std::uint16_t shape) { return shape == 0x31C || (shape >= 0x31F && shape <= 0x327); }

// The impact of a bullet (impact_sprite: fire types 1, 2, 0xB, 0xD): a
// spark, so a small light, smaller than the flash at the muzzle.
constexpr std::uint16_t kBulletImpactShape = 0x1D8;

// Muzzle flash by fire type (dtable.cpp: the weapons' fire types): bullets
// 1 (RP-22, RP-32), 0xB (BA-40, BA-41), several pellets 2 (SG-A1) and 0xD
// (AC-88), grenades 3 (GL-303). Energy weapons and rockets have none: their
// shot is a bright sprite that lights from the muzzle on.
struct FlashKind {
    Vec3 colour;
    float strength;
    float radius;
    std::uint32_t life;  // ticks (60 a second)
};
// @orig none: port lighting (muzzle flash)
std::optional<FlashKind> muzzle_flash(int fire_type) {
    const Vec3 gunfire{1.0f, 0.72f, 0.38f};
    switch (fire_type) {
    case 1: case 0xB: return FlashKind{gunfire, 2.4f, 650, 4};
    case 2: case 0xD: return FlashKind{gunfire, 2.8f, 750, 5};
    case 3: return FlashKind{Vec3{1.0f, 0.6f, 0.3f}, 2.0f, 600, 5};
    default: return std::nullopt;
    }
}

float srgb_to_linear(std::uint8_t c) { return std::pow(c / 255.0f, 2.2f); }

// How much of a spot's light goes to a point `to` away from the light (to =
// light - point) at distance `dist`: 1 for a light all round.
// @orig none: port lighting (the Silencer's lamp)
float spot(const PointLight& L, const Vec3& to, float dist) {
    if (L.dir.x == 0 && L.dir.y == 0 && L.dir.z == 0) return 1;
    return smoothstep(L.cos_outer, L.cos_inner, -dot(to, L.dir) / std::max(dist, 1e-3f));
}

// Integer hash (also in the shaders).
// @orig none: port lighting
std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A flickering fixture's strength now (Fixture::flicker), times its own.
// @orig none: port lighting (light fixtures)
float flicker_at(const Fixture& fx, ObjId id, std::uint32_t ticks) {
    if (fx.flicker == Fixture::kFlame) {
        const float t = float(ticks);
        return 0.8f + 0.12f * std::sin(t * 0.9f + id * 0.7f) + 0.08f * std::sin(t * 2.3f + id * 1.3f);
    }
    // An arc: a new strength every 3 ticks, now and then almost out.
    const std::uint32_t step = ticks / 3;
    const float r = float(hash32((step + id * 7919u) * 2654435761u) & 0xFF) / 255.0f;
    return r < 0.15f ? 0.25f : 0.6f + 0.5f * r;
}

float lattice(std::int32_t x, std::int32_t y, std::int32_t z) {
    const std::uint32_t h = hash32(static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                                   static_cast<std::uint32_t>(z) * 83492791u);
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

// @orig none: port lighting
float value_noise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const std::int32_t ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy),
                       iz = static_cast<std::int32_t>(fz);
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    tz = tz * tz * (3 - 2 * tz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(lattice(ix, iy, iz), lattice(ix + 1, iy, iz), tx);
    const float x10 = lerp(lattice(ix, iy + 1, iz), lattice(ix + 1, iy + 1, iz), tx);
    const float x01 = lerp(lattice(ix, iy, iz + 1), lattice(ix + 1, iy, iz + 1), tx);
    const float x11 = lerp(lattice(ix, iy + 1, iz + 1), lattice(ix + 1, iy + 1, iz + 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

// A random number 0..1 for puff `serial`, draw `k` (smoke only: never the game's generators).
float puff_random(std::uint32_t serial, std::uint32_t k) {
    return static_cast<float>(hash32(serial * 2654435761u + k * 40503u) & 0xFFFFu) / 65535.0f;
}

}  // namespace

// @orig none: port lighting (smoke)
float smoke_noise(const Vec3& p, float seed, float t, float feature) {
    const float k = 1.0f / feature;
    const float x = p.x * k + seed * 17.3f, y = p.y * k + seed * 9.1f, z = p.z * k + seed * 5.7f - t * 0.35f;
    return 0.5f * value_noise(x, y, z) + 0.3f * value_noise(x * 2.03f, y * 2.03f, z * 2.03f) +
           0.2f * value_noise(x * 4.01f, y * 4.01f, z * 4.01f);
}

// @orig none: port lighting (liquids)
float liquid_glow(const Vec3& p, float t) {
    const float x = p.x / 170.0f + t * 0.06f, y = p.y / 170.0f - t * 0.04f, z = t * 0.12f;
    const float n = 0.65f * value_noise(x, y, z) + 0.35f * value_noise(x * 2.1f + 5.2f, y * 2.1f + 1.3f, z * 1.7f);
    return (0.35f + 0.9f * smoothstep(0.3f, 0.75f, n)) * (0.85f + 0.15f * std::sin(t * 1.1f));
}

// The ambient light round a liquid is lower, so that the place is lit by its
// green glow: down to 1 - kLiquidDim over most of the light's reach, back to
// whole at its edge. The strongest liquid near counts, not their sum.
// @orig none: port lighting (liquids)
float liquid_dim(const std::vector<PointLight>& lights, const std::vector<std::uint32_t>* ids, const Vec3& p) {
    float most = 0;
    const std::size_t count = ids ? ids->size() : lights.size();
    for (std::size_t k = 0; k < count; ++k) {
        const PointLight& L = lights[ids ? (*ids)[k] : k];
        if (L.dim <= 0) continue;
        const Vec3 to = L.pos - p;
        const float f = 1 - dot(to, to) / (L.radius * L.radius);
        if (f > 0) most = std::max(most, L.dim * smoothstep(0.0f, 0.5f, f));
    }
    return 1 - kLiquidDim * most;
}

// A frame decoded once: colours (-1 transparent), a normal rounded from the
// silhouette (screen x / y, -127..127), and how many pixels of each colour.
struct WorldLighting::Decoded {
    int w = 0, h = 0;
    std::vector<std::int16_t> color;
    std::vector<std::int8_t> nx, ny;
    std::vector<std::pair<std::uint8_t, std::uint16_t>> histogram;
    int glow_pixels = 0;
};

// An occluder: the item's box, cut by its sprite. Seen from the camera the
// item covers only the sprite's opaque pixels, so a point of the box whose
// projection falls on a transparent pixel is empty (railings, grids and
// bodies cast their own outline, not a block).
struct WorldLighting::Box {
    Vec3 lo, hi;
    ObjId owner;
    const Decoded* sprite = nullptr;
    int left = 0, top = 0;  // frame rectangle, absolute screen coordinates
    bool mirrored = false;
};

// Worker threads kept for the whole run: run(n, f) calls f(0..n-1) spread
// over them and the caller, and returns when all are done.
class WorldLighting::Pool {
public:
    Pool() {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency()) - 1;
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { work(); });
    }
    // @orig none: port lighting
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    std::size_t size() const { return threads_.size() + 1; }

    // @orig none: port lighting
    void run(std::size_t n, const std::function<void(std::size_t)>& f) {
        if (n == 0) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = &f;
            count_ = n;
            next_ = 0;
            done_ = 0;
            ++generation_;
        }
        wake_.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&] { return done_ == count_; });
        task_ = nullptr;
    }

private:
    // @orig none: port lighting
    void drain() {
        for (std::size_t i; (i = next_++) < count_;) {
            (*task_)(i);
            if (++done_ == count_) {
                std::lock_guard<std::mutex> lock(mutex_);
                finished_.notify_all();
            }
        }
    }
    // @orig none: port lighting
    void work() {
        std::uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drain();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, finished_;
    const std::function<void(std::size_t)>* task_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0}, done_{0};
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

WorldLighting::WorldLighting(ShapeCache& shapes, const XformTable& xform, const AnimDat* anims)
    : shapes_(shapes), xform_(xform), anims_(anims), pool_(std::make_unique<Pool>()) {
    for (int i = 0; i < 4096; ++i)
        out_lut_[i] = static_cast<std::uint8_t>(std::lround(255.0 * std::pow(i / 4095.0, 1 / 2.2)));
}

WorldLighting::~WorldLighting() = default;

// @orig none: port lighting
const WorldLighting::Decoded& WorldLighting::decode(const ShapeFrame& f) {
    auto it = decoded_.find(&f);
    if (it != decoded_.end()) return *it->second;
    auto d = std::make_unique<Decoded>();
    d->w = f.width;
    d->h = f.height;
    const std::size_t n = static_cast<std::size_t>(d->w) * d->h;
    d->color.resize(n);
    d->nx.assign(n, 0);
    d->ny.assign(n, 0);
    std::array<std::uint16_t, 256> counts{};
    for (int y = 0; y < d->h; ++y)
        for (int x = 0; x < d->w; ++x) {
            const int c = f.pixel(x, y);
            d->color[y * d->w + x] = static_cast<std::int16_t>(c);
            if (c >= 0 && counts[c] < 0xFFFF) ++counts[c];
        }
    for (int c = 0; c < 256; ++c) {
        if (!counts[c]) continue;
        d->histogram.emplace_back(static_cast<std::uint8_t>(c), counts[c]);
        if (glowing(c)) d->glow_pixels += counts[c];
    }
    // Rows: the place between the first and last opaque pixels gives nx;
    // columns the same for ny, weaker (bodies are rounder across than along).
    for (int y = 0; y < d->h; ++y) {
        int a = d->w, b = -1;
        for (int x = 0; x < d->w; ++x)
            if (d->color[y * d->w + x] >= 0) a = std::min(a, x), b = std::max(b, x);
        for (int x = a; x <= b; ++x) {
            const float t = b > a ? (x - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d->nx[y * d->w + x] = static_cast<std::int8_t>((2 * t - 1) * 120);
        }
    }
    for (int x = 0; x < d->w; ++x) {
        int a = d->h, b = -1;
        for (int y = 0; y < d->h; ++y)
            if (d->color[y * d->w + x] >= 0) a = std::min(a, y), b = std::max(b, y);
        for (int y = a; y <= b; ++y) {
            const float t = b > a ? (y - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d->ny[y * d->w + x] = static_cast<std::int8_t>((1 - 2 * t) * 50);
        }
    }
    return *decoded_.emplace(&f, std::move(d)).first->second;
}

// 3D point and normal of the frame pixel (fx, fy) of an item drawn at
// absolute screen position (sx, sy). `face`: 0 top, 1 +x, 2 +y, 3 billboard.
// @orig none: port lighting
void WorldLighting::surface(const DrawItem& d, bool actor, float sx, float sy, int fx, int fy, bool bump, Vec3& p,
                            Vec3& n, int* face) const {
    const float xl = float(d.x_left), xh = float(d.x), yl = float(d.y_far), yh = float(d.y);
    const float zl = float(d.z), zh = float(d.z_top);
    // Points of the view ray: x = (s + 4sx)/2, y = (s - 4sx)/2, z = s/8 - sy.
    const float ux = 2 * xh - 4 * sx, uy = 2 * yh + 4 * sx, uz = 8 * (zh + sy);
    const float lo = std::max({2 * xl - 4 * sx, 2 * yl + 4 * sx, 8 * (zl + sy)});
    const float hi = std::min({ux, uy, uz});
    const Decoded& f = decoded(d.frame);
    float s;
    if (!actor && hi >= lo - 24) {
        s = hi;
        n = hi == uz ? Vec3{0, 0, 1} : hi == ux ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        if (face) *face = hi == uz ? 0 : hi == ux ? 1 : 2;
    } else {
        // Billboard through the box centre, rounded by the silhouette.
        s = (xl + xh + yl + yh) * 0.5f;
        float nx = f.nx[fy * f.w + fx] / 127.0f, ny = f.ny[fy * f.w + fx] / 127.0f;
        if (d.mirrored) nx = -nx;
        const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
        n = normalize(kScreenRight * nx + kScreenUp * ny + kToViewer * nz);
        if (face) *face = 3;
    }
    p = {(s + 4 * sx) * 0.5f, (s - 4 * sx) * 0.5f, (s / 8 - sy) * kLightZScale};
    if (!bump) return;
    // Bumps from the sprite's own shading (brighter = raised), within the frame.
    const int c = f.color[fy * f.w + fx];
    auto l = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= f.w || y >= f.h) return lum_[c];
        const int v = f.color[y * f.w + x];
        return v < 0 ? lum_[c] : lum_[v];
    };
    float gx = (l(fx + 1, fy) - l(fx - 1, fy)) * 0.5f;
    const float gy = (l(fx, fy + 1) - l(fx, fy - 1)) * 0.5f;
    if (d.mirrored) gx = -gx;
    const Vec3 tr = normalize(kScreenRight - n * dot(kScreenRight, n));
    const Vec3 tu = normalize(kScreenUp - n * dot(kScreenUp, n));
    constexpr float k = 2.5f;
    n = normalize(n - tr * (k * gx) + tu * (k * gy));
}

// The point and normal of an owned pixel.
// @orig none: port lighting
void WorldLighting::point_at(int x, int y, Vec3& p, Vec3& n) const {
    const std::int32_t o = owner_[static_cast<std::size_t>(y) * width_ + x];
    const DrawItem& d = list_[o];
    const int fx = d.mirrored ? d.right - 1 - x : x - d.left;
    surface(d, actor_[o], view_left_ + x + 0.5f, view_top_ + y + 0.5f, fx, y - d.top, true, p, n);
}

// @orig none: port lighting
void WorldLighting::capture(const World& world, const std::vector<DrawItem>& list, const Surface8& screen,
                            int view_left, int view_top, const Palette& palette, const LightingOptions& opt,
                            std::uint32_t ticks) {
    list_ = list;
    view_left_ = view_left;
    view_top_ = view_top;
    width_ = screen.width;
    height_ = screen.height;
    ticks_ = ticks;
    std::erase_if(flashes_, [&](const MuzzleFlash& f) { return ticks - f.born >= f.life; });
    const std::size_t n = static_cast<std::size_t>(width_) * height_;
    world_ = screen.pixels;
    owner_.assign(n, -1);
    emissive_.assign(n, 0);
    set_palette(palette);
    actor_.resize(list_.size());
    liquid_.resize(list_.size());
    glows_.resize(list_.size());
    for (std::size_t i = 0; i < list_.size(); ++i) {
        decode(*list_[i].frame);
        const Item& item = *list_[i].item;
        const Fixture* fx = opt.fixture_lights && !item.sprite_only ? fixture(item.shape, item.frame) : nullptr;
        glows_[i] = fx && fx->glows;
        actor_[i] = is_actor(*list_[i].item, world.shape_info(list_[i].item->shape));
        liquid_[i] = opt.liquids && !actor_[i] && !list_[i].item->sprite_only && is_liquid(list_[i].item->shape);
    }
    if (opt.liquids) gather_liquids(world);
    else liquids_.clear();
    if (opt.smoke) gather_jets(world);
    else jets_.clear();
    // Which item shows at each pixel, in the order they were drawn, by bands
    // of rows. Translucent items leave it to what is below; flashes and
    // explosions (drawn as sprites) only glow, and so do the fixtures that
    // are lights themselves (arcs), above the items behind them.
    const std::size_t bands = std::min<std::size_t>(pool_->size() * 2, static_cast<std::size_t>(height_));
    pool_->run(bands, [&](std::size_t band) {
        const int y0 = static_cast<int>(band * height_ / bands), y1 = static_cast<int>((band + 1) * height_ / bands);
        for (std::size_t i = 0; i < list_.size(); ++i) {
            const DrawItem& d = list_[i];
            if (d.bottom <= y0 || d.top >= y1) continue;
            const Decoded& f = decoded(d.frame);
            const bool translucent = d.shape_flags & ShapeInfo::kTranslucent;
            for (int y = std::max(d.top, y0); y < std::min(d.bottom, y1); ++y) {
                const int fy = y - d.top;
                for (int fx = 0; fx < f.w; ++fx) {
                    const int c = f.color[fy * f.w + fx];
                    if (c < 0) continue;
                    const int x = d.mirrored ? d.left + (f.w - 1 - fx) : d.left + fx;
                    if (x < 0 || x >= width_) continue;
                    const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                    if (d.item->sprite_only) {
                        emissive_[j] = 1;
                    } else if (!translucent) {
                        owner_[j] = static_cast<std::int32_t>(i);
                        emissive_[j] = glows_[i] && lum_[c] >= 0.62f;
                    }
                }
            }
        }
    });
    if (opt.smoke) spawn_smoke();
    else puffs_.clear();
}

// Position and normal of every owned pixel (processor shading).
// @orig none: port lighting
void WorldLighting::geometry(const LightingOptions& opt) {
    const std::size_t n = static_cast<std::size_t>(width_) * height_;
    pos_.resize(n);
    nrm_.resize(n);
    pool_->run(static_cast<std::size_t>(height_), [&](std::size_t row) {
        const int py = static_cast<int>(row);
        for (int px = 0; px < width_; ++px) {
            const std::size_t j = static_cast<std::size_t>(py) * width_ + px;
            const int o = owner_[j];
            if (o < 0) continue;
            const DrawItem& d = list_[o];
            const int fx = d.mirrored ? d.right - 1 - px : px - d.left;
            surface(d, actor_[o], view_left_ + px + 0.5f, view_top_ + py + 0.5f, fx, py - d.top, opt.bump, pos_[j],
                    nrm_[j]);
        }
    });
}

// New flashes, explosions and shots (sprites) leave smoke.
// @orig none: port lighting (smoke)
void WorldLighting::spawn_smoke() {
    std::unordered_map<ObjId, std::uint16_t> seen;
    for (const DrawItem& d : list_) {
        if (!d.item->sprite_only) continue;
        const ObjId id = d.item->id;
        const std::uint16_t shape = d.item->shape;
        seen[id] = shape;
        const auto before = sprites_seen_.find(id);
        const bool fresh = before == sprites_seen_.end() || before->second != shape;
        // Shots and impacts are points: the item's location.
        const Vec3 centre{float(d.x), float(d.y), d.z * kLightZScale};
        if (shape == kProjectileShape) {
            // A shot: first seen just out of the muzzle; seen again, its way
            // gives the direction the muzzle wisp is blown in.
            if (fresh) {
                shot_start_[id] = centre;
                continue;
            }
            if (const auto start = shot_start_.find(id); start != shot_start_.end()) {
                // Once it has moved (it moves every other tick or so): blown
                // from where it was first drawn, at the muzzle, along its way.
                const Vec3 way = centre - start->second;
                if (dot(way, way) > 1.0f) {
                    add_puff(start->second, SmokePuff::kMuzzle, 1, way);
                    shot_start_.erase(start);
                }
            }
            // A trail behind rockets, and behind other bright shots.
            if (is_rocket(*d.item)) {
                if (ticks_ % 3 == 0) add_puff(centre, SmokePuff::kTrail);
                continue;
            }
            if (ticks_ % 4 != 0) continue;
            const Decoded& f = decoded(d.frame);
            int bright = 0;
            for (const auto& [c, k] : f.histogram)
                if (lum_[c] > 0.62f) bright += k;
            if (bright >= 20) add_puff(centre, SmokePuff::kTrail);
            continue;
        }
        if (!is_blast_shape(shape)) {
            // An impact (these sprites keep their place): dust, only where the
            // shot met something; a shot that ran out of range in the air
            // leaves none.
            if (fresh && on_surface(centre)) add_puff(centre, SmokePuff::kImpact);
            continue;
        }
        {
            if (!fresh) continue;
            // Over the surface drawn where the sprite was created: these
            // sprites are raised to z 240 at the same screen place, then moved
            // by their shape's offset (sprite_process.hpp), which takes their
            // anchor tens of pixels lower on the screen. Without the offset
            // the point projects where it was created: a camera on its wall,
            // the floor under a grenade.
            int ox = 0, oy = 0;
            overlay_offset(shape, ox, oy);
            const float x0 = float(world_from_usecode(d.item->x - ox)), y0 = float(world_from_usecode(d.item->y - oy));
            const int px = static_cast<int>(std::floor((x0 - y0) * 0.25f)) - view_left_;
            const int py = static_cast<int>(std::floor((x0 + y0) * 0.125f - float(d.item->z))) - view_top_;
            std::optional<Vec3> at;
            for (int dy = 0; dy < 24 && !at; dy += 4) at = surface_at(px, py - dy);
            if (!at) at = Vec3{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, float(d.z) * kLightZScale};
            // Big shapes are explosions, small ones flashes and impacts.
            int area = 0;
            if (const Shape* s = shapes_.get(shape))
                for (const ShapeFrame& f : s->frames()) area = std::max(area, f.width * f.height);
            const bool blast = area > 3000;
            const Vec3 base = *at + Vec3{0, 0, (blast ? 16.0f : 10.0f) * kLightZScale};
            add_puff(base, blast ? SmokePuff::kExplosion : SmokePuff::kImpact,
                     blast ? std::clamp(std::sqrt(float(area)) / 70.0f, 0.8f, 2.0f) : 1.5f);
        }
    }
    sprites_seen_ = std::move(seen);
    std::erase_if(shot_start_, [&](const auto& s) { return !sprites_seen_.count(s.first); });
    // Gone when their life is over.
    std::erase_if(puffs_, [&](const SmokePuff& p) { return (ticks_ - p.born) / 60.0f >= p.life; });
}

// Whether a point is on (or just in front of) the surface drawn where it
// projects: an impact on a wall, a floor, an item, not in the air.
// @orig none: port lighting (smoke)
bool WorldLighting::on_surface(const Vec3& p) const {
    const float x = p.x, y = p.y, z = p.z / kLightZScale;
    const int px = static_cast<int>(std::floor((x - y) * 0.25f)) - view_left_;
    const int py = static_cast<int>(std::floor((x + y) * 0.125f - z)) - view_top_;
    for (int dy = -2; dy <= 2; dy += 2)
        for (int dx = -2; dx <= 2; dx += 2)
            if (const auto s = surface_at(px + dx, py + dy); s && dot(*s - p, *s - p) < 64.0f * 64.0f) return true;
    return false;
}

// @orig none: port lighting (smoke)
void WorldLighting::add_puff(const Vec3& at, SmokePuff::Kind kind, float scale, Vec3 dir) {
    const std::uint32_t serial = ++puff_serial_;
    auto rnd = [&](std::uint32_t k) { return puff_random(serial, k); };
    const int count = kind == SmokePuff::kExplosion ? 6 : kind == SmokePuff::kMuzzle ? 2 : 1;
    const Vec3 ahead = dot(dir, dir) > 1e-6f ? normalize(dir) : Vec3{};
    for (int i = 0; i < count; ++i) {
        SmokePuff p;
        p.kind = kind;
        p.born = ticks_;
        p.seed = rnd(10 + i) * 100.0f;
        const float jx = rnd(20 + i) * 2 - 1, jy = rnd(30 + i) * 2 - 1, jz = rnd(40 + i);
        switch (kind) {
        case SmokePuff::kExplosion:
            p.pos = at + Vec3{jx * 60, jy * 60, jz * 40} * scale;
            p.drift = {jx * 12, jy * 12, 0};
            p.rise = 45 + rnd(50 + i) * 30;
            p.radius0 = (45 + rnd(60 + i) * 30) * scale;
            p.radius1 = (140 + rnd(70 + i) * 80) * scale;
            // Thinner the bigger, so that a big blast is no thicker to see
            // through than a small one: what happens in it stays visible.
            p.density = 0.010f / scale;
            p.life = 4.5f + rnd(80 + i) * 2;
            p.feature = 45;
            break;
        case SmokePuff::kMuzzle:
            // Blown out along the shot, quickly slowed down, hardly rising.
            p.pos = at + ahead * (i * 6.0f);
            p.drift = ahead * (140.0f + 80.0f * i) + Vec3{jx * 10, jy * 10, jz * 10};
            p.drag = 5;
            p.rise = 8;
            p.radius0 = 9;
            p.radius1 = 28 + 8 * i;
            p.density = 0.06f;
            p.life = 0.7f + 0.2f * i;
            p.feature = 12;
            break;
        case SmokePuff::kImpact:
            p.pos = at;
            p.drift = Vec3{jx * 20, jy * 20, 0} * scale;
            p.drag = 3;
            p.rise = 12;
            p.radius0 = 8 * scale;
            p.radius1 = 30 * scale;
            p.density = 0.035f;
            p.life = 1.1f * scale;
            p.feature = 14;
            break;
        case SmokePuff::kTrail:
            p.pos = at;
            p.drift = {jx * 4, jy * 4, 0};
            p.rise = 14;
            p.radius0 = 10;
            p.radius1 = 34;
            p.density = 0.015f;
            p.life = 1.0f;
            p.feature = 16;
            break;
        }
        if (puffs_.size() >= kMaxPuffs) puffs_.erase(puffs_.begin());
        puffs_.push_back(p);
    }
}

// The liquids around the view (steam and light from just off screen too).
// @orig none: port lighting (liquids)
void WorldLighting::gather_liquids(const World& world) {
    liquids_.clear();
    // The view in x - y and x + y (z from 0 up to 256), with a margin.
    constexpr float kMargin = 900;
    const float dlo = 4.0f * view_left_ - kMargin, dhi = 4.0f * (view_left_ + width_) + kMargin;
    const float slo = 8.0f * view_top_ - kMargin, shi = 8.0f * (view_top_ + height_ + 256) + kMargin;
    world.for_each([&](const Item& item) {
        if (!is_liquid(item.shape) || item.off_map || item.parent != 0 || item.sprite_only ||
            (item.flags & (kItemInvisible | kItemContained)))
            return;
        const ShapeInfo* info = world.shape_info(item.shape);
        if (!info || !info->footpad_x || !info->footpad_y) return;
        const bool mirrored = item.flags & kItemFlipped;
        const float x = float(world_from_usecode(item.x)), y = float(world_from_usecode(item.y));
        const float dx = float((mirrored ? info->footpad_y : info->footpad_x) * kFootpadXY);
        const float dy = float((mirrored ? info->footpad_x : info->footpad_y) * kFootpadXY);
        if (x - (y - dy) < dlo || (x - dx) - y > dhi || x + y < slo || (x - dx) + (y - dy) > shi) return;
        const float top = float(item.z + info->footpad_z * kFootpadZ) * kLightZScale;
        liquids_.push_back({{x - dx, y - dy, top}, {x, y, top}, steams(item.shape)});
    });
}

// The game's jets of steam around the view, open or shut (is_steam_jet):
// the pierced pipes behind them do not leak.
// @orig none: port lighting (smoke)
void WorldLighting::gather_jets(const World& world) {
    jets_.clear();
    constexpr float kMargin = 900;
    const float dlo = 4.0f * view_left_ - kMargin, dhi = 4.0f * (view_left_ + width_) + kMargin;
    const float slo = 8.0f * view_top_ - kMargin, shi = 8.0f * (view_top_ + height_ + 256) + kMargin;
    world.for_each([&](const Item& item) {
        if (!is_steam_jet(item.shape) || item.off_map || item.parent != 0 || (item.flags & kItemContained)) return;
        const ShapeInfo* info = world.shape_info(item.shape);
        if (!info) return;
        const bool mirrored = item.flags & kItemFlipped;
        const float x = float(world_from_usecode(item.x)), y = float(world_from_usecode(item.y));
        const float dx = float((mirrored ? info->footpad_y : info->footpad_x) * kFootpadXY);
        const float dy = float((mirrored ? info->footpad_x : info->footpad_y) * kFootpadXY);
        if (x - (y - dy) < dlo || (x - dx) - y > dhi || x + y < slo || (x - dx) + (y - dy) > shi) return;
        const float z = float(item.z), top = float(item.z + info->footpad_z * kFootpadZ);
        jets_.push_back({{x - dx, y - dy, z * kLightZScale}, {x, y, top * kLightZScale}});
    });
}

// Whether `p` (lighting space) is by one of the game's jets of steam.
// @orig none: port lighting (smoke)
bool WorldLighting::by_jet(const Vec3& p) const {
    for (const auto& [lo, hi] : jets_) {
        if (p.x >= lo.x - kJetReach && p.x <= hi.x + kJetReach && p.y >= lo.y - kJetReach &&
            p.y <= hi.y + kJetReach && p.z >= lo.z - 32 * kLightZScale && p.z <= hi.z + 32 * kLightZScale)
            return true;
    }
    return false;
}

// The liquids' glow on what is around them: a green light high over (so that
// it falls on the floors past the rims) each cell of liquid, breathing with
// the glow. It does not light the liquids themselves (their glow is their
// own).
// @orig none: port lighting (liquids)
void WorldLighting::liquid_lights(std::vector<PointLight>& lights) const {
    struct Cell {
        float area = 0, x = 0, y = 0, z = 0;
    };
    std::map<std::pair<int, int>, Cell> cells;
    for (const Liquid& l : liquids_) {
        const float area = (l.hi.x - l.lo.x) * (l.hi.y - l.lo.y);
        const float cx = (l.lo.x + l.hi.x) * 0.5f, cy = (l.lo.y + l.hi.y) * 0.5f;
        Cell& c = cells[{static_cast<int>(std::floor(cx / kLiquidCell)), static_cast<int>(std::floor(cy / kLiquidCell))}];
        c.area += area;
        c.x += cx * area;
        c.y += cy * area;
        c.z += l.hi.z * area;
    }
    const float t = ticks_ / 60.0f;
    const float breathe = 0.85f + 0.15f * std::sin(t * 1.1f);
    for (const auto& [key, c] : cells) {
        const float share = std::min(1.0f, c.area / (kLiquidCell * kLiquidCell * 0.5f));
        if (share < 0.05f) continue;
        const Vec3 at{c.x / c.area, c.y / c.area, c.z / c.area + 160 * kLightZScale};
        PointLight light{at, kLiquidTint * (kLiquidLight * share * breathe), 1600, 0};
        light.liquid = true;
        light.dim = share;
        lights.push_back(light);
    }
}

// Thin steam over the liquids: puffs that need no memory, each a slot of a
// pool coming back every few seconds at a new place, so it is there as soon
// as the pool is in view and the same whenever it is seen.
// @orig none: port lighting (liquids)
void WorldLighting::steam(std::vector<PuffState>& out) const {
    const float t = ticks_ / 60.0f;
    std::size_t count = 0;
    for (const Liquid& l : liquids_) {
        if (!l.steams) continue;
        const float area = (l.hi.x - l.lo.x) * (l.hi.y - l.lo.y);
        const int slots = std::max(1, static_cast<int>(std::lround(area / (512.0f * 512.0f) * 3)));
        for (int k = 0; k < slots && count < kMaxSteam; ++k) {
            const std::uint32_t serial =
                hash32(static_cast<std::uint32_t>(static_cast<std::int32_t>(l.lo.x)) * 73856093u ^
                       static_cast<std::uint32_t>(static_cast<std::int32_t>(l.lo.y)) * 19349663u ^
                       static_cast<std::uint32_t>(k) * 83492791u);
            const float period = 7.0f + 4.0f * puff_random(serial, 1);
            const float phase = t / period + puff_random(serial, 2);
            const float cycle = std::floor(phase), u = phase - cycle;
            const std::uint32_t round = serial + static_cast<std::uint32_t>(cycle) * 0x9E3779B9u;
            const float age = u * period;
            PuffState s;
            const Vec3 drift{(puff_random(round, 5) * 2 - 1) * 10, (puff_random(round, 6) * 2 - 1) * 10, 5};
            s.centre = Vec3{l.lo.x + (l.hi.x - l.lo.x) * puff_random(round, 3),
                            l.lo.y + (l.hi.y - l.lo.y) * puff_random(round, 4), l.hi.z + 4 * kLightZScale} +
                       drift * age;
            s.radius = 60 + 90 * u + 30 * puff_random(round, 7);
            // Off screen: nothing to draw.
            const float x = s.centre.x, y = s.centre.y, z = s.centre.z / kLightZScale;
            const float sx = (x - y) * 0.25f - view_left_, sy = (x + y) * 0.125f - z - view_top_;
            const float r = s.radius * kPixelScale * 1.5f;
            if (sx < -r || sy < -r || sx > width_ + r || sy > height_ + r) continue;
            s.density = 0.006f * smoothstep(0.0f, 0.25f, u) * (1 - smoothstep(0.55f, 1.0f, u));
            s.heat = 0;
            s.grey = 0.7f;
            s.seed = puff_random(round, 8) * 100.0f;
            s.time = age;
            s.feature = 40;
            s.emit = 0.08f;
            if (s.density <= 1e-5f) continue;
            out.push_back(s);
            ++count;
        }
    }
}

// Smoke rising from the things in view that give some off (kSmoulders):
// the broken electric things, fires, glowing cores, leaks. As the steam,
// puffs that need no memory, each slot of an item coming back every few
// seconds, so that it smokes as soon as it is seen.
// Where a leak's hole is in `frame` of `shape` (frame coordinates): the
// middle of its opaque pixels that differ from `whole`, the same thing whole.
// @orig none: port lighting (smoke)
std::optional<std::pair<float, float>> WorldLighting::hole(std::uint16_t shape, std::uint32_t frame,
                                                           std::uint32_t whole) const {
    const std::uint32_t key = std::uint32_t{shape} << 16 | frame << 8 | whole;
    if (const auto it = holes_.find(key); it != holes_.end()) return it->second;
    std::optional<std::pair<float, float>> found;
    const Shape* s = shapes_.get(shape);
    if (s && frame < s->frames().size() && whole < s->frames().size()) {
        const ShapeFrame& a = s->frames()[frame];
        const ShapeFrame& b = s->frames()[whole];
        float sx = 0, sy = 0;
        int n = 0;
        for (int y = 0; y < a.height; ++y) {
            for (int x = 0; x < a.width; ++x) {
                const int c = a.pixel(x, y);
                if (c < 0) continue;
                const int bx = x - a.xoff + b.xoff, by = y - a.yoff + b.yoff;
                const bool inside = bx >= 0 && by >= 0 && bx < b.width && by < b.height;
                if (inside && b.pixel(bx, by) == c) continue;
                sx += float(x);
                sy += float(y);
                ++n;
            }
        }
        if (n > 0) found = std::pair{sx / n, sy / n};
    }
    holes_.emplace(key, found);
    return found;
}

// @orig none: port lighting (smoke)
void WorldLighting::smoulder(std::vector<PuffState>& out) const {
    const float t = ticks_ / 60.0f;
    std::size_t count = 0;
    for (const DrawItem& d : list_) {
        if (count >= kMaxSmoulder) break;
        const Item& item = *d.item;
        const Smoulder* kind = item.sprite_only ? nullptr : smoulders(item.shape, item.frame);
        if (!kind) continue;
        const Plume plume = kind->plume;
        // From the top of the thing, in the middle of its box. A fan's motor:
        // the middle of its face (the box's middle, on its near side along
        // its thin axis), a little out of it.
        Vec3 top{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, float(d.z_top) * kLightZScale};
        if (plume == Plume::kMotor) {
            top.z = (d.z + d.z_top) * 0.5f * kLightZScale;
            if (d.x - d.x_left < d.y - d.y_far) top.x = float(d.x) + 10;
            else top.y = float(d.y) + 10;
        }
        // A leak: out of the hole, at the box's middle depth (as a lamp's
        // bulb), just in front of the pipe, blown out towards the viewer.
        Vec3 blow{};
        if (kind->mended && item.frame >= kind->mended) {
            if (const auto at = hole(item.shape, item.frame, item.frame - kind->mended)) {
                const float u = float(d.mirrored ? d.left + (d.frame->width - 1) - at->first : d.left + at->first) -
                                float(d.sx);
                const float v = float(d.top) + at->second - float(d.sy);
                const float depth = (d.x_left - d.x + d.y_far - d.y) * 0.5f;
                top = Vec3{d.x + 2 * u + depth * 0.5f, d.y - 2 * u + depth * 0.5f, (d.z - v + depth / 8) * kLightZScale};
                // The game blows its own steam out of this hole, or will.
                if (by_jet(top)) continue;
                top = top + kToViewer * 8.0f;
                blow = Vec3{kToViewer.x, kToViewer.y, 0} * 30.0f;
            }
        }
        // Its slots share a period, evenly spaced: a steady thread.
        int slots = 6;
        float period = 2.6f;
        switch (plume) {
        case Plume::kWisp: break;
        case Plume::kMotor: slots = 8, period = 3.4f; break;
        case Plume::kFire: slots = 8, period = 2.2f; break;
        case Plume::kHeat: slots = 5, period = 6.0f; break;
        case Plume::kSteam: slots = 6, period = 1.4f; break;
        }
        const std::uint32_t own = hash32(item.id * 2654435761u);
        period += 0.8f * puff_random(own, 1);
        for (int k = 0; k < slots && count < kMaxSmoulder; ++k) {
            const std::uint32_t serial = hash32(own ^ static_cast<std::uint32_t>(k) * 83492791u);
            const float phase = t / period + puff_random(own, 2) + (k + 0.4f * puff_random(serial, 2)) / slots;
            const float cycle = std::floor(phase), u = phase - cycle;
            const std::uint32_t round = serial + static_cast<std::uint32_t>(cycle) * 0x9E3779B9u;
            const float age = u * period;
            PuffState s;
            const Vec3 drift{(puff_random(round, 5) * 2 - 1) * 6, (puff_random(round, 6) * 2 - 1) * 6, 0};
            s.feature = 9;
            s.heat = 0;
            switch (plume) {
            case Plume::kWisp:
                s.centre = top + drift * age + Vec3{0, 0, 4 + 30 * age};
                s.radius = 5 + 20 * u;
                s.density = 0.02f * smoothstep(0.0f, 0.15f, u) * (1 - smoothstep(0.4f, 1.0f, u));
                s.grey = 0.5f;
                break;
            case Plume::kMotor:
                // Thick and dark, pushed out of the face, then rising.
                s.centre = top + drift * age + Vec3{0, 0, 2 + 48 * age};
                s.radius = 12 + 48 * u;
                s.density = 0.04f * smoothstep(0.0f, 0.1f, u) * (1 - smoothstep(0.4f, 1.0f, u));
                s.grey = 0.2f;
                break;
            case Plume::kFire:
                // Out of the flames' top, thick and dark, rising fast and
                // spreading; hot at first.
                s.centre = top + drift * (age * 1.5f) + Vec3{0, 0, 70 * age};
                s.radius = 14 + 56 * u;
                s.density = 0.035f * smoothstep(0.0f, 0.1f, u) * (1 - smoothstep(0.35f, 1.0f, u));
                s.grey = 0.15f;
                s.heat = 0.35f * (1 - smoothstep(0.0f, 0.25f, u));
                s.feature = 14;
                break;
            case Plume::kHeat: {
                // Anywhere over the surface, wide, faint and slow.
                const Vec3 at{d.x_left + (d.x - d.x_left) * puff_random(round, 3),
                              d.y_far + (d.y - d.y_far) * puff_random(round, 4), top.z};
                s.centre = at + drift * age + Vec3{0, 0, 4 + 22 * age};
                s.radius = 30 + 80 * u;
                s.density = 0.007f * smoothstep(0.0f, 0.25f, u) * (1 - smoothstep(0.5f, 1.0f, u));
                s.grey = 0.6f;
                s.feature = 30;
                break;
            }
            case Plume::kSteam:
                // White, blown out and quickly gone.
                s.centre = top + (drift * 2.0f + blow) * age + Vec3{0, 0, 2 + 40 * age};
                s.radius = 6 + 36 * u;
                s.density = 0.06f * smoothstep(0.0f, 0.08f, u) * (1 - smoothstep(0.3f, 1.0f, u));
                s.grey = 0.85f;
                s.feature = 7;
                break;
            }
            if (s.density <= 1e-5f) continue;
            s.seed = puff_random(round, 8) * 100.0f;
            s.time = age;
            out.push_back(s);
            ++count;
        }
    }
}

// The puffs now, far to near.
// @orig none: port lighting (smoke)
void WorldLighting::puff_states(std::vector<PuffState>& out) const {
    out.clear();
    steam(out);
    smoulder(out);
    for (const SmokePuff& p : puffs_) {
        const float age = (ticks_ - p.born) / 60.0f;
        const float u = std::clamp(age / p.life, 0.0f, 1.0f);
        PuffState s;
        // The drift slows down (drag), the rise keeps up.
        const float travelled = p.drag > 0 ? (1 - std::exp(-p.drag * age)) / p.drag : age;
        s.centre = p.pos + p.drift * travelled + Vec3{0, 0, p.rise * age};
        s.radius = p.radius0 + (p.radius1 - p.radius0) * (1 - (1 - u) * (1 - u));
        // Fades in (at once for a muzzle wisp), thins out to nothing.
        const float fade_in = p.kind == SmokePuff::kMuzzle ? 0.02f : 0.1f;
        s.density = p.density * std::min(1.0f, age / fade_in) * std::pow(1 - u, 1.5f);
        s.heat = p.kind == SmokePuff::kExplosion ? std::max(0.0f, 1 - age / 0.9f) : 0.0f;
        s.grey = p.kind == SmokePuff::kExplosion ? 0.3f
                 : p.kind == SmokePuff::kImpact  ? 0.55f
                                                 : 0.75f;
        s.seed = p.seed;
        s.time = age;
        s.feature = p.feature;
        if (s.density > 1e-5f) out.push_back(s);
    }
    std::sort(out.begin(), out.end(),
              [](const PuffState& a, const PuffState& b) { return dot(a.centre, kToViewer) < dot(b.centre, kToViewer); });
}

// @orig none: port lighting
void WorldLighting::shot_fired(ObjId shooter, std::int32_t x, std::int32_t y, std::int32_t z, int dir, int fire_type,
                               std::uint32_t ticks) {
    const auto kind = muzzle_flash(fire_type);
    if (!kind) return;
    // A little ahead of the muzzle (direction 0 towards -y, 4 towards +x),
    // so that the wall the shooter faces catches it as much as the shooter.
    const float a = static_cast<float>(dir & 15) * 0.39269908f;
    const Vec3 ahead{std::sin(a), -std::cos(a), 0};
    const Vec3 at = Vec3{float(world_from_usecode(x)), float(world_from_usecode(y)), float(z) * kLightZScale} +
                    ahead * 10.0f;
    // Each flash a little brighter or dimmer than the last.
    const float flicker = 0.85f + 0.3f * float(hash32(ticks * 2654435761u + shooter) & 0xFF) / 255.0f;
    flashes_.push_back({at, kind->colour * (kind->strength * flicker), kind->radius, ticks, kind->life, shooter});
}

// @orig none: port lighting
void WorldLighting::gather_lights(const World& world, const LightingOptions& opt, std::vector<PointLight>& lights) {
    for (const DrawItem& d : list_) {
        const Decoded& f = decoded(d.frame);
        if (d.item->sprite_only) {
            if (!opt.flash_lights) continue;
            if (is_rocket(*d.item)) {
                // A rocket's exhaust: an orange light that flickers, where it is.
                const float flicker = 0.8f + 0.2f * std::sin(ticks_ * 1.9f + d.item->id * 0.7f) *
                                                 std::sin(ticks_ * 0.83f + d.item->id);
                lights.push_back({{float(d.x), float(d.y), d.z * kLightZScale},
                                  Vec3{1.0f, 0.55f, 0.2f} * (1.6f * flicker), 520, d.item->id});
                continue;
            }
            // Flashes, explosions, shots: their bright pixels.
            int count = 0;
            Vec3 sum;
            for (const auto& [c, k] : f.histogram) {
                if (lum_[c] < 0.62f) continue;
                count += k;
                sum = sum + lin_[c] * float(k);
            }
            if (count < 4) continue;
            Vec3 col = sum * (1.0f / count);
            col = col * (1.0f / std::max({col.x, col.y, col.z, 0.05f}));
            // Shots and impacts keep their place: the light is where they
            // are. Explosions and flashes are moved up over the scene
            // (sprite_process.hpp): their light goes over the surface drawn
            // under their anchor.
            Vec3 at{float(d.x), float(d.y), d.z * kLightZScale};
            if (is_blast_shape(d.item->shape)) {
                std::optional<Vec3> under;
                for (int dy = 0; dy < 24 && !under; dy += 4) under = surface_at(d.sx, d.sy - dy);
                at = under ? *under + Vec3{0, 0, 24 * kLightZScale}
                           : Vec3{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, float(d.z + 24) * kLightZScale};
            }
            float strength = std::min(2.2f, 0.7f + count / 120.0f);
            float radius = std::min(900.0f, 300.0f + count * 1.5f);
            if (d.item->shape == kBulletImpactShape) {
                strength *= 0.4f;
                radius *= 0.6f;
            }
            lights.push_back({at, col * strength, radius, d.item->id});
            continue;
        }
        if (opt.fixture_lights) {
            if (const Fixture* fx = fixture(d.item->shape, d.item->frame)) {
                // Screen offset (u, v) from the item's anchor, at the depth of
                // the box's middle: x - y = 4 sx, x + y the depth, z = depth / 8 - sy.
                float u = 0, v = 0;
                int bright = 0;
                if (fx->bulb) {
                    float su = 0, sv = 0;
                    for (int y = 0; y < f.h; ++y) {
                        for (int x = 0; x < f.w; ++x) {
                            const int c = f.color[y * f.w + x];
                            if (c < 0 || (fx->pulse ? !glowing(c) : lum_[c] < 0.85f)) continue;
                            su += float(d.mirrored ? f.w - 1 - x : x);
                            sv += float(y);
                            ++bright;
                        }
                    }
                    if (bright > 0) {
                        u = float(d.left) + su / bright - float(d.sx);
                        v = float(d.top) + sv / bright - float(d.sy);
                    }
                }
                Vec3 at, dir;
                if (fx->face) {
                    // Mirrored, the glass shows on the other face.
                    const bool on_x = (fx->face == 1) != d.mirrored;
                    const float z = (d.z + (d.z_top - d.z) * kFaceHeight) * kLightZScale;
                    at = on_x ? Vec3{d.x + kFaceOut, (d.y + d.y_far) * 0.5f, z}
                              : Vec3{(d.x + d.x_left) * 0.5f, d.y + kFaceOut, z};
                    dir = on_x ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
                } else if (bright > 0) {
                    const float depth = (d.x_left - d.x + d.y_far - d.y) * 0.5f;
                    at = Vec3{d.x + 2 * u + depth * 0.5f, d.y - 2 * u + depth * 0.5f,
                              (d.z - v + depth / 8) * kLightZScale} +
                         kToViewer * 12.0f;
                } else {
                    at = {(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, (d.z + d.z_top) * 0.5f * kLightZScale};
                    at = at + kToViewer * 12.0f;
                }
                float strength = fx->strength * fixture_boost(opt.ambient);
                if (fx->levels) strength *= fx->levels[d.item->frame];  // lit frames only: 0..2
                if (fx->flicker != Fixture::kSteady) strength *= flicker_at(*fx, d.item->id, ticks_);
                if (fx->pulse) {
                    float lit = 0;
                    for (const auto& [c, k] : f.histogram)
                        if (glowing(c)) lit += std::max({lin_[c].x, lin_[c].y, lin_[c].z}) * float(k);
                    strength *= f.glow_pixels > 0 ? lit / float(f.glow_pixels) : 1.0f;
                }
                PointLight light{at, fx->colour * strength, fx->radius, d.item->id};
                light.halo = fx->halo;
                light.halo_gain = fx->halo_gain;
                if (fx->face) {
                    light.dir = dir;
                    light.cos_inner = kFaceCosInner;
                    light.cos_outer = kFaceCosOuter;
                }
                lights.push_back(light);
                continue;  // in place of the light of its colour-cycled pixels (the alarms')
            }
        }
        if (!opt.glow_lights || f.glow_pixels < 6) continue;
        Vec3 sum;
        for (const auto& [c, k] : f.histogram)
            if (glowing(c)) sum = sum + lin_[c] * float(k);
        Vec3 col = sum * (1.0f / f.glow_pixels);
        col = col * (1.0f / std::max({col.x, col.y, col.z, 0.05f}));
        const float strength = std::min(1.2f, 0.35f + f.glow_pixels / 80.0f);
        const Vec3 centre{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, (d.z_top + 4.0f) * kLightZScale};
        lights.push_back({centre, col * strength, 260.0f + std::min(f.glow_pixels, 200) * 2.0f, d.item->id});
    }
    if (opt.flash_lights) {
        // Muzzle flashes: full on the tick of the shot, then fading out.
        for (const MuzzleFlash& f : flashes_) {
            const float age = float(ticks_ - f.born) / float(f.life);
            if (age >= 1) continue;
            const float k = (1 - age) * (1 - age);
            lights.push_back({f.pos, f.colour * k, f.radius * (0.6f + 0.4f * k), f.owner});
        }
    }
    if (opt.player_lamp) {
        // The Silencer's lamp: a beam from his chest the way he faces
        // (direction 0..15, 0 towards -y, 4 towards +x), tipped down so that
        // it lights the floor ahead and what stands there.
        const Item* p = world.get(1);
        if (p && !p->off_map && !p->parent) {
            const ShapeInfo* info = world.shape_info(p->shape);
            const float x = float(world_from_usecode(p->x)), y = float(world_from_usecode(p->y));
            const float half = info ? info->footpad_x * kFootpadXY * 0.5f : 32.0f;
            const float height = float(info ? info->footpad_z * kFootpadZ : 40);
            const float a = static_cast<float>(world.npc_direction(1) & 15) * 0.39269908f;
            const Vec3 ahead{std::sin(a), -std::cos(a), 0};
            const Vec3 at = Vec3{x - half, y - half, (p->z + height * 0.9f) * kLightZScale} + ahead * (half + 8);
            PointLight lamp{at, Vec3{1.0f, 0.95f, 0.85f} * 4.5f, 1600, p->id};
            lamp.dir = normalize(ahead + Vec3{0, 0, -0.55f});
            lamp.cos_inner = 0.92f;   // 23 degrees
            lamp.cos_outer = 0.77f;   // 40 degrees
            lights.push_back(lamp);
        }
    }
    if (opt.liquids) liquid_lights(lights);
}

// Boxes that can shadow the lit area.
// @orig none: port lighting
void WorldLighting::gather_occluders(const World& world) {
    boxes_.clear();
    per_light_.assign(lights_.size(), {});
    if (lights_.empty()) return;
    float lox = 1e9f, loy = 1e9f, hix = -1e9f, hiy = -1e9f;
    for (const PointLight& l : lights_) {
        lox = std::min(lox, l.pos.x - l.radius);
        loy = std::min(loy, l.pos.y - l.radius);
        hix = std::max(hix, l.pos.x + l.radius);
        hiy = std::max(hiy, l.pos.y + l.radius);
    }
    world.for_each([&](const Item& item) {
        if (item.off_map || item.parent != 0 || item.sprite_only ||
            (item.flags & (kItemInvisible | kItemContained | kItemEthereal)))
            return;
        const ShapeInfo* info = world.shape_info(item.shape);
        if (!info || info->is(ShapeInfo::kEditor) || info->is(ShapeInfo::kTranslucent)) return;
        // Lasers, electrified floors: light, not matter.
        if (info->is(ShapeInfo::kDamaging)) return;
        // Flat items (floors, grates, mats: one z unit of footpad) cast none:
        // a low light would see the floor tiles shade one another.
        if (!info->footpad_x || !info->footpad_y || info->footpad_z <= 1) return;
        const bool mirrored = item.flags & kItemFlipped;
        const std::int32_t wx = world_from_usecode(item.x), wy = world_from_usecode(item.y);
        const float x = float(wx), y = float(wy);
        const float dx = float((mirrored ? info->footpad_y : info->footpad_x) * kFootpadXY);
        const float dy = float((mirrored ? info->footpad_x : info->footpad_y) * kFootpadXY);
        if (x < lox || x - dx > hix || y < loy || y - dy > hiy) return;
        Box b{{x - dx, y - dy, item.z * kLightZScale},
              {x, y, (item.z + info->footpad_z * kFootpadZ) * kLightZScale},
              item.id};
        if (const Shape* shape = shapes_.get(item.shape); shape && item.frame < shape->frames().size()) {
            const ShapeFrame& frame = shape->frames()[item.frame];
            const int sx = (wx - wy) >> 2, sy = ((wx + wy) >> 3) - item.z;
            b.sprite = &decode(frame);
            b.mirrored = mirrored;
            b.left = mirrored ? sx + frame.xoff - (frame.width - 1) : sx - frame.xoff;
            b.top = sy - frame.yoff;
        }
        boxes_.push_back(b);
    });
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        for (std::uint32_t i = 0; i < boxes_.size(); ++i) {
            const Box& b = boxes_[i];
            if (b.hi.x < L.pos.x - L.radius || b.lo.x > L.pos.x + L.radius || b.hi.y < L.pos.y - L.radius ||
                b.lo.y > L.pos.y + L.radius || b.hi.z < L.pos.z - L.radius || b.lo.z > L.pos.z + L.radius)
                continue;
            per_light_[l].push_back(i);
        }
    }
}

// Whether a box hides light `l` from point p: the segment from just off the
// surface to the light crosses an opaque part of it.
// @orig none: port lighting
bool WorldLighting::occluded(const Vec3& p, const Vec3& n, ObjId self, std::size_t light, bool sprite_cut,
                             const std::vector<std::uint32_t>& boxes) const {
    const PointLight& L = lights_[light];
    const Vec3 o = p + n * 3.0f;
    const Vec3 dir = L.pos - o;
    const float os[3] = {o.x, o.y, o.z};
    const float is[3] = {1 / (std::abs(dir.x) > 1e-6f ? dir.x : 1e-6f), 1 / (std::abs(dir.y) > 1e-6f ? dir.y : 1e-6f),
                         1 / (std::abs(dir.z) > 1e-6f ? dir.z : 1e-6f)};
    for (std::uint32_t bi : boxes) {
        const Box& b = boxes_[bi];
        if (b.owner == self || b.owner == L.owner) continue;
        // Ends excluded: a light or a surface touching a box is not hidden by it.
        float t0 = 1e-3f, t1 = 0.98f;
        const float lo[3] = {b.lo.x, b.lo.y, b.lo.z}, hi[3] = {b.hi.x, b.hi.y, b.hi.z};
        bool inside = true;
        for (int a = 0; a < 3 && inside; ++a) {
            float ta = (lo[a] - os[a]) * is[a], tb = (hi[a] - os[a]) * is[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            inside = t0 <= t1;
        }
        if (!inside) continue;
        if (!sprite_cut || !b.sprite) return true;
        const Decoded& f = *b.sprite;
        constexpr int kSamples = 5;
        for (int k = 0; k < kSamples; ++k) {
            const float t = t0 + (t1 - t0) * (k + 0.5f) / kSamples;
            const Vec3 q = o + dir * t;
            const int sx = static_cast<int>(std::floor((q.x - q.y) * 0.25f));
            const int sy = static_cast<int>(std::floor((q.x + q.y) * 0.125f - q.z / kLightZScale));
            const int fx = b.mirrored ? b.left + f.w - 1 - sx : sx - b.left;
            const int fy = sy - b.top;
            if (fx < 0 || fy < 0 || fx >= f.w || fy >= f.h) continue;
            if (f.color[fy * f.w + fx] >= 0) return true;
        }
    }
    return false;
}

// Linear colour of palette entry c at a point lit by the lights `ids`, with
// `boxes` (when given) the occluders of each, in the same order.
// @orig none: port lighting
Vec3 WorldLighting::shade_point(int c, const Vec3& p, const Vec3& n, ObjId self, const LightingOptions& opt,
                                const std::vector<std::uint32_t>& ids,
                                const std::vector<std::vector<std::uint32_t>>* boxes, bool liquid) const {
    // The liquid itself is the light: its ambient stays.
    const float a = opt.ambient * (liquid ? 1.0f : liquid_dim(lights_, &ids, p));
    Vec3 light{a, a * 1.02f, a * 1.12f};
    for (std::size_t k = 0; k < ids.size(); ++k) {
        const PointLight& L = lights_[ids[k]];
        if (liquid && L.liquid) continue;
        const Vec3 to = L.pos - p;
        const float d2 = dot(to, to);
        if (d2 >= L.radius * L.radius) continue;
        const float dist = std::max(std::sqrt(d2), 1e-3f);
        const float ndl = dot(n, to) / dist;
        if (ndl <= 0) continue;
        const float cone = spot(L, to, dist);
        if (cone <= 0) continue;
        const float f = 1 - d2 / (L.radius * L.radius);
        const float w = ndl * f * f * cone;
        if (opt.shadows &&
            occluded(p, n, self, ids[k], opt.sprite_shadows, boxes ? (*boxes)[k] : per_light_[ids[k]]))
            continue;
        light = light + L.color * w;
    }
    Vec3 col = mul(lin_[c], light);
    if (glowing(c)) col = col + lin_[c];
    if (liquid) col = col + mul(lin_[c], kLiquidTint) * (kLiquidGlow * liquid_glow(p, ticks_ / 60.0f));
    return col;
}

// The glow of the lamps `ids` (PointLight::halo) over a pixel of linear
// colour `colour`: a soft disc round the light where the pixel's view ray
// passes by it. With `clip`, only when the light is in front of the pixel's
// surface (s = x + y of its point) or the pixel is the lamp's own (`self`).
// A character's surface is a flat billboard through the middle of its box,
// where the ray passes closest to the light shifts with the pixel's row, and
// a halo cut there would cross the character in a straight line: over a
// `billboard`, the glow behind fades with how far behind it is, in halo radii.
// @orig none: port lighting (halo)
Vec3 WorldLighting::halo_over(Vec3 colour, int px, int py, bool clip, float surface_s, ObjId self, bool billboard,
                              const std::vector<std::uint32_t>& ids) const {
    const float sx = view_left_ + px + 0.5f, sy = view_top_ + py + 0.5f;
    const Vec3 a{2 * sx, -2 * sx, -sy * kLightZScale};
    const float bb = dot(kRayStep, kRayStep);
    for (std::uint32_t id : ids) {
        const PointLight& L = lights_[id];
        const Vec3 d = a - L.pos;
        const float mid = -dot(kRayStep, d) / bb;  // where the ray passes closest to the light
        float behind = 1;
        if (clip && L.owner != self && mid < surface_s - kHaloDepth) {
            if (!billboard) continue;
            const float t = (surface_s - kHaloDepth - mid) * kRayStepLength / L.halo;
            behind = std::exp(-t * t);
        }
        const Vec3 e = d + kRayStep * mid;
        const float k = dot(e, e) / (L.halo * L.halo);
        if (k >= 9) continue;
        const float edge = 1 - k / 9;  // down to nothing at 3 radii, without a ring on black
        colour = colour + L.color * (kHaloGain * L.halo_gain * std::exp(-k) * edge * edge * behind);
    }
    return colour;
}

// The puffs `ids` (far to near) over a pixel of linear colour `colour`;
// with `clip`, only in front of its surface (s = x + y of its point).
// @orig none: port lighting (smoke)
Vec3 WorldLighting::smoke_over(Vec3 colour, int px, int py, bool clip, float surface_s, const LightingOptions& opt,
                               const std::vector<std::uint32_t>& ids) const {
    const float sx = view_left_ + px + 0.5f, sy = view_top_ + py + 0.5f;
    const Vec3 a{2 * sx, -2 * sx, -sy * kLightZScale};
    const float bb = dot(kRayStep, kRayStep);
    for (std::uint32_t id : ids) {
        const PuffState& puff = puff_now_[id];
        // The ray's closest point to the centre, then the half chord (no
        // b^2 - 4ac: these numbers are far too big for floats).
        const Vec3 d = a - puff.centre;
        const float mid = -dot(kRayStep, d) / bb;
        const Vec3 e = d + kRayStep * mid;
        const float perp2 = dot(e, e), r2 = puff.radius * puff.radius;
        if (perp2 >= r2) continue;
        const float half = std::sqrt((r2 - perp2) / bb);
        float s0 = mid - half;
        const float s1 = mid + half;
        if (clip) s0 = std::max(s0, surface_s);
        if (s1 <= s0) continue;
        auto light_at = [&](const Vec3& p) {
            const float a = opt.ambient * liquid_dim(lights_, nullptr, p);
            Vec3 light{a, a * 1.02f, a * 1.12f};
            for (const PointLight& L : lights_) {
                const Vec3 to = L.pos - p;
                const float d2 = dot(to, to);
                if (d2 >= L.radius * L.radius) continue;
                const float f = 1 - d2 / (L.radius * L.radius);
                light = light + L.color * (f * f * 0.8f * spot(L, to, std::sqrt(d2)));
            }
            return light;
        };
        // A liquid's steam is thin and small: lit as at its centre.
        const bool steam = puff.emit > 0;
        const Vec3 centre_light = steam ? light_at(puff.centre) : Vec3{};
        const int steps = steam ? kSmokeSteps / 2 : kSmokeSteps;
        const float ds = (s1 - s0) / steps;
        const float len = ds * kRayStepLength;
        // Far to near.
        for (int i = 0; i < steps; ++i) {
            const Vec3 p = a + kRayStep * (s0 + (i + 0.5f) * ds);
            const float dist = std::sqrt(dot(p - puff.centre, p - puff.centre)) / puff.radius;
            const float shape = 1 - smoothstep(0.35f, 1.0f, dist);
            if (shape <= 0) continue;
            const float noise =
                std::clamp((smoke_noise(p, puff.seed, puff.time, puff.feature) - 0.3f) * 1.8f, 0.0f, 1.0f);
            const float sigma = puff.density * shape * noise;
            if (sigma <= 0) continue;
            const float keep = std::exp(-sigma * len);
            const Vec3 light = steam ? centre_light : light_at(p);
            const Vec3 glow = Vec3{1.0f, 0.45f, 0.12f} * (puff.heat * puff.heat * 2.5f * shape) +
                              kLiquidTint * (puff.emit * shape);
            const Vec3 radiance = light * puff.grey + glow;
            colour = colour * keep + radiance * (1 - keep);
        }
    }
    return colour;
}

// The palette shown (fades, colour cycling): linear colours and luminance.
// @orig none: port lighting
void WorldLighting::set_palette(const Palette& palette) {
    for (int i = 0; i < 256; ++i) {
        const Rgb& c = palette.colors[i];
        lin_[i] = {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)};
        lum_[i] = std::sqrt(lin_[i].x * 0.3f + lin_[i].y * 0.59f + lin_[i].z * 0.11f);
        flat_[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
    }
}

std::uint32_t WorldLighting::pack(const Vec3& c) const {
    auto q = [&](float v) { return std::uint32_t{out_lut_[static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 4095)]}; };
    return 0xFF000000u | q(c.x) << 16 | q(c.y) << 8 | q(c.z);
}

// @orig none: port lighting
bool WorldLighting::shade(const World& world, const Surface8& screen, const Palette& palette,
                          const LightingOptions& opt, std::vector<PointLight> extra, std::uint32_t* argb) {
    if (!captured() || screen.width != width_ || screen.height != height_) return false;
    set_palette(palette);
    lights_ = std::move(extra);
    gather_lights(world, opt, lights_);
    gather_occluders(world);
    if (opt.smoke) puff_states(puff_now_);
    else puff_now_.clear();
    used_gpu_ = gpu_ && !opt.show_normals && shade_gpu(screen, opt, argb);
    if (!used_gpu_) shade_cpu(screen, opt, argb);
    lit_.assign(argb, argb + static_cast<std::size_t>(width_) * height_);
    return true;
}

// @orig none: port lighting
void WorldLighting::shade_cpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb) {
    geometry(opt);
    // The puffs' screen circles (absolute screen coordinates).
    struct Disc {
        float x, y, r;
    };
    std::vector<Disc> discs;
    for (const PuffState& p : puff_now_) {
        const float x = p.centre.x, y = p.centre.y, z = p.centre.z / kLightZScale;
        discs.push_back({(x - y) * 0.25f, (x + y) * 0.125f - z, p.radius * kPixelScale});
    }
    // The lamps' halos, by light (radius 0: none); exp(-9) at 3 halo radii.
    std::vector<Disc> halo_discs;
    for (const PointLight& L : lights_) {
        const float x = L.pos.x, y = L.pos.y, z = L.pos.z / kLightZScale;
        halo_discs.push_back({(x - y) * 0.25f, (x + y) * 0.125f - z, L.halo * 3 * kPixelScale});
    }
    const int tiles_x = (width_ + kTile - 1) / kTile, tiles_y = (height_ + kTile - 1) / kTile;
    pool_->run(static_cast<std::size_t>(tiles_x) * tiles_y, [&](std::size_t t) {
        const int x0 = static_cast<int>(t % tiles_x) * kTile, y0 = static_cast<int>(t / tiles_x) * kTile;
        const int x1 = std::min(x0 + kTile, width_), y1 = std::min(y0 + kTile, height_);
        // The tile's lit points: their box, the lights reaching it, and for
        // each the occluders between.
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                if (owner_[j] < 0) continue;
                const Vec3& p = pos_[j];
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        std::vector<std::uint32_t> ids;
        std::vector<std::vector<std::uint32_t>> boxes;
        if (lo.x <= hi.x) {
            for (std::uint32_t l = 0; l < lights_.size(); ++l) {
                const PointLight& L = lights_[l];
                const float dx = std::max({lo.x - L.pos.x, 0.0f, L.pos.x - hi.x});
                const float dy = std::max({lo.y - L.pos.y, 0.0f, L.pos.y - hi.y});
                const float dz = std::max({lo.z - L.pos.z, 0.0f, L.pos.z - hi.z});
                if (dx * dx + dy * dy + dz * dz >= L.radius * L.radius) continue;
                ids.push_back(l);
                auto& list = boxes.emplace_back();
                if (!opt.shadows) continue;
                const Vec3 a{std::min(lo.x, L.pos.x) - 4, std::min(lo.y, L.pos.y) - 4, std::min(lo.z, L.pos.z) - 4};
                const Vec3 b{std::max(hi.x, L.pos.x) + 4, std::max(hi.y, L.pos.y) + 4, std::max(hi.z, L.pos.z) + 4};
                for (std::uint32_t bi : per_light_[l]) {
                    const Box& box = boxes_[bi];
                    if (box.hi.x < a.x || box.lo.x > b.x || box.hi.y < a.y || box.lo.y > b.y || box.hi.z < a.z ||
                        box.lo.z > b.z)
                        continue;
                    list.push_back(bi);
                }
            }
        }
        // The puffs over the tile.
        std::vector<std::uint32_t> puffs;
        const float tx0 = float(view_left_ + x0), ty0 = float(view_top_ + y0);
        const float tx1 = float(view_left_ + x1), ty1 = float(view_top_ + y1);
        for (std::uint32_t i = 0; i < discs.size(); ++i) {
            const Disc& d = discs[i];
            const float cx = std::clamp(d.x, tx0, tx1), cy = std::clamp(d.y, ty0, ty1);
            if ((cx - d.x) * (cx - d.x) + (cy - d.y) * (cy - d.y) < d.r * d.r) puffs.push_back(i);
        }
        // The lamps' halos over the tile.
        std::vector<std::uint32_t> halos;
        for (std::uint32_t i = 0; i < halo_discs.size(); ++i) {
            const Disc& d = halo_discs[i];
            if (d.r <= 0) continue;
            const float cx = std::clamp(d.x, tx0, tx1), cy = std::clamp(d.y, ty0, ty1);
            if ((cx - d.x) * (cx - d.x) + (cy - d.y) * (cy - d.y) < d.r * d.r) halos.push_back(i);
        }
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                const int c = screen.pixels[j];
                if (c != world_[j]) {
                    // Drawn over the world (status bar, cursor, menus): as it is.
                    argb[j] = flat_[c];
                    continue;
                }
                const bool lit = owner_[j] >= 0 && !emissive_[j];
                if (opt.show_normals && lit) {
                    const Vec3& n = nrm_[j];
                    argb[j] = 0xFF000000u | std::uint32_t((n.x * 0.5f + 0.5f) * 255) << 16 |
                              std::uint32_t((n.y * 0.5f + 0.5f) * 255) << 8 | std::uint32_t((n.z * 0.5f + 0.5f) * 255);
                    continue;
                }
                Vec3 col;
                if (emissive_[j]) col = lin_[c];  // a flash: as it is
                else if (!lit) col = lin_[c] * opt.ambient;
                else
                    col = shade_point(c, pos_[j], nrm_[j], list_[owner_[j]].item->id, opt, ids, &boxes,
                                      liquid_[owner_[j]] != 0);
                if (!halos.empty())
                    col = halo_over(col, x, y, lit, lit ? pos_[j].x + pos_[j].y : 0.0f,
                                    owner_[j] >= 0 ? list_[owner_[j]].item->id : 0,
                                    owner_[j] >= 0 && actor_[owner_[j]], halos);
                if (!puffs.empty())
                    col = smoke_over(col, x, y, lit, lit ? pos_[j].x + pos_[j].y : 0.0f, opt, puffs);
                argb[j] = pack(col);
            }
    });
}

// Where a decoded frame is in the GPU's atlas, added (to upload) if new.
// @orig none: port lighting (GPU)
void WorldLighting::atlas_place(const Decoded& d, LitFrame& frame) {
    if (atlas_.count(&d)) return;
    constexpr int kSize = LitFrame::kAtlasSize;
    if (d.w > kSize || d.h > kSize || d.w <= 0 || d.h <= 0) return;
    if (shelf_x_ + d.w > kSize) {
        shelf_y_ += shelf_h_;
        shelf_x_ = 0;
        shelf_h_ = 0;
    }
    if (shelf_y_ + d.h > kSize) {
        atlas_full_ = true;
        return;
    }
    atlas_[&d] = {shelf_x_, shelf_y_};
    LitFrame::Upload up{shelf_x_, shelf_y_, d.w, d.h,
                        std::vector<std::uint8_t>(static_cast<std::size_t>(d.w) * d.h * 4)};
    for (int i = 0; i < d.w * d.h; ++i) {
        up.rgba[i * 4] = d.color[i] >= 0 ? 255 : 0;
        up.rgba[i * 4 + 1] = static_cast<std::uint8_t>(d.nx[i] + 128);
        up.rgba[i * 4 + 2] = static_cast<std::uint8_t>(d.ny[i] + 128);
        up.rgba[i * 4 + 3] = 255;
    }
    frame.uploads.push_back(std::move(up));
    shelf_x_ += d.w;
    shelf_h_ = std::max(shelf_h_, d.h);
}

// The frame for the GPU: per pixel codes, items, lights, occluders, puffs.
// @orig none: port lighting (GPU)
bool WorldLighting::shade_gpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb) {
    if (list_.size() >= LitFrame::kMaxItems) return false;
    LitFrame& f = frame_;
    for (int attempt = 0; attempt < 2; ++attempt) {
        f.uploads.clear();
        f.atlas_reset = atlas_reset_;
        atlas_full_ = false;
        if (atlas_reset_) {
            atlas_.clear();
            shelf_x_ = shelf_y_ = shelf_h_ = 0;
            atlas_reset_ = false;
        }
        f.items.assign(list_.size() * 16, 0.0f);
        for (std::size_t i = 0; i < list_.size(); ++i) {
            const DrawItem& d = list_[i];
            const Decoded& dec = decoded(d.frame);
            atlas_place(dec, f);
            const auto it = atlas_.find(&dec);
            const std::pair<int, int> at = it != atlas_.end() ? it->second : std::pair{0, 0};
            float* o = &f.items[i * 16];
            o[0] = float(d.x_left), o[1] = float(d.x), o[2] = float(d.y_far), o[3] = float(d.y);
            o[4] = float(d.z), o[5] = float(d.z_top);
            o[6] = float((actor_[i] ? LitFrame::kActor : 0) | (d.mirrored ? LitFrame::kMirrored : 0) |
                         (liquid_[i] ? LitFrame::kLiquid : 0));
            o[7] = float(d.item->id);
            o[8] = float(at.first), o[9] = float(at.second), o[10] = float(dec.w), o[11] = float(dec.h);
            o[12] = float(view_left_ + d.left), o[13] = float(view_top_ + d.top);
        }
        f.boxes.assign(boxes_.size() * 16, 0.0f);
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            const Box& b = boxes_[i];
            float* o = &f.boxes[i * 16];
            o[0] = b.lo.x, o[1] = b.lo.y, o[2] = b.lo.z, o[3] = float(b.owner);
            o[4] = b.hi.x, o[5] = b.hi.y, o[6] = b.hi.z, o[7] = b.mirrored ? 1.0f : 0.0f;
            if (!b.sprite) continue;
            atlas_place(*b.sprite, f);
            if (const auto it = atlas_.find(b.sprite); it != atlas_.end()) {
                o[8] = float(it->second.first), o[9] = float(it->second.second);
                o[10] = float(b.sprite->w), o[11] = float(b.sprite->h);
                o[12] = float(b.left), o[13] = float(b.top), o[14] = 1.0f;
            }
        }
        if (!atlas_full_) break;
        atlas_reset_ = true;  // full: start it again with this frame's sprites
    }
    if (atlas_full_) return false;
    f.width = width_;
    f.height = height_;
    f.view_left = view_left_;
    f.view_top = view_top_;
    f.time = ticks_ / 60.0f;
    f.screen = screen.pixels.data();
    f.options = opt;
    for (int i = 0; i < 256; ++i) f.palette[i] = flat_[i] & 0xFFFFFFu;
    f.code.resize(static_cast<std::size_t>(width_) * height_);
    pool_->run(static_cast<std::size_t>(height_), [&](std::size_t row) {
        for (std::size_t j = row * width_; j < (row + 1) * width_; ++j) {
            if (screen.pixels[j] != world_[j]) f.code[j] = LitFrame::kFlat;
            else if (emissive_[j]) f.code[j] = LitFrame::kEmissive;
            else if (owner_[j] < 0) f.code[j] = LitFrame::kAmbient;
            else f.code[j] = static_cast<std::uint16_t>(owner_[j]);
        }
    });
    f.lights.assign(lights_.size() * 20, 0.0f);
    f.light_boxes.clear();
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        float* o = &f.lights[l * 20];
        o[0] = L.pos.x, o[1] = L.pos.y, o[2] = L.pos.z, o[3] = L.radius;
        o[4] = L.color.x, o[5] = L.color.y, o[6] = L.color.z, o[7] = float(L.owner);
        o[8] = float(f.light_boxes.size()), o[9] = float(per_light_[l].size()), o[10] = L.liquid ? 1.0f : 0.0f;
        o[11] = L.cos_inner;
        o[12] = L.dir.x, o[13] = L.dir.y, o[14] = L.dir.z, o[15] = L.cos_outer;
        o[16] = L.halo, o[17] = L.halo_gain, o[18] = L.dim;
        for (std::uint32_t b : per_light_[l]) f.light_boxes.push_back(static_cast<std::int32_t>(b));
    }
    f.puffs.assign(puff_now_.size() * 12, 0.0f);
    for (std::size_t i = 0; i < puff_now_.size(); ++i) {
        const PuffState& p = puff_now_[i];
        float* o = &f.puffs[i * 12];
        o[0] = p.centre.x, o[1] = p.centre.y, o[2] = p.centre.z, o[3] = p.radius;
        o[4] = p.density, o[5] = p.heat, o[6] = p.grey, o[7] = p.seed, o[8] = p.time, o[9] = p.feature;
        o[10] = p.emit;
    }
    if (!gpu_->shade(f, argb)) {
        atlas_reset_ = true;  // the GPU may have lost it
        return false;
    }
    return true;
}

// @orig none: port lighting
std::optional<Vec3> WorldLighting::surface_at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return std::nullopt;
    if (owner_[static_cast<std::size_t>(y) * width_ + x] < 0) return std::nullopt;
    Vec3 p, n;
    point_at(x, y, p, n);
    return p;
}

// @orig none: port lighting
std::optional<Vec3> WorldLighting::light_spot(int x, int y, float height) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return std::nullopt;
    if (owner_[static_cast<std::size_t>(y) * width_ + x] < 0) return std::nullopt;
    Vec3 p, n;
    point_at(x, y, p, n);
    const Vec3 off = std::abs(n.z) > 0.7f ? Vec3{} : normalize(Vec3{n.x, n.y, 0}) * 40.0f;
    return p + off + Vec3{0, 0, height * kLightZScale};
}

// @orig none: port lighting (diagnostics)
void WorldLighting::probe(const World& world, int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
    if (owner_[j] < 0) {
        std::printf("probe (%d, %d): %s\n", x, y, emissive_[j] ? "flash" : "background");
        return;
    }
    const DrawItem& d = list_[owner_[j]];
    Vec3 p, n;
    point_at(x, y, p, n);
    std::printf("probe (%d, %d): item %u shape %u frame %u box x[%d..%d] y[%d..%d] z[%d..%d]\n"
                "  surface (%.0f %.0f %.1f) normal (%.2f %.2f %.2f)\n",
                x, y, d.item->id, d.item->shape, d.item->frame, d.x_left, d.x, d.y_far, d.y, d.z, d.z_top, p.x, p.y,
                p.z / kLightZScale, n.x, n.y, n.z);
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        std::printf("  light %zu (item %u) at (%.0f %.0f %.1f) distance %.0f\n", l, L.owner, L.pos.x, L.pos.y,
                    L.pos.z / kLightZScale, std::sqrt(dot(L.pos - p, L.pos - p)));
        if (L.dir.x != 0 || L.dir.y != 0 || L.dir.z != 0)
            std::printf("    spot towards (%.2f %.2f %.2f), cone %.2f here\n", L.dir.x, L.dir.y, L.dir.z,
                        spot(L, L.pos - p, std::sqrt(dot(L.pos - p, L.pos - p))));
        for (std::uint32_t bi : per_light_[l]) {
            if (!occluded(p, n, d.item->id, l, true, {bi})) continue;
            const Box& b = boxes_[bi];
            const Item* it = world.get(b.owner);
            std::printf("    blocked by item %u shape %u frame %u box (%.0f %.0f %.1f)-(%.0f %.0f %.1f)\n", b.owner,
                        it ? it->shape : 0, it ? it->frame : 0, b.lo.x, b.lo.y, b.lo.z / kLightZScale, b.hi.x,
                        b.hi.y, b.hi.z / kLightZScale);
        }
    }
}

// The frame a character shows when it faces `steps` sixteenths of a turn
// further: the animation frame (action, direction, index) that gives its
// current frame is found in ANIM.DAT, then taken in the new direction. The
// current frame when no animation has it.
// @orig none: port lighting (experiment)
std::pair<const ShapeFrame*, bool> WorldLighting::turned_frame(const DrawItem& d, int steps) {
    const std::pair<const ShapeFrame*, bool> same{d.frame, d.mirrored};
    if (!anims_ || steps % 16 == 0) return same;
    const std::uint16_t shape = d.item->shape, frame = d.item->frame;
    const std::uint32_t key = std::uint32_t{shape} << 17 | std::uint32_t{frame} << 1 | (d.mirrored ? 1u : 0u);
    auto it = anim_of_frame_.find(key);
    if (it == anim_of_frame_.end()) {
        std::optional<AnimPose> found;
        for (std::uint16_t a = 0; a < AnimDat::kActions && !found; ++a) {
            const AnimAction* action = anims_->action(shape, a);
            if (!action) continue;
            for (std::size_t dir = 0; dir < action->directions() && !found; ++dir)
                for (std::size_t i = 0; i < action->frame_count(); ++i) {
                    const AnimFrame& af = action->frames[dir * action->frame_count() + i];
                    if (af.frame == frame && af.mirrored == d.mirrored) {
                        found = AnimPose{a, static_cast<int>(action->sixteen_directions() ? dir : dir * 2), i};
                        break;
                    }
                }
        }
        it = anim_of_frame_.emplace(key, found).first;
    }
    if (!it->second) return same;
    const AnimPose& pose = *it->second;
    const int dir = ((pose.dir16 - steps) % 16 + 16) % 16;
    const AnimAction* action = anims_->action(shape, pose.action);
    const AnimFrame* af = action ? action->frame(dir, pose.index) : nullptr;
    const Shape* sh = shapes_.get(shape);
    if (!af || !sh || af->frame >= sh->frames().size()) return same;
    return {&sh->frames()[af->frame], af->mirrored};
}

// Every pixel of every sprite around the view (not only those left on top)
// becomes a lit point, splatted with a depth buffer from the turned camera.
// Characters face it with the frame of their direction; the far faces of
// boxes, never drawn, repeat the near ones mirrored through the box.
// @orig none: port lighting (experiment)
void WorldLighting::reproject(const World& world, const Camera& centre, const LightingOptions& opt,
                              std::uint32_t* argb) {
    constexpr float kDeg = 3.14159265f / 180.0f;
    const float phi = (45.0f + opt.yaw) * kDeg, theta = (30.0f + opt.pitch) * kDeg;
    const Vec3 f{std::cos(theta) * std::cos(phi), std::cos(theta) * std::sin(phi), std::sin(theta)};
    const Vec3 r{std::sin(phi), -std::cos(phi), 0.0f};
    const Vec3 u{-std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
    const Vec3 c{float(centre.x), float(centre.y), centre.z * kLightZScale};
    const int w = width_, h = height_;
    constexpr int kMargin = 320;
    const auto around =
        build_display_list(world, shapes_, View{view_left_ - kMargin, view_top_ - kMargin}, w + 2 * kMargin,
                           h + 2 * kMargin);
    const int steps = static_cast<int>(std::lround(opt.yaw / 22.5f));
    std::vector<std::pair<const ShapeFrame*, bool>> sprite(around.size());
    std::vector<std::uint8_t> actor(around.size());
    for (std::size_t k = 0; k < around.size(); ++k) {
        const DrawItem& d = around[k];
        actor[k] = is_actor(*d.item, world.shape_info(d.item->shape));
        sprite[k] = {d.frame, d.mirrored};
        if (actor[k] && opt.turn_actors) sprite[k] = turned_frame(d, steps);
        decode(*d.frame);
        decode(*sprite[k].first);
    }
    std::fill(argb, argb + static_cast<std::size_t>(w) * h, 0xFFFF00FFu);
    std::vector<float> depth(static_cast<std::size_t>(w) * h, -1e30f);
    std::vector<std::mutex> row_locks(h);
    std::vector<std::uint32_t> all(lights_.size());
    for (std::uint32_t l = 0; l < all.size(); ++l) all[l] = l;
    auto splat = [&](float sx, float sy, float z, int col, const Vec3& p, const Vec3& n, ObjId self, bool flash) {
        const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
        if (x0 < -1 || y0 < -1 || x0 >= w || y0 >= h) return;
        std::uint32_t value = 0;
        bool shaded = false;
        for (int dy = 0; dy < 2; ++dy) {
            const int y = y0 + dy;
            if (y < 0 || y >= h) continue;
            std::lock_guard<std::mutex> lock(row_locks[y]);
            for (int dx = 0; dx < 2; ++dx) {
                const int x = x0 + dx;
                if (x < 0 || x >= w) continue;
                const std::size_t j = static_cast<std::size_t>(y) * w + x;
                if (z <= depth[j]) continue;
                if (!shaded) {
                    value = flash ? pack(lin_[col]) : pack(shade_point(col, p, n, self, opt, all, nullptr));
                    shaded = true;
                }
                depth[j] = z;
                argb[j] = value;
            }
        }
    };
    pool_->run(around.size(), [&](std::size_t k) {
        const DrawItem& d = around[k];
        if (d.shape_flags & ShapeInfo::kTranslucent) return;
        if (actor[k] || d.item->sprite_only) {
            // A sprite facing the turned camera, standing on the box centre.
            const ShapeFrame& frame = *sprite[k].first;
            const bool mirrored = sprite[k].second;
            const Decoded& fr = decoded(&frame);
            const float xc = (d.x + d.x_left) * 0.5f, yc = (d.y + d.y_far) * 0.5f;
            const Vec3 base{xc, yc, d.z * kLightZScale};
            // Where the centre falls in the sprite, from its anchor (the box corner).
            const float cx = (xc - yc) * 0.25f - (d.x - d.y) * 0.25f;
            const float cy = (xc + yc) * 0.125f - (d.x + d.y) * 0.125f;
            const Vec3 q0 = base - c;
            const float base_z = dot(q0, f);
            const float csx = kPixelScale * dot(q0, r) + w / 2.0f, csy = -kPixelScale * dot(q0, u) + h / 2.0f;
            for (int fy = 0; fy < fr.h; ++fy)
                for (int fx = 0; fx < fr.w; ++fx) {
                    const int col = fr.color[fy * fr.w + fx];
                    if (col < 0) continue;
                    const float ox = (mirrored ? frame.xoff - fx : fx - frame.xoff) - cx;
                    const float oy = (fy - frame.yoff) - cy;
                    float nx = fr.nx[fy * fr.w + fx] / 127.0f, ny = fr.ny[fy * fr.w + fx] / 127.0f;
                    if (mirrored) nx = -nx;
                    const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
                    const Vec3 n = normalize(r * nx + u * ny + f * nz);
                    const Vec3 p = base + r * (ox / kPixelScale) - u * (oy / kPixelScale) + f * (nz * 12.0f);
                    splat(csx + ox, csy + oy, base_z + nz * 12.0f, col, p, n, d.item->id, d.item->sprite_only);
                }
            return;
        }
        const Decoded& fr = decoded(d.frame);
        // DrawItem screen positions are relative to the wider view.
        const int left = d.left + view_left_ - kMargin, top = d.top + view_top_ - kMargin;
        for (int fy = 0; fy < fr.h; ++fy)
            for (int fx = 0; fx < fr.w; ++fx) {
                const int col = fr.color[fy * fr.w + fx];
                if (col < 0) continue;
                const int ax = d.mirrored ? left + fr.w - 1 - fx : left + fx;
                Vec3 p, n;
                int face = 0;
                surface(d, false, ax + 0.5f, top + fy + 0.5f, fx, fy, opt.bump, p, n, &face);
                const Vec3 q = p - c;
                splat(kPixelScale * dot(q, r) + w / 2.0f, -kPixelScale * dot(q, u) + h / 2.0f, dot(q, f), col, p, n,
                      d.item->id, false);
                if (!opt.back_faces || (face != 1 && face != 2)) continue;
                // The far side of the box, never drawn: the near side mirrored through it.
                Vec3 pb = p, nb = n;
                if (face == 1) {
                    pb.x = d.x_left - (p.x - d.x);
                    nb.x = -n.x;
                } else {
                    pb.y = d.y_far - (p.y - d.y);
                    nb.y = -n.y;
                }
                const Vec3 qb = pb - c;
                splat(kPixelScale * dot(qb, r) + w / 2.0f, -kPixelScale * dot(qb, u) + h / 2.0f, dot(qb, f), col, pb,
                      nb, d.item->id, false);
            }
    });
}

}  // namespace crusader
