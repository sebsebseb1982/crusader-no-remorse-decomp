#pragma once
// Swept-box collisions (ITEMFIND.C, segment 10E8: 032E per item, 0B39 sweep,
// 162F step up / down; ScummVM's CurrentMap::sweepTest).
//
// Boxes in usecode units: an item at (x, y, z) with footprint (fx, fy, fz)
// spans x - 16 fx + 1 .. x, y - 16 fy + 1 .. y, z .. z + h - 1 with
// h = 8 fz, or 1 for a flat shape that is not fixed. A mirrored item (flag
// 0x20) swaps its x and y extents.
//
// Collision masks (SEG1018 0000): 0x8000, plus 1 for solid shapes, 0x20 for
// damaging ones. A contact blocks when the two masks share a bit below 0x100.

#include "engine/world/world.hpp"

#include <cstdint>
#include <vector>

namespace crusader {

// Sweep flags (the original's argument bits).
enum SweepFlag : std::uint8_t {
    kSweepSupportOnly = 0x01,  // a solid item right under the end point counts as support
    kSweepNoGround = 0x04,     // the end point need not stand on anything
    kSweepGroundSlack = 0x08,  // support may be up to 9 units below
    kSweepStepUp = 0x10,       // when blocked, try 8 then 9 units higher (stairs)
};

struct SweepHit {
    ObjId item;
    std::int32_t dx, dy, dz;  // movement done when touching it
    bool blocking;
};

struct SweepResult {
    bool ok = false;           // reached the end and, if required, stands on support
    World::Point end{};        // where the box stops
    ObjId blocker = 0;         // first blocking item
    std::uint16_t axes = 0;    // blocked axes: 1 x, 2 y, 4 z
    bool off_map = false;      // x or y below 0, or the end out of the fast area (+0x33)
    std::int32_t ground_gap = 0;
    bool supported = false;
    std::vector<SweepHit> hits;     // in the order met (the original's list is newest first)
    World::Point delta{};           // movement asked for (the original's +0x16)
    World::Point done{};            // movement up to the blocker, all of it when none (+0x10)
    // With a goal item (the original's +0x2D): the goal became the blocker
    // while no other was (+0x49, cleared when a closer one replaces it), and
    // the goal was a blocker at some point (+0x50).
    bool goal_first = false;
    bool goal_hit = false;
};

// Collision mask of a shape.
std::uint16_t collision_mask(const ShapeInfo* info);

// Moves the box of `shape` (as `mover`, which is ignored) from `start`
// towards `end` (ITEMFIND.C 0B39). `mask` overrides the shape's mask when
// non-zero.
SweepResult sweep(const World& world, std::uint16_t shape, ObjId mover, const World::Point& start,
                  const World::Point& end, std::uint8_t flags, std::uint16_t mask = 0, ObjId goal = 0);

// ITEMFIND.C 00D9 with SEG1018 00FD: whether a box of w x d x h at `at`
// (x - w + 1 .. x, y - d + 1 .. y, z .. z + h - 1, at most 255) is free: the
// items whose boxes overlap it share no bit of `mask` below 0x8000. Nothing
// is excluded, not even an item standing there. The items' boxes ignore their
// mirroring, and the candidates are the grid items up to 0x100 beyond the box.
// A box reaching below 0 is never free; one spanning z 0 to 255 always is.
// `first_solid` receives the first solid item met, 0 if none. `ignore` (port
// only) stands for an item the original takes out of the world for the test.
bool box_free(const World& world, std::int32_t w, std::int32_t d, std::int32_t h, std::uint16_t mask,
              const World::Point& at, ObjId* first_solid = nullptr, ObjId ignore = 0);
// The same with the box and mask of `shape` (ITEMFIND.C 1874).
bool position_free(const World& world, std::uint16_t shape, const World::Point& at, ObjId* first_solid = nullptr,
                   ObjId ignore = 0);

// Item::I_legalCreateAtPoint (10A8:104E): a new item of `shape` / `frame`
// at `at` when its box is free there (shapes up to 0x7FF); its id, or 0.
ObjId legal_create_at(World& world, std::uint16_t shape, std::uint8_t frame, const World::Point& at);

// Walking move (ITEMFIND.C 162F): on level moves the actor follows the ground
// down by up to 9 units and, with kSweepStepUp, climbs steps of 8 or 9 (not
// when the goal blocks first). Refused, the result is that of the last sweep
// tried, as the original's sweep object keeps it.
SweepResult try_move(const World& world, ObjId mover, const World::Point& end, std::uint8_t flags);
// Same from `start` instead of the mover's position (simulated moves).
SweepResult try_move(const World& world, ObjId mover, const World::Point& start, const World::Point& end,
                     std::uint8_t flags, ObjId goal = 0);

// ITEMFIND.C 123A, after a sweep of `mover` (that it describes): moves the
// mover to `r.end` and sends the contact events, in this order:
//  - with a blocker: two stackable items of the same shape (families 2 and 9,
//    9 with the same frame too) merge instead (1088:0000, see the .cpp);
//    otherwise the mover gets event 5 ("hit") and the blocker event 6
//    ("gotHit", ITEM.C 275F), both with the other's id and a force of
//    (|dx / 4| + |dy / 4| + |dz| + the largest of them) / 2 from `r.delta`;
//  - then each item touched before the stop (on one axis at least), newest
//    first, the blocker excepted: the same pair of events with force 0. The
//    item the mover stands on (gap 0) counts, which is how floor pads,
//    teleporters and mines notice who walks on them.
// The original queues the events and starts them when all are known; event
// processes are not run on the spot, so starting them in turn is the same.
// The actor and legalMoveToPoint paths call leave_surface (ITEM.C 2970,
// gravity.hpp) just before this.
void finish_move(World& world, ObjId mover, const SweepResult& r);

}  // namespace crusader
