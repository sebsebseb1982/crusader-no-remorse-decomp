#pragma once
// Actor animations (STATIC/ANIM.DAT), read by ANIMFILE.C / ASTREAM.C / ANIMCACH.C
// and played by the actor animation process (ANIMPRIM.C 1908).
//
// File: 2048 u32 offsets, one per shape (0 = no animations). Each points to
// 256 u32 offsets, one per action. An action is a 4-byte header followed by
// frame_count * directions 8-byte frames, grouped by direction.
//
// Action header:
//   [0] frame count
//   [1] flags: bit 0 looping (other bits read by the animation process)
//   [2] bits 0-3 ticks between frames; bit 4 direction turned by +4
//   [3] bit 6: 16 directions (else 8: the 0..15 direction is halved)
//
// Frame:
//   u16 [0-1]  bits 0-11 shape frame; 0x1000 fire the weapon; 0x4000 frame
//              event (ITEM.C 2B01); 0x8000 mirrored (item flag 0x20)
//   s8  [2]    z change
//   u8  [3]    sound effect (0 = none)
//   u16 [4-5]  bits 0-8 signed distance along the direction; bit 9: the
//              collision check also tests the ground (sweep flags 0x11, not 4)
//   [5-7]      weapon overlay offsets: bits 6-10 of u16 [5-6] and bits 3-7 of
//              [6] (signed), bits 0-3 of [7]

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace crusader {

struct AnimFrame {
    std::uint16_t frame = 0;
    // 0x1000: special attack of a few NPC shapes (WPNCACHE_1160_01f0: robots
    // exploding, sound loops); not the weapon shot.
    bool special = false;
    bool event = false;
    bool mirrored = false;
    std::int8_t delta_z = 0;
    std::uint8_t sfx = 0;
    std::int16_t distance = 0;
    bool ground_check = false;
    // Muzzle offset of the weapon shot fired on this frame (ANIMPRIM.C, then
    // WPNCACHE_1160_005f -> Item_I_fireWeapon): bits 6..10 of bytes 5-6, bits
    // 3..7 of byte 6 (both as signed 5-bit values) and the low nibble of byte
    // 7. The weapon fires on a frame where any of them is not 0.
    std::int8_t muzzle_a = 0;
    std::int8_t muzzle_b = 0;
    std::uint8_t muzzle_z = 0;
    bool fires_weapon() const { return muzzle_a != 0 || muzzle_b != 0 || muzzle_z != 0; }
    std::array<std::uint8_t, 8> raw{};

    static AnimFrame decode(std::span<const std::uint8_t, 8> d);
};

struct AnimAction {
    std::array<std::uint8_t, 4> header{};
    std::vector<AnimFrame> frames;  // directions * frame_count, grouped by direction

    std::size_t frame_count() const { return header[0]; }
    bool looping() const { return header[1] & 0x01; }
    std::uint8_t flags() const { return header[1]; }
    std::uint8_t ticks_per_frame() const { return header[2] & 0x0F; }
    bool turned() const { return header[2] & 0x10; }
    bool sixteen_directions() const { return header[3] & 0x40; }
    // The actor is destroyed when the action ends (ANIMPRIM.C 2D09 / 2DC6):
    // the deaths of shapes 1423 and 1436.
    bool destroys_actor() const { return header[3] & 0x80; }
    std::size_t directions() const { return sixteen_directions() ? 16 : 8; }

    // Frame `index` facing `dir16` (0..15). With 8 directions the direction
    // is halved (ASTREAM.C 0106). nullptr when out of range.
    const AnimFrame* frame(int dir16, std::size_t index) const;
};

class AnimDat {
public:
    static constexpr std::size_t kShapes = 0x800;
    static constexpr std::size_t kActions = 0x100;

    static AnimDat load(const std::filesystem::path& path);
    static AnimDat parse(std::span<const std::uint8_t> data);

    // nullptr when the shape has no such action.
    const AnimAction* action(std::uint16_t shape, std::uint16_t action) const;
    bool has_shape(std::uint16_t shape) const { return shape < kShapes && !shapes_[shape].empty(); }
    std::size_t action_count() const;

private:
    // Per shape: 256 optional actions (empty vector when the shape has none).
    std::vector<std::vector<std::optional<AnimAction>>> shapes_ =
        std::vector<std::vector<std::optional<AnimAction>>>(kShapes);
};

}  // namespace crusader
