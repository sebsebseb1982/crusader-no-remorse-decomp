#include "core/game_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
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
    if (!in) return {};
    std::ostringstream s;
    s << in.rdbuf();
    return parse(s.str());
}

// @orig 1020:0000
GameConfig GameConfig::parse(std::string_view text) {
    GameConfig c;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty()) continue;
        c.lines_.emplace_back(line);
        const Entry e = entry_of(line);
        const std::string_view key = e.key;
        const std::string_view value = e.value;
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
        else if (key == "nativeresolution") c.native_resolution = value != "false";
        else if (key == "autosave") c.autosave = value != "false";
        else if (key == "directionalaim") c.directional_aim = value == "true";
        else if (key == "drawonaim") c.draw_on_aim = value != "false";
        else if (key == "preciseaim") c.precise_aim = value == "true";
        else if (key == "togglerun") c.toggle_run = value == "true";
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
    return c;
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
    if (follow_player) out.set("followplayer", "true");
    else out.erase("followplayer");
    if (directional_move) out.set("directionalmove", "true");
    else out.erase("directionalmove");
    if (view_zoom_pct) out.set("viewzoom", view_zoom_text(view_zoom_pct));
    else out.erase("viewzoom");
    if (smooth_scaling) out.set("smoothscaling", "true");
    else out.erase("smoothscaling");
    if (!native_resolution) out.set("nativeresolution", "false");
    else out.erase("nativeresolution");
    if (!autosave) out.set("autosave", "false");
    else out.erase("autosave");
    if (directional_aim) out.set("directionalaim", "true");
    else out.erase("directionalaim");
    if (!draw_on_aim) out.set("drawonaim", "false");
    else out.erase("drawonaim");
    if (precise_aim) out.set("preciseaim", "true");
    else out.erase("preciseaim");
    if (toggle_run) out.set("togglerun", "true");
    else out.erase("togglerun");
    if (fix_original_bugs) out.set("fixoriginalbugs", "true");
    else out.erase("fixoriginalbugs");
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
