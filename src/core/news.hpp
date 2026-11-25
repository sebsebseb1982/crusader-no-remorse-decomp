#pragma once
// Port addition (@orig none): the port's online news and update notice
// (GitHub issue #36), the parts that need no network.
//
// At start-up, when the player agreed to it (GameConfig::check_updates), the
// game fetches two files (src/platform/http_get.hpp, Game::enable_online):
//   - the latest release of the port, from the GitHub API
//     (kLatestReleaseUrl): its "tag_name" ("v1.79.0") is compared with the
//     version built in, and the main menu tells when a newer one exists;
//   - news.json, at the root of the repository (kNewsUrl), the news the
//     authors write by hand:
//
//   { "format": 1,
//     "news": [ { "date": "2026-11-25T18:00:00Z",       (UTC; or "2026-11-25")
//                 "expires": "2027-01-31",              (optional)
//                 "min_version": "1.78.0",              (optional)
//                 "max_version": "1.79.9",              (optional)
//                 "title": { "en": "...", "fr": "...", "de": "..." },
//                 "body":  { "en": "...", "fr": "...", "de": "..." } } ] }
//
// A news is shown from its date (a date to come keeps it back) until
// "expires", by default kNewsLifetime after its date, and only to the
// versions between min_version and max_version (both included; a version of
// the port that is not a release, "0.0.0-dev", sees them all). Texts are
// picked in the language asked for, else English, else the first one given;
// title and body may also be plain strings. The newest comes first.
//
// The ids of the news the player has read are kept (GameConfig::news_seen):
// the others are new, counted on the main menu's news button. An optional
// "id" in a news ("id": "no-limit") keeps its identity when its title is
// reworded.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crusader {

inline constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/sebsebseb1982/crusader-no-remorse-decomp/releases/latest";
inline constexpr const char* kNewsUrl =
    "https://raw.githubusercontent.com/sebsebseb1982/crusader-no-remorse-decomp/master/news.json";
inline constexpr std::int64_t kNewsLifetime = 30 * 24 * 3600;  // seconds

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const SemVer&) const = default;
    std::string text() const;  // "1.79.0"
};
// "1.79.0" or "v1.79.0"; none for anything else, a pre-release
// ("0.0.0-dev") included.
std::optional<SemVer> parse_semver(std::string_view text);

// "YYYY-MM-DD", optionally followed by "THH:MM", "THH:MM:SS" and a "Z", as
// UTC seconds since 1970; none when malformed.
std::optional<std::int64_t> parse_utc_date(std::string_view text);
// A day as "DD.MM.YYYY" (UTC).
std::string format_day(std::int64_t utc_seconds);

struct ReleaseInfo {
    SemVer version;
    std::string url;  // the release's page
};
// The GitHub API's answer for the latest release; none without a valid
// "tag_name".
std::optional<ReleaseInfo> parse_latest_release(std::string_view json);

struct NewsItem {
    std::int64_t date = 0;  // UTC seconds
    std::uint32_t id = 0;   // stable: from the "id" text, else the date's text and the title (en)
    std::string title;      // as the menus print it (to_menu_text)
    std::string body;       // the same, '\n' between paragraphs
};
// What the player has read. A news is read when its id is in `seen` (the ids
// kept in the config), or when it is dated before `before` (the old
// "newsread" of the config, a date: two news of the same day were not told
// apart, the second one was never shown as new).
struct NewsRead {
    std::int64_t before = 0;
    std::vector<std::uint32_t> seen;
    bool has(const NewsItem& item) const;
};
// The news of news.json to show at `now` (UTC seconds) to `version` (none
// for a build that is not a release), in `language` ("fr", "en", "de"...),
// newest first. Malformed entries are skipped; malformed JSON gives none.
std::vector<NewsItem> select_news(std::string_view json, std::int64_t now, std::optional<SemVer> version,
                                  std::string_view language);
// How many of `news` are not read.
int count_unread(const std::vector<NewsItem>& news, const NewsRead& read);

// UTF-8 text as the menu fonts print it: capitals of code page 850 without
// accents ("Été" gives "ETE"), the characters they lack as '?'.
std::string to_menu_text(std::string_view utf8);

}  // namespace crusader
