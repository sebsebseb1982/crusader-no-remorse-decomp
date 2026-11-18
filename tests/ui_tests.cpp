// Gumps and other UI pieces on synthetic shapes (no game data).

#include "core/flex.hpp"
#include "core/language.hpp"
#include "core/palette.hpp"
#include "core/shape.hpp"
#include "core/surface.hpp"
#include "engine/ui/christmas_hat.hpp"
#include "engine/ui/enhancements_screen.hpp"
#include "engine/ui/keypad_gump.hpp"
#include "engine/ui/gump_keys.hpp"
#include "engine/world/render.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace crusader;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

void put_u16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put_u24(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
    b[at + 2] = static_cast<std::uint8_t>(v >> 16);
}

void put_u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
}

// A Flat2D (GUMPS.FLX) frame: a solid block of one palette index.
std::vector<std::uint8_t> solid_frame(int width, int height, std::uint8_t color) {
    std::vector<std::uint8_t> f(20 + 4 * static_cast<std::size_t>(height), 0);
    put_u32(f, 0, 0);  // uncompressed
    put_u32(f, 4, static_cast<std::uint32_t>(width));
    put_u32(f, 8, static_cast<std::uint32_t>(height));
    put_u32(f, 12, 0);  // xoff
    put_u32(f, 16, 0);  // yoff
    // Line offsets are relative to their own position; all lines share one
    // RLE stream: skip 0, run `width`, then the colour bytes.
    for (int y = 0; y < height; ++y) put_u32(f, 20 + 4 * static_cast<std::size_t>(y), 4u * (height - y));
    f.push_back(0);
    f.push_back(static_cast<std::uint8_t>(width));
    f.insert(f.end(), static_cast<std::size_t>(width), color);
    return f;
}

// A Flat2D shape blob holding `frames`.
std::vector<std::uint8_t> shape_blob(const std::vector<std::vector<std::uint8_t>>& frames) {
    const std::size_t table = 6 + 8 * frames.size();
    std::vector<std::uint8_t> s(table, 0);
    put_u16(s, 4, static_cast<std::uint16_t>(frames.size()));
    std::size_t offset = table;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        put_u24(s, 6 + 8 * i, static_cast<std::uint32_t>(offset));
        put_u32(s, 10 + 8 * i, static_cast<std::uint32_t>(frames[i].size()));
        s.insert(s.end(), frames[i].begin(), frames[i].end());
        offset += frames[i].size();
    }
    return s;
}

// KeypadGump::draw (13C8:08F5, GUMPSHAP_1340_0242): the frame rect is
// 0, 0, width - 1, height - 1, so the next digit starts at the frame's width
// + 1, not + 2. Frames 4 and 5 pixels wide show the one-pixel difference.
void test_keypad_digit_spacing() {
    const std::vector<std::vector<std::uint8_t>> digit_frames = {solid_frame(4, 2, 0x21),
                                                                 solid_frame(5, 2, 0x22)};
    const auto blob = shape_blob(digit_frames);
    const auto path = std::filesystem::temp_directory_path() / "crusader_keypad_spacing.flx";
    {
        FlexWriter out(13);  // GUMPS shapes 0..12; only the digits (12) are used here
        out.add(KeypadGump::kDigitShape, blob);
        out.write(path);
    }
    const FlexArchive flex = FlexArchive::open(path);
    ShapeCache gumps(flex, ShapeLayout::Flat2D);

    KeypadGump pad(gumps, 0, [](int) {}, 640, 480);
    pad.key('1');
    pad.key('2');
    CHECK(pad.digit_count() == 2);

    Surface8 screen(640, 480, 0);
    pad.draw(screen);
    int first = -1, second = -1;
    for (int y = 0; y < screen.height && (first < 0 || second < 0); ++y) {
        for (int x = 0; x < screen.width; ++x) {
            const std::uint8_t c = screen.row(y)[x];
            if (c == 0x21 && first < 0) first = x;
            if (c == 0x22 && second < 0) second = x;
        }
    }
    CHECK(first >= 0 && second >= 0);
    CHECK(second - first == 4 + 1);
}

// Port addition: the enhancements window, its first page and its pages.
using Entry = EnhancementsScreen::Entry;
using Page = EnhancementsScreen::Page;

// Down to `entry` on the page shown, then Enter.
void choose(EnhancementsScreen& screen, Entry entry) {
    const int row = screen.row_of(entry);
    CHECK(row >= 0);
    while (screen.focus() > row) screen.key(gump_key::kUp);
    while (screen.focus() < row) screen.key(gump_key::kDown);
    screen.key(gump_key::kEnter);
}

void test_vibration_settings() {
    FontSet fonts;
    GameConfig config;
    int pad_focus = 0;
    int previews = 0;
    EnhancementsScreen screen(fonts, config, pad_focus, [&] { ++previews; });
    choose(screen, Entry::Controls);
    CHECK(screen.page() == Page::Controls);
    for (int i = 0; i < screen.row_of(Entry::VibrationIntensity); ++i) screen.key(gump_key::kDown);
    screen.key(gump_key::kLeft);
    CHECK(config.vibration_intensity == 90 && previews == 1);
    screen.key(gump_key::kEnter);
    CHECK(config.vibration_intensity == 100 && previews == 2);
    screen.key(gump_key::kRight);
    CHECK(config.vibration_intensity == 100 && previews == 2);
    choose(screen, Entry::Vibration);
    CHECK(!config.vibration);

    EnhancementsScreen smooth(fonts, config, pad_focus);
    choose(smooth, Entry::Display);
    choose(smooth, Entry::Smooth);
    CHECK(config.smooth_scaling);
}

void test_enhancements_pages() {
    FontSet fonts;
    GameConfig config;
    int pad_focus = 0;
    EnhancementsScreen screen(fonts, config, pad_focus);
    // The first page: a row per page, "retour" last.
    CHECK(screen.page() == Page::Main && screen.entries().size() == 5 && screen.entries().back() == Entry::Back);
    // Every page ends with "retour" and fits the window.
    for (Entry page : {Entry::Controls, Entry::Display, Entry::Effects, Entry::Game}) {
        EnhancementsScreen s(fonts, config, pad_focus);
        choose(s, page);
        CHECK(s.page() != Page::Main && s.focus() == 0 && s.entries().back() == Entry::Back);
        CHECK(s.entries().size() <= 9 && s.row_rect(static_cast<int>(s.entries().size()) - 1).y1 < 460);
    }
    // Effects: both toggles on their own.
    choose(screen, Entry::Effects);
    choose(screen, Entry::Blood);
    CHECK(config.blood_marks && !config.impact_marks);
    choose(screen, Entry::Impact);
    CHECK(config.impact_marks && config.blood_marks);
    // Blood: on, gore, off.
    choose(screen, Entry::Blood);
    CHECK(config.blood_marks && config.gore_mode);
    choose(screen, Entry::Blood);
    CHECK(!config.blood_marks && !config.gore_mode);
    choose(screen, Entry::Blood);
    CHECK(config.blood_marks && !config.gore_mode);
    // Escape goes back to the first page on the page's row, then closes.
    screen.key(gump_key::kEscape);
    CHECK(screen.page() == Page::Main && screen.focus() == screen.row_of(Entry::Effects) && !screen.closed());
    // "retour" on a page does the same.
    choose(screen, Entry::Game);
    choose(screen, Entry::Back);
    CHECK(screen.page() == Page::Main && screen.focus() == screen.row_of(Entry::Game));
    // Language (the repository's files, French data): auto, German, English,
    // French, auto.
    choose(screen, Entry::Game);
    CHECK(std::string(tr("BACK")) == "RETOUR");
    choose(screen, Entry::Language);
    CHECK(config.language == "de" && selected_language().name == "DEUTSCH" && std::string(tr("BACK")) == "ZURUECK");
    choose(screen, Entry::Language);
    CHECK(config.language == "en" && selected_language().id == "en" && std::string(tr("BACK")) == "BACK");
    choose(screen, Entry::Language);
    CHECK(config.language == "fr" && selected_language().name == "FRANCAIS" && std::string(tr("BACK")) == "RETOUR");
    choose(screen, Entry::Language);
    CHECK(config.language.empty() && selected_language().id.empty() && std::string(tr("BACK")) == "RETOUR");
    screen.key(gump_key::kEscape);
    screen.key(gump_key::kEscape);
    CHECK(screen.closed());
    // The mouse: a row pressed opens it.
    EnhancementsScreen mouse(fonts, config, pad_focus);
    const auto r = mouse.row_rect(mouse.row_of(Entry::Display));
    mouse.mouse_down(r.x0 + 5, r.y0 + 5);
    CHECK(mouse.page() == Page::Display);
    const auto follow = mouse.row_rect(mouse.row_of(Entry::Follow));
    mouse.mouse_down(follow.x0 + 5, follow.y0 + 5);
    CHECK(config.follow_player);
}

// The Christmas hat of the main menu: 24 to 31 December only, drawn over the
// Silencer's head (the background's top row is at y = 120 in No Remorse, 75
// in No Regret) and nowhere else.
void test_christmas_hat() {
    std::tm date{};
    date.tm_mon = 11;
    date.tm_mday = 23;
    CHECK(!is_christmas(date));
    date.tm_mday = 24;
    CHECK(is_christmas(date));
    date.tm_mday = 31;
    CHECK(is_christmas(date));
    date.tm_mon = 0;
    date.tm_mday = 1;
    CHECK(!is_christmas(date));
    date.tm_mon = 10;
    date.tm_mday = 25;
    CHECK(!is_christmas(date));

    for (const bool regret : {false, true}) {
        Surface8 screen(640, 480, 7);
        draw_christmas_hat(screen, regret);
        int drawn = 0, x0 = 640, y0 = 480, x1 = -1, y1 = -1;
        for (int y = 0; y < screen.height; ++y) {
            for (int x = 0; x < screen.width; ++x) {
                const std::uint8_t c = screen.row(y)[x];
                if (c == 7) continue;
                CHECK(c != kTransparentIndex);
                ++drawn;
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
        }
        CHECK(drawn > 1500);
        CHECK(x0 >= 240 && x1 < 360);
        CHECK(y0 >= (regret ? 75 : 120) && y1 < (regret ? 160 : 200));
    }
    // A small screen: clipped, no write outside.
    Surface8 small(300, 150, 7);
    draw_christmas_hat(small, false);
    CHECK(small.pixels.size() == 300u * 150u);
}

}  // namespace

int main() {
    set_languages_dir(CRUSADER_LANGUAGES_DIR);  // the French texts (languages/fr/port.txt)
    test_keypad_digit_spacing();
    test_vibration_settings();
    test_enhancements_pages();
    test_christmas_hat();
    if (failures == 0) std::printf("all ui checks passed\n");
    return failures == 0 ? 0 : 1;
}
