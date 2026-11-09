// Level editor model on the original data (data/, not in the repository):
// every map of FIXED.DAT and every glob of GLOB.FLX is read and written back
// byte for byte, so that saving a project changes only what was edited.
// Usage: editor_data_tests <data_dir>   (skips with exit code 0 when data is absent)

#include "core/flex.hpp"
#include "editor/level.hpp"
#include "editor/project.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>

using namespace crusader;
using namespace crusader::editor;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_maps(const std::filesystem::path& data) {
    const auto fixed = FlexArchive::open(data / "STATIC/FIXED.DAT");
    std::size_t maps = 0, records = 0, contained = 0, monster_eggs = 0, bad = 0;
    for (std::size_t m = 0; m < fixed.count(); ++m) {
        const auto entry = fixed.entry(m);
        if (entry.empty()) continue;
        ++maps;
        const Level level = Level::parse(entry);
        records += level.record_count();
        for (const LevelItem& i : level.items) {
            contained += i.contents.size();
            if (i.shape == 0x24F && i.contents.size() == 1 && i.contents[0].shape == 2) ++monster_eggs;
        }
        const auto back = level.serialize();
        if (!std::equal(back.begin(), back.end(), entry.begin(), entry.end())) {
            std::fprintf(stderr, "map %zu differs after a round trip\n", m);
            ++bad;
        }
    }
    std::printf("maps: %zu, records: %zu, in containers: %zu\n", maps, records, contained);
    check(maps == 63, "63 maps");
    check(bad == 0, "every map written back byte for byte");
    check(monster_eggs == 199, "the 199 monster eggs hold their template");
}

void check_globs(const std::filesystem::path& data) {
    const auto globs = FlexArchive::open(data / "STATIC/GLOB.FLX");
    std::size_t count = 0, bad = 0;
    for (std::size_t g = 0; g < globs.count(); ++g) {
        const auto entry = globs.entry(g);
        if (entry.empty()) continue;
        ++count;
        const auto back = Glob::parse(entry).serialize();
        if (!std::equal(back.begin(), back.end(), entry.begin(), entry.end())) ++bad;
    }
    std::printf("globs: %zu, differing: %zu\n", count, bad);
    check(bad == 0, "every glob written back byte for byte");
}

void check_project(const std::filesystem::path& data) {
    // Saving a project writes only into the mod folder; an untouched map
    // reads back the same.
    const auto mod = std::filesystem::temp_directory_path() / "crusader_editor_data_tests_mod";
    std::filesystem::remove_all(mod);
    Project project(data, mod);
    const std::size_t free_map = project.free_map();
    check(free_map != 0, "a free map slot");
    Level level = project.load_map(1);
    const std::size_t before = level.record_count();
    LevelItem item;
    item.uid = new_uid();
    item.x = 1000;
    item.y = 2000;
    item.shape = 0x30;
    level.items.push_back(item);
    project.store_map(free_map, level);
    check(project.dirty(), "dirty after a change");
    project.save();
    check(std::filesystem::exists(mod / "STATIC/FIXED.DAT") && !std::filesystem::exists(mod / "STATIC/GLOB.FLX"),
          "only the changed archive written");
    const Project reopened(data, mod);
    check(reopened.load_map(free_map).record_count() == before + 1, "new map read back from the mod");
    check(reopened.load_map(1).serialize() == project.load_map(1).serialize(), "other maps unchanged");
    std::filesystem::remove_all(mod);
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data = argc > 1 ? argv[1] : "data";
    if (!std::filesystem::exists(data / "STATIC/FIXED.DAT")) {
        std::printf("no game data in %s, skipped\n", data.string().c_str());
        return 0;
    }
    try {
        check_maps(data);
        check_globs(data);
        check_project(data);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
