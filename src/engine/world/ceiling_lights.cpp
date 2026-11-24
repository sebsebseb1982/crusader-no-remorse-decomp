#include "engine/world/ceiling_lights.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace crusader {

namespace {

// The built-in kinds, as kinds.json ships them. Few lamps (some 1150 units
// apart), strong and far reaching: a lamp's light falls off fast ((1 - d^2 /
// r^2)^2), so that with many weak lamps close together each lit only the
// floor under it and the next ones filled the shadows; one lamp lighting most
// of its stretch of floor makes crates, machines and people cast long, dark
// shadows. Round them the ambient light is much lower (a third of the game's
// default), so that the lamps, not the general light, light the room.
const std::vector<CeilingLightKind> kBuiltIn = {
    {"warm", {1.0f, 0.86f, 0.66f}, 1.15f, 1200, 0.8f},    // offices, quarters
    {"neon", {0.82f, 0.93f, 1.0f}, 1.15f, 1200, 0.8f},    // labs, corridors, machine rooms
    {"sodium", {1.0f, 0.68f, 0.38f}, 1.15f, 1200, 0.8f},  // stores, docks
    {"dim", {1.0f, 0.84f, 0.62f}, 0.6f, 800, 0.6f},       // small rooms, service areas
    {"red", {1.0f, 0.22f, 0.12f}, 0.9f, 900, 0.8f},       // emergency
};

struct Store {
    std::vector<std::filesystem::path> dirs;
    std::optional<std::vector<CeilingLightKind>> kinds;
    std::map<int, std::vector<CeilingLight>> maps;
};

Store& store() {
    static Store s;
    return s;
}

// The document of the first folder's `name`, if any (a file that does not
// parse is told on the error output, and read as none).
// @orig none: port lighting (ceiling lights)
std::optional<JsonValue> read_json_file(const std::string& name) {
    for (const auto& dir : store().dirs) {
        const auto path = dir / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::string why;
        if (auto doc = parse_json(text.str(), &why)) return doc;
        std::fprintf(stderr, "ceiling lights: %s: %s\n", path.string().c_str(), why.c_str());
        return std::nullopt;
    }
    return std::nullopt;
}

// The kind named `name`, else -1.
int find_kind(const std::vector<CeilingLightKind>& kinds, const std::string& name) {
    for (std::size_t i = 0; i < kinds.size(); ++i)
        if (kinds[i].name == name) return static_cast<int>(i);
    return -1;
}

}  // namespace

// @orig none: port lighting (ceiling lights)
void set_ceiling_lights_dirs(std::vector<std::filesystem::path> dirs) {
    store().dirs = std::move(dirs);
    reload_ceiling_lights();
}

// @orig none: port lighting (ceiling lights)
void reload_ceiling_lights() {
    store().kinds.reset();
    store().maps.clear();
}

// @orig none: port lighting (ceiling lights)
std::vector<CeilingLightKind> parse_ceiling_light_kinds(const JsonValue& doc) {
    std::vector<CeilingLightKind> kinds = kBuiltIn;
    const JsonValue* list = doc.get("kinds");
    if (!list || !list->is_object()) return kinds;
    for (const auto& [name, v] : list->object) {
        if (!v.is_object()) continue;
        const int at = find_kind(kinds, name);
        CeilingLightKind k = kinds[at >= 0 ? at : 0];
        k.name = name;
        if (const JsonValue* c = v.get("colour"); c && c->is_array() && c->array.size() == 3)
            k.colour = {float(c->array[0].number_or(k.colour.x)), float(c->array[1].number_or(k.colour.y)),
                        float(c->array[2].number_or(k.colour.z))};
        k.strength = std::max(0.0f, float(v.number_at("strength", k.strength)));
        k.radius = std::max(1.0f, float(v.number_at("radius", k.radius)));
        k.dim = std::clamp(float(v.number_at("dim", k.dim)), 0.0f, 1.0f);
        if (at >= 0) kinds[at] = k;
        else kinds.push_back(k);
    }
    return kinds;
}

// @orig none: port lighting (ceiling lights)
std::vector<CeilingLight> parse_ceiling_lights(const JsonValue& doc, const std::vector<CeilingLightKind>& kinds) {
    std::vector<CeilingLight> out;
    const JsonValue* rooms = doc.get("rooms");
    if (!rooms || !rooms->is_array()) return out;
    const auto kind_of = [&](const std::string& name) { return std::max(0, find_kind(kinds, name)); };
    for (const JsonValue& room : rooms->array) {
        const JsonValue* lights = room.get("lights");
        if (!lights || !lights->is_array()) continue;
        const int room_kind = kind_of(room.string_at("kind", "warm"));
        for (const JsonValue& l : lights->array) {
            if (!l.is_array() || l.array.size() < 3) continue;
            CeilingLight c;
            c.x = static_cast<std::int32_t>(l.array[0].number_or(0));
            c.y = static_cast<std::int32_t>(l.array[1].number_or(0));
            c.z = static_cast<std::int32_t>(l.array[2].number_or(0));
            c.radius = l.array.size() > 3 ? std::max(0.0f, float(l.array[3].number_or(0))) : 0.0f;
            c.kind = l.array.size() > 4 && l.array[4].is_string() ? kind_of(l.array[4].string) : room_kind;
            out.push_back(c);
        }
    }
    return out;
}

// @orig none: port lighting (ceiling lights)
const std::vector<CeilingLightKind>& ceiling_light_kinds() {
    Store& s = store();
    if (!s.kinds) {
        const auto doc = read_json_file("kinds.json");
        s.kinds = doc ? parse_ceiling_light_kinds(*doc) : kBuiltIn;
    }
    return *s.kinds;
}

// @orig none: port lighting (ceiling lights)
const std::vector<CeilingLight>& ceiling_lights(int map) {
    Store& s = store();
    if (const auto it = s.maps.find(map); it != s.maps.end()) return it->second;
    char name[32];
    std::snprintf(name, sizeof name, "map%03d.json", map);
    const auto doc = map >= 0 ? read_json_file(name) : std::nullopt;
    return s.maps[map] = doc ? parse_ceiling_lights(*doc, ceiling_light_kinds()) : std::vector<CeilingLight>{};
}

// @orig none: port lighting (ceiling lights)
void add_ceiling_lights(const std::vector<CeilingLight>& lights, float ambient, std::vector<PointLight>& out) {
    // As the lamps (lighting.cpp, fixture_boost): they stand out at the
    // game's bright default ambient light.
    const float boost = 1 + 1.2f * std::clamp(ambient, 0.0f, 1.0f);
    const auto& kinds = ceiling_light_kinds();
    for (const CeilingLight& l : lights) {
        const CeilingLightKind& k = kinds[std::clamp<std::size_t>(static_cast<std::size_t>(l.kind), 0, kinds.size() - 1)];
        const float s = k.strength * boost;
        PointLight p{{float(l.x), float(l.y), l.z * kLightZScale},
                     {k.colour.x * s, k.colour.y * s, k.colour.z * s},
                     l.radius > 0 ? l.radius : k.radius};
        p.dim = k.dim;
        out.push_back(p);
    }
}

}  // namespace crusader
