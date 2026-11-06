// Game configuration parsing and writing, data detection (no game data).

#include "core/data_info.hpp"
#include "core/game_config.hpp"
#include "core/iso_image.hpp"

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
                      "mousespeed=2\r\nanimation=0\r\nframeskip=3\r\n",
          "config written over its lines, other keys kept");

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

    GameConfig rumble = GameConfig::parse("irq=7\r\n");
    check(rumble.vibration && rumble.vibration_intensity == 100, "config vibration defaults");
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
    check(is_disc_image("GAME.GOG") && is_disc_image("x.Iso") && !is_disc_image("x.bin"), "disc images by extension");
    prepare_game_data(dir / "gog");
    check(fs::exists(dir / "gog/D/B.BIN"), "prepare_game_data extracts the GAME.GOG of data/");
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

int main() {
    check_config();
    check_iso();
    check_data_info();
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
