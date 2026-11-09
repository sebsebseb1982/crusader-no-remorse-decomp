#include "editor/item_semantics.hpp"

#include "core/language.hpp"

#include <cstdio>

namespace crusader::editor {

const char* family_name(std::uint8_t family) {
    switch (family) {
        case kFamGeneric: return tr("décor", "scenery");
        case kFamQuality: return tr("décor à qualité", "scenery with quality");
        case kFamGlobEgg: return tr("œuf de glob", "glob egg");
        case kFamTriggerEgg: return tr("œuf déclencheur", "trigger egg");
        case kFamContainer: return tr("conteneur", "container");
        case kFamMonsterEgg: return tr("œuf de monstre", "monster egg");
        case kFamTeleportEgg: return tr("œuf de téléportation", "teleport egg");
        case kFamWeapon: return tr("arme", "weapon");
        case kFamAmmo: return tr("munitions", "ammunition");
        case kFamInventory: return tr("objet d'inventaire", "inventory item");
        case kFamUsable: return tr("objet utilisable", "usable item");
        default: return tr("famille inconnue", "unknown family");
    }
}

std::uint16_t usecode_class_of(const LevelItem& item, std::uint8_t family) {
    if (family == kFamTriggerEgg) return static_cast<std::uint16_t>(kClassEggs + item.quality_low());
    return item.shape;
}

const char* event_name(std::uint32_t event) {
    static const char* const names[] = {
        "look",          "use",           "anim",         "setActivity", "cachedIn",
        "hit",           "gotHit",        "hatch",        "schedule",    "release",
        "equip",         "unequip",       "combine",      "func0D",      "calledFromAnim",
        "enterFastArea", "leaveFastArea", "cast",         "justMoved",   "turnOff",
        "animGetHit",    "unhatch",
    };
    return event < std::size(names) ? names[event] : "";
}

const char* event_help(std::uint32_t event) {
    switch (event) {
        case 0x01: return tr("le joueur utilise l'objet", "the player uses the item");
        case 0x04: return tr("la carte se charge (objets « bruyants »)", "the map loads (noisy items)");
        case 0x05: return tr("l'objet en heurte un autre en bougeant", "the item bumps into another while moving");
        case 0x06: return tr("touché : contact (dalle, plaque) ou dégâts", "hit: contact (pad, plate) or damage");
        case 0x07: return tr("le joueur entre dans la zone de l'œuf", "the player enters the egg's area");
        case 0x09: return tr("un objet quitte sa surface (téléporteurs, ascenseurs)",
                             "an item leaves its surface (teleporters, lifts)");
        case 0x0A: return tr("mise en marche (equip, envoyé par TRIGGER et les scripts)",
                             "switch on (equip, sent by TRIGGER and scripts)");
        case 0x0B: return tr("remise en route d'un marqueur (unequip)", "restart from a marker (unequip)");
        case 0x0E: return tr("image marquée d'une animation", "flagged frame of an animation");
        case 0x0F: return tr("entre dans la zone active", "enters the fast area");
        case 0x10: return tr("sort de la zone active", "leaves the fast area");
        case 0x13: return tr("arrêt (lasers, murs d'énergie, portes)", "switch off (lasers, force fields, doors)");
        case 0x15: return tr("le joueur sort de la zone de l'œuf", "the player leaves the egg's area");
        default: return "";
    }
}

HatchZone hatch_zone(const LevelItem& egg) {
    return {32 * (egg.npc_number >> 4), 32 * (egg.npc_number & 0xF)};
}

void set_hatch_zone(LevelItem& egg, int units_x, int units_y) {
    if (units_x < 0) units_x = 0;
    if (units_x > 15) units_x = 15;
    if (units_y < 0) units_y = 0;
    if (units_y > 15) units_y = 15;
    egg.npc_number = static_cast<std::uint8_t>(units_x << 4 | units_y);
}

bool hatches(std::uint8_t family, const LevelItem& item) {
    return family == kFamTriggerEgg || (family == kFamTeleportEgg && item.frame == 0);
}

MonsterEgg read_monster_egg(const LevelItem& egg) {
    MonsterEgg m;
    m.direction = egg.npc_number & 0xF;
    m.record_12 = egg.npc_number >> 4;
    m.min_difficulty = (egg.map_number & 3) + 1;
    m.other_weapon = (egg.map_number & 4) != 0;
    if (!egg.contents.empty()) {
        const LevelItem& t = egg.contents.front();
        m.npc_type = t.npc_number;
        m.activity[0] = t.quality_high();
        m.activity[1] = t.quality_low();
        m.activity[2] = t.map_number;
    }
    return m;
}

void write_monster_egg(LevelItem& egg, const MonsterEgg& m) {
    egg.npc_number = static_cast<std::uint8_t>((m.record_12 & 0xF) << 4 | (m.direction & 0xF));
    egg.map_number = static_cast<std::uint8_t>((egg.map_number & ~7) | ((m.min_difficulty - 1) & 3) |
                                               (m.other_weapon ? 4 : 0));
    if (egg.contents.empty()) {
        LevelItem t;
        t.uid = new_uid();
        t.x = 1;
        t.shape = kShapeNpcTemplate;
        t.flags = kFlagContained;
        egg.contents.push_back(t);
        set_depths(egg);
    }
    LevelItem& t = egg.contents.front();
    t.npc_number = static_cast<std::uint8_t>(m.npc_type);
    t.quality = static_cast<std::uint16_t>((m.activity[0] & 0xFF) << 8 | (m.activity[1] & 0xFF));
    t.map_number = static_cast<std::uint8_t>(m.activity[2]);
}

LevelItem make_monster_egg(std::int32_t x, std::int32_t y, std::int32_t z, int npc_type) {
    LevelItem egg;
    egg.uid = new_uid();
    egg.x = x;
    egg.y = y;
    egg.z = z;
    egg.shape = kShapeMonsterEgg;
    MonsterEgg m;
    m.npc_type = npc_type;
    m.activity[0] = 8;  // guard, as most eggs of the original maps
    m.activity[1] = 1;
    m.activity[2] = 5;
    write_monster_egg(egg, m);
    return egg;
}

MarkerTarget read_marker(const LevelItem& marker) {
    return {static_cast<std::uint16_t>(marker.map_number + (marker.npc_number & 0xE0) * 8),
            static_cast<std::uint8_t>(marker.npc_number & 0xF)};
}

void write_marker(LevelItem& marker, MarkerTarget t) {
    marker.map_number = static_cast<std::uint8_t>(t.shape & 0xFF);
    marker.npc_number = static_cast<std::uint8_t>(((t.shape & 0x700) >> 3) | (t.frame & 0xF));
}

const char* activity_name(int activity) {
    switch (activity) {
        case 0: return tr("aucune", "none");
        case 1: return tr("debout (attend de voir le joueur)", "stand (waits to see the player)");
        case 2: return tr("flâne autour de sa place", "loiter around its place");
        case 3: return tr("marche droit devant", "walk straight on");
        case 5: return tr("attaque", "attack");
        case 7: return tr("se rend", "surrender");
        case 8: return tr("monte la garde", "guard");
        case 9: return tr("combat (variante 9)", "combat (variant 9)");
        case 10: return tr("combat (variante 10)", "combat (variant 10)");
        case 11: return tr("combat (variante 11)", "combat (variant 11)");
        case 12: return tr("combat (variante 12)", "combat (variant 12)");
        default: return "?";
    }
}

std::string describe(const LevelItem& item, std::uint8_t family, const std::string& class_name) {
    char buf[160];
    switch (family) {
        case kFamGlobEgg:
            std::snprintf(buf, sizeof buf, "glob %u", item.quality);
            return buf;
        case kFamTriggerEgg:
            std::snprintf(buf, sizeof buf, "%s %s 0x%02X (id %u)", tr("œuf", "egg"),
                          class_name.empty() ? "?" : class_name.c_str(), item.quality_low(), item.map_number);
            return buf;
        case kFamTeleportEgg:
            if (item.frame == 1) {
                std::snprintf(buf, sizeof buf, "%s 0x%02X", tr("arrivée", "arrival"), item.quality_low());
            } else {
                std::snprintf(buf, sizeof buf, "%s -> %s %u, %s 0x%02X", tr("départ", "departure"),
                              tr("carte", "map"), item.map_number, tr("œuf", "egg"), item.quality_low());
            }
            return buf;
        case kFamMonsterEgg: {
            const MonsterEgg m = read_monster_egg(item);
            std::snprintf(buf, sizeof buf, "%s %d", tr("monstre type", "monster type"), m.npc_type);
            return buf;
        }
        default:
            break;
    }
    if (item.shape == kShapeMarker) {
        const MarkerTarget t = read_marker(item);
        std::snprintf(buf, sizeof buf, "%s 0x%03X:%u (q 0x%02X)", tr("marqueur", "marker"), t.shape, t.frame,
                      item.quality_low());
        return buf;
    }
    if (!class_name.empty()) {
        std::snprintf(buf, sizeof buf, "%s (q 0x%04X)", class_name.c_str(), item.quality);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%s 0x%03X:%u", tr("forme", "shape"), item.shape, item.frame);
    return buf;
}

}  // namespace crusader::editor
