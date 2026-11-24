#include "editor/generator/manifest.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

namespace crusader::editor::gen {

namespace {

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

}  // namespace

Options ManifestLevel::options() const {
    Options o;
    o.seed = seed;
    o.biome = biome;
    o.rooms = rooms;
    o.locks = locks;
    o.first_card = first_card;
    o.map = map;
    o.next_map = next_map;
    return o;
}

ManifestLevel manifest_level(const Options& options, const Result& result, const std::vector<Biome>& biomes) {
    ManifestLevel l;
    l.map = options.map;
    l.seed = options.seed;
    l.biome = options.biome;
    l.rooms = options.rooms;
    l.locks = options.locks;
    l.first_card = options.first_card;
    l.next_map = options.next_map;
    if (result.biome >= 0 && result.biome < static_cast<int>(biomes.size())) {
        l.biome_name = biomes[static_cast<std::size_t>(result.biome)].name();
    }
    l.room_count = static_cast<int>(result.rooms.size());
    return l;
}

std::optional<Manifest> Manifest::read(const std::filesystem::path& mod, std::string* error) {
    std::ifstream in(mod / kManifestFile, std::ios::binary);
    if (!in) {
        if (error) *error = "no " + std::string(kManifestFile);
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto json = parse_json(text, error);
    if (!json || !json->is_object()) return std::nullopt;
    Manifest m;
    m.version = static_cast<int>(json->number_at("version", 0));
    if (const JsonValue* levels = json->get("levels"); levels && levels->is_array()) {
        for (const JsonValue& v : levels->array) {
            if (!v.is_object()) continue;
            ManifestLevel l;
            l.map = static_cast<int>(v.number_at("map", 0));
            l.seed = static_cast<std::uint32_t>(v.number_at("seed", 0));
            l.biome = static_cast<int>(v.number_at("biome", -1));
            l.rooms = static_cast<int>(v.number_at("rooms", 0));
            l.locks = static_cast<int>(v.number_at("locks", -1));
            l.first_card = static_cast<int>(v.number_at("first_card", 1));
            l.next_map = static_cast<int>(v.number_at("next_map", 0));
            l.biome_name = v.string_at("biome_name", "");
            l.room_count = static_cast<int>(v.number_at("room_count", 0));
            if (l.map > 0) m.levels.push_back(l);
        }
    }
    return m;
}

std::string Manifest::to_json() const {
    std::ostringstream out;
    out << "{\n  \"generator\": " << quoted(kGeneratorName) << ",\n  \"version\": " << version
        << ",\n  \"levels\": [";
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const ManifestLevel& l = levels[i];
        out << (i ? ",\n" : "\n") << "    {\"map\": " << l.map << ", \"seed\": " << l.seed << ", \"biome\": " << l.biome
            << ", \"rooms\": " << l.rooms << ", \"locks\": " << l.locks
            << ", \"first_card\": " << l.first_card << ", \"next_map\": " << l.next_map
            << ", \"biome_name\": " << quoted(l.biome_name) << ", \"room_count\": " << l.room_count << "}";
    }
    out << (levels.empty() ? "]\n}\n" : "\n  ]\n}\n");
    return out.str();
}

void Manifest::write(const std::filesystem::path& mod) const {
    std::filesystem::create_directories(mod);
    std::ofstream out(mod / kManifestFile, std::ios::binary);
    out << to_json();
}

void Manifest::set(const ManifestLevel& level) {
    auto it = std::find_if(levels.begin(), levels.end(), [&](const ManifestLevel& l) { return l.map == level.map; });
    if (it != levels.end()) *it = level;
    else levels.push_back(level);
}

const ManifestLevel* Manifest::find(int map) const {
    auto it = std::find_if(levels.begin(), levels.end(), [&](const ManifestLevel& l) { return l.map == map; });
    return it == levels.end() ? nullptr : &*it;
}

}  // namespace crusader::editor::gen
