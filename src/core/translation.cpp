#include "core/translation.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace crusader {

namespace {

// Code page 850, characters 0x80..0xFF.
constexpr std::array<std::uint16_t, 128> kCp850 = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
    0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
    0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
    0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
    0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
    0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
    0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

// @orig none: language files of the port
char cp850_of(std::uint32_t code_point) {
    if (code_point < 0x80) return static_cast<char>(code_point);
    for (std::size_t i = 0; i < kCp850.size(); ++i) {
        if (kCp850[i] == code_point) return static_cast<char>(0x80 + i);
    }
    return '?';
}

// @orig none: language files of the port
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// "\n" and "\\" turned into a line break and a backslash.
// @orig none: language files of the port
std::string unescape(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == '\\')) {
            out += s[i + 1] == 'n' ? '\n' : '\\';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The printf conversions of `s`, one per entry: what the arguments are read
// as ('*' widths, length and conversion); flags, widths and precisions do not
// matter.
// @orig none: language files of the port
std::vector<std::string> conversions(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        if (++i < s.size() && s[i] == '%') continue;
        std::string c;
        while (i < s.size() && std::string_view("-+ #0").find(s[i]) != std::string_view::npos) ++i;
        for (bool precision = false;; precision = true) {
            if (i < s.size() && s[i] == '*') {
                c += '*';
                ++i;
            }
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (precision || i >= s.size() || s[i] != '.') break;
            ++i;
        }
        while (i < s.size() && std::string_view("hljztL").find(s[i]) != std::string_view::npos) c += s[i++];
        c += i < s.size() ? s[i] : '?';
        out.push_back(c);
    }
    return out;
}

}  // namespace

// @orig none: language files of the port
std::string utf8_to_cp850(std::string_view utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto b = static_cast<unsigned char>(utf8[i]);
        const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + extra >= utf8.size()) {
            out += '?';
            ++i;
            continue;
        }
        std::uint32_t cp = extra == 0 ? b : b & (0x3F >> extra);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const auto c = static_cast<unsigned char>(utf8[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            cp = cp << 6 | (c & 0x3F);
        }
        out += ok ? cp850_of(cp) : '?';
        i += ok ? 1 + extra : 1;
    }
    return out;
}

// @orig none: language files of the port
std::string cp850_to_utf8(std::string_view cp850) {
    std::string out;
    for (const char ch : cp850) {
        const auto b = static_cast<unsigned char>(ch);
        const std::uint32_t cp = b < 0x80 ? b : kCp850[b - 0x80];
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | cp >> 12);
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// @orig none: language files of the port
Translation Translation::parse(std::string_view utf8, std::vector<std::string>* warnings) {
    Translation t;
    if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);  // a byte order mark
    int number = 0;
    while (!utf8.empty()) {
        const auto end = utf8.find('\n');
        std::string_view line = utf8.substr(0, end);
        utf8 = end == std::string_view::npos ? std::string_view{} : utf8.substr(end + 1);
        ++number;
        if (line.ends_with('\r')) line.remove_suffix(1);
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto warn = [&](const std::string& why) {
            if (warnings) warnings->push_back("line " + std::to_string(number) + ": " + why);
        };
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            warn("no '='");
            continue;
        }
        const std::string key = utf8_to_cp850(unescape(trim(line.substr(0, equals))));
        const std::string value = utf8_to_cp850(unescape(trim(line.substr(equals + 1))));
        if (key == "@name") {
            t.name_ = value;
        } else if (key.empty()) {
            warn("no English text");
        } else if (value.empty()) {
            continue;  // not translated
        } else if (conversions(key) != conversions(value)) {
            warn("the translation of \"" + cp850_to_utf8(key) + "\" does not keep its % conversions");
        } else {
            t.texts_.insert_or_assign(key, value);
        }
    }
    return t;
}

// @orig none: language files of the port
Translation Translation::load(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return parse(text.str(), warnings);
}

const char* Translation::find(std::string_view english) const {
    const auto it = texts_.find(english);
    return it == texts_.end() ? nullptr : it->second.c_str();
}

}  // namespace crusader
