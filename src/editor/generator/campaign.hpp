#pragma once
// Crusader: No Limit, generated levels written into an editor project (a mod
// folder): each level
// takes a free map of FIXED.DAT, and its exit teleporter leads to the next
// level's start; the last one leads back to the first.

#include "core/typeflags.hpp"
#include "editor/generator/biome.hpp"
#include "editor/generator/generator.hpp"
#include "editor/generator/manifest.hpp"
#include "editor/generator/mission.hpp"
#include "editor/project.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace crusader::editor::gen {

// The original data the generator learns from (never the mod's files).
struct GameData {
    std::vector<ShapeInfo> shapes;
    std::vector<Glob> globs;
    std::vector<Level> maps;
    std::vector<bool> scripted;  // by shape: a usecode class of that number exists
    std::vector<std::string> class_names;  // by shape: its usecode class name (EUSECODE.FLX entry 1), or empty
    std::vector<Biome> biomes;
    // By wall shape (walls and posts, 96 high): its frame drawn as a black
    // frame, for a building's near side; absent when it has none.
    std::map<std::uint16_t, std::uint8_t> black_frames;
    static GameData load(const std::filesystem::path& data);
};

struct GeneratedLevel {
    int map = 0;
    Result result;
    ManifestLevel entry;  // its seed and options, for NOLIMIT.JSON
};

// Generates `levels` levels from `seed` (level k uses seed + k) into free
// maps of the project, stores them (not saved). `biome` -1 draws each one.
// Empty when the project has too few free maps; `error` says why.
std::vector<GeneratedLevel> generate_levels(Project& project, const GameData& data, std::uint32_t seed, int levels,
                                            int biome, std::string& error);

// The levels of a manifest generated again on their maps (stored, not
// saved): what a shared NOLIMIT.JSON gives back. Empty on an error (another
// generator version, a seed that fails).
std::vector<GeneratedLevel> regenerate_levels(Project& project, const GameData& data, const Manifest& manifest,
                                              std::string& error);

// The demo mission (mission.hpp) from the rooms of `source_map`, on a free
// map of the project (stored, not saved); its exit leads back to its start.
std::vector<GeneratedLevel> generate_mission_levels(Project& project, const GameData& data, std::uint32_t seed,
                                                    int source_map, std::string& error);

// Adds the levels to the mod's NOLIMIT.JSON (written).
void record_levels(const std::filesystem::path& mod, const std::vector<GeneratedLevel>& levels);

}  // namespace crusader::editor::gen
