#pragma once
// Impact marks on the scenery (port addition, the original has nothing like
// it; Enhancements screen, GameConfig::impact_marks): where a shot ends, the
// scenery it hits keeps a darkened mark. Bullets leave a small hole, energy
// weapons (lasers, plasma, electric, fire...) a burn, explosions a scorch on
// the item hit and on the scenery around within reach.
//
// A mark belongs to one item (its host) and is drawn right after it, inside
// its opaque pixels: the display list's order puts whatever stands in front
// of the host over the mark as well. It is kept as a screen offset from the
// host's anchor, which only holds while the host stays as it was: a host
// moved, turned to another frame or shape, mirrored or destroyed loses its
// marks. Marks are not saved; changing map, loading a game or switching the
// option off forgets them. The oldest go first past kMaxMarks.
//
// Blood (GameConfig::blood_marks): a
// human hit by a bullet or a blast bleeds on the scenery. A spray on the wall
// behind (in the shot's direction, with drips running down), drops on the
// floor around, a pool under a body; killed by a blast, a bigger pool, pieces
// and drops thrown in a fan away from it and sprays on the walls around. The
// gore mode (GameConfig::gore_mode) throws much more of it, further. Robots,
// energy weapons (they burn, freeze or melt) and the dead do not bleed. Blood
// marks follow the same rules as the others.
//
// Casings (GameConfig::casings, casings.hpp): a casing (a shotgun shell)
// that landed lies on the floor as a mark of brass (red) pixels with a
// little shadow, on the same rules. Past kMaxCasings the oldest go first.
//
// Marks only read the world: no random number of the game is drawn, so the
// game plays the same with or without them.

#include "core/palette.hpp"
#include "core/surface.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace crusader {

enum class MarkKind : std::uint8_t { Bullet, Burn, Scorch, BloodSpray, BloodDrop, BloodPool, Casing, Shell };

// The mark a shot of `fire_type` leaves: a scorch when it splashes, a hole
// for the bullets (fire types 1, 2, 0xB, 0xD), a burn otherwise.
MarkKind mark_kind(int fire_type);

inline bool is_blood(MarkKind kind) {
    return kind == MarkKind::BloodSpray || kind == MarkKind::BloodDrop || kind == MarkKind::BloodPool;
}

inline bool is_casing(MarkKind kind) { return kind == MarkKind::Casing || kind == MarkKind::Shell; }

// For each colour, the palette colour nearest to it darkened to `percent` of
// its brightness. Colours 1..14 (cycled and translucent) and 0xFF are never
// chosen; colours 1..14 map to themselves.
std::array<std::uint8_t, 256> shade_table(const Palette& palette, int percent);

// The same, the colour mixed with `target` at `percent` (blood).
std::array<std::uint8_t, 256> tint_table(const Palette& palette, Rgb target, int percent);

// A pixel of a mark: its offset from the mark's centre and its shade (0
// light, 1 medium, 2 dark, 3 almost black; blood 4 light, 5 red, 6 dark;
// casings 7 bright brass, 8 brass, 9 dark brass, 10 red, 11 dark red: those
// replace the colour underneath instead of mixing with it).
struct MarkPixel {
    std::int8_t dx, dy;
    std::uint8_t shade;
};

// The pixels of a mark of `kind` and `radius` (a hole is about 2 x 2 pixels
// with its rim, `radius` aside), squashed to half its height when `flat`;
// `seed` shapes its edge. Blood flies in `angle` (radians, screen x right and
// y down; on a flat mark, in the ground's own proportions). A casing or a
// shell lies one of four ways picked by `seed`.
std::vector<MarkPixel> mark_pattern(MarkKind kind, int radius, bool flat, std::uint32_t seed, double angle = 0);

class DamageMarks {
public:
    static constexpr std::size_t kMaxMarks = 2048;
    static constexpr std::size_t kMaxCasings = 512;
    static constexpr int kShades = 12;
    enum Shade : std::uint8_t { kBrassLight = 7, kBrass = 8, kBrassDark = 9, kShellRed = 10, kShellDark = 11 };

    struct Mark {
        ObjId host = 0;
        // The host as it was marked.
        std::uint16_t shape = 0, frame = 0, flags = 0;
        std::int32_t x = 0, y = 0, z = 0;
        // Centre of the mark, in screen pixels from the host's anchor.
        std::int16_t dx = 0, dy = 0;
        MarkKind kind = MarkKind::Bullet;
        std::uint8_t radius = 0;  // pixels (scorch, burn)
        bool flat = false;        // on a floor: squashed to the isometric ground
        std::uint32_t seed = 0;
        std::shared_ptr<const std::vector<MarkPixel>> pixels;  // mark_pattern
    };

    // A shot of `fire_type` ended at `at` (usecode units) on `hit` (0: on
    // nothing, a floor below may take the mark).
    void record(const World& world, ObjId hit, World::Point at, int fire_type);

    // An NPC lost hit points (WorldHooks::npc_wounded): its blood, much more
    // of it in `gore` mode (GameConfig::gore_mode).
    void bleed(const World& world, const Wound& wound, bool gore = false);

    // A casing (`shell`: a shotgun shell) landed at `at`: it lies on the
    // floor under it, at most 16 below. False when there is none.
    bool lay_casing(const World& world, World::Point at, bool shell, std::uint32_t seed);

    // Once per frame, before draw: forgets the marks of another map, of
    // hosts that changed and of a kind switched off (`impacts`: holes,
    // burns, scorches; `blood`; `casings`), indexes the others by host and
    // follows the game palette.
    void prepare(const World& world, const Palette& game_palette, bool impacts = true, bool blood = true,
                 bool casings = true);

    // The marks of the item just drawn.
    void draw(Surface8& screen, const DrawItem& d) const;

    // Colour `under` seen through shade `shade` (MarkPixel::shade), once
    // prepared; `under` itself before.
    std::uint8_t shaded(int shade, std::uint8_t under) const { return shade_ready_ ? shade_[shade][under] : under; }

    void clear();
    std::size_t size() const { return marks_.size(); }
    const std::deque<Mark>& marks() const { return marks_; }

private:
    // `flat`: on a top face (a floor), squashed as the ground is.
    void add(const Item& host, World::Point surface, MarkKind kind, int radius, bool flat, double angle = 0);
    // From `from` along (ux, uy), level, up to `reach` units: a spray of
    // `radius` on the first wall met, if the camera sees its face.
    bool spray_wall(const World& world, World::Point from, double ux, double uy, int reach, int radius);
    // On the floor under (x, y), at most 16 below `z`; false when none.
    bool drop(const World& world, World::Point at, MarkKind kind, int radius, double ux, double uy);
    int roll(int n);  // 0..n-1, the marks' own numbers
    bool opaque(const DrawItem& d, int sx, int sy) const;

    std::deque<Mark> marks_;
    std::unordered_map<ObjId, std::vector<std::uint32_t>> by_host_;
    int map_ = -1;
    std::uint32_t next_seed_ = 0x9E3779B9u;
    std::array<std::array<std::uint8_t, 256>, kShades> shade_{};
    Palette shade_palette_{};
    bool shade_ready_ = false;
    // Opacity of the host frames, one byte a pixel, made on first use.
    mutable std::unordered_map<const ShapeFrame*, std::vector<std::uint8_t>> masks_;
};

}  // namespace crusader
