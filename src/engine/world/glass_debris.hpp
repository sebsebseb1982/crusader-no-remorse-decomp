#pragma once
// Glass debris (port addition; Enhancements screen, GameConfig::glass_debris):
// a glass wall shatters into pieces that fly the way the shot went, and
// blasts blow them about.
//
// In the original, a glass wall or divider (classes GLASS_EW 0x27B, GLASS_NS
// 0x2F3, GLDIV_EW 0x541, GLDIV_NS 0x542) breaks at its first hit: its event 6
// ("gotHit") plays frames 1 to 3, lays a pile of broken glass (0x27A, 0x349)
// on each side of it where one fits, and leaves its foot (0x348, 0x2F2). The
// piles never move: the blasts throw only what has a weight, and theirs is 0.
//
// With this option a pile is shown as the pieces of glass it is made of: its
// sprite cut into some fifty irregular pieces (add_pile_art), each a sprite
// of its own, drawn and ordered among the items (DisplayExtras) while the
// pile itself is hidden.
//  - A wall that breaks under a shot throws its piles' pieces out of the
//    pane, from all along it and from up to 56 units high, the way the shot
//    went (an explosive one twice as hard; a blast that breaks it from beside,
//    away from itself): they fly, turning over, fall, bounce, slide to a stop
//    and lie scattered on the floor.
//  - A blast (a fire type that splashes) throws the pieces within its reach
//    up and away from it, the harder the nearer, and shatters the piles it
//    reaches that were still whole. A shot of another kind that hits a pile
//    kicks the pieces round where it hit.
//  - A piece stops against what is solid, bouncing back a little, and falls
//    off the edge of a floor onto the one below.
//
// The pile items stay in the world: once a pile's pieces have come to rest,
// the pile moves to their middle, which is what a saved game keeps. The
// pieces are not saved: after a load or a map change, or with the option
// switched off, the piles show again where they are. Only those moves change
// the world; the pieces use their own numbers, never the game's random
// generators.

#include "core/shape.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace crusader {

// A glass wall that breaks (its class lays piles of glass).
bool is_breakable_glass(std::uint16_t shape);
// A pile of broken glass.
bool is_glass_pile(std::uint16_t shape);

class GlassDebris {
public:
    static constexpr std::size_t kMaxPieces = 4000;
    static constexpr int kWatchTicks = 180;  // a hit wall that has not broken by then is forgotten

    // A piece of a pile's sprite.
    struct Chunk {
        std::uint16_t shape = 0;
        std::vector<std::uint8_t> data;  // the frame's runs
        ShapeFrame frame;                // drawn with its anchor at the piece's place
        double ox = 0, oy = 0;           // its place on the floor from the pile's corner (usecode units)
        int pixels = 0;
    };

    struct Piece {
        const Chunk* chunk = nullptr;
        ObjId pile = 0;
        double x = 0, y = 0, z = 0;     // usecode units
        double vx = 0, vy = 0, vz = 0;  // units a tick
        bool resting = false;
        bool mirrored = false;
        int spin = 0;  // ticks between turns over in flight
        int age = 0;   // ticks since it was last thrown
        // The floor under it, found at (gx, gy).
        double gx = 0, gy = 0;
        std::int32_t ground = 0;
        bool ground_known = false;
        mutable Item proxy;  // stands for it in the display list
    };

    // A pile shown as its pieces.
    struct Shattered {
        ObjId pile = 0;
        std::uint16_t shape = 0;
        bool settled = true;  // its pieces at rest and the pile moved to them
    };

    // A wall hit, watched until it breaks.
    struct Watch {
        ObjId wall = 0;
        std::uint16_t shape = 0;
        double ux = 0, uy = 0;  // the way its pieces will go (unit)
        double strength = 1;    // 1 a shot, more for an explosive or a blast
        std::vector<ObjId> piles_before;
        std::uint32_t tick = 0;
    };

    // Cuts the sprite of the pile shape `shape` into pieces (once per shape).
    void add_pile_art(std::uint16_t shape, const ShapeFrame& frame);

    // WorldHooks::item_hit: an item, not an NPC, takes a shot or a blast.
    void hit(const World& world, const ItemHit& hit, std::uint32_t now);

    // Moves the pieces on to game tick `now`: the walls that broke throw
    // theirs, the pieces fly and come to rest, and the piles follow them.
    void advance(World& world, std::uint32_t now);

    // The pieces to draw among the items, and the piles they stand for.
    void extras(DisplayExtras& out) const;

    void clear();
    bool empty() const { return pieces_.empty() && watches_.empty(); }
    const std::deque<Piece>& pieces() const { return pieces_; }
    const std::vector<Shattered>& shattered() const { return shattered_; }
    const std::vector<Watch>& watches() const { return watches_; }
    std::size_t chunk_count(std::uint16_t shape) const;

private:
    double unit();  // 0..1, the debris' own numbers
    void new_map(const World& world);
    void break_wall(World& world, const Watch& w);
    // Shows `pile` as its pieces, where they lie in it; false if it cannot.
    bool shatter(const World& world, ObjId pile);
    bool is_shattered(ObjId pile) const;
    void throw_piece(Piece& p, double vx, double vy, double vz);
    void blow(const World& world, const World::Point& at, double reach, std::uint32_t now);
    void kick(const World& world, ObjId pile, const World::Point& from, const World::Point& to);
    void step(const World& world, Piece& p);
    bool find_ground(const World& world, Piece& p);
    void settle(World& world);

    std::deque<Chunk> chunks_;
    std::deque<Piece> pieces_;
    std::vector<Shattered> shattered_;
    std::vector<Watch> watches_;
    int map_ = -1;
    std::uint32_t tick_ = 0;
    std::uint32_t next_seed_ = 0x6C8E9CF5u;
    // The last blast (one per blast, not per item it reaches).
    World::Point last_blast_{};
    std::uint32_t last_blast_tick_ = 0xFFFFFFFFu;
};

}  // namespace crusader
