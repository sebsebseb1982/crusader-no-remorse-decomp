// Game configuration parsing and writing, data detection, language files (no
// game data).

#include "core/data_files.hpp"
#include "core/data_info.hpp"
#include "core/game_config.hpp"
#include "core/iso_image.hpp"
#include "core/json.hpp"
#include "core/language.hpp"
#include "core/news.hpp"
#include "core/translation.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace crusader;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_config() {
    // As installed, CRUSADER.CFG has none of the option keys but "bigvideo"
    // (the data's may have been rewritten by the options screen since).
    const GameConfig c = GameConfig::parse("irq=7\r\ndma=1\r\nbigvideo=true\r\nsubtitles=true\r\n");
    check(c.sound_volume == 0x40 && c.music_volume == 0x20 && c.filter && c.mouse_speed == 2, "config defaults");
    check(c.big_video && c.animation == 0 && c.frameskip == 0 && !c.limit_blasts, "config bigvideo");
    // A missing file gives the defaults.
    const GameConfig none = GameConfig::load("NO_SUCH.CFG");
    check(none.music_volume == 0x20 && !none.big_video && none.mouse_speed == 2, "config missing");

    GameConfig e = GameConfig::parse("irq=7\r\nmusicvolume=12\r\nbigvideo=true\r\nfilter=false\r\nlimitblasts=true\r\n");
    check(e.music_volume == 12 && e.big_video && !e.filter && e.limit_blasts, "config parse");
    e.music_volume = 40;
    e.big_video = false;
    e.frameskip = 3;
    check(e.text() == "irq=7\r\nmusicvolume=40\r\nfilter=false\r\nlimitblasts=true\r\nsoundvolume=64\r\n"
                      "mousespeed=2\r\nanimation=0\r\nframeskip=3\r\n"
                      // A fresh install's file: the enhancements it got, written.
                      "followplayer=true\r\ndirectionalmove=true\r\nviewzoom=2\r\nsmoothscaling=true\r\n"
                      "directionalaim=true\r\ntogglerun=true\r\nimpactmarks=true\r\nbloodmarks=true\r\n"
                      "casings=true\r\nbarrelleaks=true\r\nglassdebris=true\r\nlighting=true\r\n"
                      "flashlight=false\r\nremainsloot=true\r\nfixoriginalbugs=true\r\n",
          "config written over its lines, other keys kept");

    // 1020:0000 lowers keys and values, cuts them at blanks, skips comments.
    {
        const GameConfig l = GameConfig::parse("# filter=false\r\n  MusicVolume = 12  \r\nBIGVIDEO=True\r\n/limitblasts=true\r\n");
        check(l.music_volume == 12 && l.big_video && l.filter && !l.limit_blasts, "config case, blanks, comments");
        GameConfig r = l;
        r.music_volume = 20;
        check(r.text().find("musicvolume=20\r\n") != std::string::npos &&
                  r.text().find("MusicVolume") == std::string::npos && r.text().find("# filter=false\r\n") == 0,
              "config line of a lowered key replaced");
    }

    // "noweasel": read, and written only when set (1020:0C7F).
    GameConfig w = GameConfig::parse("irq=7\r\nnoweasel=true\r\n");
    check(w.no_weasel && !GameConfig::parse("noweasel=false\r\n").no_weasel, "config noweasel");
    w.no_weasel = false;
    check(w.text().find("noweasel") == std::string::npos, "config noweasel erased when clear");

    // "autosave" (port addition): on by default, written only when switched off.
    GameConfig saving = GameConfig::parse("irq=7\r\n");
    check(saving.autosave && saving.text().find("autosave") == std::string::npos, "config autosave default on, not written");
    saving.autosave = false;
    check(!GameConfig::parse(saving.text()).autosave && saving.text().find("autosave=false") != std::string::npos,
          "config autosave off round trip");

    // "smoothscaling" (port addition): on for a fresh install, off in a file
    // the port wrote without it (it was written only when set), written either way.
    check(GameConfig::parse("irq=7\r\n").smooth_scaling && GameConfig::parse("smoothscaling=true\r\n").smooth_scaling &&
              !GameConfig::parse("irq=7\r\nlighting=true\r\n").smooth_scaling,
          "config smoothscaling");
    {
        GameConfig smooth = GameConfig::parse("smoothscaling=true\r\n");
        check(smooth.text().find("smoothscaling=true") != std::string::npos, "config smoothscaling written");
        smooth.smooth_scaling = false;
        check(smooth.text().find("smoothscaling=false") != std::string::npos &&
                  !GameConfig::parse(smooth.text()).smooth_scaling,
              "config smoothscaling off written");
    }
    // "volumetricsmoke" (port addition): on by default, written only when off.
    {
        GameConfig smoke = GameConfig::parse("irq=7\r\n");
        check(smoke.volumetric_smoke && smoke.text().find("volumetricsmoke") == std::string::npos,
              "config volumetricsmoke default on");
        smoke.volumetric_smoke = false;
        check(smoke.text().find("volumetricsmoke=false") != std::string::npos &&
                  !GameConfig::parse(smoke.text()).volumetric_smoke,
              "config volumetricsmoke off round trip");
    }
    // "flashlight" (port addition): on by default, off for a fresh install,
    // written either way.
    {
        GameConfig lamp = GameConfig::parse("irq=7\r\nlighting=true\r\n");
        check(lamp.flashlight && lamp.text().find("flashlight=true") != std::string::npos &&
                  !GameConfig::parse("irq=7\r\n").flashlight,
              "config flashlight default");
        lamp.flashlight = false;
        check(lamp.text().find("flashlight=false") != std::string::npos && !GameConfig::parse(lamp.text()).flashlight,
              "config flashlight off round trip");
    }
    // "ambientlight" (port addition): 85 by default, written only when changed, clamped.
    {
        GameConfig ambient = GameConfig::parse("irq=7\r\n");
        check(ambient.ambient_light == 85 && ambient.text().find("ambientlight") == std::string::npos,
              "config ambientlight default 85");
        ambient.ambient_light = 40;
        check(ambient.text().find("ambientlight=40") != std::string::npos &&
                  GameConfig::parse(ambient.text()).ambient_light == 40,
              "config ambientlight round trip");
        check(GameConfig::parse("ambientlight=150\r\n").ambient_light == 100, "config ambientlight clamped");
    }
    // The controller's left trigger switches the lamp (L) by default.
    check(PadSettings{}.buttons[kPadLeftTrigger] == kPadFlashlight && kPadActions[kPadFlashlight].scancode == 0x26,
          "pad left trigger: flashlight");
    // "viewzoom" (port addition): hundredths, whole values keep their old file form, x1.5 is new.
    check(GameConfig::parse("irq=7\r\n").view_zoom_pct == 200 && GameConfig::parse("lighting=true\r\n").view_zoom_pct == 0,
          "config viewzoom default x2 for a fresh install, else auto");
    check(GameConfig::parse("viewzoom=2\r\n").view_zoom_pct == 200, "config viewzoom whole");
    check(GameConfig::parse("viewzoom=1.5\r\n").view_zoom_pct == 150 && GameConfig::parse("viewzoom=1,5\r\n").view_zoom_pct == 150,
          "config viewzoom 1.5");
    check(GameConfig::parse("viewzoom=9\r\n").view_zoom_pct == 400 && GameConfig::parse("viewzoom=0.2\r\n").view_zoom_pct == 100 &&
              GameConfig::parse("viewzoom=x\r\n").view_zoom_pct == 0,
          "config viewzoom clamped");
    GameConfig zoom = GameConfig::parse("irq=7\r\n");
    zoom.view_zoom_pct = 150;
    check(zoom.text().find("viewzoom=1.5") != std::string::npos && GameConfig::parse(zoom.text()).view_zoom_pct == 150,
          "config viewzoom 1.5 round trip");
    zoom.view_zoom_pct = 300;
    check(zoom.text().find("viewzoom=3\r\n") != std::string::npos, "config viewzoom whole written as before");
    zoom.view_zoom_pct = 0;
    check(zoom.text().find("viewzoom=0\r\n") != std::string::npos && GameConfig::parse(zoom.text()).view_zoom_pct == 0,
          "config viewzoom auto written");
    // The cycle: auto, x1, x1.5, x2, x3, x4 up to what the window allows (160 % on 1360 x 768).
    check(GameConfig::next_view_zoom(0, 160) == 100 && GameConfig::next_view_zoom(100, 160) == 150 &&
              GameConfig::next_view_zoom(150, 160) == 0 && GameConfig::next_view_zoom(150, 225) == 200 &&
              GameConfig::next_view_zoom(200, 225) == 0 && GameConfig::next_view_zoom(0, 100) == 100 &&
              GameConfig::next_view_zoom(100, 100) == 0 && GameConfig::next_view_zoom(400, 900) == 0,
          "config viewzoom cycle");

    GameConfig rumble = GameConfig::parse("irq=7\r\n");
    check(rumble.vibration &&rumble.vibration_intensity == 100, "config vibration defaults");
    rumble.vibration = false;
    rumble.vibration_intensity = 60;
    const GameConfig rumble_round_trip = GameConfig::parse(rumble.text());
    check(!rumble_round_trip.vibration && rumble_round_trip.vibration_intensity == 60,
          "config vibration settings round trip");
    const GameConfig rumble_clamped =
        GameConfig::parse("vibration=false\r\nvibrationintensity=120\r\n");
    check(!rumble_clamped.vibration && rumble_clamped.vibration_intensity == 100,
          "config vibration intensity clamped");

    // "drawonaim" (port addition): on by default, written only when switched off.
    GameConfig drawing = GameConfig::parse("irq=7\r\n");
    check(drawing.draw_on_aim && drawing.text().find("drawonaim") == std::string::npos,
          "config drawonaim default on, not written");
    drawing.draw_on_aim = false;
    check(!GameConfig::parse(drawing.text()).draw_on_aim && drawing.text().find("drawonaim=false") != std::string::npos,
          "config drawonaim off round trip");

    // "preciseaim" (port addition): off by default, written only when set.
    GameConfig precise = GameConfig::parse("irq=7\r\n");
    check(!precise.precise_aim && precise.text().find("preciseaim") == std::string::npos,
          "config preciseaim default off, not written");
    precise.precise_aim = true;
    check(GameConfig::parse(precise.text()).precise_aim && precise.text().find("preciseaim=true") != std::string::npos,
          "config preciseaim on round trip");
    GameConfig precise_off = GameConfig::parse(precise.text());
    precise_off.precise_aim = false;
    check(precise_off.text().find("preciseaim") == std::string::npos, "config preciseaim off erased");

    // "remainsloot" (port addition, a cheat): on for a fresh install, off in a
    // file the port wrote without it, written either way.
    GameConfig loot = GameConfig::parse("irq=7\r\nlighting=true\r\n");
    check(!loot.remains_loot && loot.text().find("remainsloot=false") != std::string::npos &&
              GameConfig::parse("irq=7\r\n").remains_loot,
          "config remainsloot default");
    loot.remains_loot = true;
    check(GameConfig::parse(loot.text()).remains_loot && loot.text().find("remainsloot=true") != std::string::npos,
          "config remainsloot on round trip");
    GameConfig loot_off = GameConfig::parse(loot.text());
    loot_off.remains_loot = false;
    check(loot_off.text().find("remainsloot=false") != std::string::npos, "config remainsloot off written");

    // A fresh install (no key of the enhancements window: the original's file,
    // or none) gets the port's author's settings; any of those keys (a file the
    // port wrote) keeps the old defaults for the missing ones.
    for (const GameConfig& f : {GameConfig::parse("irq=7\r\ndma=1\r\n"), GameConfig::load("NO_SUCH.CFG")}) {
        check(f.follow_player && f.directional_move && f.directional_aim && f.smooth_scaling && f.toggle_run &&
                  f.impact_marks && f.blood_marks && !f.gore_mode && f.casings && f.barrel_leaks && f.glass_debris &&
                  f.lighting && f.remains_loot && f.view_zoom_pct == 200 && f.fix_original_bugs && !f.flashlight,
              "config fresh install defaults");
        check(f.language.empty() && f.check_updates == -1 && f.native_resolution && f.autosave,
              "config fresh install: AUTO language, the online question still asked");
    }
    {
        const GameConfig old = GameConfig::parse("irq=7\r\nlanguage=fr\r\n");
        check(!old.follow_player && !old.lighting && !old.remains_loot && old.view_zoom_pct == 0,
              "config a file the port wrote keeps the old defaults");
        GameConfig fresh = GameConfig::parse("irq=7\r\n");
        fresh.lighting = false;
        const GameConfig again = GameConfig::parse(fresh.text());
        check(!again.lighting && again.follow_player && again.view_zoom_pct == 200,
              "config a fresh install's choices kept once written");
    }

    // Controller settings (port addition): defaults, not written until changed.
    GameConfig p = GameConfig::parse("irq=7\r\n");
    check(p.pad.is_default() && p.text().find("pad") == std::string::npos, "config pad defaults not written");
    p.pad.buttons[kPadA] = kPadMedkit;
    p.pad.aim = 5;
    p.pad.dead_zone = 0;
    const GameConfig q = GameConfig::parse(p.text());
    check(q.pad.buttons == p.pad.buttons && q.pad.aim == 5 && q.pad.dead_zone == 0, "config pad round trip");
    check(GameConfig::parse("padaim=99\r\npaddeadzone=-3\r\npadmap=1,99,2\r\n").pad.aim == 5, "config pad aim clamped");
    const GameConfig c2 = GameConfig::parse("paddeadzone=-3\r\npadmap=1,99,x\r\n");
    check(c2.pad.dead_zone == 0 && c2.pad.buttons[1] == kPadActionCount - 1 && c2.pad.buttons[2] == kPadNone &&
              c2.pad.buttons[3] == PadSettings{}.buttons[3],
          "config pad values clamped, short list keeps defaults");
    // assign(): the button's old action and the action's old button are unbound.
    PadSettings a;
    a.assign(kPadB, kPadFire);  // fire was on RT, jump on B
    check(a.buttons[kPadB] == kPadFire && a.buttons[kPadRightTrigger] == kPadNone && a.button_of(kPadJump) < 0 &&
              a.button_of(kPadFire) == kPadB,
          "pad assign moves the action");
    // A short list leaves the other inputs at their defaults; a direction round-trips.
    check(c2.pad.buttons[kPadRightStickUp] == kPadDefaultRole && c2.pad.buttons[kPadDpadRight] == PadSettings{}.buttons[kPadDpadRight],
          "config pad directions default");
    PadSettings dir;
    dir.assign(kPadRightStickLeft, kPadStepLeft);
    GameConfig dc;
    dc.pad = dir;
    check(GameConfig::parse(dc.text()).pad.buttons == dir.buttons, "config pad direction round trip");
    p.pad = PadSettings{};
    check(p.text().find("pad") == std::string::npos, "config pad erased when back to defaults");
}

// A minimal ISO 9660 image: root with A.TXT;1 and a directory D holding B.BIN;1.
void check_iso() {
    namespace fs = std::filesystem;
    std::vector<unsigned char> img(24 * 2048, 0);
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) img[at + i] = static_cast<unsigned char>(v >> (8 * i));
    };
    auto record = [&](std::size_t at, std::uint32_t lba, std::uint32_t size, bool dir, const std::string& name) {
        const std::size_t n = name.size();
        img[at] = static_cast<unsigned char>(33 + n + ((n % 2) ? 0 : 1));
        put32(at + 2, lba);
        put32(at + 10, size);
        img[at + 25] = dir ? 2 : 0;
        img[at + 32] = static_cast<unsigned char>(n);
        std::memcpy(&img[at + 33], name.data(), n);
        return at + img[at];
    };
    img[16 * 2048] = 1;
    std::memcpy(&img[16 * 2048 + 1], "CD001", 5);
    record(16 * 2048 + 156, 18, 2048, true, std::string(1, 0));
    std::size_t at = 18 * 2048;
    at = record(at, 18, 2048, true, std::string(1, 0));
    at = record(at, 18, 2048, true, std::string(1, 1));
    at = record(at, 20, 5, false, "A.TXT;1");
    record(at, 19, 2048, true, "D");
    record(19 * 2048, 21, 3, false, "B.BIN;1");
    std::memcpy(&img[20 * 2048], "hello", 5);
    std::memcpy(&img[21 * 2048], "abc", 3);
    const fs::path dir = fs::temp_directory_path() / "crusader_iso_test";
    std::error_code ignored0;
    fs::remove_all(dir, ignored0);
    fs::create_directories(dir);
    std::ofstream(dir / "t.iso", std::ios::binary).write(reinterpret_cast<const char*>(img.data()), img.size());

    {
    const IsoImage iso = IsoImage::open(dir / "t.iso");
    check(iso.files().size() == 2, "iso lists two files");
    iso.extract(dir / "out", {});
    std::string a, b;
    std::ifstream(dir / "out/A.TXT") >> a;
    std::ifstream(dir / "out/D/B.BIN") >> b;
    check(a == "hello" && b == "abc", "iso extracts files and subdirectories");
    iso.extract(dir / "skip", {"D"});
    check(fs::exists(dir / "skip/A.TXT") && !fs::exists(dir / "skip/D"), "iso skips a directory");
    }  // the image is closed before the folder is removed

    // prepare_game_data: an ISO in a data folder without STATIC/ is extracted.
    fs::create_directories(dir / "data");
    fs::copy_file(dir / "t.iso", dir / "data/game.iso");
    prepare_game_data(dir / "data");
    check(fs::exists(dir / "data/D/B.BIN"), "prepare_game_data extracts the ISO of data/");
    // The GOG release's image is GAME.GOG: an ISO under another extension.
    fs::create_directories(dir / "gog");
    fs::copy_file(dir / "t.iso", dir / "gog/GAME.GOG");
    check(is_disc_image("GAME.GOG") && is_disc_image("x.Iso") && is_disc_image("x.bin") && is_disc_image("x.CUE") &&
              !is_disc_image("x.img"),
          "disc images by extension");
    prepare_game_data(dir / "gog");
    check(fs::exists(dir / "gog/D/B.BIN"), "prepare_game_data extracts the GAME.GOG of data/");
    // A .bin of raw 2352-byte sectors (sync and header, 2048 bytes of data,
    // error codes), opened through its .cue sheet.
    {
        std::vector<char> raw(24 * 2352, 0);
        for (std::size_t s = 0; s < 24; ++s) std::memcpy(&raw[s * 2352 + 16], &img[s * 2048], 2048);
        fs::create_directories(dir / "cue");
        std::ofstream(dir / "cue/Game (Disc).bin", std::ios::binary).write(raw.data(), raw.size());
        std::ofstream(dir / "cue/Game (Disc).cue") << "FILE \"Game (Disc).bin\" BINARY\n  TRACK 01 MODE1/2352\n";
        const IsoImage iso = IsoImage::open(dir / "cue/Game (Disc).cue");
        iso.extract(dir / "cue/out", {});
        std::string a;
        std::ifstream(dir / "cue/out/A.TXT") >> a;
        check(iso.files().size() == 2 && a == "hello", "a .cue sheet opens its raw .bin image");
    }
    prepare_game_data(dir / "fresh");
    check(fs::exists(dir / "fresh/PUT_GAME_DATA_HERE.txt"), "prepare_game_data creates data/ with its note");
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

// detect_data_info on stand-ins: an executable with the banner and a text,
// or only the usecode's intrinsic table, whose size tells the version.
void check_data_info() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "crusader_data_info_test";
    std::error_code ignored0;
    fs::remove_all(dir, ignored0);
    fs::create_directories(dir / "USECODE");
    const auto exe = [&](const std::string& text) {
        std::ofstream(dir / "CRUSADER.EXE", std::ios::binary) << text;
        return detect_data_info(dir);
    };
    DataInfo info = exe(std::string("CRUSADER:  No Remorse V1.01\0", 28) + "SPIDER BOMB");
    check(info.version == "1.01" && info.us() && info.v101() && info.known(), "1.01 told by its banner and item names");
    info = exe(std::string("CRUSADER:  No Remorse V1.23\0", 28) + "Press any key to continue.");
    check(info.version == "1.23" && info.us() && !info.v101(), "US 1.23 told by its banner and message");
    info = exe("CRUSADER:  No Remorse V1.08 Appuyer sur une touche pour continuer.");
    check(info.version == "1.08" && info.language == "FR", "French 1.08 told by its banner and message");
    info = exe("CRUSADER:  No Remorse V1.07 [Beliebige Taste dr\x81" "cken, um fortzufahren.]");
    check(info.version == "1.07" && info.german() && info.text_language() == Language::German,
          "German 1.07 told by its banner and message");
    info = exe("CRUSADER:  No Remorse V2.00");
    check(info.version == "2.00" && !info.known(), "an executable without a known text is unknown");

    fs::remove(dir / "CRUSADER.EXE");
    const auto table = [&](std::size_t entries) {
        std::ofstream(dir / "USECODE/UNKCOFF.DAT", std::ios::binary) << std::string(entries * 4, '\0');
        return detect_data_info(dir);
    };
    info = table(308);
    check(info.v101(), "without the executable, 308 intrinsics: 1.01");
    info = table(311);
    check(info.us() && !info.v101() && info.version.empty(), "without the executable, 311 intrinsics: US");
    info = table(309);
    check(info.language == "FR" && info.version.empty(), "without the executable, 309 intrinsics: French");
    info = table(300);
    check(!info.known(), "without the executable, another table size is unknown");
    fs::remove(dir / "USECODE/UNKCOFF.DAT");
    check(!detect_data_info(dir).known(), "no executable and no usecode: unknown");
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

}  // namespace

// Language files (port addition): parsing, code page 850, picking a language.
void check_json() {
    // Objects keep their keys in order, numbers are read without the locale,
    // strings unescaped (UTF-8 out of \u), a byte order mark skipped.
    std::string error;
    const auto doc = parse_json("\xEF\xBB\xBF { \"a\": [1, -2.5, 3e2, 0.65], \"b\": {\"t\": true, \"n\": null},\n"
                                " \"s\": \"x\\\"y\\u00e9\\n\" } ",
                                &error);
    check(doc.has_value() && error.empty(), "json: a document parses");
    if (!doc) return;
    const JsonValue* a = doc->get("a");
    check(a && a->is_array() && a->array.size() == 4, "json: an array");
    if (a && a->array.size() == 4)
        check(a->array[0].number == 1 && a->array[1].number == -2.5 && a->array[2].number == 300 &&
                  a->array[3].number == 0.65,
              "json: numbers");
    check(doc->object.size() == 3 && doc->object[0].first == "a" && doc->object[2].first == "s", "json: key order");
    const JsonValue* b = doc->get("b");
    check(b && b->get("t") && b->get("t")->boolean && b->get("n") && b->get("n")->type == JsonValue::Type::kNull,
          "json: true and null");
    check(doc->string_at("s", "") == "x\"y\xC3\xA9\n", "json: escapes");
    check(doc->number_at("missing", 7) == 7 && doc->string_at("a", "no") == "no", "json: fallbacks");
    // Errors say where.
    check(!parse_json("{\"a\": [1, 2,]}", &error) && error.find("line 1") == 0, "json: a stray comma fails");
    check(!parse_json("{\"a\": 1}\n x", &error) && error.find("line 2") == 0, "json: text after the value fails");
    check(!parse_json("[\"open", &error), "json: an unterminated string fails");
}

void check_languages() {
    // UTF-8 to code page 850 and back: the Portuguese letters are in it.
    check(utf8_to_cp850("a\xC3\xA3\xC3\xA7\xC3\x89") == "a\xC6\x87\x90", "cp850: a, a tilde, c cedilla, E acute");
    check(utf8_to_cp850("\xE2\x82\xAC\xC3") == "??", "cp850: euro sign and cut sequence");
    check(cp850_to_utf8("\x82\xC6") == "\xC3\xA9\xC3\xA3", "cp850 to utf-8");

    std::vector<std::string> warnings;
    const Translation t = Translation::parse(
        "\xEF\xBB\xBF# comment\r\n"
        "@name = PORTUGU\xC3\x8AS\r\n"
        "\r\n"
        "BACK = VOLTAR\r\n"
        "  ON=LIGADO  \r\n"
        "OFF =\r\n"
        "Do you want\\nanything else? = Quer\\nmais alguma coisa?\n"
        "Cost:%u = Pre\xC3\xA7o: %3u\n"
        "Owned:%02u = Seus: %s\n"
        "no equals sign\n",
        &warnings);
    check(t.name() == "PORTUGU\xD2S", "language file: name");
    check(t.find("BACK") && std::string(t.find("BACK")) == "VOLTAR", "language file: text");
    check(t.find("ON") && std::string(t.find("ON")) == "LIGADO", "language file: spaces dropped");
    check(!t.find("OFF"), "language file: empty translation kept out");
    check(t.find("Do you want\nanything else?") &&
              std::string(t.find("Do you want\nanything else?")) == "Quer\nmais alguma coisa?",
          "language file: line breaks");
    check(t.find("Cost:%u") && std::string(t.find("Cost:%u")) == "Pre\x87o: %3u", "language file: same conversion");
    check(!t.find("Owned:%02u"), "language file: other conversion dropped");
    check(warnings.size() == 2 && t.size() == 4, "language file: two warnings");

    // The languages folder.
    const auto dir = std::filesystem::temp_directory_path() / "crusader_core_tests_languages";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // A folder per language: port.txt, and data files in subfolders.
    for (const char* id : {"pt-BR/STATIC", "fr", "en", "de"}) std::filesystem::create_directories(dir / id);
    std::ofstream(dir / "pt-BR/port.txt", std::ios::binary) << "@name = PORTUGUES\nBACK = VOLTAR\n";
    std::ofstream(dir / "pt-BR/STATIC/FONTS.FLX", std::ios::binary) << "x";
    std::ofstream(dir / "fr/port.txt", std::ios::binary) << "@name = FRANCAIS\nBACK = RETOUR\nON = MARCHE\n";
    std::ofstream(dir / "en/port.txt", std::ios::binary) << "@name = ENGLISH\nBACK = BACK\n";
    std::ofstream(dir / "not-a-folder.txt", std::ios::binary) << "ON = AN\n";
    set_languages_dir(dir);
    const std::vector<LanguageChoice> choices = language_choices();
    check(choices.size() == 5 && choices[0].id.empty() && choices[0].name == "AUTO" && choices[1].id == "de" &&
              choices[1].name == "DE" && choices[2].id == "en" && choices[3].id == "fr" &&
              choices[3].name == "FRANCAIS" && !choices[3].data_files && choices[4].id == "pt-br" &&
              choices[4].name == "PORTUGUES" && choices[4].data_files,
          "languages: auto, then the folders by id");

    // Auto: the file of the data's language.
    set_text_language(Language::English);
    check(std::string(tr("BACK")) == "BACK" && std::string(tr("ON")) == "ON", "tr: auto, English data");
    set_text_language(Language::French);
    check(std::string(tr("BACK")) == "RETOUR" && std::string(tr("ON")) == "MARCHE", "tr: auto, French data");
    check(std::string(tr("OFF")) == "OFF", "tr: a text the file lacks stays in English");
    select_language("PT-BR");
    const char* back = tr("BACK");
    check(std::string(back) == "VOLTAR" && selected_language().name == "PORTUGUES", "tr: language picked");
    check(std::string(tr("ON")) == "ON", "tr: a text the file lacks stays in English, French data too");
    select_language("xx");
    check(selected_language().id.empty() && std::string(tr("BACK")) == "RETOUR", "tr: unknown language, auto");
    check(std::string(back) == "VOLTAR", "tr: texts handed out stay valid");
    // Data files: read at start-up, a restart for another language's.
    check(language_data_folder("PT-BR") == dir / "pt-BR" && language_data_folder("fr").empty() &&
              language_data_folder("").empty(),
          "languages: the folder of the data files");
    set_loaded_data_language("");
    select_language("fr");
    check(!language_needs_restart(), "languages: no data files, no restart");
    select_language("pt-br");
    check(language_needs_restart(), "languages: data files picked, restart");
    set_loaded_data_language("pt-br");
    check(!language_needs_restart(), "languages: data files read");
    select_language("");
    check(language_needs_restart(), "languages: back to auto, restart");
    set_loaded_data_language("");
    const DataFiles files(dir / "data", dir / "mod", language_data_folder("pt-br"));
    check(files.find("STATIC/FONTS.FLX") == dir / "pt-BR/STATIC/FONTS.FLX" &&
              files.find("STATIC/GUMPS.FLX") == dir / "data/STATIC/GUMPS.FLX",
          "data files: the language's copy, else the data's");
    // No folder: English.
    set_languages_dir({});
    check(std::string(tr("BACK")) == "BACK", "tr: no language files");
    std::filesystem::remove_all(dir);

    // The files of the repository: French complete, two-text editor form.
    set_languages_dir(CRUSADER_LANGUAGES_DIR);
    check(std::string(tr("<MORE>")) == "-SUITE-" && std::string(tr("Credits:%ld")) == "Cr\x82" "dits :%ld",
          "languages/fr/port.txt read, in code page 850");
    set_text_language(Language::English);
    check(std::string(tr("<MORE>")) == "<MORE>" && std::string(tr("RETOUR", "BACK")) == "BACK",
          "languages/en/port.txt read; the editor's texts");
    set_text_language(Language::French);
    set_languages_dir({});

    // CRUSADER.CFG: written only when picked.
    GameConfig lang = GameConfig::parse("irq=7\r\n");
    check(lang.language.empty() && lang.text().find("language") == std::string::npos, "config language default");
    lang.language = "pt-br";
    check(GameConfig::parse(lang.text()).language == "pt-br", "config language round trip");
    check(GameConfig::parse("Language = PT-BR\r\n").language == "pt-br", "config language in lower case");
}

// The online news and update notice (news.hpp), and their config keys.
void check_news() {
    // Versions: with or without "v", none for a pre-release.
    check(parse_semver("v1.79.0") == SemVer{1, 79, 0} && parse_semver("1.2.3") == SemVer{1, 2, 3}, "news: semver");
    check(!parse_semver("0.0.0-dev") && !parse_semver("1.2") && !parse_semver("") && !parse_semver("1.2.3.4"),
          "news: not a semver");
    check(SemVer{1, 10, 0} > SemVer{1, 9, 9} && SemVer{2, 0, 0} > SemVer{1, 99, 99}, "news: semver order");
    // Dates (UTC).
    check(parse_utc_date("1970-01-02") == 86400, "news: a day");
    check(parse_utc_date("2026-11-25T18:00:00Z") == 1795629600, "news: a day and time");
    check(parse_utc_date("2026-11-25T18:00") == 1795629600, "news: no seconds, no Z");
    check(!parse_utc_date("2026-13-01") && !parse_utc_date("2026-11-25T") && !parse_utc_date("25/11/2026"),
          "news: bad dates");
    check(format_day(1795629600) == "25.11.2026" && format_day(0) == "01.01.1970", "news: day printed");
    // The latest release.
    const auto release = parse_latest_release(
        R"({"tag_name": "v1.79.0", "html_url": "https://github.com/x/y/releases/tag/v1.79.0", "assets": []})");
    check(release && release->version == SemVer{1, 79, 0} && release->url.ends_with("v1.79.0"), "news: release");
    check(!parse_latest_release(R"({"message": "API rate limit exceeded"})") && !parse_latest_release("<html>"),
          "news: no release");
    // Menu text: capitals without accents.
    check(to_menu_text("\xC3\x89t\xC3\xA9 \xE2\x80\x99" "a\xE2\x80\x99 \xC5\x93uf\tz") == "ETE 'A' OEUF Z",
          "news: menu text");
    check(to_menu_text("\xE6\x97\xA5x\xC3") == "?X?", "news: characters out of the fonts");
    // Selection: shown from the date, for 30 days or until "expires", to
    // the versions between min and max, in the language asked for.
    const std::int64_t day = 86400, now = *parse_utc_date("2026-11-25T12:00:00Z");
    const std::string json = R"({"format": 1, "news": [
        {"date": "2026-11-20", "title": {"en": "Older", "fr": "Plus vieux"}, "body": "Line one\nline two"},
        {"date": "2026-11-24T08:00:00Z", "title": {"en": "Newest"}},
        {"date": "2026-11-26", "title": "Tomorrow"},
        {"date": "2026-10-20", "title": "Too old"},
        {"date": "2026-10-20", "expires": "2026-12-01", "title": "Kept longer"},
        {"date": "2026-11-21", "min_version": "1.80.0", "title": "For 1.80"},
        {"date": "2026-11-21", "max_version": "1.78.9", "title": "Update!"},
        {"date": "bad", "title": "Bad date"},
        {"date": "2026-11-21"},
        "not an object"]})";
    auto news = select_news(json, now, SemVer{1, 79, 0}, "fr");
    check(news.size() == 3, "news: selection");
    if (news.size() == 3) {
        check(news[0].title == "NEWEST" && news[1].title == "PLUS VIEUX" && news[2].title == "KEPT LONGER",
              "news: newest first, the language asked for, else English");
        check(news[1].body == "LINE ONE\nLINE TWO", "news: body");
    }
    news = select_news(json, now, SemVer{1, 78, 0}, "en");
    check(news.size() == 4 && news[1].title == "UPDATE!", "news: max_version");
    news = select_news(json, now, std::nullopt, "en");
    check(news.size() == 5, "news: a build that is not a release sees them all");
    check(select_news(json, now + 27 * day, SemVer{1, 79, 0}, "en").size() == 2, "news: expired, and a date come");
    check(select_news("{", now, std::nullopt, "en").empty() && select_news("[]", now, std::nullopt, "en").empty(),
          "news: bad JSON");
    news = select_news(json, now, SemVer{1, 79, 0}, "en");
    check(count_unread(news, {}) == 3 && count_unread(news, {news[0].date, {}}) == 1 &&
              count_unread(news, {news[0].date + 1, {}}) == 0,
          "news: unread, by the old date");
    check(count_unread(news, {0, {news[0].id}}) == 2 && count_unread(news, {0, {news[0].id, news[2].id}}) == 1,
          "news: unread, by ids");
    // Two news of the same day: reading the first leaves the second new.
    const std::string same_day = R"({"news": [{"date": "2026-11-25", "title": "First"},
        {"date": "2026-11-25", "title": "Second"}]})";
    const auto day_one = select_news(same_day, now, std::nullopt, "en");
    check(day_one.size() == 2 && day_one[0].id != day_one[1].id, "news: ids of two news of the same day");
    NewsRead read;
    for (const NewsItem& item : day_one) read.seen.push_back(item.id);
    check(count_unread(day_one, read) == 0, "news: both read");
    const auto day_two = select_news(
        R"({"news": [{"date": "2026-11-25", "title": "First"}, {"date": "2026-11-25", "title": "Second"},
            {"date": "2026-11-25", "title": "Third"}]})",
        now, std::nullopt, "en");
    check(day_two.size() == 3 && count_unread(day_two, read) == 1, "news: a third news of the same day is new");
    // Config: asked until answered, the news read.
    GameConfig c = GameConfig::parse("irq=7\r\n");
    check(c.check_updates == -1 && c.news_read == 0 && c.news_seen.empty(), "news: config defaults");
    check(c.text().find("checkupdates") == std::string::npos && c.text().find("newsread") == std::string::npos &&
              c.text().find("newsseen") == std::string::npos,
          "news: nothing written before an answer");
    c.check_updates = 1;
    c.news_read = 1795629600;
    c.news_seen = {0x0123abcdu, 0xffffffffu};
    const GameConfig back = GameConfig::parse(c.text());
    check(back.check_updates == 1 && back.news_read == 1795629600 && back.news_seen == c.news_seen,
          "news: config written and read");
    check(GameConfig::parse("checkupdates=false\r\n").check_updates == 0, "news: refused");
}

int main() {
    check_config();
    check_iso();
    check_data_info();
    check_languages();
    check_json();
    check_news();
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
