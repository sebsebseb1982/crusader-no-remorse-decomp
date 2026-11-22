#include "editor/level_tools.hpp"

#include "core/language.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    std::map<std::uint8_t, int> carried;
    for (const LevelItem& i : items) {
        if (is_linked_item(i, info)) ++carried[i.quality_low()];
    }
    std::map<std::uint8_t, std::uint8_t> links;
    for (const LevelItem& i : items) {
        if (!is_linked_item(i, info) || links.count(i.quality_low()) || !used.count(i.quality_low())) continue;
        if (source_map == target_map && carried[i.quality_low()] < 2) continue;
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

const LevelItem* monster_partner(const Level& level, const LevelItem& item, const LevelInfo& info) {
    if (item.shape != kShapeMonster || item.frame > 1 || !info.has_class(kShapeMonster)) return nullptr;
    const LevelItem* best = nullptr;
    std::int32_t best_d = 0;
    for (const LevelItem& i : level.items) {
        if (i.shape != kShapeMonster || i.frame + item.frame != 1 || i.quality_low() != item.quality_low()) continue;
        const std::int32_t d = std::max(std::abs(i.x - item.x), std::abs(i.y - item.y));
        if (d > kMonsterRange || (best && d >= best_d)) continue;
        best = &i;
        best_d = d;
    }
    return best;
}

std::vector<std::uint32_t> linked_group(const Level& level, const LevelItem& item, const LevelInfo& info,
                                        std::int32_t range) {
    std::vector<std::uint32_t> group;
    if (!is_linked_item(item, info)) return group;
    group.push_back(item.uid);
    for (const LevelItem& i : level.items) {
        if (i.uid == item.uid || i.quality_low() != item.quality_low() || !is_linked_item(i, info)) continue;
        if (std::abs(i.x - item.x) > range || std::abs(i.y - item.y) > range) continue;
        group.push_back(i.uid);
    }
    return group;
}

std::vector<bool> find_trigger_users(const std::vector<Level>& maps, const LevelInfo& info,
                                     const std::vector<bool>& calls_trigger) {
    std::vector<int> uses(calls_trigger.size(), 0), named(calls_trigger.size(), 0);
    for (const Level& level : maps) {
        for (const LevelItem& i : level.items) {
            if (i.shape >= calls_trigger.size() || !calls_trigger[i.shape]) continue;
            const auto group = linked_group(level, i, info, 2048);
            if (group.size() < 2) continue;
            ++uses[i.shape];
            for (std::size_t k = 1; k < group.size(); ++k) {
                if (level.find(group[k])->shape == trigger_target(i)) {
                    ++named[i.shape];
                    break;
                }
            }
        }
    }
    std::vector<bool> users(calls_trigger.size(), false);
    for (std::size_t s = 0; s < users.size(); ++s) users[s] = named[s] >= 3 && named[s] * 10 >= uses[s] * 6;
    return users;
}

TriggerSettings trigger_settings(const std::vector<Level>& maps, const LevelInfo& info, const std::vector<bool>& users) {
    std::map<std::uint32_t, std::map<std::pair<std::uint8_t, std::uint8_t>, int>> counts;
    for (const Level& level : maps) {
        for (const LevelItem& i : level.items) {
            if (i.shape >= users.size() || !users[i.shape]) continue;
            const std::uint16_t target = trigger_target(i);
            const auto group = linked_group(level, i, info, 2048);
            for (std::size_t k = 1; k < group.size(); ++k) {
                if (level.find(group[k])->shape != target) continue;
                const std::pair<std::uint8_t, std::uint8_t> setting{i.quality_high(),
                                                                    static_cast<std::uint8_t>(i.map_number & 0x1F)};
                ++counts[std::uint32_t{i.shape} << 16 | target][setting];
                ++counts[0xFFFFu << 16 | target][setting];
                break;
            }
        }
    }
    TriggerSettings settings;
    for (const auto& [key, c] : counts) {
        TriggerSetting best;
        for (const auto& [setting, n] : c) {
            best.of += n;
            if (n > best.uses) best.quality_high = setting.first, best.flags = setting.second, best.uses = n;
        }
        settings[key] = best;
    }
    return settings;
}

const TriggerSetting* find_trigger_setting(const TriggerSettings& settings, std::uint16_t user, std::uint16_t target) {
    if (auto it = settings.find(std::uint32_t{user} << 16 | target); it != settings.end() && it->second.uses >= 3) {
        return &it->second;
    }
    auto it = settings.find(0xFFFFu << 16 | target);
    return it != settings.end() ? &it->second : nullptr;
}

void apply_trigger_setting(LevelItem& user, const TriggerSetting& setting) {
    user.quality = static_cast<std::uint16_t>(setting.quality_high << 8 | user.quality_low());
    user.map_number = static_cast<std::uint8_t>((user.map_number & 0xE0) | (setting.flags & 0x1F));
}

std::vector<std::uint8_t> Prefab::serialize() const {
    std::vector<std::uint8_t> out = {'C', 'P', 'F', '1'};
    for (const std::int32_t v : {anchor_x, anchor_y, anchor_z, static_cast<std::int32_t>(source_map)}) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    Level level;
    level.items = items;
    const auto records = level.serialize();
    out.insert(out.end(), records.begin(), records.end());
    return out;
}

std::optional<Prefab> Prefab::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes[0] != 'C' || bytes[1] != 'P' || bytes[2] != 'F' || bytes[3] != '1') {
        return std::nullopt;
    }
    auto u16 = [&](std::size_t at) { return static_cast<std::int32_t>(bytes[at] | bytes[at + 1] << 8); };
    Prefab p;
    p.anchor_x = u16(4);
    p.anchor_y = u16(6);
    p.anchor_z = u16(8);
    p.source_map = u16(10);
    Level level = Level::parse(bytes.subspan(12));
    for (LevelItem& i : level.items) assign_uids(i);
    p.items = std::move(level.items);
    if (p.items.empty()) return std::nullopt;
    return p;
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
        } else if (i.shape == kShapeMonster && i.frame <= 1 && info.has_class(kShapeMonster) &&
                   !monster_partner(level, i, info)) {
            add(Severity::Warning, i.uid,
                i.frame == 1
                    ? tr("Type de PNJ (MONSTER image 1) sans place (MONSTER image 0) de même lien à moins de 64 "
                         "unités : il ne sert à rien.",
                         "NPC kind (MONSTER frame 1) without a place (MONSTER frame 0) of the same link within 64 "
                         "units: it is of no use.")
                    : tr("Place de PNJ (MONSTER image 0) sans type (MONSTER image 1) de même lien à moins de 64 "
                         "unités : aucun PNJ n'y apparaît.",
                         "NPC place (MONSTER frame 0) without a kind (MONSTER frame 1) of the same link within 64 "
                         "units: no NPC appears there."));
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
