#include "core/game_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace crusader {

namespace {

bool is_blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// A line as 1020:0000 reads it (FILEPARS.C): lines starting with '#' or '/'
// are comments; then blanks skipped, the key up to '=' and cut at its first
// blank, the value the first word after '=', both lowered (strlwr, 1000:7088).
struct Entry {
    std::string key, value;
};

// @orig 1020:0000
Entry entry_of(std::string_view line) {
    if (line.empty() || line[0] == '#' || line[0] == '/') return {};
    std::size_t i = 0;
    while (i < line.size() && is_blank(line[i])) ++i;
    const auto eq = line.find('=', i);
    if (eq == std::string_view::npos) return {};
    std::size_t k = i;
    while (k < eq && !is_blank(line[k])) ++k;
    std::size_t v = eq + 1;
    while (v < line.size() && is_blank(line[v])) ++v;
    std::size_t w = v;
    while (w < line.size() && !is_blank(line[w])) ++w;
    return {lower(line.substr(i, k - i)), lower(line.substr(v, w - v))};
}

// The keys of the port's enhancements window (not "checkupdates" and the
// news, which the main menu writes, nor the controller's): a file without any
// is a fresh install's (or the original game's), which gets the port's
// recommended settings (set_fresh_install_defaults).
constexpr std::string_view kEnhancementKeys[] = {
    "followplayer", "directionalmove", "viewzoom", "smoothscaling", "lighting", "volumetricsmoke", "flashlight",
    "ceilinglights", "ambientlight", "nativeresolution", "autosave", "directionalaim", "drawonaim", "preciseaim",
    "togglerun", "impactmarks", "bloodmarks", "goremode", "casings", "barrelleaks", "glassdebris", "remainsloot",
    "language", "fixoriginalbugs", "vibration", "vibrationintensity"
};

bool is_enhancement_key(std::string_view key) {
    return std::find(std::begin(kEnhancementKeys), std::end(kEnhancementKeys), key) != std::end(kEnhancementKeys);
}

// atoi, as the original reads numbers.
int number(std::string_view value) { return std::atoi(std::string(value).c_str()); }

// "viewzoom" in hundredths: 0, or 100..400 ("2", "1.5", "1,5"); anything else is clamped.
// @orig none: port-only fractional view zoom
int view_zoom_pct_of(std::string_view value) {
    std::string text(value);
    std::replace(text.begin(), text.end(), ',', '.');
    const double zoom = std::atof(text.c_str());
    if (!(zoom >= 0.005)) return 0;
    return std::clamp(static_cast<int>(std::lround(zoom * 100)), 100, 400);
}

// @orig none: port-only fractional view zoom
std::string view_zoom_text(int pct) {
    if (pct % 100 == 0) return std::to_string(pct / 100);
    std::string text = std::to_string(pct / 100) + '.' + (pct % 100 < 10 ? "0" : "") + std::to_string(pct % 100);
    if (text.back() == '0') text.pop_back();
    return text;
}

}  // namespace

// @orig none: port-only view zoom steps
int GameConfig::next_view_zoom(int current_pct, int limit_pct) {
    for (const int step : {100, 150, 200, 300, 400}) {
        if (step > current_pct && (step <= limit_pct || step == 100)) return step;
    }
    return 0;
}

// @orig 1020:0000
GameConfig GameConfig::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return parse({});
    std::ostringstream s;
    s << in.rdbuf();
    return parse(s.str());
}

// @orig 1020:0000
GameConfig GameConfig::parse(std::string_view text) {
    GameConfig c;
    bool enhancements = false;  // a key of the enhancements window
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) continue;
        c.lines_.emplace_back(line);
        const Entry e = entry_of(line);
        const std::string_view key = e.key;
        const std::string_view value = e.value;
        enhancements = enhancements || is_enhancement_key(key);
        if (key == "soundvolume") c.sound_volume = number(value);
        else if (key == "musicvolume") c.music_volume = number(value);
        else if (key == "filter") c.filter = value == "true";
        else if (key == "animation") c.animation = number(value);
        else if (key == "frameskip") c.frameskip = number(value);
        else if (key == "limitblasts") c.limit_blasts = value == "true";
        else if (key == "mousespeed") c.mouse_speed = number(value);
        else if (key == "bigvideo") c.big_video = value == "true";
        else if (key == "subtitles") c.subtitles = value == "true";
        else if (key == "noweasel") c.no_weasel = value == "true";
        else if (key == "followplayer") c.follow_player = value == "true";
        else if (key == "directionalmove") c.directional_move = value == "true";
        else if (key == "viewzoom") c.view_zoom_pct = view_zoom_pct_of(value);
        else if (key == "smoothscaling") c.smooth_scaling = value == "true";
        else if (key == "lighting") c.lighting = value == "true";
        else if (key == "volumetricsmoke") c.volumetric_smoke = value != "false";
        else if (key == "flashlight") c.flashlight = value != "false";
        else if (key == "ceilinglights") c.ceiling_lights = value != "false";
        else if (key == "ambientlight") c.ambient_light = std::clamp(number(value), 0, 100);
        else if (key == "nativeresolution") c.native_resolution = value != "false";
        else if (key == "autosave") c.autosave = value != "false";
        else if (key == "directionalaim") c.directional_aim = value == "true";
        else if (key == "drawonaim") c.draw_on_aim = value != "false";
        else if (key == "preciseaim") c.precise_aim = value == "true";
        else if (key == "togglerun") c.toggle_run = value == "true";
        else if (key == "impactmarks") c.impact_marks = value == "true";
        else if (key == "bloodmarks") c.blood_marks = value == "true";
        else if (key == "goremode") c.gore_mode = value == "true";
        else if (key == "casings") c.casings = value == "true";
        else if (key == "barrelleaks") c.barrel_leaks = value == "true";
        else if (key == "glassdebris") c.glass_debris = value == "true";
        else if (key == "remainsloot") c.remains_loot = value == "true";
        else if (key == "language") c.language = value;
        else if (key == "checkupdates") c.check_updates = value == "false" ? 0 : value == "true" ? 1 : -1;
        else if (key == "newsread") c.news_read = std::max<long long>(std::atoll(std::string(value).c_str()), 0);
        else if (key == "newsseen") {
            std::size_t pos = 0;
            while (pos < value.size()) {
                std::size_t end = value.find(',', pos);
                if (end == std::string_view::npos) end = value.size();
                const std::string id(value.substr(pos, end - pos));
                if (!id.empty()) c.news_seen.push_back(static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 16)));
                pos = end + 1;
            }
        }
        else if (key == "fixoriginalbugs") c.fix_original_bugs = value == "true";
        else if (key == "vibration") c.vibration = value != "false";
        else if (key == "vibrationintensity") c.vibration_intensity = std::clamp(number(value), 0, 100);
        else if (key == "padaim") c.pad.aim = std::clamp(number(value), 1, PadSettings::kAimLevels);
        else if (key == "paddeadzone") c.pad.dead_zone = std::clamp(number(value), 0, PadSettings::kDeadZoneLevels - 1);
        else if (key == "padmap") {
            std::string_view rest = value;
            for (int& b : c.pad.buttons) {
                if (rest.empty()) break;
                const auto comma = rest.find(',');
                b = std::clamp(number(rest.substr(0, comma)), kPadDefaultRole, static_cast<int>(kPadActionCount) - 1);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        }
    }
    if (!enhancements) c.set_fresh_install_defaults();
    return c;
}

// @orig none: port addition, the settings the port's author plays with
// (the enhancements window's), for a file that has none of its keys.
void GameConfig::set_fresh_install_defaults() {
    follow_player = true;
    directional_move = true;
    directional_aim = true;
    smooth_scaling = true;
    toggle_run = true;
    impact_marks = true;
    blood_marks = true;
    casings = true;
    barrel_leaks = true;
    glass_debris = true;
    lighting = true;
    remains_loot = true;
    fix_original_bugs = true;
    flashlight = false;
    view_zoom_pct = 200;
}

// @orig 1020:0C7F
std::string GameConfig::text() const {
    GameConfig out = *this;
    out.set("musicvolume", std::to_string(music_volume));
    out.set("soundvolume", std::to_string(sound_volume));
    out.set("mousespeed", std::to_string(mouse_speed));
    out.set("animation", std::to_string(animation));
    out.set("limitblasts", limit_blasts ? "true" : "false");
    out.set("frameskip", std::to_string(frameskip));
    out.set("filter", filter ? "true" : "false");
    // Written only when set (DS:68CE == 0x80013).
    if (big_video) out.set("bigvideo", "true");
    else out.erase("bigvideo");
    // Written only when set (DS:0179 != 0).
    if (subtitles) out.set("subtitles", "true");
    else out.erase("subtitles");
    // Written only when set (DS:0178 != 0).
    if (no_weasel) out.set("noweasel", "true");
    else out.erase("noweasel");
    out.set("followplayer", follow_player ? "true" : "false");
    out.set("directionalmove", directional_move ? "true" : "false");
    out.set("viewzoom", view_zoom_text(view_zoom_pct));
    out.set("smoothscaling", smooth_scaling ? "true" : "false");
    if (!native_resolution) out.set("nativeresolution", "false");
    else out.erase("nativeresolution");
    if (!autosave) out.set("autosave", "false");
    else out.erase("autosave");
    out.set("directionalaim", directional_aim ? "true" : "false");
    if (!draw_on_aim) out.set("drawonaim", "false");
    else out.erase("drawonaim");
    if (precise_aim) out.set("preciseaim", "true");
    else out.erase("preciseaim");
    out.set("togglerun", toggle_run ? "true" : "false");
    out.set("impactmarks", impact_marks ? "true" : "false");
    out.set("bloodmarks", blood_marks ? "true" : "false");
    if (gore_mode) out.set("goremode", "true");
    else out.erase("goremode");
    out.set("casings", casings ? "true" : "false");
    out.set("barrelleaks", barrel_leaks ? "true" : "false");
    out.set("glassdebris", glass_debris ? "true" : "false");
    out.set("lighting", lighting ? "true" : "false");
    if (!volumetric_smoke) out.set("volumetricsmoke", "false");
    else out.erase("volumetricsmoke");
    out.set("flashlight", flashlight ? "true" : "false");
    if (!ceiling_lights) out.set("ceilinglights", "false");
    else out.erase("ceilinglights");
    if (ambient_light != 85) out.set("ambientlight", std::to_string(ambient_light));
    else out.erase("ambientlight");
    out.set("remainsloot", remains_loot ? "true" : "false");
    if (!language.empty()) out.set("language", language);
    else out.erase("language");
    if (check_updates >= 0) out.set("checkupdates", check_updates ? "true" : "false");
    else out.erase("checkupdates");
    if (news_read > 0) out.set("newsread", std::to_string(news_read));
    else out.erase("newsread");
    if (!news_seen.empty()) {
        std::string ids;
        char hex[16];
        for (const std::uint32_t id : news_seen) {
            std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(id));
            if (!ids.empty()) ids += ',';
            ids += hex;
        }
        out.set("newsseen", ids);
    } else {
        out.erase("newsseen");
    }
    out.set("fixoriginalbugs", fix_original_bugs ? "true" : "false");
    if (!vibration) out.set("vibration", "false");
    else out.erase("vibration");
    if (vibration_intensity != 100) out.set("vibrationintensity", std::to_string(vibration_intensity));
    else out.erase("vibrationintensity");
    // The controller settings: only when they differ from the defaults.
    // ("padbuttons" was an earlier format, dropped.)
    out.erase("padbuttons");
    if (pad.is_default()) {
        out.erase("padmap");
        out.erase("padaim");
        out.erase("paddeadzone");
    } else {
        std::string buttons;
        for (const int b : pad.buttons) buttons += (buttons.empty() ? "" : ",") + std::to_string(b);
        out.set("padmap", buttons);
        out.set("padaim", std::to_string(pad.aim));
        out.set("paddeadzone", std::to_string(pad.dead_zone));
    }
    std::string s;
    for (const std::string& line : out.lines_) s += line + "\r\n";
    return s;
}

// @orig 1020:0C7F
void GameConfig::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string s = text();
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

// @orig none: the original sets its globals directly
void GameConfig::set(std::string_view key, const std::string& value) {
    const std::string line = std::string(key) + "=" + value;
    for (std::string& l : lines_) {
        if (entry_of(l).key == key) {
            l = line;
            return;
        }
    }
    lines_.push_back(line);
}

void GameConfig::erase(std::string_view key) {
    std::erase_if(lines_, [key](const std::string& l) { return entry_of(l).key == key; });
}

}  // namespace crusader
