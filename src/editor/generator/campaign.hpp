#pragma once
// Generated levels written into an editor project (a mod folder): each level
// takes a free map of FIXED.DAT, and its exit teleporter leads to the next
// level's start; the last one leads back to the first.

#include "core/typeflags.hpp"
#include "editor/generator/biome.hpp"
#include "editor/generator/generator.hpp"
#include "editor/project.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace crusader::editor::gen {

// The original data the generator learns from (never the mod's files).
struct GameData {
    std::vector<ShapeInfo> shapes;
    std::vector<Glob> globs;
    std::vector<Level> maps;
    std::vector<bool> scripted;  // by shape: a usecode class of that number exists
    std::vector<Biome> biomes;
    static GameData load(const std::filesystem::path& data);
};

struct GeneratedLevel {
    int map = 0;
    Result result;
};

// Generates `levels` levels from `seed` (level k uses seed + k) into free
// maps of the project, stores them (not saved). `biome` -1 draws each one.
// Empty when the project has too few free maps; `error` says why.
std::vector<GeneratedLevel> generate_levels(Project& project, const GameData& data, std::uint32_t seed, int levels,
                                            int biome, std::string& error);

}  // namespace crusader::editor::gen
