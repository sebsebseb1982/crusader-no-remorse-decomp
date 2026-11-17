#pragma once
// Language of the game data. The French and US releases ship their texts in
// CRUSADER.EXE (item names, messages, the shop of the weasel) and in the data
// files; the port prints the texts of the executable itself. Game sets the
// language when it loads the data (French until then).
//
// Port addition: those texts, and the ones of the port's own screens, are
// written in English in the code (tr("BACK")) and translated by the language
// folders of the "languages" folder (docs/TRANSLATING.md): languages/<id>/
// holds port.txt, the texts (translation.hpp; fr/port.txt: the French
// executable's), and may hold data files that replace the data's
// (data_files.hpp). The player picks the language in the enhancements window
// ("language" in CRUSADER.CFG): AUTO, the default, takes the texts of the
// data's (fr or en) and no data files. The data's language still places the
// buttons (us_version).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crusader {

enum class Language { French, English };

Language text_language();
void set_text_language(Language language);

// The translation of an English text in the language in use: the text itself
// when the language file has none (or there is no file). The pointer stays
// valid when another language is picked.
const char* tr(const char* english);

// The level editor's own texts, which are not in the language files: the
// French or the English one, after the data's language.
inline const char* tr(const char* french, const char* english) {
    return text_language() == Language::English ? english : french;
}

// The data in use is the US version (the only English one supported): some
// screens of the US executable place their buttons for its English images.
inline bool us_version() { return text_language() == Language::English; }

// The languages offered: "" (AUTO, the data's), then the folders of the
// languages folder (ids: their names in lower case), by id.
struct LanguageChoice {
    std::string id;           // as written in CRUSADER.CFG
    std::string name;         // as shown (code page 850): port.txt's "@name"
    bool data_files = false;  // the folder has data files (a subfolder)
};
void set_languages_dir(const std::filesystem::path& dir);
std::vector<LanguageChoice> language_choices();
// Picks a language by id (any case); an unknown one, or a port.txt that
// cannot be read, gives AUTO (with a message on stderr). The texts change at
// once, and are read again after each pick, so a translator sees the changes.
void select_language(std::string_view id);
const LanguageChoice& selected_language();

// The folder of language `id` when it has data files, else empty. The game
// reads them at start-up (Game's constructor), and says which it read: a
// language picked afterwards needs a restart for its data files.
std::filesystem::path language_data_folder(std::string_view id);
void set_loaded_data_language(std::string_view id);
bool language_needs_restart();

}  // namespace crusader
