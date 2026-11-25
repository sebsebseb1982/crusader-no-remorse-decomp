#include "core/news.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <cstdio>

namespace crusader {

namespace {

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar
// (Howard Hinnant's days_from_civil).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// The digits of text[pos, pos + count), none when one is not a digit.
std::optional<int> digits(std::string_view text, std::size_t pos, std::size_t count) {
    if (pos + count > text.size()) return std::nullopt;
    int value = 0;
    for (std::size_t i = pos; i < pos + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return std::nullopt;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

// A text of news.json: a string, or an object of strings by language.
std::string pick_text(const JsonValue* value, std::string_view language) {
    if (!value) return {};
    if (value->is_string()) return value->string;
    if (!value->is_object()) return {};
    for (const std::string_view want : {language, std::string_view("en")}) {
        if (const JsonValue* v = value->get(want); v && v->is_string()) return v->string;
    }
    for (const auto& [key, v] : value->object) {
        if (v.is_string()) return v.string;
    }
    return {};
}

}  // namespace

// @orig none: port's online news
std::string SemVer::text() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// @orig none: port's online news
std::optional<SemVer> parse_semver(std::string_view text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    int parts[3] = {};
    std::size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t start = pos;
        long value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && pos - start < 6) {
            value = value * 10 + (text[pos] - '0');
            ++pos;
        }
        if (pos == start) return std::nullopt;
        parts[i] = static_cast<int>(value);
        if (i < 2) {
            if (pos >= text.size() || text[pos] != '.') return std::nullopt;
            ++pos;
        }
    }
    if (pos != text.size()) return std::nullopt;
    return SemVer{parts[0], parts[1], parts[2]};
}

// @orig none: port's online news
std::optional<std::int64_t> parse_utc_date(std::string_view text) {
    const auto year = digits(text, 0, 4), month = digits(text, 5, 2), day = digits(text, 8, 2);
    if (!year || !month || !day || text[4] != '-' || text[7] != '-' || *month < 1 || *month > 12 || *day < 1 ||
        *day > 31) {
        return std::nullopt;
    }
    int hour = 0, minute = 0, second = 0;
    std::size_t pos = 10;
    if (pos < text.size() && (text[pos] == 'T' || text[pos] == ' ')) {
        const auto h = digits(text, pos + 1, 2), m = digits(text, pos + 4, 2);
        if (!h || !m || text[pos + 3] != ':' || *h > 23 || *m > 59) return std::nullopt;
        hour = *h;
        minute = *m;
        pos += 6;
        if (pos < text.size() && text[pos] == ':') {
            const auto s = digits(text, pos + 1, 2);
            if (!s || *s > 60) return std::nullopt;
            second = *s;
            pos += 3;
        }
    }
    if (pos < text.size() && text[pos] == 'Z') ++pos;
    if (pos != text.size()) return std::nullopt;
    return days_from_civil(*year, static_cast<unsigned>(*month), static_cast<unsigned>(*day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

// @orig none: port's online news
std::string format_day(std::int64_t utc_seconds) {
    // civil_from_days (Howard Hinnant).
    std::int64_t z = (utc_seconds >= 0 ? utc_seconds : utc_seconds - 86399) / 86400 + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);
    char text[32];
    std::snprintf(text, sizeof text, "%02u.%02u.%04lld", d, m, static_cast<long long>(y));
    return text;
}

// @orig none: port's online news
std::optional<ReleaseInfo> parse_latest_release(std::string_view json) {
    const auto doc = parse_json(json);
    if (!doc || !doc->is_object()) return std::nullopt;
    const auto version = parse_semver(doc->string_at("tag_name", ""));
    if (!version) return std::nullopt;
    ReleaseInfo release{*version, doc->string_at("html_url", ""), {}};
    if (const JsonValue* assets = doc->get("assets"); assets && assets->is_array()) {
        for (const JsonValue& asset : assets->array) {
            if (!asset.is_object()) continue;
            ReleaseAsset a{asset.string_at("name", ""), asset.string_at("browser_download_url", ""), 0};
            if (const double size = asset.number_at("size", 0); size > 0) a.size = static_cast<std::uint64_t>(size);
            if (!a.name.empty() && !a.url.empty()) release.assets.push_back(std::move(a));
        }
    }
    return release;
}

// @orig none: port's online news
std::vector<NewsItem> select_news(std::string_view json, std::int64_t now, std::optional<SemVer> version,
                                  std::string_view language) {
    std::vector<NewsItem> news;
    const auto doc = parse_json(json);
    if (!doc || !doc->is_object()) return news;
    const JsonValue* list = doc->get("news");
    if (!list || !list->is_array()) return news;
    for (const JsonValue& entry : list->array) {
        if (!entry.is_object()) continue;
        const auto date = parse_utc_date(entry.string_at("date", ""));
        if (!date || *date > now) continue;
        const std::string expires_text = entry.string_at("expires", "");
        const auto expires = expires_text.empty() ? std::optional<std::int64_t>(*date + kNewsLifetime)
                                                  : parse_utc_date(expires_text);
        if (!expires || now >= *expires) continue;
        if (version) {
            const std::string min_text = entry.string_at("min_version", "");
            const std::string max_text = entry.string_at("max_version", "");
            const auto min = parse_semver(min_text), max = parse_semver(max_text);
            if ((!min_text.empty() && (!min || *version < *min)) || (!max_text.empty() && (!max || *version > *max))) {
                continue;
            }
        }
        NewsItem item;
        item.date = *date;
        {
            // FNV-1a of the id, else of the date's text and the title (en).
            std::string key = entry.string_at("id", "");
            if (key.empty()) key = entry.string_at("date", "") + "|" + pick_text(entry.get("title"), "en");
            std::uint32_t h = 2166136261u;
            for (const char c : key) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
            item.id = h;
        }
        item.title = to_menu_text(pick_text(entry.get("title"), language));
        item.body = to_menu_text(pick_text(entry.get("body"), language));
        if (item.title.empty()) continue;
        news.push_back(std::move(item));
    }
    std::stable_sort(news.begin(), news.end(), [](const NewsItem& a, const NewsItem& b) { return a.date > b.date; });
    return news;
}

// @orig none: port's online news
bool NewsRead::has(const NewsItem& item) const {
    return item.date < before || std::find(seen.begin(), seen.end(), item.id) != seen.end();
}

// @orig none: port's online news
int count_unread(const std::vector<NewsItem>& news, const NewsRead& read) {
    return static_cast<int>(std::count_if(news.begin(), news.end(), [&](const NewsItem& n) { return !read.has(n); }));
}

// @orig none: port's online news
std::string to_menu_text(std::string_view utf8) {
    std::string out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            ++i;
            if (c == '\r') continue;
            if (c == '\t') {
                out += ' ';
                continue;
            }
            out += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
            continue;
        }
        // One code point of 2 to 4 bytes.
        const int length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        std::uint32_t cp = length == 4 ? c & 0x07 : length == 3 ? c & 0x0F : c & 0x1F;
        bool bad = length == 1 || i + length > utf8.size();
        for (int k = 1; !bad && k < length; ++k) {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            if ((next & 0xC0) != 0x80) bad = true;
            cp = (cp << 6) | (next & 0x3F);
        }
        i += bad ? 1 : static_cast<std::size_t>(length);
        if (bad) {
            out += '?';
            continue;
        }
        // Latin-1 letters without their accents, a few punctuation marks.
        const char* plain = nullptr;
        if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5)) plain = "A";
        else if (cp == 0xC6 || cp == 0xE6) plain = "AE";
        else if (cp == 0xC7 || cp == 0xE7) plain = "C";
        else if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) plain = "E";
        else if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) plain = "I";
        else if (cp == 0xD1 || cp == 0xF1) plain = "N";
        else if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8 || (cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8) plain = "O";
        else if ((cp >= 0xD9 && cp <= 0xDC) || (cp >= 0xF9 && cp <= 0xFC)) plain = "U";
        else if (cp == 0xDD || cp == 0xFD || cp == 0xFF || cp == 0x178) plain = "Y";
        else if (cp == 0xDF) plain = "SS";
        else if (cp == 0x152 || cp == 0x153) plain = "OE";
        else if (cp == 0xA0 || cp == 0x202F) plain = " ";
        else if (cp == 0x2018 || cp == 0x2019) plain = "'";
        else if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB) plain = "\"";
        else if (cp == 0x2013 || cp == 0x2014) plain = "-";
        else if (cp == 0x2026) plain = "...";
        out += plain ? plain : "?";
    }
    return out;
}

}  // namespace crusader
