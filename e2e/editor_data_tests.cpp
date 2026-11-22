// Level editor model on the original data (data/, not in the repository):
// every map of FIXED.DAT and every glob of GLOB.FLX is read and written back
// byte for byte, so that saving a project changes only what was edited.
// Usage: editor_data_tests <data_dir>   (skips with exit code 0 when data is absent)

#include "core/flex.hpp"
#include "core/typeflags.hpp"
#include "editor/level.hpp"
#include "editor/level_tools.hpp"
#include "editor/project.hpp"
#include "engine/usecode/usecode_file.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <string>

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

void check_level_check(const std::filesystem::path& data) {
    // The level check on the original maps: what it reports there is what
    // the original designers left (an unused teleport, a lone switch...), so
    // it must stay rare, and no original map may have a trigger egg without
    // its class or a monster egg without its template.
    const Project project(data, {});
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    const auto usecode = UsecodeFile::from_flex(FlexArchive::open(data / "USECODE/EUSECODE.FLX"));
    CheckContext context;
    context.info.family = [&](std::uint16_t s) -> std::uint8_t { return s < typeflags.size() ? typeflags[s].family : 0; };
    context.info.has_class = [&](std::uint16_t cid) { return usecode.has_class(cid); };
    context.glob_pieces = [&](std::uint16_t g) { return project.glob(g).pieces.size(); };
    context.map_exists = [&](int m) { return m > 0 && project.map_used(static_cast<std::size_t>(m)); };
    context.arrivals_on = [&](int m) { return arrival_ids(project.load_map(static_cast<std::size_t>(m)), context.info); };
    context.busiest_records = busiest_map_records(FlexEntries::read((data / "STATIC/FIXED.DAT").string()).entries);
    std::map<std::string, int> kinds;
    int errors = 0, items = 0;
    for (std::size_t m : project.used_maps()) {
        if (m == 0 || m > 255) continue;
        context.map = static_cast<int>(m);
        const Level level = project.load_map(m);
        items += static_cast<int>(level.items.size());
        for (const Problem& p : check_level(level, context)) {
            const std::string kind = p.text.substr(0, p.text.find_first_of("(:0"));
            ++kinds[(p.severity == Severity::Error ? "E " : p.severity == Severity::Warning ? "W " : "N ") + kind];
            errors += p.severity == Severity::Error;
        }
    }
    for (const auto& [kind, n] : kinds) std::printf("  %4d %s\n", n, kind.c_str());
    std::printf("level check: %d error(s) over %d items\n", errors, items);
    check(errors * 500 < items, "few errors in the original maps");
}

void check_trigger_targets(const std::filesystem::path& data) {
    // The objects that act through TRIGGER on the shape their map and NPC
    // numbers name (switches, panels, plates, cameras...), told apart by the
    // original maps: switches and panels are, chests (their contents) are not.
    const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    const auto usecode = UsecodeFile::from_flex(FlexArchive::open(data / "USECODE/EUSECODE.FLX"));
    std::uint16_t trigger = 0;
    for (std::uint16_t c = 0xA00; c < 0xB00 && !trigger; ++c) {
        if (usecode.has_class(c) && usecode.class_name(c) == "TRIGGER") trigger = c;
    }
    check(trigger != 0, "TRIGGER class");
    std::vector<bool> calls(0x800, false);
    for (std::uint16_t c = 0; c < 0x800; ++c) {
        calls[c] = usecode.has_class(c) && code_calls_class(usecode.code(c), trigger);
    }
    LevelInfo info;
    info.family = [&](std::uint16_t s) -> std::uint8_t { return s < typeflags.size() ? typeflags[s].family : 0; };
    info.has_class = [&](std::uint16_t cid) { return usecode.has_class(cid); };
    const FlexEntries fixed = FlexEntries::read((data / "STATIC/FIXED.DAT").string());
    std::vector<Level> maps;
    for (std::size_t m = 1; m < fixed.entries.size() && m < 256; ++m) {
        if (!fixed.entries[m].empty()) maps.push_back(Level::parse(fixed.entries[m]));
    }
    const std::vector<bool> users = find_trigger_users(maps, info, calls);
    std::string names;
    int count = 0;
    bool spanel = false, chest = false;
    for (std::uint16_t c = 0; c < users.size(); ++c) {
        if (!users[c]) continue;
        ++count;
        const std::string n = usecode.class_name(c);
        names += " " + n;
        spanel |= n == "SPANEL";
        chest |= n == "CHEST_NS" || n == "CHEST_EW";
    }
    std::printf("TRIGGER (0x%03X): %d classes act on the shape they name:%s\n", trigger, count, names.c_str());
    check(count >= 15 && spanel && !chest, "switches and panels name their target shape, chests do not");
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
        check_level_check(data);
        check_trigger_targets(data);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
