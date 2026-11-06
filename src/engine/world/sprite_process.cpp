#include "engine/world/sprite_process.hpp"

#include "engine/audio/audio.hpp"
#include "engine/usecode/world_intrinsics.hpp"

#include <array>

namespace crusader {

namespace {

// SPRITE_1140_0007: shapes drawn over the scene, with their offsets.
constexpr std::array<std::uint16_t, 10> kOverlayShapes = {0x326, 0x31C, 0x31F, 0x320, 0x321,
                                                          0x323, 0x324, 0x325, 0x322, 0x327};
constexpr std::array<int, 10> kOverlayDx = {0x98, 0xA8, 0x59, 0x8B, 0x98, 0xB4, -8, 0x0A, 0x91, 0x5A};
constexpr std::array<int, 10> kOverlayDy = {0xA2, 0x94, 0x4E, 0x93, 0x53, 0xA3, -0x17, -0x24, 0x3A, 0x4D};

// The C library's rand() * n / 0x8000 (core/random.hpp).
int rnd(World& w, int n) { return crt_rand_below(w.state.crt_rand_state, n); }

// SPRITE_1140_0007: same screen place at z 240, then the shape's offset.
// @orig 1140:0007
void raise_overlay(const World& world, std::uint16_t shape, World::Point& at) {
    for (std::size_t i = 0; i < kOverlayShapes.size(); ++i) {
        if (kOverlayShapes[i] != shape) continue;
        // SHAPHAND_1090_07bd, then ANIMCACH_1188_0467 (16-bit arithmetic).
        const auto x = static_cast<std::int16_t>(at.x), y = static_cast<std::int16_t>(at.y);
        int sx = static_cast<std::int16_t>(((x - y) >> 1) - world.state.view_left);
        int sy = static_cast<std::int16_t>((static_cast<std::uint16_t>(x + y) >> 2) - world.state.view_top);
        sy += 0xF0 - (at.z & 0xFF);
        sx += world.state.view_left;
        sy += world.state.view_top;
        at.x = static_cast<std::int16_t>(sx + sy * 2) + kOverlayDx[i];
        at.y = static_cast<std::int16_t>(sy * 2 - sx) + kOverlayDy[i];
        at.z = 0xF0;
        return;
    }
}

}  // namespace

SpriteProcess::SpriteProcess(World& world, std::uint16_t shape, int first, int last, int delay, World::Point at)
    : world_(world), shape_(shape), first_(first), last_(last), frame_(first), delay_(delay), at_(at) {
    set_type(kType);
}

// @orig 1140:029F 1140:00DE
void SpriteProcess::on_added() {
    if (shape_ == kTargetMarker) {
        GameState::Aim& aim = world_.state.aim;
        slot_ = world_.state.screen_sprites.add(shape_, static_cast<std::uint16_t>(frame_), at_.x, at_.y,
                                                static_cast<std::uint8_t>(at_.z));
        if (slot_ != -1) {
            aim.marker = slot_;
            aim.marker_pid = pid();
        }
        return;
    }
    raise_overlay(world_, shape_, at_);
    sprite_ = world_.create(shape_, static_cast<std::uint8_t>(frame_));
    if (Item* item = world_.get(sprite_)) {
        item->sprite_only = true;
        world_.pop_to_coords(at_.x, at_.y, at_.z);
    }
}

// @orig 1140:04E2
void SpriteProcess::run() {
    if (last_ < first_) --frame_;
    else ++frame_;
    if (frame_ > last_ || frame_ < 0) {
        terminate();
        return;
    }
    if (delay_ != 0) {
        if (Item* item = world_.get(sprite_)) world_.set_frame(*item, static_cast<std::uint16_t>(frame_));
        world_.state.screen_sprites.set_frame(slot_, static_cast<std::uint16_t>(frame_));
        sleep(static_cast<std::uint32_t>(delay_));
    }
}

// @orig 1140:0444
void SpriteProcess::on_terminate() {
    if (sprite_ && world_.get(sprite_)) world_.destroy(sprite_);
    sprite_ = 0;
    GameState::Aim& aim = world_.state.aim;
    if (slot_ != -1) {
        if (slot_ == aim.marker) world_.state.screen_sprites.set_frame(slot_, 5);
        else world_.state.screen_sprites.remove(slot_);
        slot_ = -1;
    }
    if (pid() == aim.marker_pid) aim.marker_pid = 0;
    if (pid() == world_.state.shield_sprite) {
        world_.state.shield_sprite = 0;
        world_.state.shield_sprite_item = 0;
    }
}

Pid create_sprite(Kernel& kernel, World& world, std::uint16_t shape, int first, int last, int delay,
                  World::Point at) {
    return kernel.add(std::make_unique<SpriteProcess>(world, shape, first, last, delay, at));
}

// @orig 1150:02DF
void impact_sprite(Kernel& kernel, World& world, int fire_type, World::Point at) {
    // Sounds: DS:2A7A (bullets), DS:2A74 (explosions), DS:2A86 (electric).
    static constexpr int kBullet[] = {0x26, 0x27, 0x41, 0x42, 0x45, 0x46};
    static constexpr int kBlast[] = {0x1C, 0x6C, 0x3E};
    static constexpr int kElectric[] = {0x48, 0x05};
    std::uint16_t shape = 0x537;
    int sound = 0;
    switch (fire_type) {
    case 1: case 0xB: case 0xD: shape = 0x1D8; sound = kBullet[rnd(world, 6)]; break;
    case 2: shape = 0x1D8; break;
    case 3: case 4:
        shape = std::array<std::uint16_t, 3>{0x326, 0x320, 0x321}[rnd(world, 3)];
        sound = kBlast[rnd(world, 3)];
        break;
    case 6: shape = 0x578; break;
    case 7: {
        // Drawn among 4 entries of a table of 2: the next ones are the
        // explosion shapes 0x31C, 0x31F, beyond SOUND.FLX, which play nothing.
        const int r = rnd(world, 4);
        if (r < 2) sound = kElectric[r];
        break;
    }
    case 10:
        shape = std::array<std::uint16_t, 3>{0x31C, 0x31F, 0x322}[rnd(world, 3)];
        sound = kBlast[rnd(world, 3)];
        break;
    case 0xE: shape = 0x56B; break;
    case 0xF: shape = 0x59B; sound = kElectric[rnd(world, 2)]; break;
    default: break;
    }
    if (world.hooks().explosion) {
        if (fire_type == 3) world.hooks().explosion(1);
        else if (fire_type == 4 || fire_type == 10) world.hooks().explosion(2);
    }
    int first = 0, last = 0x27;
    switch (shape) {
    case 0x56B: first = rnd(world, 3) * 6; last = first + 5; break;
    case 0x537: last = 10; break;
    case 0x578: first = rnd(world, 3) * 5; last = first + 4; break;
    case 0x59B: first = rnd(world, 2) * 4; last = first + 3; break;
    case 0x1D8: {
        static constexpr int kRanges[4][2] = {{0, 4}, {5, 8}, {9, 12}, {13, 16}};
        const int r = random_between(world.state.random_state, 0, 3);  // UCMachine::I_rndRange
        first = kRanges[r][0];
        last = kRanges[r][1];
        break;
    }
    default: break;
    }
    create_sprite(kernel, world, shape, first, last, 3, at);
    // As an interface sound: full volume wherever the impact is.
    if (sound != 0) {
        if (Audio* audio = world.audio()) audio->play_sfx(sound);
    }
}

}  // namespace crusader
