#pragma once
// An editor project: the original game data, read only, and a mod folder
// that holds the edited files under the same names (STATIC/FIXED.DAT,
// STATIC/GLOB.FLX). The game loads such a folder with --mod: a file there
// replaces the one of the data folder (game_file, src/game/game.hpp).

#include "editor/level.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace crusader::editor {

// `mod`/`relative` when that file exists, else `data`/`relative`.
std::filesystem::path resolve_file(const std::filesystem::path& data, const std::filesystem::path& mod,
                                   const std::string& relative);

class Project {
public:
    Project(std::filesystem::path data, std::filesystem::path mod);

    const std::filesystem::path& data() const { return data_; }
    const std::filesystem::path& mod() const { return mod_; }
    std::filesystem::path file(const std::string& relative) const { return resolve_file(data_, mod_, relative); }

    // Maps (FIXED.DAT entries; 1..255 are usable map numbers).
    std::size_t map_slots() const { return fixed_.entries.size(); }
    bool map_used(std::size_t map) const { return map < fixed_.entries.size() && !fixed_.entries[map].empty(); }
    std::vector<std::size_t> used_maps() const;
    // First empty slot from 1, 0 when none.
    std::size_t free_map() const;
    Level load_map(std::size_t map) const;
    void store_map(std::size_t map, const Level& level);

    // Globs (GLOB.FLX entries; glob 0 means "none" for an egg).
    std::size_t glob_count() const { return globs_.entries.size(); }
    Glob glob(std::size_t index) const;
    const std::vector<std::uint8_t>& glob_bytes(std::size_t index) const { return globs_.entries.at(index); }
    void store_glob(std::size_t index, const Glob& glob);
    // A new glob number (an empty slot, or a new entry at the end).
    std::size_t add_glob(const Glob& glob);

    bool dirty() const { return fixed_dirty_ || globs_dirty_; }
    // Writes FIXED.DAT at the next save even if no map changed.
    void touch_maps() { fixed_dirty_ = true; }
    // Writes the changed archives into the mod folder (created if needed).
    void save();

private:
    std::filesystem::path data_, mod_;
    FlexEntries fixed_, globs_;
    bool fixed_dirty_ = false, globs_dirty_ = false;
};

}  // namespace crusader::editor
