#pragma once
// Where the game's files are read (port addition). A file of the data folder
// ("STATIC/GUMPS.FLX", "FLICS/01A.IFF"...) is replaced by the copy with the
// same path:
//   1. in the folder of the language picked (languages/<id>/, language.hpp):
//      a translation's pictures, fonts, scripts, subtitles...;
//   2. in the mod folder (the level editor's maps, game_file in game.hpp);
// else it is the data folder's.

#include <filesystem>
#include <string>

namespace crusader {

class DataFiles {
public:
    explicit DataFiles(std::filesystem::path data, std::filesystem::path mod = {}, std::filesystem::path language = {});

    // `relative` ("STATIC/FONTS.FLX") in the first folder that has it, else
    // in the data folder (which may not have it either).
    std::filesystem::path find(const std::string& relative) const;

    const std::filesystem::path& data() const { return data_; }
    const std::filesystem::path& mod() const { return mod_; }
    const std::filesystem::path& language() const { return language_; }

private:
    std::filesystem::path data_, mod_, language_;
};

}  // namespace crusader
