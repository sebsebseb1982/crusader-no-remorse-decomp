#include "core/language.hpp"

#include "core/translation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <system_error>

namespace crusader {

namespace {

Language g_language = Language::French;

std::filesystem::path g_languages_dir;
LanguageChoice g_choice{"", "AUTO", false};
// The id of the language whose data files the game loaded ("" for none).
std::string g_loaded_data;
// The texts read, by id (null: no such language). Every translation read is
// kept in g_read so that the texts handed out stay valid.
std::map<std::string, const Translation*, std::less<>> g_loaded;
std::vector<std::unique_ptr<Translation>> g_read;

constexpr const char* kTexts = "port.txt";

// @orig none: language folders of the port
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The language folders, by id.
// @orig none: language folders of the port
std::vector<std::pair<std::string, std::filesystem::path>> language_folders() {
    std::vector<std::pair<std::string, std::filesystem::path>> folders;
    std::error_code error;
    if (g_languages_dir.empty() || !std::filesystem::is_directory(g_languages_dir, error)) return folders;
    for (const auto& entry : std::filesystem::directory_iterator(g_languages_dir, error)) {
        if (entry.is_directory(error)) folders.emplace_back(lower(entry.path().filename().string()), entry.path());
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

// A folder with data files: one with a subfolder (STATIC, FLICS, USECODE...).
// @orig none: language folders of the port
bool has_data_files(const std::filesystem::path& folder) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.is_directory(error)) return true;
    }
    return false;
}

// The texts of a folder: its port.txt, none when it has no such file.
// @orig none: language folders of the port
Translation read_texts(const std::filesystem::path& folder, std::vector<std::string>* warnings) {
    std::error_code error;
    const std::filesystem::path path = folder / kTexts;
    return std::filesystem::exists(path, error) ? Translation::load(path, warnings) : Translation{};
}

// @orig none: language folders of the port
std::string name_of(const std::string& id, const Translation& t) {
    if (!t.name().empty()) return t.name();
    std::string name = id;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return name;
}

// @orig none: language folders of the port
std::filesystem::path folder_of(std::string_view id) {
    for (const auto& [folder_id, path] : language_folders()) {
        if (folder_id == id) return path;
    }
    return {};
}

// The texts of `id`, read on first use; null when there is no such folder or
// its file cannot be read.
// @orig none: language folders of the port
const Translation* translation(std::string_view id) {
    if (const auto it = g_loaded.find(id); it != g_loaded.end()) return it->second;
    const Translation* found = nullptr;
    if (const std::filesystem::path folder = folder_of(id); !folder.empty()) {
        try {
            std::vector<std::string> warnings;
            g_read.push_back(std::make_unique<Translation>(read_texts(folder, &warnings)));
            for (const std::string& w : warnings)
                std::fprintf(stderr, "%s: %s\n", (folder / kTexts).string().c_str(), w.c_str());
            found = g_read.back().get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    g_loaded.emplace(std::string(id), found);
    return found;
}

}  // namespace

Language text_language() { return g_language; }

void set_text_language(Language language) { g_language = language; }

// @orig none: language folders of the port
const char* tr(const char* english) {
    const std::string_view id = !g_choice.id.empty()                ? std::string_view(g_choice.id)
                                : g_language == Language::English ? std::string_view("en")
                                                                  : std::string_view("fr");
    if (const Translation* t = translation(id)) {
        if (const char* text = t->find(english)) return text;
    }
    return english;
}

void set_languages_dir(const std::filesystem::path& dir) {
    g_languages_dir = dir;
    g_loaded.clear();
}

// @orig none: language folders of the port
std::vector<LanguageChoice> language_choices() {
    std::vector<LanguageChoice> choices = {{"", "AUTO", false}};
    for (const auto& [id, folder] : language_folders()) {
        try {
            choices.push_back({id, name_of(id, read_texts(folder, nullptr)), has_data_files(folder)});
        } catch (const std::exception&) {
            // Unreadable: not offered.
        }
    }
    return choices;
}

// @orig none: language folders of the port
void select_language(std::string_view id) {
    g_loaded.clear();
    g_choice = {"", "AUTO", false};
    const std::string wanted = lower(id);
    if (wanted.empty()) return;
    if (const Translation* t = translation(wanted)) {
        g_choice = {wanted, name_of(wanted, *t), has_data_files(folder_of(wanted))};
    } else {
        std::fprintf(stderr, "language '%s' not found in '%s'\n", wanted.c_str(), g_languages_dir.string().c_str());
    }
}

const LanguageChoice& selected_language() { return g_choice; }

// @orig none: language folders of the port
std::filesystem::path language_data_folder(std::string_view id) {
    const std::filesystem::path folder = folder_of(lower(id));
    return !folder.empty() && has_data_files(folder) ? folder : std::filesystem::path{};
}

// @orig none: language folders of the port
void set_loaded_data_language(std::string_view id) {
    g_loaded_data = language_data_folder(id).empty() ? std::string() : lower(id);
}

bool language_needs_restart() { return (g_choice.data_files ? g_choice.id : std::string()) != g_loaded_data; }

}  // namespace crusader
