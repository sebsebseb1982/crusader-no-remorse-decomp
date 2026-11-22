#include "editor/level_tools.hpp"

#include "core/language.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace crusader::editor {

namespace {

std::string hex2(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", v & 0xFF);
    return buf;
}

// Quality lows that the map's links use (linked items, trigger egg
// parameters).
std::set<std::uint8_t> used_link_qualities(const Level& level, const LevelInfo& info) {
    std::set<std::uint8_t> used;
    for (const LevelItem& i : level.items) {
        const std::uint8_t fam = info.family(i.shape);
        if (is_linked_item(i, info)) used.insert(i.quality_low());
        if (fam == kFamTriggerEgg && i.quality_high() != 0) used.insert(i.quality_high());
    }
    return used;
}

std::uint8_t first_free(const std::set<std::uint8_t>& used, std::uint8_t skip) {
    for (int v = 1; v < 256; ++v) {
        if (v != skip && !used.count(static_cast<std::uint8_t>(v))) return static_cast<std::uint8_t>(v);
    }
    return 0;
}

}  // namespace

bool is_arrival(const LevelItem& item, std::uint8_t family) { return family == kFamTeleportEgg && item.frame == 1; }

bool is_departure(const LevelItem& item, std::uint8_t family) { return family == kFamTeleportEgg && item.frame == 0; }

bool links_by_quality(std::uint8_t family) {
    return family != kFamGlobEgg && family != kFamTriggerEgg && family != kFamTeleportEgg && family != kFamMonsterEgg;
}

bool is_linked_item(const LevelItem& item, const LevelInfo& info) {
    const std::uint8_t fam = info.family(item.shape);
    if (item.quality_low() == 0 || !links_by_quality(fam)) return false;
    return item.shape == kShapeMarker || info.has_class(usecode_class_of(item, fam));
}

std::map<std::uint8_t, int> arrival_ids(const Level& level, const LevelInfo& info) {
    std::map<std::uint8_t, int> ids;
    for (const LevelItem& i : level.items) {
        if (is_arrival(i, info.family(i.shape))) ++ids[i.quality_low()];
    }
    return ids;
}

std::uint8_t free_arrival_id(const Level& level, const LevelInfo& info) {
    std::set<std::uint8_t> used;
    for (const auto& [id, n] : arrival_ids(level, info)) used.insert(id);
    return first_free(used, kStartEgg);
}

std::uint8_t free_link_quality(const Level& level, const LevelInfo& info) {
    return first_free(used_link_qualities(level, info), 0);
}

int relink_pasted(std::vector<LevelItem>& items, const Level& target, const LevelInfo& info, int source_map,
                  int target_map) {
    int changed = 0;
    // Links.
    std::set<std::uint8_t> used = used_link_qualities(target, info);
    std::map<std::uint8_t, std::uint8_t> links;
    for (const LevelItem& i : items) {
        if (!is_linked_item(i, info) || links.count(i.quality_low()) || !used.count(i.quality_low())) continue;
        const std::uint8_t q = first_free(used, 0);
        if (q == 0) break;
        used.insert(q);
        links[i.quality_low()] = q;
        ++changed;
    }
    // Arrival ids.
    std::set<std::uint8_t> ids;
    for (const auto& [id, n] : arrival_ids(target, info)) ids.insert(id);
    std::map<std::uint8_t, std::uint8_t> arrivals;
    for (const LevelItem& i : items) {
        if (!is_arrival(i, info.family(i.shape)) || arrivals.count(i.quality_low())) continue;
        std::uint8_t id = i.quality_low();
        if (ids.count(id)) {
            id = first_free(ids, kStartEgg);
            if (id == 0) continue;
            ++changed;
        }
        ids.insert(id);
        arrivals[i.quality_low()] = id;
    }
    auto set_low = [](LevelItem& i, std::uint8_t v) {
        i.quality = static_cast<std::uint16_t>((i.quality & 0xFF00) | v);
    };
    for (LevelItem& i : items) {
        const std::uint8_t fam = info.family(i.shape);
        if (is_linked_item(i, info)) {
            if (auto it = links.find(i.quality_low()); it != links.end()) set_low(i, it->second);
        } else if (fam == kFamTriggerEgg) {
            if (auto it = links.find(i.quality_high()); it != links.end()) {
                i.quality = static_cast<std::uint16_t>(it->second << 8 | i.quality_low());
            }
        } else if (is_arrival(i, fam)) {
            if (auto it = arrivals.find(i.quality_low()); it != arrivals.end()) set_low(i, it->second);
        } else if (is_departure(i, fam) && i.map_number == source_map) {
            // A departure to an arrival pasted with it goes to the pasted one.
            if (auto it = arrivals.find(i.quality_low()); it != arrivals.end()) {
                set_low(i, it->second);
                i.map_number = static_cast<std::uint8_t>(target_map);
            }
        }
    }
    return changed;
}

std::size_t busiest_map_records(const std::vector<std::vector<std::uint8_t>>& entries) {
    std::size_t most = 0;
    for (const auto& e : entries) most = std::max(most, e.size() / kRecordSize);
    return most;
}

std::vector<Problem> check_level(const Level& level, const CheckContext& context) {
    const LevelInfo& info = context.info;
    std::vector<Problem> out;
    auto add = [&](Severity s, std::uint32_t uid, std::string text) { out.push_back({s, uid, std::move(text)}); };

    const std::map<std::uint8_t, int> here = arrival_ids(level, info);
    if (!here.count(kStartEgg)) {
        add(Severity::Note, 0,
            tr("Pas de départ de mission (œuf d'arrivée 0x1E) : une nouvelle partie ou F5 ne peuvent pas commencer sur "
               "cette carte ; un téléporteur peut toujours y mener.",
               "No mission start (arrival egg 0x1E): a new game or F5 cannot start on this map; a teleport can still "
               "lead here."));
    }
    std::set<std::uint8_t> reported;

    for (const LevelItem& i : level.items) {
        const std::uint8_t fam = info.family(i.shape);
        if (is_arrival(i, fam)) {
            const auto n = here.find(i.quality_low());
            if (n != here.end() && n->second > 1 && reported.insert(i.quality_low()).second) {
                add(Severity::Warning, i.uid,
                    std::string(tr("Plusieurs œufs d'arrivée ont l'id ", "Several arrival eggs have id ")) +
                        hex2(i.quality_low()) +
                        tr(" : le jeu en prend un au hasard de sa recherche.",
                           ": the game takes whichever its search finds first."));
            }
        } else if (is_departure(i, fam)) {
            const int to = i.map_number;
            const std::string target = std::string(tr("carte ", "map ")) + std::to_string(to) + ", " +
                                       tr("œuf ", "egg ") + hex2(i.quality_low());
            if (to == 0 || !context.map_exists(to)) {
                add(Severity::Error, i.uid,
                    std::string(tr("Départ vers une carte qui n'existe pas (", "Departure to a map that does not exist (")) +
                        target + tr(") : le téléporteur ne fait rien.", "): the teleport does nothing."));
            } else {
                const auto ids = to == context.map ? here : context.arrivals_on(to);
                if (!ids.count(i.quality_low())) {
                    add(Severity::Error, i.uid,
                        std::string(tr("Départ vers une arrivée qui n'existe pas (", "Departure to a missing arrival (")) +
                            target + tr(") : le téléporteur ne fait rien.", "): the teleport does nothing."));
                }
            }
            if (i.npc_number == 0) {
                add(Severity::Warning, i.uid,
                    tr("Départ sans zone de déclenchement : le joueur doit être exactement sur l'œuf.",
                       "Departure without a trigger area: the player must stand exactly on the egg."));
            }
        } else if (fam == kFamTriggerEgg) {
            const std::uint16_t cid = usecode_class_of(i, fam);
            if (!info.has_class(cid)) {
                add(Severity::Error, i.uid,
                    std::string(tr("Œuf déclencheur de type ", "Trigger egg of type ")) + hex2(i.quality_low()) +
                        tr(" : aucune classe usecode ne le traite.", ": no usecode class handles it."));
            }
        } else if (fam == kFamGlobEgg) {
            // Glob 0 is "no glob" (the original maps have such eggs).
            if (i.quality != 0 && context.glob_pieces(i.quality) == 0) {
                add(Severity::Note, i.uid,
                    std::string(tr("Œuf de glob vide (glob ", "Empty glob egg (glob ")) + std::to_string(i.quality) +
                        tr(") : il ne pose rien.", "): it places nothing."));
            }
        } else if (fam == kFamMonsterEgg) {
            if (i.contents.empty() || i.contents.front().shape != kShapeNpcTemplate) {
                add(Severity::Error, i.uid,
                    tr("Œuf de monstre sans modèle de PNJ (forme 0x002 dedans) : son PNJ n'aura pas de type.",
                       "Monster egg without its NPC template (shape 0x002 inside): its NPC gets no type."));
            }
        }
    }

    const std::size_t records = level.record_count();
    if (records > kItemTableSlots) {
        add(Severity::Error, 0,
            std::string(tr("Trop d'objets pour la table du jeu (", "Too many items for the game's table (")) +
                std::to_string(records) + " / " + std::to_string(kItemTableSlots) + ").");
    } else if (records > context.busiest_records) {
        add(Severity::Warning, 0,
            std::string(tr("Carte plus chargée que celles d'origine (", "Busier than the original maps (")) +
                std::to_string(records) + tr(" enregistrements) : les globs proches pourraient manquer de place.",
                                             " records): nearby globs may run out of room."));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Problem& a, const Problem& b) { return a.severity < b.severity; });
    return out;
}

}  // namespace crusader::editor
