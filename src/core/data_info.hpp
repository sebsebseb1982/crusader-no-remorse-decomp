#pragma once
// Version and language of the game data, read from CRUSADER.EXE. The version
// is in the banner ("CRUSADER:  No Remorse V1.08"; DS:E8ED4 in the French 1.08
// executable, V1.23 in the US one, V1.01 in the first English one, V1.07 in
// the German one); the language is told by a text of the executable that
// exists in each translation. Without the executable, the size of the
// usecode's intrinsic table (USECODE/UNKCOFF.DAT) tells the French, US and
// 1.01 data apart; the German table is the French one, so German data without
// its executable is taken for French (its texts then read in French).
//
// The German 1.07 executable is the French 1.08 one translated: the same code
// and tables, the same usecode intrinsics (UNKCOFF.DAT, UNKDS.DAT and
// OVERLOAD.DAT are byte for byte the French ones), its strings in German. Its
// code differs only in the places of a few buttons, for its German pictures
// (main menu, options, help, the quick save and load questions, the
// difficulty screen as in the US one), the keyboard of the save names (QWERTZ
// and A O U umlauts instead of AZERTY) and the repeat delay of the joystick.
// Its usecode is the French one with German texts (and two signs' frames),
// its data German pictures, voices and movies: nothing is cut from the game
// itself.

#include "core/language.hpp"

#include <filesystem>
#include <string>

namespace crusader {

struct DataInfo {
    std::string version;   // "1.08", "1.23", "1.01", "1.07"; empty when unknown
    std::string language;  // "FR", "US" (English, 1.01 included) or "DE", empty when unknown

    bool known() const { return !language.empty(); }
    bool us() const { return language == "US"; }
    bool german() const { return language == "DE"; }
    // The English 1.01 data: its usecode numbers the intrinsics its own way.
    bool v101() const { return us() && version == "1.01"; }
    // The language of the texts: English for the US data, German for the
    // German data, else French.
    Language text_language() const {
        return us() ? Language::English : german() ? Language::German : Language::French;
    }
};

DataInfo detect_data_info(const std::filesystem::path& data);

}  // namespace crusader
