#pragma once
// Crusader: No Limit, the seeds of a mod's generated levels: NOLIMIT.JSON in
// the mod folder, written at each generation, so that a level can be shared
// and generated again. A seed gives the same level only with the same
// generator version and the same game data:
//
//   {
//     "generator": "Crusader: No Limit",
//     "version": 2,
//     "levels": [
//       {"map": 12, "seed": 42, "biome": -1, "rooms": 0, "locks": -1, "first_card": 1, "next_map": 18,
//        "biome_name": "floor 0x014, walls 0x008 / 0x009 (maps 14 28 149 223)", "room_count": 6}
//     ]
//   }
//
// "biome", "rooms" and "locks" are the options (-1, 0 and -1: drawn from
// the seed), "first_card" the first key card number;
// "biome_name" and "room_count" what the seed gave.

#include "editor/generator/generator.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace crusader::editor::gen {

constexpr const char* kGeneratorName = "Crusader: No Limit";
// Raised whenever a seed would give another level than before.
constexpr int kGeneratorVersion = 2;  // 2: locks and keys
constexpr const char* kManifestFile = "NOLIMIT.JSON";

struct ManifestLevel {
    int map = 0;
    std::uint32_t seed = 0;
    int biome = -1;
    int rooms = 0;
    int locks = -1;
    int first_card = 1;
    int next_map = 0;
    std::string biome_name;
    int room_count = 0;

    Options options() const;
};

struct Manifest {
    int version = kGeneratorVersion;
    std::vector<ManifestLevel> levels;

    // Nullopt when the folder has no manifest, or not a readable one.
    static std::optional<Manifest> read(const std::filesystem::path& mod, std::string* error = nullptr);
    void write(const std::filesystem::path& mod) const;
    std::string to_json() const;
    // Adds the level, replacing the one of the same map.
    void set(const ManifestLevel& level);
    const ManifestLevel* find(int map) const;
};

// The manifest entry of a level generated with `options`.
ManifestLevel manifest_level(const Options& options, const Result& result, const std::vector<Biome>& biomes);

}  // namespace crusader::editor::gen
