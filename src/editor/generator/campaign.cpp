#include "editor/generator/campaign.hpp"

#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/shape.hpp"

#include <map>

namespace crusader::editor::gen {

GameData GameData::load(const std::filesystem::path& data) {
    GameData d;
    d.shapes = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
    const FlexEntries globs = FlexEntries::read((data / "STATIC/GLOB.FLX").string());
    for (const auto& e : globs.entries) d.globs.push_back(Glob::parse(e));
    const FlexEntries fixed = FlexEntries::read((data / "STATIC/FIXED.DAT").string());
    for (const auto& e : fixed.entries) d.maps.push_back(Level::parse(e));
    // Map 244 holds records that are no items.
    if (d.maps.size() > 244) d.maps[244] = Level{};
    // Class cid is entry cid + 2 of EUSECODE.FLX (docs/usecode.md).
    const FlexEntries usecode = FlexEntries::read((data / "USECODE/EUSECODE.FLX").string());
    d.scripted.assign(d.shapes.size(), false);
    for (std::size_t c = 0; c < d.scripted.size() && c + 2 < usecode.entries.size(); ++c) {
        d.scripted[c] = !usecode.entries[c + 2].empty();
    }
    // Class names: 13 bytes each from offset 4 of entry 1.
    d.class_names.assign(d.shapes.size(), {});
    if (usecode.entries.size() > 1) {
        const auto& names = usecode.entries[1];
        for (std::size_t c = 0; c < d.class_names.size(); ++c) {
            for (std::size_t i = 4 + 13 * c; i < 4 + 13 * (c + 1) && i < names.size() && names[i]; ++i) {
                d.class_names[c] += static_cast<char>(names[i]);
            }
        }
    }
    // Black frames: most of their pixels are near black in the game palette.
    const FlexArchive shapes = FlexArchive::open(data / "STATIC/SHAPES.FLX");
    const Palette palette = Palette::load(data / "STATIC/GAMEPAL.PAL");
    std::map<Piece, bool> black;
    auto is_black = [&](Piece p) {
        const auto it = black.find(p);
        if (it != black.end()) return it->second;
        bool result = false;
        if (p.shape < shapes.count() && !shapes.entry(p.shape).empty()) {
            const Shape shape = Shape::parse(shapes.entry(p.shape), ShapeLayout::World, true);
            if (p.frame < shape.frames().size()) {
                const ShapeFrame& f = shape.frames()[p.frame];
                int opaque = 0, dark = 0;
                for (int y = 0; y < f.height; ++y) {
                    for (int x = 0; x < f.width; ++x) {
                        const int c = f.pixel(x, y);
                        if (c < 0) continue;
                        ++opaque;
                        const Rgb& rgb = palette.colors[c];
                        if (rgb.r + rgb.g + rgb.b < 60) ++dark;
                    }
                }
                result = opaque > 0 && dark * 10 > opaque * 8;
            }
        }
        black[p] = result;
        return result;
    };
    d.biomes = learn_biomes({d.shapes, d.globs, d.maps, d.scripted, is_black});
    return d;
}

std::vector<GeneratedLevel> generate_levels(Project& project, const GameData& data, std::uint32_t seed, int levels,
                                            int biome, std::string& error) {
    std::vector<int> maps;
    for (std::size_t m = 1; m < project.map_slots() && static_cast<int>(maps.size()) < levels; ++m) {
        if (!project.map_used(m)) maps.push_back(static_cast<int>(m));
    }
    if (static_cast<int>(maps.size()) < levels) {
        error = "FIXED.DAT has only " + std::to_string(maps.size()) + " free maps";
        return {};
    }
    std::vector<GeneratedLevel> out;
    for (int k = 0; k < levels; ++k) {
        Options o;
        o.seed = seed + static_cast<std::uint32_t>(k);
        o.biome = biome;
        o.map = maps[k];
        o.next_map = maps[(k + 1) % levels];
        // Cards stay with the player: each level numbers its own (up to 3).
        o.first_card = 1 + (3 * k) % 30;
        GeneratedLevel g{maps[k], generate(data.biomes, o), {}};
        g.entry = manifest_level(o, g.result, data.biomes);
        const std::string problem = check_solvable(g.result);
        if (!problem.empty()) {
            error = "seed " + std::to_string(o.seed) + ": " + problem;
            return {};
        }
        project.store_map(static_cast<std::size_t>(maps[k]), g.result.level);
        out.push_back(std::move(g));
    }
    return out;
}

std::vector<GeneratedLevel> regenerate_levels(Project& project, const GameData& data, const Manifest& manifest,
                                              std::string& error) {
    if (manifest.version != kGeneratorVersion) {
        error = std::string(kManifestFile) + " comes from generator version " + std::to_string(manifest.version) +
                ", this is version " + std::to_string(kGeneratorVersion) + ": its seeds would give other levels";
        return {};
    }
    std::vector<GeneratedLevel> out;
    for (const ManifestLevel& l : manifest.levels) {
        if (l.map <= 0 || static_cast<std::size_t>(l.map) >= project.map_slots()) {
            error = "map " + std::to_string(l.map) + " does not fit FIXED.DAT";
            return {};
        }
        if (l.source_map > 0) {
            // A mission: the same seed and source map give the same level.
            MissionOptions mo;
            mo.seed = l.seed;
            mo.source_map = l.source_map;
            mo.map = l.map;
            mo.next_map = l.next_map;
            GeneratedLevel g{l.map, generate_mission(data, demo_mission(), mo), l};
            if (g.result.rooms.empty()) {
                error = "seed " + std::to_string(l.seed) + ": " + (g.result.notes.empty() ? "no mission" : g.result.notes.front());
                return {};
            }
            out.push_back(std::move(g));
            continue;
        }
        const Options o = l.options();
        GeneratedLevel g{l.map, generate(data.biomes, o), {}};
        g.entry = manifest_level(o, g.result, data.biomes);
        const std::string problem = check_solvable(g.result);
        if (!problem.empty()) {
            error = "seed " + std::to_string(o.seed) + ": " + problem;
            return {};
        }
        out.push_back(std::move(g));
    }
    for (const GeneratedLevel& g : out) project.store_map(static_cast<std::size_t>(g.map), g.result.level);
    return out;
}

std::vector<GeneratedLevel> generate_mission_levels(Project& project, const GameData& data, std::uint32_t seed,
                                                    int source_map, std::string& error) {
    int map = 0;
    for (std::size_t m = 1; m < project.map_slots() && !map; ++m) {
        if (!project.map_used(m)) map = static_cast<int>(m);
    }
    if (!map) {
        error = "FIXED.DAT has no free map";
        return {};
    }
    MissionOptions o;
    o.seed = seed;
    o.source_map = source_map;
    o.map = map;
    o.next_map = map;
    GeneratedLevel g{map, generate_mission(data, demo_mission(), o), {}};
    g.entry.map = map;
    g.entry.seed = seed;
    g.entry.source_map = source_map;
    g.entry.next_map = map;
    g.entry.room_count = static_cast<int>(g.result.rooms.size());
    if (g.result.biome >= 0 && g.result.biome < static_cast<int>(data.biomes.size())) {
        g.entry.biome_name = data.biomes[static_cast<std::size_t>(g.result.biome)].name();
    }
    if (g.result.rooms.empty()) {
        error.clear();
        for (const std::string& n : g.result.notes) error += (error.empty() ? "" : "\n") + n;
        if (error.empty()) error = "no mission";
        return {};
    }
    const std::string problem = check_solvable(g.result);
    if (!problem.empty()) {
        error = "seed " + std::to_string(seed) + ": " + problem;
        for (const Link& l : g.result.links) {
            error += " [" + std::to_string(l.a) + "-" + std::to_string(l.b) + " lock " + std::to_string(l.lock) + "]";
        }
        for (const Lock& k : g.result.locks) {
            error += " {room " + std::to_string(k.room) + " key " + std::to_string(k.key_room) + " link " +
                     std::to_string(k.link) + "}";
        }
        return {};
    }
    project.store_map(static_cast<std::size_t>(map), g.result.level);
    return {g};
}

void record_levels(const std::filesystem::path& mod, const std::vector<GeneratedLevel>& levels) {
    Manifest manifest = Manifest::read(mod).value_or(Manifest{});
    manifest.version = kGeneratorVersion;
    for (const GeneratedLevel& g : levels) manifest.set(g.entry);
    manifest.write(mod);
}

}  // namespace crusader::editor::gen
