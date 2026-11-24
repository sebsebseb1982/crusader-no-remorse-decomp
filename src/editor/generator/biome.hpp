#pragma once
// Biomes of the procedural generator: the floor and wall families that the
// designers combined in the globs of the original maps (docs/GENERATEUR.md).
//
// The pieces of a glob lie on a 16-unit grid (the TYPEFLAG footprint unit),
// positioned by their far corner (largest x and y, lowest z):
//   floors  16 x 16 x 0 footprint units (256 x 256 usecode units, flat),
//   walls   12 high (96 units: a storey), 1 unit thick, 1, 2, 4, 8 or 16
//           units long, one shape per length and direction.
// A biome is a floor shape (with the frames seen with it) and, for each
// direction, the walls of each length that its globs use together. Walls on
// the far side of a room (its north and west sides, at the low edge of a
// cell) show their face; the designers drew those on the near side, facing
// the camera, with a black frame (frame 2 of walls 0x08 / 0x09 / 0x23 /
// 0x24 at the high edge of the cells) so that the room stays visible.

#include "core/typeflags.hpp"
#include "editor/level.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crusader::editor::gen {

constexpr std::int32_t kUnit = 16;       // footprint unit, in usecode units
constexpr std::int32_t kBlock = 256;     // a 16 x 16 floor tile
constexpr std::int32_t kStorey = 96;     // wall height
constexpr int kWallLengths[5] = {1, 2, 4, 8, 16};

struct Piece {
    std::uint16_t shape = 0;
    std::uint8_t frame = 0;
    bool valid() const { return shape != 0; }
    auto operator<=>(const Piece&) const = default;
};

enum Axis { kAlongX = 0, kAlongY = 1 };
enum Side { kBack = 0, kFront = 1 };  // far (north, west) or near (south, east) side of a room

struct Biome {
    Piece floor;                            // the most used frame
    std::vector<Piece> floor_variants;      // frames seen with these walls, most used first
    std::array<std::array<std::array<Piece, 5>, 2>, 2> walls{};  // [side][axis][length index]; 1: a post
    std::vector<int> maps;                  // original maps that use it, most first
    int globs = 0;                          // original globs that have the floor and both long walls

    std::string name() const;
    // A straight wall of `units` footprint units can be made of its walls.
    bool can_build(int side, int axis, int units) const;
    // The walls that make a run of `units`, longest first (empty if none).
    std::vector<Piece> wall_run(int side, int axis, int units) const;
};

// What a piece is to the generator.
bool is_floor_tile(const ShapeInfo& info);
// A wall (12 high, 1 thick): its direction and length index (kWallLengths),
// false otherwise. A 1 x 1 post counts for both directions.
bool wall_kind(const ShapeInfo& info, int& axis, int& length_index);

struct BiomeSource {
    const std::vector<ShapeInfo>& shapes;
    const std::vector<Glob>& globs;   // GLOB.FLX, by glob number
    const std::vector<Level>& maps;   // FIXED.DAT, by map number (empty levels for none)
    // Shapes that have a usecode class (EUSECODE.FLX): a scripted floor or
    // wall (a death floor, a breakable wall) is no plain piece.
    const std::vector<bool>& scripted;
    // The frame is black (a near-side wall); may be empty.
    std::function<bool(Piece)> black;
};

// The biomes of the original globs, most used first: those whose walls can
// build any straight run (both long walls and posts), in at least
// `min_globs` globs, at most `max_biomes`.
std::vector<Biome> learn_biomes(const BiomeSource& source, std::size_t max_biomes = 12, int min_globs = 12);

}  // namespace crusader::editor::gen
