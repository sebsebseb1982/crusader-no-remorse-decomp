#pragma once
// Leaking barrels (port addition, the original has nothing like it;
// Enhancements screen, GameConfig::barrel_leaks). The explosive barrels
// (BARRELUP 0xAA upright, BARRELS..BARRELSE 0x151..0x155 lying, in both
// games) take several bullets before they blow up: their class counts the
// damage of each hit (BARREL, ITEM_066B) and explodes at 100 in No Remorse,
// 150 in No Regret. With the option, bullets do them less damage
// (barrel_bullet_damage), and every bullet before the last pierces the
// barrel and its fuel runs out of the hole:
//
// - A hole below the fuel's level leaks: a jet out of the barrel's side,
//   strong while the barrel is full, weaker as the level falls, at last a
//   dribble down the side, and nothing once the level is under the hole. A
//   hole in the top lets nothing out. Every hole lowers the level.
// - The fuel makes a pool on the floor where the jet lands and at the foot
//   of the barrel, growing with what has run out, with fingers running out
//   of it and a warped, ragged edge; dark, with a sheen, mirroring what
//   stands behind it. It lies on the floors at the barrel's level (none
//   down a ledge, none under what stands on the floor); a barrel standing
//   on another spills down its side to the floor under both.
// - A leaking barrel that moves (a rolling barrel, ROLLB_NS / ROLLB_EW,
//   rolling, or a barrel thrown by a blast) leaves its pool behind and lays
//   a trail of fuel along its way, from every hole for a lying barrel (it
//   rolls them under the fuel: a splash once a turn, a dribble between),
//   wandering a little; at rest, its jets make a pool where it lies. The
//   trail goes on from the pool it starts from, in pools that touch: the
//   fire runs along it from one to the next, lower than over a pool.
// - Every barrel that blows up (the last bullet, a blast, another barrel)
//   throws what was left in it: most burns in the blast, some (kLeft)
//   runs out where it stood, in a pool of fire that spreads out within
//   kSpreadTicks, some (kThrown) flies all round in burning drops, a few
//   metres, splashing where they land in streaks of fire along their way,
//   setting alight the pools, floors, walls and people they land on. Its own pool catches fire too: the fire spreads
//   over it unevenly, burns a few seconds (longer the bigger the pool),
//   dies down from its edge and leaves a burnt stain. A blast or an energy
//   shot (laser, plasma, fire...) that ends on a pool sets it alight too,
//   and a burning pool lights the pools it touches.
// - The fire is a sheet of flames drawn pixel by pixel: a bed of fire over
//   the pool, tongues licking up from it, rising noise in fire colours, in
//   the game's draw order by patches of the pool, or over the wall hit.
// - Whoever stands in the fire, the Silencer included, catches fire: flames
//   over their body, and damage of fire type 0xC (the game's own fire: a
//   human it kills burns and runs) every kBurnEvery ticks, until kAfterBurn
//   ticks after they left it (take_burns: the game applies them).
// - With the 3D lighting, the fire lights the scene, its flames glow and
//   give off a dark smoke.
//
// Only bullets pierce (the fire types that leave a bullet hole or eject a
// casing). Leaks use their own numbers, never the game's random generators;
// only the damage of the fire and of the bullets on the barrels changes the
// game. They are not saved: a load or a map change forgets them.

#include "core/palette.hpp"
#include "core/surface.hpp"
#include "engine/world/lighting.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace crusader {

// An explosive barrel (BARRELUP, BARRELS..BARRELSE): its class counts the
// damage it takes.
bool is_fuel_barrel(std::uint16_t shape);
// A rolling barrel (ROLLB_NS 0x586, ROLLB_EW 0x58B): a shot sets it rolling
// along its way (its class, event 0x21) until it meets something and blows
// up (event 0x20).
bool is_rolling_barrel(std::uint16_t shape);
// A barrel that leaks when pierced: either of them.
bool leaks_fuel(std::uint16_t shape);

// The damage a shot of `fire_type` does to `shape` with the option on: a
// bullet does a fuel barrel 3/5 of it (at least 1), so that it takes about
// 1.7 times as many before it blows up; blasts and the rest are unchanged.
int barrel_bullet_damage(std::uint16_t shape, int damage, int fire_type);

class BarrelLeaks {
public:
    static constexpr std::size_t kMaxBarrels = 32;
    static constexpr std::size_t kMaxHoles = 8;  // per barrel
    static constexpr std::size_t kMaxPools = 160;
    static constexpr std::size_t kMaxDrops = 160;
    static constexpr std::size_t kMaxWallFires = 96;
    static constexpr std::uint32_t kLeakDelay = 6;    // ticks between a hit and its leak
    static constexpr std::uint32_t kRest = 16;        // ticks without a move: a barrel is at rest
    static constexpr double kTrailStep = 6;           // usecode units between two drops of a trail
    static constexpr std::size_t kMaxTrail = 64;      // trail lobes in a pool
    static constexpr int kGrid = 4;                   // a fuel cell, usecode units
    static constexpr int kPatch = 8;                  // cells a side of a patch of fire
    static constexpr std::uint32_t kBurnEvery = 15;   // ticks between two burns
    static constexpr std::uint32_t kAfterBurn = 60;   // ticks one burns on after leaving the fire
    static constexpr int kBurnDamage = 5;             // fire type 0xC's damage
    static constexpr int kBurnFireType = 0xC;
    // Of what is left in a barrel that blows up, the share that runs out
    // where it stood and the share its blast throws; the rest burns in the
    // blast.
    static constexpr double kLeft = 0.2, kThrown = 0.35;
    static constexpr std::uint32_t kSpreadTicks = 12; // ticks a blown up barrel's pool takes to spread out

    // Scenery: a box, usecode units.
    struct Solid {
        ObjId id;
        std::int32_t x_left, x, y_far, y, z, z_top;
    };

    struct Hole {
        double ux = 0, uy = 0;     // outwards, level, unit
        double x = 0, y = 0;       // on the barrel's side, from its anchor (usecode units)
        double height = 0;         // above the barrel's bottom
        std::uint32_t made = 0;    // game tick of the hit
        bool seen = false;         // on a side the camera sees
        bool started = false;      // has leaked
        double spilled = 0;        // level units run out of it
        std::uint32_t seed = 0;
        int land_lobe = -1, foot_lobe = -1;  // its lobes in the pool
        double flow = 0;           // let out this tick while the barrel rolls
    };

    struct Barrel {
        ObjId id = 0;
        std::uint16_t shape = 0, frame = 0;
        std::int32_t x = 0, y = 0, z = 0;  // anchor when last seen
        std::int32_t floor = 0;            // the top of the floor under it, the pool's level
        double length_x = 0, length_y = 0, height = 0;  // box, usecode units
        double radius = 0;                 // cross-section
        double level = 0;                  // fuel's height above the bottom
        std::vector<Hole> holes;
        std::uint32_t pool = 0;            // Pool::serial
        // Moving (thrown) since game tick moved_at at most kRest ago; its
        // trail laid up to (trail_x, trail_y), the middle of its foot; the
        // pools of its trail at other levels (Pool::serial).
        bool rolling = false;
        std::uint32_t moved_at = 0;
        double trail_x = 0, trail_y = 0;
        double trail_way = 0;  // how far it has laid its trail, usecode units
        std::vector<std::uint32_t> trail_pools;
    };

    // A disc of fuel. A finger follows another lobe (`anchor`): `frac` of
    // its radius, `ox`, `oy` its radii away from it. A trail's drop is
    // warped no more than its size, so that the trail runs on unbroken.
    struct Lobe {
        double x = 0, y = 0, r = 0;  // usecode units
        int anchor = -1;
        double ox = 0, oy = 0, frac = 0;
        bool trail = false;
    };

    struct Pool {
        std::uint32_t serial = 0;
        std::int32_t z = 0;  // the floor's top
        std::vector<Lobe> lobes;
        std::uint32_t seed = 0;
        bool burning = false, burnt = false;
        float flames = 1;  // its flames' height, of the tallest: lower over a trail (ignite)
        std::uint32_t lit_at = 0, burn_ticks = 0;
        double fire_x = 0, fire_y = 0;  // where it caught
        // Set when it catches: the fuel on kGrid cells from (grid_x, grid_y),
        // its depth (0 edge .. 1), -1 where none lies on the floor.
        double grid_x = 0, grid_y = 0;
        int grid_w = 0, grid_h = 0;
        std::vector<float> fuel;
        // How far the fire has to come to reach each corner of those cells
        // (grid_w + 1 by grid_h + 1), for reached.
        std::vector<float> arrive;
        std::vector<std::pair<int, int>> patches;  // first cells of the patches with fuel
        // field() at the corners of kGrid cells from (field_x, field_y), as
        // of game tick field_tick; dirty when its lobes changed since.
        double field_x = 0, field_y = 0;
        int field_w = 0, field_h = 0;
        std::vector<float> field_grid;
        std::uint32_t field_tick = 0;
        bool field_dirty = true;
        // The box of the cells of field_grid where the fuel lies (field_at
        // 1 or more), usecode units; wet_x1 < wet_x0 when none.
        double wet_x0 = 0, wet_y0 = 0, wet_x1 = -1, wet_y1 = -1;
    };

    // A burning drop thrown by an exploding barrel.
    struct Drop {
        double x = 0, y = 0, z = 0;     // usecode units
        double vx = 0, vy = 0, vz = 0;  // units a tick
        std::uint32_t born = 0;
        std::uint32_t seed = 0;
        float size = 1;
        double fuel = 0;  // what it carries: level units times the barrel's cross-section
        std::shared_ptr<const std::vector<Solid>> solids;  // the scenery around the barrel
    };

    // Fuel burning on a wall face the camera sees (face 0: +x, 1: +y),
    // around point (x, y, z), then a burnt stain.
    struct WallFire {
        ObjId host = 0;
        std::uint16_t shape = 0, frame = 0;
        std::int32_t ax = 0, ay = 0, az = 0;  // the host's anchor
        int face = 0;
        double x = 0, y = 0, z = 0;
        float r = 0;
        std::uint32_t lit_at = 0, burn_ticks = 0, seed = 0;
    };

    // Lobe `lobe` of pool `pool` (Pool::serial) spreading out from game
    // tick `from` to radius `to`: the fuel of a barrel blown up runs out.
    struct Spread {
        std::uint32_t pool = 0;
        std::size_t lobe = 0;
        double to = 0;
        std::uint32_t from = 0;
    };

    // Someone on fire.
    struct Burning {
        ObjId id = 0;
        std::uint32_t until = 0;  // burns till this tick
        std::uint32_t next = 0;   // next burn
    };

    // A shot of `fire_type` ended at `at` (usecode units) on `hit` at game
    // tick `now` (WorldHooks::shot_landed): a bullet in a barrel pierces it;
    // a blast or an energy shot on a pool sets it alight.
    void shot(const World& world, ObjId hit, World::Point at, int fire_type, std::uint32_t now);
    // `item` blows up at game tick `now` (WorldHooks::item_exploding): a fuel
    // barrel throws its fuel, burning.
    void exploded(const World& world, ObjId item, std::uint32_t now);

    // Runs the leaks and fires on to game tick `now`: the fuel runs out, the
    // pools grow, a barrel gone sets its pool alight, the drops fly, fires
    // burn out, whoever stands in one catches fire. A tick earlier than the
    // last (a load) forgets everything.
    void advance(const World& world, std::uint32_t now);
    // The burns due since the last call, one per burn (the NPC).
    std::vector<ObjId> take_burns();

    // Once per frame, before draw: the colours of the game palette, where
    // each patch of fire comes in the draw order of `list`, the view's corner.
    void prepare(const World& world, const Palette& game_palette, const std::vector<DrawItem>& list,
                 int view_left, int view_top, int width, int height);
    // The fire drawn before every item of the list.
    void draw_first(Surface8& screen);
    // Over the item just drawn, `list[index]`: the pools on a floor, the
    // fires on a wall it shows, the holes and the jets of a barrel, the flames on
    // someone burning, the patches of fire that come after it.
    void draw(Surface8& screen, const DrawItem& d, std::size_t index);
    // Once the list is drawn: the drops in flight, then what the pools
    // mirror.
    void draw_last(Surface8& screen);

    // The fires' lights (lighting space), at the tick of the last advance.
    std::vector<PointLight> lights(const World& world) const;
    // Screen pixels (index, colour) of the fire drawn this frame: they glow
    // (WorldLighting::glow).
    const std::vector<std::pair<std::uint32_t, std::uint8_t>>& glowing() const { return glowing_; }
    // Where the fires give off a puff of smoke (lighting space) after game
    // tick `since` up to the last advance.
    std::vector<Vec3> smoke_since(std::uint32_t since) const;

    void clear();
    const std::vector<Barrel>& barrels() const { return barrels_; }
    const std::vector<Pool>& pools() const { return pools_; }
    const std::vector<Drop>& drops() const { return drops_; }
    const std::vector<WallFire>& wall_fires() const { return wall_fires_; }
    const std::vector<Burning>& burning() const { return burning_; }
    const std::vector<Spread>& spreads() const { return spreads_; }
    const Pool* pool(std::uint32_t serial) const;
    bool on_fire(ObjId id) const;
    // Pool pixels that mirrored something in the last frame drawn.
    std::size_t reflected() const { return reflected_; }
    // Wall fires (or their stains) shown in the last frame drawn.
    std::size_t walls_shown() const { return walls_shown_; }

    // How much fuel there is at (x, y) of pool `p`: 1 on its edge, more
    // inside (metaballs of its lobes, warped), with a ragged edge.
    static double field(const Pool& p, double x, double y);
    // The same from the pool's grid (bilinear), once built.
    static double field_at(const Pool& p, double x, double y);
    // How strongly pool `p` burns at (x, y) at game tick `tick`: 0 where the
    // fire has not come or has gone out, up to 1.
    double fire_at(const Pool& p, double x, double y, std::uint32_t tick) const;

private:
    enum Tone : std::uint8_t {
        kRim, kBody, kDeep, kSheen, kStream, kStreamLight, kHole, kWet, kHot, kAsh, kAshDark, kSoot, kMirror, kMirrorFaint,
        kFlameWhite, kFlameYellow, kFlameOrange, kFlameDeep, kFlameRed, kFlameDark, kFlameSmoke, kTones
    };
    struct Rect {
        int left, top, right, bottom;
    };

    Pool* pool_of(std::uint32_t serial);
    Pool& new_pool(std::int32_t z);
    void step(const World& world, std::uint32_t tick);
    void step_drops(const World& world, std::uint32_t tick);
    void ignite(const World& world, Pool& p, double x, double y, std::uint32_t tick);
    void blow_up(const World& world, const Barrel& b, std::uint32_t tick);
    void splash_floor(const World& world, const Drop& d, double x, double y, std::int32_t z, std::uint32_t tick);
    void splash_wall(const World& world, const Drop& d, const Solid& s, int face, double x, double y, double z,
                     std::uint32_t tick);
    void grow(Barrel& b);
    void spread(const World& world, std::uint32_t tick);
    Pool& pool_for(Barrel& b, bool trail);
    void place_jet(Barrel& b, Hole& h);
    void lay_trail(Barrel& b);
    void catch_fire(const World& world, const Pool& p, std::uint32_t tick);
    static double arrival(const Pool& p, double x, double y);
    double reached(const Pool& p, double x, double y, std::uint32_t tick) const;
    // fire_at where the fire came `come` units past (x, y) (reached).
    double fire_from(const Pool& p, double x, double y, std::uint32_t tick, double come) const;
    bool opaque(const DrawItem& d, int sx, int sy) const;
    const std::vector<std::uint8_t>& mask_of(const ShapeFrame& f) const;
    void build_field(Pool& p, std::uint32_t tick);
    void draw_pool(Surface8& screen, const DrawItem& d, const Pool& p);
    void mark_floor(const Surface8& screen, const DrawItem& d);
    void draw_barrel(Surface8& screen, const DrawItem& d, const Barrel& b);
    void draw_patch(Surface8& screen, const Pool& p, std::pair<int, int> patch);
    void draw_wall_fire(Surface8& screen, const DrawItem& d, const WallFire& w);
    void draw_person(Surface8& screen, const DrawItem& d, const Burning& b);
    void reflect(Surface8& screen);
    // One column of fire at screen x `px`: `base[k]` is how strongly the
    // row `bottom - k` burns; flames rise from it up to `height` pixels.
    void fire_column(Surface8& screen, int px, int bottom, const std::vector<float>& base, float height,
                     std::uint32_t seed);
    void put(Surface8& screen, int x, int y, Tone tone, bool glows = false);
    float fire_strength(const Pool& p, std::uint32_t tick) const;

    std::vector<Barrel> barrels_;
    std::vector<Pool> pools_;
    std::vector<Drop> drops_;
    std::vector<WallFire> wall_fires_;
    std::vector<Burning> burning_;
    std::vector<Spread> spreads_;
    std::vector<ObjId> burns_;
    int map_ = -1;
    std::uint32_t now_ = 0;
    bool started_ = false;
    std::uint32_t next_serial_ = 1;
    std::uint32_t next_seed_ = 0x6A09E667u;

    // Per frame.
    int view_left_ = 0, view_top_ = 0;
    std::array<std::array<std::uint8_t, 256>, kTones> tone_{};
    Palette tone_palette_{};
    bool tone_ready_ = false;
    // The patches (pool, patch) to draw after the item of the list at an index.
    std::unordered_map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> fire_after_;
    std::vector<std::pair<std::size_t, std::size_t>> fire_first_;
    // The wall fires to draw after the item of the list at an index.
    std::unordered_map<std::size_t, std::vector<std::size_t>> walls_after_;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> glowing_;
    // Per pixel, stamped with the frame: the fire already drawn (how hot, its
    // colour, still there if unchanged) and the flames' noise there for a
    // seed (the same for every patch, wall or body drawn there), side by side
    // for the columns of fire_column.
    struct Flame {
        std::uint32_t heat_frame = 0, noise_frame = 0, seed = 0;
        float heat = 0, noise = 0;
        std::uint8_t colour = 0;
    };
    std::vector<Flame> flames_;
    // Per pixel, stamped with the frame: the floors at a pool's level and the
    // pools' own pixels (the colour drawn, still there if unchanged).
    std::uint32_t frame_ = 0;
    std::vector<std::uint32_t> floor_frame_, pool_frame_;
    std::vector<std::uint8_t> floor_colour_, pool_colour_;
    std::vector<Rect> mirror_areas_;  // the pools' screen boxes, raised by what they may mirror
    std::size_t reflected_ = 0;
    std::size_t walls_shown_ = 0;
    std::vector<float> column_;
    mutable std::unordered_map<const ShapeFrame*, std::vector<std::uint8_t>> masks_;
};

}  // namespace crusader
