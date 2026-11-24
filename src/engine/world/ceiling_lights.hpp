#pragma once
// Port addition: lights in the rooms' ceilings, for the 3D lighting
// (GameConfig::lighting, lighting.hpp). The original draws no ceiling and has
// no light there: the rooms are lit only by the light painted in the sprites.
// With the 3D lighting, a few lamps placed by hand under each room's ceiling
// light the floor in pools and make what stands there cast shadows. Closets,
// recesses and the outside get none. They are not drawn: the ceiling is not.
//
// They are read from editable JSON files (tools/lights/README.md), in the
// folder `ceiling_lights` next to the executable (a mod's own folder first):
//   kinds.json    each kind of lamp: colour, strength, reach, how much it
//                 lowers the ambient light round it (built-in ones without
//                 the file);
//   mapNNN.json   a map's lamps, room by room (NNN its number: 001...).

#include "core/json.hpp"
#include "engine/world/lighting.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace crusader {

// A kind of lamp (kinds.json).
struct CeilingLightKind {
    std::string name;  // "warm", "neon"...
    Vec3 colour;       // 0..1
    float strength;    // times the colour, at no ambient light (brighter with it)
    float radius;      // reach, lighting-space units
    float dim;         // 0..1: how much it lowers the ambient light round it
};

struct CeilingLight {
    int kind = 0;               // in the kinds
    std::int32_t x = 0, y = 0;  // world units
    std::int32_t z = 0;         // z units, the bulb
    float radius = 0;           // lighting-space units; 0: the kind's
};

// Where the files are looked for; the first folder that has a file wins.
void set_ceiling_lights_dirs(std::vector<std::filesystem::path> dirs);
// Forgets the files read: the next calls read them again.
void reload_ceiling_lights();

// The kinds (kinds.json, else built in): warm, neon, sodium, dim, red first.
const std::vector<CeilingLightKind>& ceiling_light_kinds();
// The ceiling lights of `map` (none when it has no file).
const std::vector<CeilingLight>& ceiling_lights(int map);

// A kinds.json document's kinds (the built-in ones replaced or added to).
std::vector<CeilingLightKind> parse_ceiling_light_kinds(const JsonValue& doc);
// A map file's lights: rooms[].lights[] = [x, y, z (, radius (, kind))], a
// room's "kind" for its lights without one.
std::vector<CeilingLight> parse_ceiling_lights(const JsonValue& doc, const std::vector<CeilingLightKind>& kinds);

// The lighting's lights of `lights` (lighting space) at the ambient light
// `ambient` (LightingOptions::ambient): brighter, as the lamps, the brighter
// the ambient.
void add_ceiling_lights(const std::vector<CeilingLight>& lights, float ambient, std::vector<PointLight>& out);

}  // namespace crusader
