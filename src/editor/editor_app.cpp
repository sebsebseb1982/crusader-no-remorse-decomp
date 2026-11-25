#include "editor/editor_app.hpp"

#include "core/language.hpp"
#include "core/shape.hpp"
#include "core/surface.hpp"

#include <SDL.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace crusader::editor {

namespace {

constexpr float kLeftWidth = 340.0f;
constexpr float kRightWidth = 380.0f;
constexpr float kStatusHeight = 26.0f;
constexpr std::size_t kUndoLimit = 200;

constexpr ImU32 kColSelected = IM_COL32(255, 220, 0, 255);
constexpr ImU32 kColPrimary = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kColLinked = IM_COL32(0, 230, 255, 220);
constexpr ImU32 kColZone = IM_COL32(255, 80, 200, 200);
constexpr ImU32 kColTeleport = IM_COL32(120, 255, 120, 230);
constexpr ImU32 kColGhost = IM_COL32(255, 255, 255, 150);
constexpr ImU32 kColHover = IM_COL32(255, 255, 255, 110);
constexpr ImVec4 kTextError(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kTextWarning(1.0f, 0.75f, 0.3f, 1.0f);
constexpr ImVec4 kTextNote(0.6f, 0.8f, 1.0f, 1.0f);

ImTextureID tex_id(SDL_Texture* t) { return static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(t)); }

std::string hex(unsigned v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%0*X", digits, v);
    return buf;
}

bool input_u8(const char* label, std::uint8_t& v, bool hexa = true) {
    const std::uint8_t one = 1;
    return ImGui::InputScalar(label, ImGuiDataType_U8, &v, &one, nullptr, hexa ? "%02X" : "%u",
                              hexa ? ImGuiInputTextFlags_CharsHexadecimal : 0);
}

bool input_u16(const char* label, std::uint16_t& v, bool hexa = true) {
    const std::uint16_t one = 1;
    return ImGui::InputScalar(label, ImGuiDataType_U16, &v, &one, nullptr, hexa ? "%04X" : "%u",
                              hexa ? ImGuiInputTextFlags_CharsHexadecimal : 0);
}

bool contains_ci(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

EditorApp::EditorApp(SDL_Renderer* renderer, EditorOptions options)
    : renderer_(renderer), options_(std::move(options)) {
    project_ = std::make_unique<Project>(options_.data, options_.mod);
    mod_path_text_ = options_.mod.string();
    load_resources();
    open_map(project_->map_used(static_cast<std::size_t>(options_.map)) ? options_.map
                                                                       : static_cast<int>(project_->used_maps().front()));
    if (options_.new_map) {
        new_empty_map();
        save();
    }
    if (options_.select_shape >= 0) {
        for (const LevelItem& i : level_.items) {
            if (i.shape == options_.select_shape) {
                select_only(i.uid);
                center_on(i.x, i.y, i.z);
                break;
            }
        }
    }
    if (options_.center_x >= 0) center_on(options_.center_x, options_.center_y, options_.center_z);
}

EditorApp::~EditorApp() {
    for (auto& [k, t] : frame_textures_) {
        if (t) SDL_DestroyTexture(t);
    }
    for (auto& [k, t] : glob_textures_) {
        if (t.first) SDL_DestroyTexture(t.first);
    }
    if (view_texture_) SDL_DestroyTexture(view_texture_);
}

// ---------------------------------------------------------------------------
// Data

void EditorApp::load_resources() {
    shapes_flex_ = std::make_unique<FlexArchive>(FlexArchive::open(project_->file("STATIC/SHAPES.FLX")));
    shapes_ = std::make_unique<ShapeCache>(*shapes_flex_, ShapeLayout::World);
    typeflags_ = load_typeflags(project_->file("STATIC/TYPEFLAG.DAT"));
    palette_ = Palette::load(project_->file("STATIC/GAMEPAL.PAL"));
    xform_ = XformTable::load(project_->file("STATIC/XFORMPAL.DAT"));
    usecode_ = std::make_unique<UsecodeFile>(
        UsecodeFile::from_flex(FlexArchive::open(project_->file("USECODE/EUSECODE.FLX"))));
    npc_types_ = dtable::NpcTypes::load(project_->file("STATIC/DTABLE.FLX"));
    busiest_records_ = busiest_map_records(FlexEntries::read((options_.data / "STATIC/FIXED.DAT").string()).entries);
    trigger_class_ = 0;
    for (std::uint16_t c = 0xA00; c < 0xB00 && c < usecode_->class_count() && !trigger_class_; ++c) {
        if (usecode_->has_class(c) && usecode_->class_name(c) == "TRIGGER") trigger_class_ = c;
    }
    // The objects that act on the shape they name (find_trigger_users), told
    // apart by the original maps.
    std::vector<bool> calls(std::min<std::size_t>(usecode_->class_count(), 0x800), false);
    for (std::size_t c = 0; trigger_class_ && c < calls.size(); ++c) {
        const auto cid = static_cast<std::uint16_t>(c);
        if (usecode_->has_class(cid)) calls[c] = code_calls_class(usecode_->code(cid), trigger_class_);
    }
    std::vector<Level> originals;
    try {
        const FlexEntries fixed = FlexEntries::read((options_.data / "STATIC/FIXED.DAT").string());
        for (std::size_t m = 1; m < fixed.entries.size() && m < 256; ++m) {
            if (!fixed.entries[m].empty()) originals.push_back(Level::parse(fixed.entries[m]));
        }
    } catch (const std::exception&) {
    }
    trigger_source_ = find_trigger_users(originals, level_info(), calls);
    trigger_settings_ = trigger_settings(originals, level_info(), trigger_source_);
    load_prefabs();
}

void EditorApp::open_map(int map) {
    commit_map();
    map_ = map;
    level_ = project_->load_map(static_cast<std::size_t>(map));
    level_dirty_ = false;
    undo_.clear();
    redo_.clear();
    selection_.clear();
    primary_ = 0;
    pair_departure_ = link_source_ = hovered_ = 0;
    if (brush_.kind == BrushKind::TeleportPair) brush_ = {};
    world_dirty_ = true;
    center_on_default();
}

void EditorApp::commit_map() {
    if (!level_dirty_ || !project_) return;
    project_->store_map(static_cast<std::size_t>(map_), level_);
    level_dirty_ = false;
    maps_changed();
}

void EditorApp::save() {
    try {
        options_.mod = mod_path_text_;
        if (options_.mod.empty()) {
            message(tr("Indiquez un dossier de mod avant d'enregistrer.", "Set a mod folder before saving."));
            return;
        }
        // The project keeps its mod folder: a new one gets the changes.
        if (project_->mod() != options_.mod) {
            commit_map();
            Project moved(options_.data, options_.mod);
            for (std::size_t m : project_->used_maps()) moved.store_map(m, project_->load_map(m));
            for (std::size_t g = 1; g < project_->glob_count(); ++g) {
                if (!project_->glob_bytes(g).empty()) moved.store_glob(g, project_->glob(g));
            }
            *project_ = std::move(moved);
            maps_changed();
        }
        commit_map();
        project_->save();
        if (manifest_dirty_) {
            manifest().write(options_.mod);
            manifest_dirty_ = false;
        }
        message(std::string(tr("Enregistré dans ", "Saved to ")) + options_.mod.string());
    } catch (const std::exception& e) {
        message(std::string(tr("Échec de l'enregistrement : ", "Save failed: ")) + e.what());
    }
}

void EditorApp::test_in_game() {
    std::uint16_t egg = kStartEgg;
    if (const LevelItem* p = level_.find(primary_);
        p && family_of(p->shape) == kFamTeleportEgg && p->frame == 1) {
        egg = p->quality_low();
    }
    bool found = false;
    for (const LevelItem& i : level_.items) {
        if (family_of(i.shape) == kFamTeleportEgg && i.frame == 1 && i.quality_low() == egg) found = true;
    }
    if (!found) {
        message(tr("Pas de départ de mission sur la carte : onglet Œufs > Départ de mission, puis cliquez dans la vue "
                   "(ou sélectionnez un œuf d'arrivée).",
                   "No mission start on the map: Eggs tab > Mission start, then click in the view (or select an "
                   "arrival egg)."));
        return;
    }
    save();
    if (project_->dirty()) return;
    if (!std::filesystem::exists(project_->mod() / "STATIC/FIXED.DAT")) {
        // Nothing edited yet: the mod folder still gets the maps.
        project_->touch_maps();
        project_->save();
    }
    const auto exe = options_.game_exe;
    if (exe.empty() || !std::filesystem::exists(exe)) {
        message(tr("crusader introuvable à côté de l'éditeur.", "crusader not found next to the editor."));
        return;
    }
    const std::string args = " \"" + std::filesystem::absolute(options_.data).string() + "\" --mod \"" +
                             std::filesystem::absolute(project_->mod()).string() + "\" --map " +
                             std::to_string(map_) + " --egg " + std::to_string(egg);
#ifdef _WIN32
    const std::string cmd = "start \"\" \"" + exe.string() + "\"" + args;
#else
    const std::string cmd = "\"" + exe.string() + "\"" + args + " &";
#endif
    message(std::string(tr("Lancement : ", "Starting: ")) + cmd);
    if (std::system(cmd.c_str()) != 0) message(tr("Le jeu n'a pas pu être lancé.", "The game could not start."));
}

void EditorApp::new_empty_map() {
    commit_map();
    const std::size_t slot = project_->free_map();
    if (slot == 0) {
        message(tr("Plus d'emplacement de carte libre.", "No free map slot left."));
        return;
    }
    // A floor taken from the first mission: the most used glob of map 1
    // whose pieces all lie flat at z 0, cover its cell and can be seen (the
    // most used one, glob 2, is four BLACK16 tiles that fill the void).
    std::map<std::uint16_t, int> uses;
    for (const LevelItem& i : project_->load_map(1).items) {
        if (family_of(i.shape) == kFamGlobEgg && i.quality) ++uses[i.quality];
    }
    std::vector<std::pair<int, std::uint16_t>> ranked;
    for (auto [g, n] : uses) ranked.push_back({n, g});
    std::sort(ranked.rbegin(), ranked.rend());
    std::uint16_t floor = 0;
    int floor_top = 0;
    for (auto [n, g] : ranked) {
        const Glob& glob = glob_cached(g);
        bool flat = !glob.pieces.empty();
        int top = 0;
        long area = 0;
        for (const GlobPiece& p : glob.pieces) {
            if (p.shape >= typeflags_.size()) {
                flat = false;
                break;
            }
            const ShapeInfo& info = typeflags_[p.shape];
            flat &= p.z == 0 && info.family == kFamGeneric;
            area += long{info.footpad_x} * 16 * info.footpad_y * 16;
            top = std::max(top, info.footpad_z * 8);
        }
        auto bright = [&](const GlobPiece& p) {
            const Shape* sh = shapes_->get(p.shape);
            if (!sh || p.frame >= sh->frames().size()) return false;
            const ShapeFrame& f = sh->frames()[p.frame];
            int lit = 0, opaque = 0;
            for (int y = 0; y < f.height; y += 2) {
                for (int x = 0; x < f.width; x += 2) {
                    const int c = f.pixel(x, y);
                    if (c < 0) continue;
                    ++opaque;
                    const Rgb& rgb = palette_.colors[static_cast<std::size_t>(c)];
                    if (rgb.r + rgb.g + rgb.b > 120) ++lit;
                }
            }
            return opaque > 0 && lit * 2 > opaque;
        };
        if (flat && top <= 8 && area >= long{kChunk} * kChunk && bright(glob.pieces.front())) {
            floor = g;
            floor_top = top;
            break;
        }
    }
    Level level;
    constexpr std::int32_t base = 16 * kChunk;
    if (floor) {
        for (int cy = 0; cy < 3; ++cy) {
            for (int cx = 0; cx < 3; ++cx) {
                LevelItem egg;
                egg.uid = new_uid();
                egg.shape = kShapeGlobEgg;
                egg.x = base + cx * kChunk + kChunk - 1;
                egg.y = base + cy * kChunk + kChunk - 1;
                egg.quality = floor;
                level.items.push_back(egg);
            }
        }
    }
    LevelItem start;
    start.uid = new_uid();
    start.shape = kShapeTeleportEgg;
    start.frame = 1;
    start.x = base + kChunk + kChunk / 2;
    start.y = base + kChunk + kChunk / 2;
    start.z = floor_top;
    start.quality = kStartEgg;
    level.items.push_back(start);
    project_->store_map(slot, level);
    maps_changed();
    open_map(static_cast<int>(slot));
    message(std::string(tr("Nouvelle carte ", "New map ")) + std::to_string(slot) +
            tr(" : sol de la mission 1 (glob ", ": floor of mission 1 (glob ") + std::to_string(floor) +
            tr(") et œuf de départ 0x1E.", ") and start egg 0x1E."));
}

void EditorApp::generate_level() {
    commit_map();
    const std::size_t slot = project_->free_map();
    if (slot == 0) {
        message(tr("Plus d'emplacement de carte libre.", "No free map slot left."));
        return;
    }
    if (!generator_data_) generator_data_ = std::make_unique<gen::GameData>(gen::GameData::load(project_->data()));
    gen::Options o;
    o.seed = static_cast<std::uint32_t>(generate_seed_);
    o.biome = generate_biome_;
    o.rooms = generate_rooms_;
    o.locks = generate_locks_;
    o.storeys = generate_storeys_;
    o.guards = generate_guards_ ? -1 : 0;
    // Key cards stay with the player from level to level: each generated
    // map of the mod numbers its own.
    o.first_card = 1 + (3 * static_cast<int>(manifest().levels.size())) % 30;
    o.map = static_cast<int>(slot);
    o.next_map = static_cast<int>(slot);  // the exit leads back to the start
    const gen::Result result = gen::generate(generator_data_->biomes, o);
    const std::string problem = gen::check_solvable(result);
    if (!problem.empty() || result.level.items.empty()) {
        message(std::string(tr("Génération impossible : ", "Generation failed: ")) + problem);
        return;
    }
    project_->store_map(slot, result.level);
    // The seed goes to the mod's NOLIMIT.JSON at the next save.
    manifest().set(gen::manifest_level(o, result, generator_data_->biomes));
    manifest_dirty_ = true;
    maps_changed();
    open_map(static_cast<int>(slot));
    message(std::string(tr("Niveau généré sur la carte ", "Level generated on map ")) + std::to_string(slot) + " (" +
            (result.notes.empty() ? std::string() : result.notes.front()) + ")");
}

gen::Manifest& EditorApp::manifest() {
    if (!manifest_) manifest_ = gen::Manifest::read(project_->mod()).value_or(gen::Manifest{});
    return *manifest_;
}

void EditorApp::copy_map_to_free_slot() {
    commit_map();
    const std::size_t slot = project_->free_map();
    if (slot == 0) {
        message(tr("Plus d'emplacement de carte libre.", "No free map slot left."));
        return;
    }
    project_->store_map(slot, level_);
    maps_changed();
    const int from = map_;
    open_map(static_cast<int>(slot));
    message(std::string(tr("Carte ", "Map ")) + std::to_string(from) + tr(" copiée vers la carte ", " copied to map ") +
            std::to_string(slot));
}

// ---------------------------------------------------------------------------
// Undo

void EditorApp::push_undo() {
    undo_.push_back(level_);
    if (undo_.size() > kUndoLimit) undo_.pop_front();
    redo_.clear();
}

void EditorApp::note_change(const LevelItem& before) {
    if (!edit_open_) {
        Level snapshot = level_;
        if (LevelItem* i = snapshot.find(before.uid)) *i = before;
        undo_.push_back(std::move(snapshot));
        if (undo_.size() > kUndoLimit) undo_.pop_front();
        redo_.clear();
        edit_open_ = true;
    }
    level_dirty_ = true;
    world_dirty_ = true;
}

void EditorApp::undo() {
    if (undo_.empty()) return;
    redo_.push_back(std::move(level_));
    level_ = std::move(undo_.back());
    undo_.pop_back();
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(level_));
    level_ = std::move(redo_.back());
    redo_.pop_back();
    level_dirty_ = world_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Helpers

std::uint8_t EditorApp::family_of(std::uint16_t shape) const {
    return shape < typeflags_.size() ? typeflags_[shape].family : 0;
}

bool EditorApp::links_by_quality(const LevelItem& item) const {
    // Eggs use their quality otherwise (glob number, trigger type, teleport
    // id, NPC template).
    return editor::links_by_quality(family_of(item.shape));
}

bool EditorApp::acts_through_trigger(const LevelItem& item) const {
    return item.shape < trigger_source_.size() && trigger_source_[item.shape] && links_by_quality(item);
}

LevelInfo EditorApp::level_info() const {
    LevelInfo info;
    info.family = [this](std::uint16_t shape) { return family_of(shape); };
    info.has_class = [this](std::uint16_t cid) { return usecode_ && usecode_->has_class(cid); };
    return info;
}

std::map<std::uint8_t, int> EditorApp::arrivals_of(int map) {
    if (map == map_) return arrival_ids(level_, level_info());
    auto it = arrivals_cache_.find(map);
    if (it == arrivals_cache_.end()) {
        const bool used = map > 0 && project_->map_used(static_cast<std::size_t>(map));
        it = arrivals_cache_
                 .emplace(map, used ? arrival_ids(project_->load_map(static_cast<std::size_t>(map)), level_info())
                                    : std::map<std::uint8_t, int>{})
                 .first;
    }
    return it->second;
}

void EditorApp::run_checks() {
    CheckContext context;
    context.info = level_info();
    context.map = map_;
    context.glob_pieces = [this](std::uint16_t glob) { return glob_cached(glob).pieces.size(); };
    context.map_exists = [this](int map) {
        return map == map_ || (map > 0 && project_->map_used(static_cast<std::size_t>(map)));
    };
    context.arrivals_on = [this](int map) { return arrivals_of(map); };
    context.busiest_records = busiest_records_;
    problems_ = check_level(level_, context);
}

const LevelItem* EditorApp::find_start_egg() const {
    for (const LevelItem& i : level_.items) {
        if (is_arrival(i, family_of(i.shape)) && i.quality_low() == kStartEgg) return &i;
    }
    return nullptr;
}

std::string EditorApp::class_name(std::uint16_t cid) const {
    return usecode_ && usecode_->has_class(cid) ? usecode_->class_name(cid) : std::string();
}

std::string EditorApp::shape_name(std::uint16_t shape) const { return shape < 0x800 ? class_name(shape) : ""; }

int EditorApp::frame_count(std::uint16_t shape) {
    const Shape* s = shapes_->get(shape);
    return s ? static_cast<int>(s->frames().size()) : 0;
}

SDL_Texture* EditorApp::frame_texture(std::uint16_t shape, std::uint8_t frame, int* w, int* h, int* xoff,
                                      int* yoff) {
    const std::uint32_t key = std::uint32_t{shape} << 16 | frame;
    if (auto it = frame_textures_.find(key); it != frame_textures_.end()) {
        const FrameMeta& m = frame_meta_[key];
        *w = m.w;
        *h = m.h;
        *xoff = m.xoff;
        *yoff = m.yoff;
        return it->second;
    }
    SDL_Texture* tex = nullptr;
    FrameMeta meta{0, 0, 0, 0};
    const Shape* s = shapes_->get(shape);
    if (s && frame < s->frames().size()) {
        const ShapeFrame& f = s->frames()[frame];
        if (f.width > 0 && f.height > 0 && f.width < 4096 && f.height < 4096) {
            std::vector<std::uint32_t> px(static_cast<std::size_t>(f.width) * f.height, 0);
            for (int y = 0; y < f.height; ++y) {
                for (int x = 0; x < f.width; ++x) {
                    const int c = f.pixel(x, y);
                    if (c < 0) continue;
                    const Rgb& rgb = palette_.colors[static_cast<std::size_t>(c)];
                    px[static_cast<std::size_t>(y) * f.width + x] =
                        0xFF000000u | std::uint32_t{rgb.b} << 16 | std::uint32_t{rgb.g} << 8 | rgb.r;
                }
            }
            tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, f.width, f.height);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, px.data(), f.width * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
            }
            meta = {f.width, f.height, f.xoff, f.yoff};
        }
    }
    frame_textures_[key] = tex;
    frame_meta_[key] = meta;
    *w = meta.w;
    *h = meta.h;
    *xoff = meta.xoff;
    *yoff = meta.yoff;
    return tex;
}

const Glob& EditorApp::glob_cached(std::size_t index) {
    auto it = glob_cache_.find(index);
    if (it == glob_cache_.end()) it = glob_cache_.emplace(index, project_->glob(index)).first;
    return it->second;
}

SDL_Texture* EditorApp::glob_texture(std::size_t glob, int* w, int* h) {
    if (auto it = glob_textures_.find(glob); it != glob_textures_.end()) {
        *w = it->second.second.first;
        *h = it->second.second.second;
        return it->second.first;
    }
    SDL_Texture* tex = nullptr;
    int tw = 0, th = 0;
    const Glob& g = glob_cached(glob);
    if (!g.pieces.empty()) {
        World world(typeflags_);
        LevelItem egg;
        egg.x = egg.y = kChunk - 1;
        for (const GlobPiece& p : g.pieces) {
            const Point3 pos = glob_piece_position(egg, p);
            Item item;
            item.shape = p.shape;
            item.frame = p.frame;
            item.x = pos.x;
            item.y = pos.y;
            item.z = pos.z;
            world.add(std::move(item));
        }
        const int sw = 1024, sh = 900;
        const View view{-sw / 2, 128 - sh / 2 - 150};
        auto list = build_display_list(world, *shapes_, view, sw, sh);
        if (!list.empty()) {
            int l = sw, t = sh, r = 0, b = 0;
            for (const DrawItem& d : list) {
                l = std::min(l, std::max(0, d.left));
                t = std::min(t, std::max(0, d.top));
                r = std::max(r, std::min(sw, d.right));
                b = std::max(b, std::min(sh, d.bottom));
            }
            Surface8 surface(sw, sh, 0);
            draw_display_list(surface, list, xform_ ? &*xform_ : nullptr);
            tw = std::max(1, r - l);
            th = std::max(1, b - t);
            std::vector<std::uint32_t> px(static_cast<std::size_t>(tw) * th, 0);
            for (int y = 0; y < th; ++y) {
                for (int x = 0; x < tw; ++x) {
                    const Rgb& rgb = palette_.colors[surface.row(t + y)[l + x]];
                    px[static_cast<std::size_t>(y) * tw + x] =
                        0xFF000000u | std::uint32_t{rgb.b} << 16 | std::uint32_t{rgb.g} << 8 | rgb.r;
                }
            }
            tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tw, th);
            if (tex) SDL_UpdateTexture(tex, nullptr, px.data(), tw * 4);
        }
    }
    glob_textures_[glob] = {tex, {tw, th}};
    *w = tw;
    *h = th;
    return tex;
}

void EditorApp::shape_thumbnail(std::uint16_t shape, std::uint8_t frame, float size) {
    int w, h, xo, yo;
    SDL_Texture* t = frame_texture(shape, frame, &w, &h, &xo, &yo);
    if (!t) {
        ImGui::Dummy(ImVec2(size, size));
        return;
    }
    const float k = std::min(1.0f, size / static_cast<float>(std::max(w, h)));
    ImGui::Image(tex_id(t), ImVec2(w * k, h * k));
}

void EditorApp::message(std::string text) {
    message_ = std::move(text);
    message_time_ = ImGui::GetTime();
}

// ---------------------------------------------------------------------------
// World view

void EditorApp::rebuild_world() {
    std::vector<ShapeInfo> tf = typeflags_;
    if (show_markers_) {
        for (ShapeInfo& i : tf) i.flags &= ~static_cast<std::uint32_t>(ShapeInfo::kEditor);
    }
    world_ = std::make_unique<World>(std::move(tf));
    id_to_uid_.clear();
    uid_to_id_.clear();
    try {
        for (const LevelItem& it : level_.items) {
            if (it.shape == 0) continue;
            Item w;
            w.shape = it.shape;
            w.frame = it.frame;
            w.x = it.x;
            w.y = it.y;
            w.z = it.z & 0xFF;
            w.flags = static_cast<std::uint16_t>(it.flags & ~kFlagContained);
            if (show_invisible_) w.flags &= static_cast<std::uint16_t>(~kFlagInvisible);
            w.quality = it.quality;
            w.npc_number = it.npc_number;
            w.map_number = it.map_number;
            const ObjId id = world_->add(std::move(w)).id;
            id_to_uid_[id] = it.uid;
            uid_to_id_[it.uid] = id;
            if (show_globs_ && family_of(it.shape) == kFamGlobEgg && it.quality) {
                for (const GlobPiece& p : glob_cached(it.quality).pieces) {
                    const Point3 pos = glob_piece_position(it, p);
                    Item g;
                    g.shape = p.shape;
                    g.frame = p.frame;
                    g.x = pos.x;
                    g.y = pos.y;
                    g.z = pos.z;
                    g.flags = 0x0082;  // glob pieces, as GLOB.C flags them
                    const ObjId gid = world_->add(std::move(g)).id;
                    id_to_uid_[gid] = it.uid;
                }
            }
        }
    } catch (const std::exception& e) {
        message(std::string(tr("Vue incomplète : ", "Incomplete view: ")) + e.what());
    }
    world_items_ = world_->item_count();
    view_dirty_ = true;
    run_checks();
}

void EditorApp::render_view_if_needed() {
    const int sw = std::max(1, static_cast<int>(view_w_ / zoom_));
    const int sh = std::max(1, static_cast<int>(view_h_ / zoom_));
    if (world_dirty_) {
        rebuild_world();
        world_dirty_ = false;
    }
    if (!view_dirty_ && sw == surface_w_ && sh == surface_h_) return;
    surface_w_ = sw;
    surface_h_ = sh;
    view_corner_ = View{static_cast<int>(std::lround(center_x_ - sw / 2.0)),
                        static_cast<int>(std::lround(center_y_ - sh / 2.0))};
    display_ = build_display_list(*world_, *shapes_, view_corner_, sw, sh);
    if (z_cut_ < 255) {
        std::erase_if(display_, [&](const DrawItem& d) { return d.item->z > z_cut_; });
    }
    Surface8 surface(sw, sh, 0);
    draw_display_list(surface, display_, xform_ ? &*xform_ : nullptr);
    if (!view_texture_ || view_texture_w_ != sw || view_texture_h_ != sh) {
        if (view_texture_) SDL_DestroyTexture(view_texture_);
        view_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, sw, sh);
        SDL_SetTextureScaleMode(view_texture_, SDL_ScaleModeNearest);
        view_texture_w_ = sw;
        view_texture_h_ = sh;
    }
    std::uint32_t lut[256];
    for (int i = 0; i < 256; ++i) {
        const Rgb& c = palette_.colors[static_cast<std::size_t>(i)];
        lut[i] = 0xFF000000u | std::uint32_t{c.b} << 16 | std::uint32_t{c.g} << 8 | c.r;
    }
    void* pixels = nullptr;
    int pitch = 0;
    if (view_texture_ && SDL_LockTexture(view_texture_, nullptr, &pixels, &pitch) == 0) {
        for (int y = 0; y < sh; ++y) {
            auto* dst = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(pixels) + y * pitch);
            const std::uint8_t* src = surface.row(y);
            for (int x = 0; x < sw; ++x) dst[x] = lut[src[x]];
        }
        SDL_UnlockTexture(view_texture_);
    }
    view_dirty_ = false;
}

void EditorApp::center_on(std::int32_t x, std::int32_t y, std::int32_t z) {
    center_x_ = (2.0 * x - 2.0 * y) / 4.0;
    center_y_ = (2.0 * x + 2.0 * y) / 8.0 - z;
    view_dirty_ = true;
}

void EditorApp::center_on_default() {
    for (const LevelItem& i : level_.items) {
        if (family_of(i.shape) == kFamTeleportEgg && i.frame == 1 && i.quality_low() == kStartEgg) {
            center_on(i.x, i.y, i.z);
            return;
        }
    }
    // The densest map cell.
    std::map<std::pair<int, int>, int> cells;
    for (const LevelItem& i : level_.items) ++cells[{i.x / kChunk, i.y / kChunk}];
    if (cells.empty()) {
        center_on(16 * kChunk, 16 * kChunk, 0);
        return;
    }
    const auto best =
        std::max_element(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    center_on(best->first.first * kChunk + kChunk / 2, best->first.second * kChunk + kChunk / 2, 0);
}

EditorApp::ScreenPoint EditorApp::to_screen(double ux, double uy, double uz) const {
    const double px = (2.0 * ux - 2.0 * uy) / 4.0 - view_corner_.left;
    const double py = (2.0 * ux + 2.0 * uy) / 8.0 - uz - view_corner_.top;
    return {static_cast<float>(view_x_ + px * zoom_), static_cast<float>(view_y_ + py * zoom_)};
}

std::pair<std::int32_t, std::int32_t> EditorApp::to_world(float mx, float my, std::int32_t z) const {
    const double px = (mx - view_x_) / zoom_ + view_corner_.left;
    const double py = (my - view_y_) / zoom_ + view_corner_.top;
    const double ux = px + 2.0 * (py + z);
    const double uy = 2.0 * (py + z) - px;
    return {static_cast<std::int32_t>(std::lround(ux)), static_cast<std::int32_t>(std::lround(uy))};
}

std::int32_t EditorApp::snap(std::int32_t v) const {
    if (grid_ <= 1) return v;
    // Items stand at the far corner of their cell (x = 32k - 1 for a tile).
    const std::int32_t cell = static_cast<std::int32_t>(std::floor(static_cast<double>(v) / grid_));
    return cell * grid_ + grid_ - 1;
}

void EditorApp::draw_box(const LevelItem& item, unsigned color, float thickness) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    int fx = 0, fy = 0, fz = 0;
    if (item.shape < typeflags_.size()) {
        fx = typeflags_[item.shape].footpad_x;
        fy = typeflags_[item.shape].footpad_y;
        fz = typeflags_[item.shape].footpad_z;
    }
    if (item.flags & kFlagMirrored) std::swap(fx, fy);
    const double x1 = item.x, y1 = item.y, z0 = item.z;
    const double x0 = x1 - fx * 16.0, y0 = y1 - fy * 16.0, z1 = z0 + fz * 8.0;
    if (fx == 0 && fy == 0) {
        const ScreenPoint p = to_screen(x1, y1, z0);
        dl->AddQuad(ImVec2(p.x, p.y - 6), ImVec2(p.x + 6, p.y), ImVec2(p.x, p.y + 6), ImVec2(p.x - 6, p.y), color,
                    thickness);
        return;
    }
    const double xs[2] = {x0, x1}, ys[2] = {y0, y1}, zs[2] = {z0, z1};
    ScreenPoint c[8];
    for (int i = 0; i < 8; ++i) c[i] = to_screen(xs[i & 1], ys[(i >> 1) & 1], zs[i >> 2]);
    auto line = [&](int a, int b) { dl->AddLine(ImVec2(c[a].x, c[a].y), ImVec2(c[b].x, c[b].y), color, thickness); };
    for (int z = 0; z < 8; z += 4) {
        line(z + 0, z + 1);
        line(z + 1, z + 3);
        line(z + 3, z + 2);
        line(z + 2, z + 0);
    }
    for (int i = 0; i < 4; ++i) line(i, i + 4);
}

void EditorApp::draw_zone(const LevelItem& egg, unsigned color) {
    const HatchZone z = hatch_zone(egg);
    if (z.half_x == 0 && z.half_y == 0) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ScreenPoint a = to_screen(egg.x - z.half_x, egg.y - z.half_y, egg.z);
    const ScreenPoint b = to_screen(egg.x + z.half_x, egg.y - z.half_y, egg.z);
    const ScreenPoint c = to_screen(egg.x + z.half_x, egg.y + z.half_y, egg.z);
    const ScreenPoint d = to_screen(egg.x - z.half_x, egg.y + z.half_y, egg.z);
    dl->AddQuadFilled(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(c.x, c.y), ImVec2(d.x, d.y), (color & 0x00FFFFFF) | 0x30000000);
    dl->AddQuad(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(c.x, c.y), ImVec2(d.x, d.y), color, 1.5f);
}

void EditorApp::draw_overlays() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    const LevelItem* primary = level_.find(primary_);

    // Trigger zones and teleport links of the eggs.
    std::map<std::uint8_t, const LevelItem*> arrivals;
    for (const LevelItem& i : level_.items) {
        if (family_of(i.shape) == kFamTeleportEgg && i.frame == 1) arrivals[i.quality_low()] = &i;
    }
    for (const LevelItem& i : level_.items) {
        if (i.z > z_cut_) continue;
        const std::uint8_t fam = family_of(i.shape);
        if (show_zones_ && hatches(fam, i)) draw_zone(i, kColZone);
        if (show_zones_ && is_departure(i, fam) && i.map_number == map_) {
            auto it = arrivals.find(i.quality_low());
            if (it != arrivals.end()) {
                const ScreenPoint a = to_screen(i.x, i.y, i.z);
                const ScreenPoint b = to_screen(it->second->x, it->second->y, it->second->z);
                dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), kColTeleport, 2.0f);
                dl->AddCircleFilled(ImVec2(b.x, b.y), 5.0f, kColTeleport);
            }
        } else if (show_zones_ && is_departure(i, fam)) {
            // To another map: where it leads, written by the egg.
            const ScreenPoint a = to_screen(i.x, i.y, i.z);
            const std::string label = std::string("→ ") + tr("carte ", "map ") + std::to_string(i.map_number) + " / " +
                                      hex(i.quality_low(), 2);
            dl->AddText(ImVec2(a.x + 8, a.y - 8), kColTeleport, label.c_str());
        }
    }
    // Items linked to the selected one by their quality low byte (TRIGGER,
    // switches, valves, chests and their markers...).
    // In link mode, the source's links are always shown, with a line to each.
    const LevelItem* source = link_source_ ? level_.find(link_source_) : primary;
    if ((show_links_ || link_source_) && source && source->quality_low() != 0 && links_by_quality(*source)) {
        const std::uint8_t q = source->quality_low();
        const ScreenPoint from = to_screen(source->x, source->y, source->z);
        for (const LevelItem& i : level_.items) {
            if (i.uid == source->uid || i.quality_low() != q) continue;
            const std::uint8_t fam = family_of(i.shape);
            if (fam == kFamGlobEgg || fam == kFamTeleportEgg) continue;
            if (!usecode_->has_class(usecode_class_of(i, fam)) && i.shape != kShapeMarker && !link_source_) continue;
            draw_box(i, kColLinked, 1.5f);
            const ScreenPoint to = to_screen(i.x, i.y, i.z);
            dl->AddLine(ImVec2(from.x, from.y), ImVec2(to.x, to.y), (kColLinked & 0x00FFFFFF) | 0x90000000, 1.0f);
        }
        if (link_source_) draw_box(*source, kColPrimary, 2.5f);
    }
    if (hovered_ && !selection_.count(hovered_)) {
        if (const LevelItem* h = level_.find(hovered_)) draw_box(*h, kColHover, 1.0f);
    }
    for (std::uint32_t uid : selection_) {
        if (const LevelItem* i = level_.find(uid)) {
            draw_box(*i, uid == primary_ ? kColPrimary : kColSelected, uid == primary_ ? 2.0f : 1.5f);
            if (family_of(i->shape) == kFamGlobEgg) {
                // The chunk the glob fills.
                const std::int32_t bx = i->x & ~0x1FF, by = i->y & ~0x1FF;
                const ScreenPoint a = to_screen(bx, by, i->z), b = to_screen(bx + kChunk, by, i->z),
                                  c = to_screen(bx + kChunk, by + kChunk, i->z), d = to_screen(bx, by + kChunk, i->z);
                dl->AddQuad(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(c.x, c.y), ImVec2(d.x, d.y), kColSelected, 1.0f);
            }
        }
    }
    // Rubber band.
    if (box_selecting_) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        dl->AddRect(ImVec2(std::min(press_x_, m.x), std::min(press_y_, m.y)),
                    ImVec2(std::max(press_x_, m.x), std::max(press_y_, m.y)), kColSelected);
    }
    // The brush, where a click would put it.
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool in_view = m.x >= view_x_ && m.y >= view_y_ && m.x < view_x_ + view_w_ && m.y < view_y_ + view_h_;
    if (brush_.kind != BrushKind::None && in_view && !ImGui::GetIO().WantCaptureMouse) {
        auto [wx, wy] = to_world(m.x, m.y, work_z_);
        wx = snap(wx);
        wy = snap(wy);
        if (brush_.kind == BrushKind::Prefab) {
            if (brush_.value < prefabs_.size()) {
                const Prefab& p = prefabs_[brush_.value].prefab;
                int drawn = 0;
                for (const LevelItem& i : p.items) {
                    if (++drawn > 300) break;
                    LevelItem ghost = i;
                    ghost.x = i.x - p.anchor_x + wx;
                    ghost.y = i.y - p.anchor_y + wy;
                    ghost.z = i.z - p.anchor_z + work_z_;
                    draw_box(ghost, kColGhost, 1.0f);
                }
            }
        } else if (brush_.kind == BrushKind::Glob) {
            const std::int32_t bx = wx & ~0x1FF, by = wy & ~0x1FF;
            const ScreenPoint a = to_screen(bx, by, work_z_), b = to_screen(bx + kChunk, by, work_z_),
                              c = to_screen(bx + kChunk, by + kChunk, work_z_), d = to_screen(bx, by + kChunk, work_z_);
            dl->AddQuad(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(c.x, c.y), ImVec2(d.x, d.y), kColGhost, 2.0f);
        } else {
            std::uint16_t shape = brush_.shape;
            std::uint8_t frame = brush_.frame;
            if (brush_.kind == BrushKind::TriggerEgg) shape = kShapeTriggerEgg, frame = 0;
            if (brush_.kind == BrushKind::TeleportArrival) shape = kShapeTeleportEgg, frame = 1;
            if (brush_.kind == BrushKind::TeleportDeparture) shape = kShapeTeleportEgg, frame = 0;
            if (brush_.kind == BrushKind::MonsterEgg) shape = kShapeMonsterEgg, frame = 0;
            if (brush_.kind == BrushKind::MissionStart) shape = kShapeTeleportEgg, frame = 1;
            if (brush_.kind == BrushKind::MonsterPair) shape = kShapeMonster, frame = 0;
            const LevelItem* pending = level_.find(pair_departure_);
            if (brush_.kind == BrushKind::TeleportPair) shape = kShapeTeleportEgg, frame = pending ? 1 : 0;
            if (brush_.kind == BrushKind::TeleportPair && pending) {
                const ScreenPoint a = to_screen(pending->x, pending->y, pending->z), b = to_screen(wx, wy, work_z_);
                dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), kColTeleport, 2.0f);
            }
            int w, h, xo, yo;
            if (SDL_Texture* t = frame_texture(shape, frame, &w, &h, &xo, &yo)) {
                const ScreenPoint p = to_screen(wx, wy, work_z_);
                dl->AddImage(tex_id(t), ImVec2(p.x - xo * zoom_, p.y - yo * zoom_),
                             ImVec2(p.x + (w - xo) * zoom_, p.y + (h - yo) * zoom_), ImVec2(0, 0), ImVec2(1, 1),
                             kColGhost);
            }
            LevelItem ghost;
            ghost.shape = shape;
            ghost.x = wx;
            ghost.y = wy;
            ghost.z = work_z_;
            draw_box(ghost, kColGhost, 1.0f);
        }
    }
    // What the current tool expects, at the top of the view.
    std::string banner;
    if (link_source_) {
        banner = tr("Mode liaison : cliquez les objets à lier à l'objet encadré en blanc (même qualité basse) ; un "
                    "objet déjà lié se délie. Échap ou clic droit : terminer.",
                    "Link mode: click the items to link to the item framed in white (same quality low); clicking a "
                    "linked item unlinks it. Esc or right click: done.");
    } else if (brush_.kind == BrushKind::TeleportPair) {
        banner = level_.find(pair_departure_)
                     ? tr("Paire de téléporteurs : cliquez maintenant où le joueur arrive.",
                          "Teleporter pair: now click where the player arrives.")
                     : tr("Paire de téléporteurs : cliquez où le joueur part (zone de déclenchement).",
                          "Teleporter pair: click where the player leaves (trigger area).");
    } else if (brush_.kind == BrushKind::MissionStart) {
        banner = tr("Départ de mission : cliquez où le joueur commence la mission.",
                    "Mission start: click where the player starts the mission.");
    }
    if (!banner.empty()) {
        const float wrap = std::max(200.0f, view_w_ - 32.0f);
        const ImVec2 size = ImGui::CalcTextSize(banner.c_str(), nullptr, false, wrap);
        const ImVec2 at(view_x_ + 12, view_y_ + 12);
        dl->AddRectFilled(ImVec2(at.x - 6, at.y - 4), ImVec2(at.x + size.x + 6, at.y + size.y + 4),
                          IM_COL32(0, 0, 0, 200), 4.0f);
        dl->AddText(nullptr, 0.0f, at, IM_COL32(255, 230, 120, 255), banner.c_str(), nullptr, wrap);
    }
    dl->PopClipRect();
}

std::vector<std::uint32_t> EditorApp::pick_at(float mx, float my) {
    const int px = static_cast<int>((mx - view_x_) / zoom_), py = static_cast<int>((my - view_y_) / zoom_);
    std::vector<std::uint32_t> uids;
    const auto hits = pick(display_, px, py);
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        const ObjId id = display_[*it].item->id;
        auto u = id_to_uid_.find(id);
        if (u == id_to_uid_.end()) continue;
        const bool glob_piece = uid_to_id_[u->second] != id;
        if (glob_piece && pick_glob_pieces_) continue;  // "click through the globs"
        if (std::find(uids.begin(), uids.end(), u->second) == uids.end()) uids.push_back(u->second);
    }
    return uids;
}

void EditorApp::handle_view_input() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool in_view = m.x >= view_x_ && m.y >= view_y_ && m.x < view_x_ + view_w_ && m.y < view_y_ + view_h_ &&
                         !io.WantCaptureMouse;

    // Zoom around the pointer.
    if (in_view && io.MouseWheel != 0.0f) {
        static const float zooms[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
        int z = 0;
        while (z < 4 && zooms[z] < zoom_) ++z;
        const int nz = std::clamp(z + (io.MouseWheel > 0 ? 1 : -1), 0, 4);
        const double ox = m.x - view_x_ - view_w_ / 2.0, oy = m.y - view_y_ - view_h_ / 2.0;
        const double px = center_x_ + ox / zoom_, py = center_y_ + oy / zoom_;
        zoom_ = zooms[nz];
        center_x_ = px - ox / zoom_;
        center_y_ = py - oy / zoom_;
        view_dirty_ = true;
    }
    // Pan: middle or right button.
    if (in_view && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        panning_ = true;
        press_x_ = m.x;
        press_y_ = m.y;
    }
    if (panning_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
                center_x_ -= io.MouseDelta.x / zoom_;
                center_y_ -= io.MouseDelta.y / zoom_;
                view_dirty_ = true;
            }
        } else {
            // A right click without moving drops the brush.
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && std::abs(m.x - press_x_) < 3 &&
                std::abs(m.y - press_y_) < 3) {
                brush_ = {};
                pair_departure_ = link_source_ = 0;
            }
            panning_ = false;
        }
    }

    // Left button: place, select, move, rubber band.
    if (in_view && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        press_x_ = m.x;
        press_y_ = m.y;
        if (link_source_) {
            // The first item under the pointer that links by its quality
            // (a floor's glob egg does not).
            for (std::uint32_t uid : pick_at(m.x, m.y)) {
                const LevelItem* i = level_.find(uid);
                if (i && links_by_quality(*i)) {
                    toggle_link(uid);
                    break;
                }
            }
        } else if (brush_.kind != BrushKind::None) {
            auto [wx, wy] = to_world(m.x, m.y, work_z_);
            place_brush(snap(wx), snap(wy));
        } else {
            std::vector<std::uint32_t> hits = pick_at(m.x, m.y);
            std::uint32_t target = 0;
            if (!hits.empty()) {
                if (io.KeyAlt && hits == last_pick_) {
                    pick_cycle_ = (pick_cycle_ + 1) % hits.size();
                } else {
                    pick_cycle_ = 0;
                }
                target = hits[pick_cycle_];
            }
            last_pick_ = hits;
            if (!target) {
                if (!io.KeyCtrl && !io.KeyShift) {
                    selection_.clear();
                    primary_ = 0;
                }
                box_selecting_ = true;
            } else if (io.KeyCtrl) {
                if (selection_.erase(target) == 0) selection_.insert(target), primary_ = target;
            } else if (io.KeyShift) {
                selection_.insert(target);
                primary_ = target;
            } else {
                if (!selection_.count(target)) selection_ = {target};
                primary_ = target;
            }
            if (target && selection_.count(target)) {
                dragging_ = true;
                drag_moved_ = false;
                const LevelItem* p = level_.find(primary_);
                drag_origin_ = to_world(m.x, m.y, p ? p->z : work_z_);
                drag_start_pos_.clear();
                for (std::uint32_t uid : selection_) {
                    if (level_.top_index(uid)) {
                        const LevelItem* i = level_.find(uid);
                        drag_start_pos_[uid] = {i->x, i->y, i->z};
                    }
                }
            }
        }
    }
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!drag_moved_ && (std::abs(m.x - press_x_) > 3 || std::abs(m.y - press_y_) > 3)) {
                drag_moved_ = true;
                push_undo();
            }
            if (drag_moved_ && drag_start_pos_.count(primary_)) {
                const Point3 start = drag_start_pos_[primary_];
                const auto [cx, cy] = to_world(m.x, m.y, start.z);
                std::int32_t dx = cx - drag_origin_.first, dy = cy - drag_origin_.second;
                if (!io.KeyAlt) {
                    dx = snap(start.x + dx) - start.x;
                    dy = snap(start.y + dy) - start.y;
                }
                for (auto& [uid, pos] : drag_start_pos_) {
                    LevelItem* i = level_.find(uid);
                    if (!i) continue;
                    const std::int32_t nx = std::clamp(pos.x + dx, 0, 0xFFFF), ny = std::clamp(pos.y + dy, 0, 0xFFFF);
                    if (nx == i->x && ny == i->y) continue;
                    const bool glob = family_of(i->shape) == kFamGlobEgg;
                    i->x = nx;
                    i->y = ny;
                    if (glob) {
                        world_dirty_ = true;
                    } else if (auto id = uid_to_id_.find(uid); id != uid_to_id_.end()) {
                        world_->move_item(id->second, nx, ny, i->z);
                        view_dirty_ = true;
                    }
                }
                level_dirty_ = true;
            }
        } else {
            dragging_ = false;
            if (drag_moved_) world_dirty_ = true;
        }
    }
    if (box_selecting_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        box_selecting_ = false;
        const float x0 = std::min(press_x_, m.x), x1 = std::max(press_x_, m.x);
        const float y0 = std::min(press_y_, m.y), y1 = std::max(press_y_, m.y);
        if (x1 - x0 > 4 && y1 - y0 > 4) {
            for (const LevelItem& i : level_.items) {
                if (i.z > z_cut_ || i.shape == 0) continue;
                const ScreenPoint p = to_screen(i.x, i.y, i.z);
                if (p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1) {
                    selection_.insert(i.uid);
                    if (!primary_) primary_ = i.uid;
                }
            }
        }
    }

    // Keyboard.
    if (!io.WantCaptureKeyboard) {
        const std::int32_t step = std::max(1, grid_);
        if (!selection_.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move_selection(-step, 0, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move_selection(step, 0, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move_selection(0, -step, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move_selection(0, step, 0);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move_selection(0, 0, 8);
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move_selection(0, 0, -8);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) delete_selection();
        } else {
            const double pan = 64.0 / zoom_;
            if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) center_x_ -= pan / 4, view_dirty_ = true;
            if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) center_x_ += pan / 4, view_dirty_ = true;
            if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) center_y_ -= pan / 4, view_dirty_ = true;
            if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) center_y_ += pan / 4, view_dirty_ = true;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) work_z_ = std::min(255, work_z_ + 8);
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) work_z_ = std::max(0, work_z_ - 8);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (link_source_) {
                link_source_ = 0;
                message(tr("Mode liaison terminé.", "Link mode finished."));
            } else if (brush_.kind != BrushKind::None) {
                if (level_.find(pair_departure_)) {
                    message(tr("Le départ reste sans arrivée : la vérification du niveau le signale.",
                               "The departure stays without an arrival: the level check reports it."));
                }
                brush_ = {};
                pair_departure_ = 0;
            } else {
                selection_.clear();
                primary_ = 0;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Home)) {
            if (const LevelItem* p = level_.find(primary_)) center_on(p->x, p->y, p->z);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !io.KeyCtrl && primary_) start_link_mode(primary_);
    }

    // The item under the pointer, for its tooltip.
    std::uint32_t hover = 0;
    if (in_view && !dragging_ && !panning_ && !box_selecting_ && brush_.kind == BrushKind::None) {
        const std::vector<std::uint32_t> hits = pick_at(m.x, m.y);
        if (!hits.empty()) hover = hits.front();
    }
    if (hover != hovered_) {
        hovered_ = hover;
        hover_since_ = ImGui::GetTime();
    }
}

void EditorApp::hover_tooltip() {
    if (!hovered_ || ImGui::GetTime() - hover_since_ < 0.35) return;
    const LevelItem* i = level_.find(hovered_);
    if (!i) return;
    const std::uint8_t fam = family_of(i->shape);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(describe(*i, fam, class_name(usecode_class_of(*i, fam))).c_str());
    ImGui::TextDisabled("%s %s  (%d, %d, %d)", hex(i->shape, 3).c_str(), family_name(fam), i->x, i->y, i->z);
    if (i->shape == kShapeMonster && i->frame == 1 && npc_types_ && i->npc_number < dtable::NpcTypes::kRecords) {
        const std::uint16_t s = npc_types_->word(i->npc_number, 0x3E);
        ImGui::Text("%s %s", hex(s, 3).c_str(), shape_name(s).c_str());
    }
    if (is_departure(*i, fam)) {
        ImGui::Text("→ %s %u, %s %s", tr("carte", "map"), i->map_number, tr("œuf", "egg"), hex(i->quality_low(), 2).c_str());
    } else if (links_by_quality(*i) && i->quality_low() != 0) {
        ImGui::TextDisabled("%s %s", tr("qualité basse", "quality low"), hex(i->quality_low(), 2).c_str());
    }
    if (link_source_) {
        const LevelItem* s = level_.find(link_source_);
        const bool linked = s && s->quality_low() != 0 && s->quality_low() == i->quality_low();
        ImGui::TextColored(kTextNote, "%s",
                           !links_by_quality(*i) ? tr("Ne peut pas être lié.", "Cannot be linked.")
                           : linked              ? tr("Clic : délier.", "Click: unlink.")
                                                 : tr("Clic : lier.", "Click: link."));
    }
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// Edits

void EditorApp::place_brush(std::int32_t x, std::int32_t y) {
    LevelItem item;
    item.uid = new_uid();
    item.x = std::clamp(x, 0, 0xFFFF);
    item.y = std::clamp(y, 0, 0xFFFF);
    item.z = work_z_;
    if (brush_.kind == BrushKind::MissionStart) {
        // One mission start per map: an existing one moves.
        push_undo();
        LevelItem* start = nullptr;
        for (LevelItem& i : level_.items) {
            if (is_arrival(i, family_of(i.shape)) && i.quality_low() == kStartEgg) {
                start = &i;
                break;
            }
        }
        if (start) {
            start->x = item.x;
            start->y = item.y;
            start->z = item.z;
            select_only(start->uid);
            message(tr("Départ de mission déplacé.", "Mission start moved."));
        } else {
            item.shape = kShapeTeleportEgg;
            item.frame = 1;
            item.quality = kStartEgg;
            level_.items.push_back(item);
            select_only(item.uid);
            message(tr("Départ de mission posé : F5 et les nouvelles parties sur cette carte commencent ici.",
                       "Mission start placed: F5 and new games on this map start here."));
        }
        brush_ = {};
        level_dirty_ = world_dirty_ = true;
        return;
    }
    if (brush_.kind == BrushKind::Prefab) {
        if (brush_.value < prefabs_.size()) {
            const Prefab& p = prefabs_[brush_.value].prefab;
            place_group(p.items, {p.anchor_x, p.anchor_y, p.anchor_z}, p.source_map, std::pair{item.x, item.y}, work_z_);
        }
        return;
    }
    if (brush_.kind == BrushKind::MonsterPair) {
        const std::uint8_t link = free_link_quality(level_, level_info());
        if (link == 0) {
            message(tr("Plus de qualité basse libre sur cette carte.", "No free quality low left on this map."));
            return;
        }
        auto [place, kind] = make_monster_pair(item.x, item.y, item.z, brush_.value, link);
        push_undo();
        level_.items.push_back(place);
        level_.items.push_back(kind);
        select_only(place.uid);
        level_dirty_ = world_dirty_ = true;
        message(tr("PNJ posé (paire MONSTER) : il apparaît ici quand le joueur arrive dans la zone ; réglez-le dans "
                   "Propriétés.",
                   "NPC placed (MONSTER pair): it appears here when the player comes into the area; set it in "
                   "Properties."));
        return;
    }
    if (brush_.kind == BrushKind::TeleportPair) {
        // First click: the departure, with a free id; second: its arrival.
        item.shape = kShapeTeleportEgg;
        if (const LevelItem* departure = level_.find(pair_departure_)) {
            item.frame = 1;
            item.quality = departure->quality_low();
            pair_departure_ = 0;
            brush_ = {};
            message(std::string(tr("Téléporteur relié : départ et arrivée ", "Teleporter linked: departure and arrival ")) +
                    hex(item.quality, 2) + ".");
        } else {
            const std::uint8_t id = free_arrival_id(level_, level_info());
            if (id == 0) {
                message(tr("Plus d'id d'arrivée libre sur cette carte.", "No free arrival id left on this map."));
                return;
            }
            item.frame = 0;
            item.quality = id;
            item.map_number = static_cast<std::uint8_t>(map_);
            set_hatch_zone(item, 2, 2);
            pair_departure_ = item.uid;
            message(tr("Départ posé : cliquez maintenant où le joueur arrive (Échap : arrêter).",
                       "Departure placed: now click where the player arrives (Esc: stop)."));
        }
        push_undo();
        level_.items.push_back(item);
        select_only(item.uid);
        level_dirty_ = world_dirty_ = true;
        return;
    }
    switch (brush_.kind) {
        case BrushKind::Shape:
            item.shape = brush_.shape;
            item.frame = brush_.frame;
            break;
        case BrushKind::Glob:
            item.shape = kShapeGlobEgg;
            item.x = (item.x & ~0x1FF) | 0x1FF;
            item.y = (item.y & ~0x1FF) | 0x1FF;
            item.quality = brush_.value;
            break;
        case BrushKind::TriggerEgg:
            item.shape = kShapeTriggerEgg;
            item.quality = brush_.value;
            set_hatch_zone(item, 2, 2);
            break;
        case BrushKind::TeleportArrival:
            item.shape = kShapeTeleportEgg;
            item.frame = 1;
            item.quality = brush_.value;
            break;
        case BrushKind::TeleportDeparture:
            item.shape = kShapeTeleportEgg;
            item.frame = 0;
            item.quality = brush_.value;
            item.map_number = static_cast<std::uint8_t>(map_);
            set_hatch_zone(item, 2, 2);
            break;
        case BrushKind::MonsterEgg:
            item = make_monster_egg(item.x, item.y, item.z, brush_.value);
            break;
        case BrushKind::None:
        case BrushKind::MissionStart:
        case BrushKind::TeleportPair:
        case BrushKind::MonsterPair:
        case BrushKind::Prefab:
            return;
    }
    push_undo();
    level_.items.push_back(item);
    selection_ = {item.uid};
    primary_ = item.uid;
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::delete_selection() {
    if (selection_.empty()) return;
    push_undo();
    for (std::uint32_t uid : selection_) level_.remove(uid);
    selection_.clear();
    primary_ = 0;
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::duplicate_selection() {
    std::vector<LevelItem> copies;
    for (const LevelItem& i : level_.items) {
        if (!selection_.count(i.uid)) continue;
        LevelItem c = i;
        assign_uids(c);
        if (family_of(c.shape) == kFamGlobEgg) {
            c.x = std::min(c.x + kChunk, 0xFFFF);
        } else {
            c.x = std::min(c.x + std::max(grid_, 16), 0xFFFF);
            c.y = std::min(c.y + std::max(grid_, 16), 0xFFFF);
        }
        copies.push_back(std::move(c));
    }
    if (copies.empty()) return;
    if (paste_relinks_) relink_pasted(copies, level_, level_info(), map_, map_);
    push_undo();
    selection_.clear();
    for (LevelItem& c : copies) {
        selection_.insert(c.uid);
        primary_ = c.uid;
        level_.items.push_back(std::move(c));
    }
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::move_selection(std::int32_t dx, std::int32_t dy, std::int32_t dz) {
    push_undo();
    auto sign = [](std::int32_t v) { return (v > 0) - (v < 0); };
    for (std::uint32_t uid : selection_) {
        if (!level_.top_index(uid)) continue;
        LevelItem* i = level_.find(uid);
        // A glob egg only matters by its cell: it moves a cell at a time.
        if (family_of(i->shape) == kFamGlobEgg) {
            i->x = std::clamp(i->x + sign(dx) * kChunk, 0, 0xFFFF);
            i->y = std::clamp(i->y + sign(dy) * kChunk, 0, 0xFFFF);
        } else {
            i->x = std::clamp(i->x + dx, 0, 0xFFFF);
            i->y = std::clamp(i->y + dy, 0, 0xFFFF);
        }
        i->z = std::clamp(i->z + dz, 0, 255);
    }
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::select_only(std::uint32_t uid) {
    selection_ = {uid};
    primary_ = uid;
}

void EditorApp::select_same_quality_low(std::uint8_t q) {
    selection_.clear();
    for (const LevelItem& i : level_.items) {
        const std::uint8_t fam = family_of(i.shape);
        if (i.quality_low() != q || fam == kFamGlobEgg) continue;
        if (!usecode_->has_class(usecode_class_of(i, fam)) && i.shape != kShapeMarker) continue;
        selection_.insert(i.uid);
    }
}

void EditorApp::start_link_mode(std::uint32_t source) {
    const LevelItem* s = level_.find(source);
    if (!s) return;
    if (!links_by_quality(*s)) {
        message(tr("Cet œuf utilise sa qualité pour autre chose : il ne se lie pas.",
                   "This egg uses its quality for something else: it does not link."));
        return;
    }
    brush_ = {};
    pair_departure_ = 0;
    link_source_ = source;
    message(tr("Mode liaison : cliquez les objets à lier, Échap pour terminer.",
               "Link mode: click the items to link, Esc when done."));
}

void EditorApp::toggle_link(std::uint32_t target) {
    if (target == link_source_ || !level_.find(link_source_) || !level_.find(target)) return;
    push_undo();
    LevelItem* source = level_.find(link_source_);
    LevelItem* t = level_.find(target);
    if (source->quality_low() == 0) {
        const std::uint8_t q = free_link_quality(level_, level_info());
        if (q == 0) {
            undo_.pop_back();
            message(tr("Plus de qualité basse libre sur cette carte.", "No free quality low left on this map."));
            return;
        }
        source->quality = static_cast<std::uint16_t>((source->quality & 0xFF00) | q);
    }
    const std::uint8_t q = source->quality_low();
    const std::uint8_t fam = family_of(t->shape);
    if (t->quality_low() == q) {
        t->quality &= 0xFF00;
        message(tr("Délié.", "Unlinked."));
    } else {
        t->quality = static_cast<std::uint16_t>((t->quality & 0xFF00) | q);
        const bool scripted = usecode_->has_class(usecode_class_of(*t, fam)) || t->shape == kShapeMarker;
        // A switch (TRIGGER) acts on one shape among its links: it takes the
        // clicked item's, unless items of its current one are linked.
        std::string acts;
        if (acts_through_trigger(*source) && trigger_target(*source) != t->shape) {
            const std::uint16_t current = trigger_target(*source);
            bool kept = false;
            for (const LevelItem& i : level_.items) {
                kept |= i.uid != source->uid && i.uid != t->uid && i.quality_low() == q && i.shape == current;
            }
            if (!kept) {
                set_trigger_target(*source, t->shape);
                acts = std::string(" ") + tr("Il agit maintenant sur ", "It now acts on ") + hex(t->shape, 3) + " " +
                       shape_name(t->shape);
                // What it does to them: as on the original maps.
                if (const TriggerSetting* ts = find_trigger_setting(trigger_settings_, source->shape, t->shape)) {
                    apply_trigger_setting(*source, *ts);
                    acts += std::string(tr(", réglé comme dans les cartes d'origine", ", set as in the original maps"));
                }
                acts += ".";
            } else {
                acts = std::string(" ") + tr("Attention : il n'agit que sur ", "Note: it only acts on ") +
                         hex(current, 3) + " " + shape_name(current) + tr(", pas sur cet objet.", ", not on this item.");
            }
        }
        message(std::string(tr("Lié par la qualité basse ", "Linked by quality low ")) + hex(q, 2) +
                (scripted ? "." : tr(" (cet objet n'a pas de script : le lien ne fera rien).",
                                     " (this item has no script: the link does nothing).")) +
                acts);
    }
    level_dirty_ = world_dirty_ = true;
}

void EditorApp::copy_selection(bool cut) {
    clipboard_.clear();
    for (const LevelItem& i : level_.items) {
        if (selection_.count(i.uid)) clipboard_.push_back(i);
    }
    if (clipboard_.empty()) return;
    const LevelItem* anchor = level_.find(primary_);
    if (!anchor || !level_.top_index(primary_)) anchor = &clipboard_.front();
    clipboard_anchor_ = {anchor->x, anchor->y, anchor->z};
    clipboard_map_ = map_;
    const std::size_t n = clipboard_.size();
    if (cut) delete_selection();
    message(std::to_string(n) + (cut ? tr(" objet(s) coupé(s).", " item(s) cut.") : tr(" objet(s) copié(s).", " item(s) copied.")));
}

void EditorApp::paste() {
    if (clipboard_.empty()) return;
    std::optional<std::pair<std::int32_t, std::int32_t>> at;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    if (m.x >= view_x_ && m.y >= view_y_ && m.x < view_x_ + view_w_ && m.y < view_y_ + view_h_ && !io.WantCaptureMouse) {
        const auto [wx, wy] = to_world(m.x, m.y, clipboard_anchor_.z);
        at = std::pair{snap(wx), snap(wy)};
    }
    place_group(clipboard_, clipboard_anchor_, clipboard_map_, at, std::nullopt);
}

void EditorApp::place_group(std::vector<LevelItem> items, const Point3& anchor, int source_map,
                            std::optional<std::pair<std::int32_t, std::int32_t>> at, std::optional<std::int32_t> z) {
    if (items.empty()) return;
    std::int32_t dx = source_map == map_ ? std::max(grid_, 16) : 0, dy = dx;
    if (at) {
        dx = at->first - anchor.x;
        dy = at->second - anchor.y;
    }
    const std::int32_t dz = z ? *z - anchor.z : 0;
    // Glob eggs fill whole cells: the group moves by whole cells.
    const bool globs = std::any_of(items.begin(), items.end(),
                                   [&](const LevelItem& i) { return family_of(i.shape) == kFamGlobEgg; });
    if (globs) {
        dx = static_cast<std::int32_t>(std::lround(dx / static_cast<double>(kChunk))) * kChunk;
        dy = static_cast<std::int32_t>(std::lround(dy / static_cast<double>(kChunk))) * kChunk;
    }
    for (LevelItem& i : items) {
        assign_uids(i);
        i.x = std::clamp(i.x + dx, 0, 0xFFFF);
        i.y = std::clamp(i.y + dy, 0, 0xFFFF);
        i.z = std::clamp(i.z + dz, 0, 255);
    }
    const int renumbered = paste_relinks_ ? relink_pasted(items, level_, level_info(), source_map, map_) : 0;
    const std::size_t count = items.size();
    push_undo();
    selection_.clear();
    for (LevelItem& i : items) {
        selection_.insert(i.uid);
        primary_ = i.uid;
        level_.items.push_back(std::move(i));
    }
    level_dirty_ = world_dirty_ = true;
    std::string text = std::to_string(count) + tr(" objet(s) posé(s)", " item(s) placed");
    if (renumbered) {
        text += std::string(tr(", liens et ids renumérotés : ", ", links and ids renumbered: ")) + std::to_string(renumbered);
    }
    message(text + ".");
}

void EditorApp::find_examples(std::uint16_t shape) {
    // The original maps (the data's, not the mod's).
    examples_.clear();
    examples_shape_ = shape;
    examples_open_ = true;
    const LevelInfo info = level_info();
    try {
        const FlexEntries fixed = FlexEntries::read((options_.data / "STATIC/FIXED.DAT").string());
        for (std::size_t m = 1; m < fixed.entries.size() && m < 256; ++m) {
            if (fixed.entries[m].empty()) continue;
            const Level level = Level::parse(fixed.entries[m]);
            for (const LevelItem& i : level.items) {
                if (i.shape != shape) continue;
                const int linked = static_cast<int>(linked_group(level, i, info, 2048).size());
                const std::uint8_t fam = family_of(i.shape);
                examples_.push_back({static_cast<int>(m), i.x, i.y, i.z, linked > 0 ? linked - 1 : 0,
                                     describe(i, fam, class_name(usecode_class_of(i, fam)))});
            }
        }
    } catch (const std::exception& e) {
        message(e.what());
    }
    std::stable_sort(examples_.begin(), examples_.end(),
                     [](const Example& a, const Example& b) { return a.linked > b.linked; });
}

void EditorApp::examples_window() {
    if (!examples_open_) return;
    ImGui::SetNextWindowSize(ImVec2(640, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Exemples des cartes d'origine", "Examples from the original maps"), &examples_open_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s %s %s : %zu. %s", tr("Utilisations de", "Uses of"), hex(examples_shape_, 3).c_str(),
                       shape_name(examples_shape_).c_str(), examples_.size(),
                       tr("Cliquez une ligne : la carte s'ouvre avec l'objet et ses objets liés sélectionnés ; Ctrl+C, "
                          "puis collez-les sur votre carte (Ctrl+V).",
                          "Click a row: the map opens with the item and its linked items selected; Ctrl+C, then "
                          "paste them on your map (Ctrl+V)."));
    const Example* go = nullptr;
    if (ImGui::BeginTable("examples", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("carte", "map"), ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn(tr("liés", "linked"), ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn(tr("objet", "item"));
        ImGui::TableSetupColumn(tr("position", "position"), ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(examples_.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Example& e = examples_[static_cast<std::size_t>(r)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string id = std::to_string(e.map) + "##ex" + std::to_string(r);
                if (ImGui::Selectable(id.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) go = &e;
                ImGui::TableNextColumn();
                ImGui::Text("%d", e.linked);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.label.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d, %d, %d", e.x, e.y, e.z);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (go) {
        const Example e = *go;
        if (e.map != map_) {
            if (!project_->map_used(static_cast<std::size_t>(e.map))) return;
            open_map(e.map);
        }
        const LevelItem* found = nullptr;
        for (const LevelItem& i : level_.items) {
            if (i.shape == examples_shape_ && i.x == e.x && i.y == e.y && i.z == e.z) found = &i;
        }
        if (!found) {
            message(tr("Cet objet n'est plus à sa place sur la carte (modifiée par le mod).",
                       "That item is no longer in its place on the map (changed by the mod)."));
            return;
        }
        selection_.clear();
        for (std::uint32_t uid : linked_group(level_, *found, level_info(), 2048)) selection_.insert(uid);
        selection_.insert(found->uid);
        primary_ = found->uid;
        center_on(found->x, found->y, found->z);
    }
}

void EditorApp::load_prefabs() {
    prefabs_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(options_.prefabs, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(options_.prefabs, ec)) {
        if (entry.path().extension() != ".prefab") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (auto p = Prefab::parse(bytes)) prefabs_.push_back({entry.path().stem().string(), std::move(*p)});
    }
    std::sort(prefabs_.begin(), prefabs_.end(),
              [](const PrefabEntry& a, const PrefabEntry& b) { return a.name < b.name; });
}

void EditorApp::save_prefab(const std::string& name) {
    std::string file;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') file += c;
    }
    if (file.empty()) {
        message(tr("Donnez un nom au prefab (lettres, chiffres, - et _).", "Give the prefab a name (letters, digits, - and _)."));
        return;
    }
    Prefab p;
    for (const LevelItem& i : level_.items) {
        if (selection_.count(i.uid)) p.items.push_back(i);
    }
    if (p.items.empty()) return;
    const LevelItem* anchor = level_.find(primary_);
    if (!anchor || !level_.top_index(primary_)) anchor = &p.items.front();
    p.anchor_x = anchor->x;
    p.anchor_y = anchor->y;
    p.anchor_z = anchor->z;
    p.source_map = map_;
    try {
        std::filesystem::create_directories(options_.prefabs);
        const auto path = options_.prefabs / (file + ".prefab");
        const auto bytes = p.serialize();
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error(path.string());
        message(std::string(tr("Prefab enregistré : ", "Prefab saved: ")) + path.string());
    } catch (const std::exception& e) {
        message(std::string(tr("Échec de l'enregistrement : ", "Save failed: ")) + e.what());
    }
    load_prefabs();
}

void EditorApp::tab_prefabs() {
    ImGui::TextWrapped("%s", tr("Des groupes d'objets enregistrés pour être reposés : sélectionnez des objets, puis "
                                "Édition > Enregistrer la sélection comme prefab. Cliquez un prefab, puis dans la vue "
                                "(au z de travail) ; ses liens sont renumérotés.",
                                "Groups of items saved to be placed again: select items, then Edit > Save the "
                                "selection as a prefab. Click a prefab, then in the view (at the work z); its links "
                                "are renumbered."));
    ImGui::TextDisabled("%s", std::filesystem::absolute(options_.prefabs).string().c_str());
    if (ImGui::SmallButton(tr("Recharger", "Reload"))) load_prefabs();
    ImGui::BeginChild("prefabs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    int remove = -1;
    for (std::size_t k = 0; k < prefabs_.size(); ++k) {
        const PrefabEntry& e = prefabs_[k];
        const std::string label = e.name + "  (" + std::to_string(e.prefab.items.size()) + " " +
                                  tr("objets", "items") + ")##pf" + std::to_string(k);
        if (ImGui::Selectable(label.c_str(), brush_.kind == BrushKind::Prefab && brush_.value == k)) {
            brush_ = {BrushKind::Prefab, 0, 0, static_cast<std::uint16_t>(k)};
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(tr("Supprimer ce prefab", "Delete this prefab"))) remove = static_cast<int>(k);
            ImGui::EndPopup();
        }
        if (ImGui::BeginItemTooltip()) {
            std::map<std::string, int> kinds;
            for (const LevelItem& i : e.prefab.items) {
                const std::uint8_t fam = family_of(i.shape);
                ++kinds[describe(i, fam, class_name(usecode_class_of(i, fam))).substr(0, 24)];
            }
            int shown = 0;
            for (const auto& [k2, n] : kinds) {
                if (++shown > 12) break;
                ImGui::Text("%d x %s", n, k2.c_str());
            }
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();
    if (remove >= 0) {
        std::error_code ec;
        std::filesystem::remove(options_.prefabs / (prefabs_[static_cast<std::size_t>(remove)].name + ".prefab"), ec);
        brush_ = {};
        load_prefabs();
    }
}

void EditorApp::go_to_arrival(int map, std::uint8_t id) {
    if (map != map_) {
        if (map <= 0 || !project_->map_used(static_cast<std::size_t>(map))) return;
        open_map(map);
    }
    for (const LevelItem& i : level_.items) {
        if (is_arrival(i, family_of(i.shape)) && i.quality_low() == id) {
            select_only(i.uid);
            center_on(i.x, i.y, i.z);
            return;
        }
    }
    message(tr("Aucune arrivée de cet id sur cette carte.", "No arrival of this id on this map."));
}

void EditorApp::glob_from_selection() {
    const LevelItem* p = level_.find(primary_);
    if (!p) return;
    const std::int32_t bx = p->x & ~0x1FF, by = p->y & ~0x1FF;
    std::int32_t min_z = 255;
    std::vector<const LevelItem*> items;
    for (const LevelItem& i : level_.items) {
        if (!selection_.count(i.uid)) continue;
        const std::uint8_t fam = family_of(i.shape);
        if (fam == kFamGlobEgg || fam == kFamTriggerEgg || fam == kFamMonsterEgg || fam == kFamTeleportEgg ||
            !i.contents.empty() || i.flags || i.quality || i.npc_number || i.map_number) {
            message(tr("Un glob ne contient que du décor simple (sans qualité, drapeaux, contenu ni œuf).",
                       "A glob holds plain scenery only (no quality, flags, contents or egg)."));
            return;
        }
        if ((i.x & ~0x1FF) != bx || (i.y & ~0x1FF) != by) {
            message(tr("Les objets d'un glob doivent tenir dans une même case de 512 x 512.",
                       "A glob's items must fit in one 512 x 512 cell."));
            return;
        }
        min_z = std::min(min_z, i.z);
        items.push_back(&i);
    }
    if (items.empty()) return;
    Glob glob;
    for (const LevelItem* i : items) {
        glob.pieces.push_back({static_cast<std::uint8_t>(std::max(0, (i->x - bx - 1) / 2)),
                               static_cast<std::uint8_t>(std::max(0, (i->y - by - 1) / 2)),
                               static_cast<std::uint8_t>(i->z - min_z), i->shape, i->frame});
    }
    const std::size_t index = project_->add_glob(glob);
    glob_cache_.erase(index);
    push_undo();
    for (const LevelItem* i : items) selection_.erase(i->uid);
    const std::set<std::uint32_t> gone = [&] {
        std::set<std::uint32_t> s;
        for (const LevelItem* i : items) s.insert(i->uid);
        return s;
    }();
    std::erase_if(level_.items, [&](const LevelItem& i) { return gone.count(i.uid) != 0; });
    LevelItem egg;
    egg.uid = new_uid();
    egg.shape = kShapeGlobEgg;
    egg.x = bx | 0x1FF;
    egg.y = by | 0x1FF;
    egg.z = min_z;
    egg.quality = static_cast<std::uint16_t>(index);
    level_.items.push_back(egg);
    select_only(egg.uid);
    level_dirty_ = world_dirty_ = true;
    message(std::string(tr("Nouveau glob ", "New glob ")) + std::to_string(index));
}

void EditorApp::explode_glob(std::uint32_t egg_uid) {
    const LevelItem* egg = level_.find(egg_uid);
    if (!egg || family_of(egg->shape) != kFamGlobEgg) return;
    const LevelItem copy = *egg;
    push_undo();
    level_.remove(egg_uid);
    selection_.clear();
    for (const GlobPiece& p : glob_cached(copy.quality).pieces) {
        const Point3 pos = glob_piece_position(copy, p);
        LevelItem i;
        i.uid = new_uid();
        i.x = pos.x;
        i.y = pos.y;
        i.z = pos.z;
        i.shape = p.shape;
        i.frame = p.frame;
        level_.items.push_back(i);
        selection_.insert(i.uid);
    }
    primary_ = selection_.empty() ? 0 : *selection_.begin();
    level_dirty_ = world_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Panels

bool EditorApp::request_quit() {
    if (level_dirty_ || project_->dirty()) {
        want_quit_confirm_ = true;
        return false;
    }
    return true;
}

void EditorApp::frame() {
    if (!ImGui::IsAnyItemActive()) edit_open_ = false;
    ImGuiIO& io = ImGui::GetIO();
    // Shortcuts that work everywhere.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) test_in_game();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_selection(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) copy_selection(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste();
    }

    main_menu();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float top = vp->WorkPos.y;
    view_x_ = vp->WorkPos.x + kLeftWidth;
    view_y_ = top;
    view_w_ = std::max(64.0f, vp->WorkSize.x - kLeftWidth - kRightWidth);
    view_h_ = std::max(64.0f, vp->WorkSize.y - kStatusHeight);

    left_panel();
    right_panel();
    status_bar();
    if (glob_window_open_) glob_window();
    problems_window();
    examples_window();
    modal_popups();
    // Picking a brush ends the link mode; a pair waits only with its brush.
    if (brush_.kind != BrushKind::None) link_source_ = 0;
    if (brush_.kind != BrushKind::TeleportPair) pair_departure_ = 0;

    handle_view_input();
    hover_tooltip();
    render_view_if_needed();
    if (view_texture_) {
        ImGui::GetBackgroundDrawList()->AddImage(
            tex_id(view_texture_), ImVec2(view_x_, view_y_),
            ImVec2(view_x_ + surface_w_ * zoom_, view_y_ + surface_h_ * zoom_));
    }
    draw_overlays();
}

void EditorApp::main_menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu(tr("Fichier", "File"))) {
        if (ImGui::MenuItem(tr("Enregistrer", "Save"), "Ctrl+S")) save();
        if (ImGui::MenuItem(tr("Tester dans le jeu", "Test in game"), "F5")) test_in_game();
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Quitter", "Quit"))) {
            if (request_quit()) quit_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Édition", "Edit"))) {
        if (ImGui::MenuItem(tr("Annuler", "Undo"), "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem(tr("Rétablir", "Redo"), "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Couper", "Cut"), "Ctrl+X", false, !selection_.empty())) copy_selection(true);
        if (ImGui::MenuItem(tr("Copier", "Copy"), "Ctrl+C", false, !selection_.empty())) copy_selection(false);
        if (ImGui::MenuItem(tr("Coller", "Paste"), "Ctrl+V", false, !clipboard_.empty())) paste();
        ImGui::MenuItem(tr("Coller : renuméroter liens et téléporteurs", "Paste: renumber links and teleports"), nullptr,
                        &paste_relinks_);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Les objets collés qui se lient entre eux (interrupteur et portes, téléporteurs) "
                                       "reçoivent des numéros libres de cette carte : ils restent liés entre eux, sans "
                                       "toucher aux liens déjà là.",
                                       "Pasted items that link to each other (switch and doors, teleporters) get "
                                       "numbers this map does not use: they stay linked together, apart from the "
                                       "links already there."));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Dupliquer", "Duplicate"), "Ctrl+D", false, !selection_.empty())) duplicate_selection();
        if (ImGui::MenuItem(tr("Supprimer", "Delete"), "Suppr", false, !selection_.empty())) delete_selection();
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Créer un glob avec la sélection", "Make a glob of the selection"), nullptr, false,
                            !selection_.empty())) {
            glob_from_selection();
        }
        const LevelItem* p = level_.find(primary_);
        if (ImGui::MenuItem(tr("Dégrouper le glob", "Explode the glob"), nullptr, false,
                            p && family_of(p->shape) == kFamGlobEgg)) {
            explode_glob(primary_);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Lier en cliquant", "Link by clicking"), "L", false, p && links_by_quality(*p))) {
            start_link_mode(primary_);
        }
        if (ImGui::MenuItem(tr("Enregistrer la sélection comme prefab...", "Save the selection as a prefab..."), nullptr,
                            false, !selection_.empty())) {
            want_save_prefab_ = true;
        }
        if (ImGui::MenuItem(tr("Exemples des cartes d'origine", "Examples from the original maps"), nullptr, false,
                            p != nullptr)) {
            find_examples(p->shape);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Carte", "Map"))) {
        if (ImGui::MenuItem(tr("Ouvrir une carte...", "Open a map..."))) {
            want_open_map_ = true;
            open_map_choice_ = map_;
        }
        if (ImGui::MenuItem(tr("Nouvelle carte", "New map"))) new_empty_map();
        if (ImGui::MenuItem(tr("Générer un niveau (Crusader: No Limit)...", "Generate a level (Crusader: No Limit)..."))) {
            want_generate_ = true;
            generate_seed_ = static_cast<int>(SDL_GetTicks() % 100000) + 1;
        }
        if (ImGui::MenuItem(tr("Copier cette carte vers un emplacement libre", "Copy this map to a free slot"))) {
            copy_map_to_free_slot();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Aller au départ de mission", "Go to the mission start"), nullptr, false,
                            find_start_egg() != nullptr)) {
            go_to_arrival(map_, kStartEgg);
        }
        if (ImGui::MenuItem(tr("Vérifier le niveau", "Check the level"), nullptr, &problems_open_)) {}
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Affichage", "View"))) {
        if (ImGui::MenuItem(tr("Marqueurs d'éditeur (œufs...)", "Editor markers (eggs...)"), nullptr, &show_markers_)) {
            world_dirty_ = true;
        }
        if (ImGui::MenuItem(tr("Objets invisibles", "Invisible items"), nullptr, &show_invisible_)) world_dirty_ = true;
        if (ImGui::MenuItem(tr("Contenu des globs", "Glob contents"), nullptr, &show_globs_)) world_dirty_ = true;
        ImGui::MenuItem(tr("Zones des œufs et téléportations", "Egg zones and teleports"), nullptr, &show_zones_);
        ImGui::MenuItem(tr("Objets liés (même qualité basse)", "Linked items (same quality low)"), nullptr, &show_links_);
        ImGui::MenuItem(tr("Clic à travers les globs", "Click through globs"), nullptr, &pick_glob_pieces_);
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Éditeur de globs", "Glob editor"), nullptr, &glob_window_open_)) {
            glob_edit_ = project_->glob(static_cast<std::size_t>(glob_edit_index_));
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorApp::left_panel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(kLeftWidth, vp->WorkSize.y - kStatusHeight));
    ImGui::Begin("##left", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Text("%s %d  (%zu %s)", tr("Carte", "Map"), map_, level_.record_count(), tr("enregistrements", "records"));
    if (const gen::ManifestLevel* seed = manifest().find(map_)) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "No Limit : %s %u", tr("graine", "seed"), seed->seed);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s %d", seed->biome_name.c_str(), tr("sortie vers la carte", "exit to map"),
                              seed->next_map);
        }
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mod", tr("dossier du mod", "mod folder"), &mod_path_text_);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(tr("Dossier où l'éditeur enregistre (STATIC/FIXED.DAT, STATIC/GLOB.FLX) ; le jeu le "
                                  "charge avec --mod. Les données d'origine ne sont jamais modifiées.",
                                  "Folder the editor saves to (STATIC/FIXED.DAT, STATIC/GLOB.FLX); the game loads it "
                                  "with --mod. The original data are never changed."));
        ImGui::EndTooltip();
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt(tr("Coupe en z", "Cut at z"), &z_cut_, 0, 255)) view_dirty_ = true;
    help_marker(tr("Cache les objets plus hauts : pour éditer un étage sans les plafonds.",
                   "Hides higher items: edit a floor without the ceilings."));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(tr("z de travail", "Work z"), &work_z_, 0, 255);
    help_marker(tr("Hauteur où les nouveaux objets sont posés (PgPréc/PgSuiv sans sélection).",
                   "Height where new items go (PgUp/PgDn without a selection)."));
    static const int grids[] = {1, 8, 16, 32, 64, 128, 256, 512};
    int g = 0;
    while (g < 7 && grids[g] != grid_) ++g;
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo(tr("Grille", "Grid"), &g, "1\0" "8\0" "16\0" "32\0" "64\0" "128\0" "256\0" "512\0")) grid_ = grids[g];
    help_marker(tr("Pas d'aimantation en unités usecode. Un carreau de sol fait 32 x 32 ; Alt pendant le glisser "
                   "désactive l'aimantation.",
                   "Snapping step in usecode units. A floor tile is 32 x 32; hold Alt while dragging to disable it."));
    ImGui::Separator();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(tr("Objets", "Items"))) {
            tab_objects();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Formes", "Shapes"))) {
            tab_shapes();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Globs")) {
            tab_globs();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Œufs", "Eggs"))) {
            tab_eggs();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Prefabs")) {
            tab_prefabs();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scripts")) {
            tab_classes();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void EditorApp::tab_objects() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ofilter", tr("filtrer (nom, forme, qualité...)", "filter (name, shape, quality...)"),
                             &object_filter_);
    ImGui::Checkbox(tr("Seulement œufs et objets à script", "Only eggs and scripted items"), &objects_scripted_only_);
    std::vector<const LevelItem*> rows;
    for (const LevelItem& i : level_.items) {
        const std::uint8_t fam = family_of(i.shape);
        const std::uint16_t cid = usecode_class_of(i, fam);
        if (objects_scripted_only_ && fam != kFamTriggerEgg && fam != kFamTeleportEgg && fam != kFamMonsterEgg &&
            fam != kFamContainer && !usecode_->has_class(cid) && i.shape != kShapeMarker) {
            continue;
        }
        if (!object_filter_.empty()) {
            const std::string label = describe(i, fam, class_name(cid)) + " " + hex(i.shape, 3);
            if (!contains_ci(label, object_filter_)) continue;
        }
        rows.push_back(&i);
    }
    ImGui::TextDisabled("%zu", rows.size());
    ImGui::BeginChild("objects");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const LevelItem& i = *rows[static_cast<std::size_t>(r)];
            const std::uint8_t fam = family_of(i.shape);
            const std::string label = describe(i, fam, class_name(usecode_class_of(i, fam))) + "  (" +
                                      std::to_string(i.x) + ", " + std::to_string(i.y) + ", " + std::to_string(i.z) +
                                      ")##" + std::to_string(i.uid);
            if (ImGui::Selectable(label.c_str(), selection_.count(i.uid) != 0)) {
                select_only(i.uid);
                center_on(i.x, i.y, i.z);
            }
        }
    }
    ImGui::EndChild();
}

void EditorApp::tab_shapes() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sfilter", tr("filtrer (numéro ou nom)", "filter (number or name)"), &shape_filter_);
    ImGui::SetNextItemWidth(200);
    std::string fam_label = family_filter_ < 0 ? std::string(tr("toutes les familles", "all families"))
                                               : std::to_string(family_filter_) + " " + family_name(static_cast<std::uint8_t>(family_filter_));
    if (ImGui::BeginCombo(tr("Famille", "Family"), fam_label.c_str())) {
        if (ImGui::Selectable(tr("toutes les familles", "all families"), family_filter_ < 0)) family_filter_ = -1;
        for (int f : {0, 1, 3, 4, 6, 7, 8, 10, 11, 12, 13}) {
            const std::string l = std::to_string(f) + " " + family_name(static_cast<std::uint8_t>(f));
            if (ImGui::Selectable(l.c_str(), family_filter_ == f)) family_filter_ = f;
        }
        ImGui::EndCombo();
    }
    if (brush_.kind == BrushKind::Shape) {
        ImGui::Text("%s %s %s", tr("Pinceau :", "Brush:"), hex(brush_.shape, 3).c_str(), shape_name(brush_.shape).c_str());
        int frame = brush_.frame;
        const int frames = frame_count(brush_.shape);
        ImGui::SetNextItemWidth(200);
        if (frames > 1 && ImGui::SliderInt(tr("Image", "Frame"), &frame, 0, std::min(frames, 256) - 1)) {
            brush_.frame = static_cast<std::uint8_t>(frame);
        }
        shape_thumbnail(brush_.shape, brush_.frame, 96);
    } else {
        ImGui::TextDisabled("%s", tr("Cliquez une forme, puis dans la vue pour la poser.",
                                     "Click a shape, then in the view to place it."));
    }
    std::vector<std::uint16_t> list;
    for (std::uint16_t s = 1; s < shapes_flex_->count() && s < typeflags_.size(); ++s) {
        if (shapes_flex_->entry(s).empty()) continue;
        if (family_filter_ >= 0 && typeflags_[s].family != family_filter_) continue;
        if (!shape_filter_.empty() && !contains_ci(hex(s, 3) + " " + std::to_string(s) + " " + shape_name(s), shape_filter_)) {
            continue;
        }
        list.push_back(s);
    }
    ImGui::BeginChild("shapes");
    const float cell = 72.0f;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 8)));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>((list.size() + columns - 1) / columns), cell + 8);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int c = 0; c < columns; ++c) {
                const std::size_t k = static_cast<std::size_t>(row) * columns + c;
                if (k >= list.size()) break;
                const std::uint16_t s = list[k];
                if (c) ImGui::SameLine();
                int w, h, xo, yo;
                SDL_Texture* t = frame_texture(s, 0, &w, &h, &xo, &yo);
                ImGui::PushID(s);
                const float scale = t ? std::min(1.0f, cell / static_cast<float>(std::max(w, h))) : 1.0f;
                const ImVec2 size = t ? ImVec2(w * scale, h * scale) : ImVec2(cell, cell);
                ImGui::BeginGroup();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool selected = brush_.kind == BrushKind::Shape && brush_.shape == s;
                if (ImGui::InvisibleButton("b", ImVec2(cell, cell))) brush_ = {BrushKind::Shape, s, 0, 0};
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell),
                                  selected ? IM_COL32(90, 90, 30, 255)
                                           : ImGui::IsItemHovered() ? IM_COL32(60, 60, 70, 255) : IM_COL32(30, 30, 36, 255));
                if (t) {
                    const ImVec2 o(p.x + (cell - size.x) / 2, p.y + (cell - size.y) / 2);
                    dl->AddImage(tex_id(t), o, ImVec2(o.x + size.x, o.y + size.y));
                }
                ImGui::EndGroup();
                if (ImGui::BeginItemTooltip()) {
                    ImGui::Text("%s %s %s", hex(s, 3).c_str(), shape_name(s).c_str(), family_name(typeflags_[s].family));
                    ImGui::Text("%d %s, %s %ux%ux%u", frame_count(s), tr("images", "frames"), tr("emprise", "footpad"),
                                typeflags_[s].footpad_x, typeflags_[s].footpad_y, typeflags_[s].footpad_z);
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void EditorApp::tab_globs() {
    static bool only_this_map = true;
    ImGui::Checkbox(tr("Seulement ceux de cette carte", "Only those of this map"), &only_this_map);
    std::vector<std::size_t> list;
    if (only_this_map) {
        std::set<std::size_t> used;
        for (const LevelItem& i : level_.items) {
            if (family_of(i.shape) == kFamGlobEgg && i.quality) used.insert(i.quality);
        }
        list.assign(used.begin(), used.end());
    } else {
        for (std::size_t g = 1; g < project_->glob_count(); ++g) {
            if (!project_->glob_bytes(g).empty()) list.push_back(g);
        }
    }
    ImGui::TextDisabled("%zu globs. %s", list.size(),
                        tr("Clic : pinceau ; double clic : éditer.", "Click: brush; double click: edit."));
    ImGui::BeginChild("globs");
    const float cell = 96.0f;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 8)));
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>((list.size() + columns - 1) / columns), cell + 24);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int c = 0; c < columns; ++c) {
                const std::size_t k = static_cast<std::size_t>(row) * columns + c;
                if (k >= list.size()) break;
                const std::size_t gi = list[k];
                if (c) ImGui::SameLine();
                ImGui::PushID(static_cast<int>(gi));
                ImGui::BeginGroup();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool selected = brush_.kind == BrushKind::Glob && brush_.value == gi;
                if (ImGui::InvisibleButton("b", ImVec2(cell, cell))) {
                    brush_ = {BrushKind::Glob, 0, 0, static_cast<std::uint16_t>(gi)};
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    glob_edit_index_ = static_cast<int>(gi);
                    glob_edit_ = project_->glob(gi);
                    glob_edit_dirty_ = false;
                    glob_window_open_ = true;
                }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell),
                                  selected ? IM_COL32(90, 90, 30, 255) : IM_COL32(30, 30, 36, 255));
                int w = 0, h = 0;
                if (SDL_Texture* t = glob_texture(gi, &w, &h)) {
                    const float s = std::min(cell / w, cell / static_cast<float>(h));
                    const ImVec2 o(p.x + (cell - w * s) / 2, p.y + (cell - h * s) / 2);
                    dl->AddImage(tex_id(t), o, ImVec2(o.x + w * s, o.y + h * s));
                }
                ImGui::Text("%zu", gi);
                ImGui::EndGroup();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void EditorApp::tab_eggs() {
    ImGui::TextWrapped("%s", tr("Les niveaux d'origine sont pilotés par des œufs : choisissez-en un puis cliquez dans "
                                "la vue. Les paramètres se règlent ensuite dans Propriétés.",
                                "The original levels are driven by eggs: pick one, then click in the view. Set "
                                "its parameters in Properties afterwards."));
    static std::uint8_t arrival_id = kStartEgg, departure_id = 1;
    ImGui::SeparatorText(tr("Outils guidés", "Guided tools"));
    if (ImGui::Button(tr("Départ de mission", "Mission start"))) brush_ = {BrushKind::MissionStart, 0, 0, 0};
    help_marker(tr("Cliquez ensuite dans la vue : l'œuf d'arrivée 0x1E, où la mission commence, y est posé (ou déplacé "
                   "s'il existe). F5 et les nouvelles parties sur cette carte commencent dessus.",
                   "Then click in the view: the arrival egg 0x1E, where the mission starts, goes there (or moves there "
                   "if the map has one). F5 and new games on this map start on it."));
    if (find_start_egg()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Y aller", "Go to it"))) go_to_arrival(map_, kStartEgg);
    }
    if (ImGui::Button(tr("Paire de téléporteurs", "Teleporter pair"))) {
        brush_ = {BrushKind::TeleportPair, 0, 0, 0};
        pair_departure_ = 0;
    }
    help_marker(tr("Premier clic : où le joueur part (une zone de déclenchement) ; second clic : où il arrive. "
                   "L'éditeur donne à la paire un id libre et relie les deux œufs.",
                   "First click: where the player leaves (a trigger area); second click: where they arrive. The "
                   "editor gives the pair a free id and links both eggs."));
    ImGui::TextWrapped("%s", tr("Relier un interrupteur à des portes, champs de force, vapeur... : sélectionnez-le, "
                                "appuyez sur L puis cliquez les objets.",
                                "To link a switch to doors, force fields, steam...: select it, press L, then click "
                                "the items."));
    ImGui::SeparatorText(tr("Téléportation", "Teleport"));
    ImGui::SetNextItemWidth(100);
    input_u8("##arr", arrival_id);
    ImGui::SameLine();
    if (ImGui::Button(tr("Arrivée (id)", "Arrival (id)"))) {
        brush_ = {BrushKind::TeleportArrival, 0, 0, static_cast<std::uint16_t>(arrival_id)};
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("id libre", "free id"))) {
        if (const std::uint8_t id = free_arrival_id(level_, level_info())) arrival_id = id;
    }
    help_marker(tr("Œuf d'arrivée : le jeu commence une mission sur l'œuf 0x1E de la carte. Les téléporteurs et "
                   "les changements de carte visent ces œufs par leur id.",
                   "Arrival egg: a mission starts on the map's egg 0x1E. Teleporters and map changes aim at "
                   "these eggs by id."));
    ImGui::SetNextItemWidth(100);
    input_u8("##dep", departure_id);
    ImGui::SameLine();
    if (ImGui::Button(tr("Départ vers l'id", "Departure to id"))) {
        brush_ = {BrushKind::TeleportDeparture, 0, 0, static_cast<std::uint16_t>(departure_id)};
    }
    help_marker(tr("Zone qui téléporte le joueur vers l'œuf d'arrivée de cet id, sur cette carte ou une autre.",
                   "Area that teleports the player to the arrival egg of this id, on this map or another."));

    ImGui::SeparatorText(tr("Déclencheurs (classes usecode 0x900+)", "Triggers (usecode classes 0x900+)"));
    ImGui::BeginChild("trig", ImVec2(0, 170), ImGuiChildFlags_Borders);
    for (std::uint16_t t = 0; t < 0x100; ++t) {
        const std::string n = class_name(static_cast<std::uint16_t>(kClassEggs + t));
        if (n.empty()) continue;
        const std::string label = hex(t, 2) + "  " + n;
        if (ImGui::Selectable(label.c_str(), brush_.kind == BrushKind::TriggerEgg && brush_.value == t)) {
            brush_ = {BrushKind::TriggerEgg, 0, 0, t};
        }
    }
    ImGui::EndChild();

    // NPCs: MONSTER pairs, as most NPCs of the original maps, or monster eggs.
    ImGui::SeparatorText(tr("PNJ (types de DTABLE.FLX)", "NPCs (DTABLE.FLX types)"));
    static bool use_eggs = false;
    {
        ImGui::Checkbox(tr("En œuf de monstre", "As a monster egg"), &use_eggs);
        help_marker(tr("Par défaut, un PNJ est une paire MONSTER (sa place et son type, liés), comme la "
                       "plupart des PNJ des cartes d'origine ; un œuf de monstre (forme 0x24F) le fait apparaître "
                       "quand le joueur arrive dans la zone.",
                       "By default an NPC is a MONSTER pair (its place and its kind, linked), as most NPCs of the "
                       "original maps; a monster egg (shape 0x24F) makes it when the player comes near."));
    }
    ImGui::TextDisabled("%s", tr("Cliquez un type, puis dans la vue.", "Click a type, then in the view."));
    const BrushKind npc_brush = use_eggs ? BrushKind::MonsterEgg : BrushKind::MonsterPair;
    ImGui::BeginChild("npcs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (int t = 0; t < dtable::NpcTypes::kRecords; ++t) {
        const std::uint16_t shape = npc_types_->word(t, 0x3E);
        if (shape == 0) continue;
        char label[96];
        std::snprintf(label, sizeof label, "%2d  %s %s  (%u %s)", t, hex(shape, 3).c_str(), shape_name(shape).c_str(),
                      npc_types_->max_hp(t), tr("PV", "HP"));
        if (ImGui::Selectable(label, brush_.kind == npc_brush && brush_.value == t)) {
            brush_ = {npc_brush, 0, 0, static_cast<std::uint16_t>(t)};
        }
        if (ImGui::BeginItemTooltip()) {
            shape_thumbnail(shape, 0, 96);
            ImGui::EndTooltip();
        }
    }
    ImGui::EndChild();
}

void EditorApp::tab_classes() {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cfilter", tr("filtrer les classes", "filter the classes"), &class_filter_);
    static int chosen = -1;
    ImGui::BeginChild("classes", ImVec2(0, 260), ImGuiChildFlags_Borders);
    for (std::size_t c = 0; c < usecode_->class_count(); ++c) {
        const auto cid = static_cast<std::uint16_t>(c);
        if (!usecode_->has_class(cid)) continue;
        const std::string label = hex(cid, 3) + "  " + usecode_->class_name(cid);
        if (!contains_ci(label, class_filter_)) continue;
        if (ImGui::Selectable(label.c_str(), chosen == static_cast<int>(c))) chosen = static_cast<int>(c);
    }
    ImGui::EndChild();
    if (chosen < 0) {
        ImGui::TextWrapped("%s", tr("0x000-0x7FF : classes des formes ; 0x900+ : œufs déclencheurs (type = qualité "
                                    "basse) ; 0xA00+ : bibliothèques appelées par les scripts (TRIGGER, FREE...).",
                                    "0x000-0x7FF: shape classes; 0x900+: trigger eggs (type = quality low); "
                                    "0xA00+: libraries called by scripts (TRIGGER, FREE...)."));
        return;
    }
    const auto cid = static_cast<std::uint16_t>(chosen);
    ImGui::Text("%s %s", hex(cid, 3).c_str(), usecode_->class_name(cid).c_str());
    ImGui::TextDisabled("%s", tr("Événements traités :", "Events handled:"));
    for (std::uint32_t e = 0; e < usecode_->event_count(cid) && e < 0x20; ++e) {
        if (!usecode_->event_offset(cid, e)) continue;
        ImGui::BulletText("%02X %s  %s", e, event_name(e), event_help(e));
    }
    std::size_t uses = 0;
    for (const LevelItem& i : level_.items) {
        if (usecode_class_of(i, family_of(i.shape)) == cid) ++uses;
    }
    ImGui::Text("%zu %s", uses, tr("objet(s) de cette carte", "item(s) on this map"));
    if (uses && ImGui::Button(tr("Les sélectionner", "Select them"))) {
        selection_.clear();
        for (const LevelItem& i : level_.items) {
            if (usecode_class_of(i, family_of(i.shape)) == cid) selection_.insert(i.uid), primary_ = i.uid;
        }
    }
}

void EditorApp::right_panel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kRightWidth, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(kRightWidth, vp->WorkSize.y - kStatusHeight));
    ImGui::Begin(tr("Propriétés", "Properties"), nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (selection_.empty()) {
        ImGui::TextWrapped("%s", tr("Rien de sélectionné.\n\nClic : sélectionner ; Ctrl/Maj+clic : ajouter ; Alt+clic "
                                    ": objet suivant sous le pointeur ; glisser : déplacer ou sélection par rectangle "
                                    "; clic droit ou molette enfoncée : faire défiler ; molette : zoom ; flèches, "
                                    "PgPréc/PgSuiv : déplacer la sélection ; F : centrer ; Suppr : supprimer ; "
                                    "Échap : lâcher le pinceau ; Ctrl+C / Ctrl+X / Ctrl+V : copier, couper, coller "
                                    "sous le pointeur (aussi vers une autre carte) ; L : lier en cliquant.\n\n"
                                    "Pour commencer : onglet Œufs > Départ de mission, puis F5 pour tester.",
                                    "Nothing selected.\n\nClick: select; Ctrl/Shift+click: add; Alt+click: next item "
                                    "under the pointer; drag: move, or select by rectangle; right or middle drag: "
                                    "scroll; wheel: zoom; arrows, PgUp/PgDn: move the selection; F: centre; Del: "
                                    "delete; Esc: drop the brush; Ctrl+C / Ctrl+X / Ctrl+V: copy, cut, paste under "
                                    "the pointer (to another map too); L: link by clicking.\n\n"
                                    "To begin: Eggs tab > Mission start, then F5 to test."));
    } else if (selection_.size() > 1) {
        ImGui::Text("%zu %s", selection_.size(), tr("objets sélectionnés", "items selected"));
        if (ImGui::Button(tr("Supprimer", "Delete"))) delete_selection();
        ImGui::SameLine();
        if (ImGui::Button(tr("Dupliquer", "Duplicate"))) duplicate_selection();
        ImGui::SameLine();
        if (ImGui::Button(tr("Créer un glob", "Make a glob"))) glob_from_selection();
        if (ImGui::Button("z +8")) move_selection(0, 0, 8);
        ImGui::SameLine();
        if (ImGui::Button("z -8")) move_selection(0, 0, -8);
        static std::uint8_t q = 0;
        ImGui::SetNextItemWidth(80);
        input_u8(tr("qualité basse", "quality low"), q);
        ImGui::SameLine();
        if (ImGui::Button(tr("Appliquer", "Apply"))) {
            push_undo();
            for (std::uint32_t uid : selection_) {
                if (LevelItem* i = level_.find(uid)) i->quality = static_cast<std::uint16_t>((i->quality & 0xFF00) | q);
            }
            level_dirty_ = world_dirty_ = true;
        }
        if (LevelItem* p = level_.find(primary_)) {
            ImGui::SeparatorText(tr("Objet principal", "Main item"));
            properties(*p);
        }
    } else if (LevelItem* p = level_.find(primary_ ? primary_ : *selection_.begin())) {
        properties(*p);
    }
    if (!deferred_items_.empty()) {
        push_undo();
        for (LevelItem& i : deferred_items_) {
            selection_.insert(i.uid);
            level_.items.push_back(std::move(i));
        }
        deferred_items_.clear();
        level_dirty_ = world_dirty_ = true;
    }
    ImGui::End();
}

void EditorApp::properties(LevelItem& item) {
    const LevelItem before = *level_.top_of(item.uid);
    bool changed = false;
    const std::uint8_t fam = family_of(item.shape);
    const std::uint16_t cid = usecode_class_of(item, fam);
    ImGui::TextWrapped("%s", describe(item, fam, class_name(cid)).c_str());
    ImGui::TextDisabled("%s %u - %s", tr("famille", "family"), fam, family_name(fam));
    shape_thumbnail(item.shape, item.frame, 128);
    if (ImGui::SmallButton(tr("Exemples des cartes d'origine", "Examples from the original maps"))) {
        find_examples(item.shape);
    }
    help_marker(tr("Où les concepteurs ont utilisé cet objet, avec ses objets liés : de quoi copier un montage qui "
                   "marche.",
                   "Where the designers used this item, with its linked items: a working set-up to copy."));

    ImGui::PushItemWidth(120);
    changed |= input_u16(tr("Forme", "Shape"), item.shape);
    ImGui::SameLine();
    ImGui::TextUnformatted(shape_name(item.shape).c_str());
    const int frames = frame_count(item.shape);
    int frame = item.frame;
    if (frames > 1 && ImGui::SliderInt(tr("Image", "Frame"), &frame, 0, std::min(frames, 256) - 1)) {
        item.frame = static_cast<std::uint8_t>(frame);
        changed = true;
    }
    if (level_.top_index(item.uid)) {
        changed |= ImGui::InputInt("x", &item.x, grid_, grid_ * 8);
        changed |= ImGui::InputInt("y", &item.y, grid_, grid_ * 8);
        changed |= ImGui::InputInt("z", &item.z, 8, 32);
        item.x = std::clamp(item.x, 0, 0xFFFF);
        item.y = std::clamp(item.y, 0, 0xFFFF);
        item.z = std::clamp(item.z, 0, 255);
    }
    changed |= input_u16(tr("Qualité", "Quality"), item.quality);
    std::uint8_t qlo = item.quality_low(), qhi = item.quality_high();
    ImGui::SetNextItemWidth(100);
    if (input_u8(tr("basse", "low"), qlo)) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (input_u8(tr("haute", "high"), qhi)) changed = true;
    item.quality = static_cast<std::uint16_t>(qhi << 8 | qlo);
    changed |= input_u8(tr("N° PNJ", "NPC number"), item.npc_number);
    changed |= input_u8(tr("N° carte", "Map number"), item.map_number);
    changed |= input_u16(tr("Drapeaux", "Flags"), item.flags);
    ImGui::PopItemWidth();
    {
        bool invisible = item.flags & kFlagInvisible, mirrored = item.flags & kFlagMirrored;
        if (ImGui::Checkbox(tr("invisible", "invisible"), &invisible)) {
            item.flags ^= kFlagInvisible;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(tr("miroir", "mirrored"), &mirrored)) {
            item.flags ^= kFlagMirrored;
            changed = true;
        }
    }
    if (changed) note_change(before);

    const std::uint32_t uid = item.uid;
    if (ImGui::CollapsingHeader(tr("Rôle dans le niveau", "Role in the level"), ImGuiTreeNodeFlags_DefaultOpen)) {
        semantic_properties(item, fam);
        if (!level_.find(uid)) return;  // exploded glob
    }
    if (ImGui::CollapsingHeader(tr("Script (usecode)", "Script (usecode)"), ImGuiTreeNodeFlags_DefaultOpen)) {
        script_properties(item, fam);
    }
    if ((fam == kFamContainer || fam == kFamMonsterEgg || !item.contents.empty()) &&
        ImGui::CollapsingHeader(tr("Contenu", "Contents"))) {
        contents_editor(item);
    }
}

void EditorApp::semantic_properties(LevelItem& item, std::uint8_t fam) {
    const LevelItem before = *level_.top_of(item.uid);
    bool changed = false;
    int goto_map = -1;
    std::uint8_t goto_id = 0;
    ImGui::PushItemWidth(150);
    if (fam == kFamGlobEgg) {
        int g = item.quality;
        if (ImGui::InputInt(tr("Glob", "Glob"), &g)) {
            item.quality = static_cast<std::uint16_t>(std::clamp(g, 0, 0xFFFF));
            changed = true;
        }
        const Glob& glob = glob_cached(item.quality);
        ImGui::Text("%zu %s", glob.pieces.size(), tr("pièces", "pieces"));
        int w, h;
        if (SDL_Texture* t = glob_texture(item.quality, &w, &h)) {
            const float s = std::min(1.0f, 300.0f / std::max(w, h));
            ImGui::Image(tex_id(t), ImVec2(w * s, h * s));
        }
        ImGui::TextWrapped("%s", tr("Le glob remplit la case de 512 x 512 où se trouve l'œuf ; seule cette case compte, "
                                    "pas la position exacte de l'œuf.",
                                    "The glob fills the 512 x 512 cell the egg is in; only that cell matters, not the "
                                    "egg's exact position."));
        if (ImGui::Button(tr("Éditer ce glob", "Edit this glob"))) {
            glob_edit_index_ = item.quality;
            glob_edit_ = project_->glob(item.quality);
            glob_edit_dirty_ = false;
            glob_window_open_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Dégrouper", "Explode"))) {
            ImGui::PopItemWidth();
            explode_glob(item.uid);
            return;
        }
    } else if (fam == kFamTriggerEgg) {
        std::uint8_t type = item.quality_low();
        const std::string current = hex(type, 2) + " " + class_name(static_cast<std::uint16_t>(kClassEggs + type));
        if (ImGui::BeginCombo(tr("Type (classe)", "Type (class)"), current.c_str())) {
            for (std::uint16_t t = 0; t < 0x100; ++t) {
                const std::string n = class_name(static_cast<std::uint16_t>(kClassEggs + t));
                if (n.empty()) continue;
                if (ImGui::Selectable((hex(t, 2) + " " + n).c_str(), t == type)) {
                    item.quality = static_cast<std::uint16_t>((item.quality & 0xFF00) | t);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        std::uint8_t qhi = item.quality_high();
        if (input_u8(tr("Paramètre (qualité haute)", "Parameter (quality high)"), qhi)) {
            item.quality = static_cast<std::uint16_t>(qhi << 8 | item.quality_low());
            changed = true;
        }
        changed |= input_u8(tr("Id d'œuf (n° carte)", "Egg id (map number)"), item.map_number);
        help_marker(tr("Egg::getEggId lit l'octet « carte » : les scripts de mission retrouvent leurs œufs par cet id.",
                       "Egg::getEggId reads the map byte: mission scripts find their eggs by this id."));
    } else if (fam == kFamTeleportEgg) {
        int kind = item.frame == 1 ? 1 : 0;
        if (ImGui::RadioButton(tr("Départ (zone)", "Departure (area)"), &kind, 0)) item.frame = 0, changed = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(tr("Arrivée", "Arrival"), &kind, 1)) item.frame = 1, changed = true;
        std::uint8_t id = item.quality_low();
        if (item.frame == 1) {
            if (input_u8(tr("Id d'arrivée", "Arrival id"), id)) changed = true;
            if (id == kStartEgg) ImGui::TextDisabled("%s", tr("Départ de mission (0x1E).", "Mission start (0x1E)."));
            const auto ids = arrivals_of(map_);
            if (auto n = ids.find(id); n != ids.end() && n->second > 1) {
                ImGui::TextColored(kTextWarning, "%s", tr("Un autre œuf d'arrivée a cet id.", "Another arrival egg has this id."));
            }
            int leading = 0;
            for (const LevelItem& i : level_.items) {
                leading += is_departure(i, family_of(i.shape)) && i.map_number == map_ && i.quality_low() == id;
            }
            ImGui::TextDisabled("%d %s", leading, tr("départ(s) de cette carte y mène(nt).", "departure(s) of this map lead here."));
        } else {
            // The map: any map of FIXED.DAT (the game takes the number as it is).
            const std::string to_map = std::string(tr("Carte ", "Map ")) + std::to_string(item.map_number) +
                                       (item.map_number == map_ ? tr(" (celle-ci)", " (this one)") : "");
            if (ImGui::BeginCombo(tr("Vers la carte", "To map"), to_map.c_str())) {
                for (std::size_t m : project_->used_maps()) {
                    if (m == 0 || m > 255) continue;
                    const std::string label = std::string(tr("Carte ", "Map ")) + std::to_string(m) +
                                              (static_cast<int>(m) == map_ ? tr(" (celle-ci)", " (this one)") : "");
                    if (ImGui::Selectable(label.c_str(), m == item.map_number)) {
                        item.map_number = static_cast<std::uint8_t>(m);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            const auto ids = arrivals_of(item.map_number);
            auto id_label = [](std::uint8_t v) {
                return hex(v, 2) + (v == kStartEgg ? tr(" (départ de mission)", " (mission start)") : "");
            };
            if (ImGui::BeginCombo(tr("Vers l'arrivée", "To arrival"), id_label(id).c_str())) {
                for (const auto& [v, n] : ids) {
                    if (ImGui::Selectable(id_label(v).c_str(), v == id)) {
                        id = v;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (input_u8(tr("id", "id"), id)) changed = true;
            bool wait = item.quality_high() != 0;
            if (ImGui::Checkbox(tr("Attendre (fondu)", "Wait (fade)"), &wait)) changed = true;
            item.quality = static_cast<std::uint16_t>((wait ? 0x100 : 0) | id);
            if (ids.count(id)) {
                if (ImGui::Button(item.map_number == map_ ? tr("Voir l'arrivée", "Show the arrival")
                                                          : tr("Ouvrir la carte de l'arrivée", "Open the arrival's map"))) {
                    goto_map = item.map_number;
                    goto_id = id;
                }
            } else {
                ImGui::TextColored(kTextError, "%s",
                                   tr("Aucune arrivée de cet id sur cette carte : le téléporteur ne fera rien.",
                                      "No arrival of this id on that map: the teleport will do nothing."));
            }
        }
        if (item.frame == 1) item.quality = static_cast<std::uint16_t>((item.quality & 0xFF00) | id);
    } else if (fam == kFamMonsterEgg) {
        MonsterEgg m = read_monster_egg(item);
        const std::uint16_t shape = npc_types_->word(m.npc_type, 0x3E);
        const std::string current = std::to_string(m.npc_type) + " " + hex(shape, 3) + " " + shape_name(shape);
        if (ImGui::BeginCombo(tr("Type de PNJ", "NPC type"), current.c_str())) {
            for (int t = 0; t < dtable::NpcTypes::kRecords; ++t) {
                const std::uint16_t s = npc_types_->word(t, 0x3E);
                if (!s) continue;
                if (ImGui::Selectable((std::to_string(t) + " " + hex(s, 3) + " " + shape_name(s)).c_str(),
                                      t == m.npc_type)) {
                    m.npc_type = t;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (shape) shape_thumbnail(shape, 0, 96);
        changed |= ImGui::SliderInt(tr("Direction", "Direction"), &m.direction, 0, 15);
        changed |= ImGui::SliderInt(tr("Difficulté minimale", "Lowest difficulty"), &m.min_difficulty, 1, 4);
        changed |= ImGui::Checkbox(tr("Autre arme", "Other weapon"), &m.other_weapon);
        changed |= ImGui::SliderInt(tr("Fiche +0x12", "Record +0x12"), &m.record_12, 0, 15);
        const char* labels[3] = {tr("Activité au départ", "Starting activity"),
                                 tr("Activité en voyant le joueur", "Activity on seeing the player"),
                                 tr("Activité quand on tire", "Activity when shot at")};
        for (int a = 0; a < 3; ++a) {
            ImGui::PushID(a);
            if (ImGui::BeginCombo(labels[a], (std::to_string(m.activity[a]) + " " + activity_name(m.activity[a])).c_str())) {
                for (int v : {0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12}) {
                    if (ImGui::Selectable((std::to_string(v) + " " + activity_name(v)).c_str(), v == m.activity[a])) {
                        m.activity[a] = v;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (changed) write_monster_egg(item, m);
    } else if (item.shape == kShapeMonster && item.frame <= 1 && class_name(kShapeMonster) == "MONSTER") {
        ImGui::PopItemWidth();
        monster_pair_properties(item);
        return;
    } else if (item.shape == kShapeMarker) {
        MarkerTarget t = read_marker(item);
        ImGui::TextWrapped("%s", tr("Marqueur : objet que donne le coffre de même qualité basse (à moins de 384 unités), "
                                    "ou objet coupé par un TRIGGER.",
                                    "Marker: item given by the chest of the same quality low (within 384 units), or "
                                    "item switched off by a TRIGGER."));
        if (input_u16(tr("Forme donnée", "Shape given"), t.shape)) changed = true;
        int f = t.frame;
        if (ImGui::SliderInt(tr("Image donnée", "Frame given"), &f, 0, 15)) t.frame = static_cast<std::uint8_t>(f), changed = true;
        shape_thumbnail(t.shape, t.frame, 64);
        if (changed) write_marker(item, t);
    } else {
        ImGui::TextWrapped("%s", tr("Les scripts de cet objet lisent sa qualité et ses octets PNJ / carte. La qualité "
                                    "basse relie souvent les objets entre eux (interrupteur -> murs d'énergie, vanne -> "
                                    "vapeur, coffre -> marqueurs) : les objets liés sont encadrés en cyan.",
                                    "This item's scripts read its quality and NPC / map bytes. The quality low often "
                                    "links items (switch -> force fields, valve -> steam, chest -> markers): linked "
                                    "items are framed in cyan."));
        if (ImGui::Button(tr("Lier en cliquant (L)", "Link by clicking (L)"))) start_link_mode(item.uid);
        help_marker(tr("Cliquez ensuite dans la vue les objets sur lesquels celui-ci agit (portes, champs de force, "
                       "vapeur, PNJ...) : ils prennent sa qualité basse (une libre s'il n'en a pas). "
                       "Un objet déjà lié se délie ; Échap pour terminer.",
                       "Then click in the view the items this one acts on (doors, force fields, steam, NPCs...): "
                       "they take its quality low (a free one if it has none). A linked item unlinks; "
                       "Esc when done."));
        if (item.quality_low() != 0 && links_by_quality(item)) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Sélectionner les objets liés", "Select the linked items"))) {
                select_same_quality_low(item.quality_low());
            }
        }
        if (acts_through_trigger(item)) {
            // The shape it acts on, among its linked items.
            const std::uint16_t target = trigger_target(item);
            const LevelInfo info = level_info();
            std::set<std::uint16_t> shapes;
            int of_target = 0;
            for (const LevelItem& i : level_.items) {
                if (i.uid == item.uid || item.quality_low() == 0 || i.quality_low() != item.quality_low() ||
                    !is_linked_item(i, info)) {
                    continue;
                }
                shapes.insert(i.shape);
                of_target += i.shape == target;
            }
            ImGui::SeparatorText(tr("Agit sur (TRIGGER)", "Acts on (TRIGGER)"));
            const std::string current = hex(target, 3) + " " + shape_name(target);
            if (ImGui::BeginCombo(tr("Forme visée", "Target shape"), current.c_str())) {
                for (std::uint16_t sh : shapes) {
                    if (ImGui::Selectable((hex(sh, 3) + " " + shape_name(sh)).c_str(), sh == target)) {
                        set_trigger_target(item, sh);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            help_marker(tr("Ses octets carte (bits 5-7) et PNJ nomment la forme des objets liés sur lesquels il agit "
                           "(TRIGGER) ; Lier en cliquant la règle sur l'objet cliqué.",
                           "Its map (bits 5-7) and NPC bytes name the shape of the linked items it acts on "
                           "(TRIGGER); Link by clicking sets it to the clicked item's."));
            // What it does to them (map number bits 0-4, quality high).
            const std::uint8_t flags = static_cast<std::uint8_t>(item.map_number & 0x1F);
            auto set_flags = [&](std::uint8_t f) {
                item.map_number = static_cast<std::uint8_t>((item.map_number & 0xE0) | (f & 0x1F));
                changed = true;
            };
            static const char* const kActionsFr[] = {"fixe leur qualité haute", "fixe leur qualité basse",
                                                     "les met en marche (equip)", "les éteint / rallume"};
            static const char* const kActionsEn[] = {"sets their quality high", "sets their quality low",
                                                     "switches them on (equip)", "turns them off / on"};
            if (flags & 4) {
                int action = flags & 3;
                const char* names[4];
                for (int k = 0; k < 4; ++k) names[k] = tr(kActionsFr[k], kActionsEn[k]);
                ImGui::SetNextItemWidth(200);
                if (ImGui::Combo(tr("Action", "Action"), &action, names, 4)) {
                    set_flags(static_cast<std::uint8_t>((flags & ~3) | action));
                }
            } else {
                ImGui::TextDisabled("%s %s", tr("Action : autre (bit 2 de carte absent), réglage", "Action: other (map bit 2 clear), setting"),
                                    hex(flags & 3, 1).c_str());
                if (ImGui::SmallButton(tr("Passer aux actions connues", "Use the known actions"))) {
                    set_flags(static_cast<std::uint8_t>(flags | 4));
                }
            }
            int state = (flags >> 3) & 1;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(tr("Agit à l'état", "Acts at state"), &state,
                             tr("0 (premier usage)\0" "1\0", "0 (first use)\0" "1\0"))) {
                set_flags(static_cast<std::uint8_t>((flags & ~8) | (state << 3)));
            }
            bool once = flags & 0x10;
            if (ImGui::Checkbox(tr("Une seule fois (le lien est ensuite effacé)", "Once (the link is then cleared)"), &once)) {
                set_flags(static_cast<std::uint8_t>(once ? flags | 0x10 : flags & ~0x10));
            }
            std::uint8_t argument = item.quality_high();
            ImGui::SetNextItemWidth(80);
            if (input_u8(tr("Argument (qualité haute)", "Argument (quality high)"), argument)) {
                item.quality = static_cast<std::uint16_t>(argument << 8 | item.quality_low());
                changed = true;
            }
            if (target == kShapeMonster && (flags & 7) == 6) {
                ImGui::TextDisabled("%s", tr("Argument = mode du MONSTER : 0 une fois, 1 à chaque fois, 2 / 3 en se "
                                             "téléportant.",
                                             "Argument = the MONSTER's mode: 0 once, 1 each time, 2 / 3 teleported "
                                             "in."));
            }
            if (const TriggerSetting* found = find_trigger_setting(trigger_settings_, item.shape, target)) {
                const TriggerSetting& ts = *found;
                ImGui::TextDisabled("%s %s %s %s (%d / %d).", tr("Dans les cartes d'origine, sur cette forme :",
                                                                 "In the original maps, on this shape:"),
                                    hex(ts.quality_high, 2).c_str(), tr("et", "and"), hex(ts.flags, 2).c_str(), ts.uses,
                                    ts.of);
                if ((ts.quality_high != item.quality_high() || ts.flags != flags) &&
                    ImGui::SmallButton(tr("Régler comme dans les cartes d'origine", "Set as in the original maps"))) {
                    apply_trigger_setting(item, ts);
                    changed = true;
                }
            }
            if (of_target == 0) {
                ImGui::TextColored(kTextWarning, "%s", tr("Aucun objet lié de cette forme : il n'agira sur rien.",
                                                          "No linked item of that shape: it will act on nothing."));
            } else {
                ImGui::TextDisabled("%d %s", of_target, tr("objet(s) lié(s) de cette forme.", "linked item(s) of that shape."));
            }
        }
    }
    if (hatches(fam, item)) {
        int zx = item.npc_number >> 4, zy = item.npc_number & 0xF;
        ImGui::SeparatorText(tr("Zone de déclenchement", "Trigger area"));
        bool zc = ImGui::SliderInt(tr("demi-largeur x (x32)", "half width x (x32)"), &zx, 0, 15);
        zc |= ImGui::SliderInt(tr("demi-largeur y (x32)", "half width y (x32)"), &zy, 0, 15);
        if (zc) {
            set_hatch_zone(item, zx, zy);
            changed = true;
        }
        ImGui::TextDisabled("%s", tr("Le joueur doit aussi être à moins de 0x30 en z.", "The player must also be within 0x30 in z."));
    }
    ImGui::PopItemWidth();
    if (changed) note_change(before);
    if (goto_map >= 0) go_to_arrival(goto_map, goto_id);
}

bool EditorApp::npc_type_combo(const char* label, int& type) {
    bool changed = false;
    const auto valid = [&](int t) { return t >= 0 && t < dtable::NpcTypes::kRecords; };
    const std::uint16_t shape = valid(type) ? npc_types_->word(type, 0x3E) : 0;
    const std::string current = std::to_string(type) + " " + hex(shape, 3) + " " + shape_name(shape);
    if (ImGui::BeginCombo(label, current.c_str())) {
        for (int t = 0; t < dtable::NpcTypes::kRecords; ++t) {
            const std::uint16_t s = npc_types_->word(t, 0x3E);
            if (!s) continue;
            char row[96];
            std::snprintf(row, sizeof row, "%2d  %s %s  (%u %s)", t, hex(s, 3).c_str(), shape_name(s).c_str(),
                          npc_types_->max_hp(t), tr("PV", "HP"));
            if (ImGui::Selectable(row, t == type)) {
                type = t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool EditorApp::activity_combo(const char* label, int& activity) {
    bool changed = false;
    if (ImGui::BeginCombo(label, (std::to_string(activity) + " " + activity_name(activity)).c_str())) {
        for (int v : {0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13}) {
            if (v == 13) continue;  // No Regret's
            if (ImGui::Selectable((std::to_string(v) + " " + activity_name(v)).c_str(), v == activity)) {
                activity = v;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void EditorApp::monster_pair_properties(LevelItem& item) {
    ImGui::TextWrapped("%s", item.frame == 0
                                 ? tr("Place d'un PNJ : le PNJ apparaît ici, du type que donne l'objet MONSTER image 1 "
                                      "de même lien à moins de 64 unités.",
                                      "An NPC's place: the NPC appears here, of the kind the MONSTER frame 1 object of "
                                      "the same link within 64 units gives.")
                                 : tr("Type d'un PNJ : il donne le type et les activités du PNJ qui apparaît sur la "
                                      "place (MONSTER image 0 de même lien, à moins de 64 unités).",
                                      "An NPC's kind: it gives the type and activities of the NPC that appears on its "
                                      "place (MONSTER frame 0 of the same link, within 64 units)."));
    const LevelItem* found = monster_partner(level_, item, level_info());
    LevelItem* partner = found ? level_.find(found->uid) : nullptr;
    if (!partner) {
        ImGui::TextColored(kTextWarning, "%s",
                           item.frame == 1 ? tr("Aucune place : il ne sert à rien.", "No place: it is of no use.")
                                           : tr("Aucun type : aucun PNJ n'apparaîtra.", "No kind: no NPC will appear."));
        if (ImGui::Button(tr("Créer l'autre objet de la paire ici", "Make the pair's other object here"))) {
            // Added once the panel is drawn: `item` would move.
            LevelItem other = item;
            assign_uids(other);
            other.frame = item.frame == 1 ? 0 : 1;
            deferred_items_.push_back(std::move(other));
        }
        return;
    }
    LevelItem& place = item.frame == 0 ? item : *partner;
    LevelItem& kind = item.frame == 1 ? item : *partner;
    MonsterPair m = read_monster_pair(place, kind);
    bool changed = false;
    ImGui::PushItemWidth(170);
    changed |= npc_type_combo(tr("Type de PNJ", "NPC type"), m.npc_type);
    if (m.npc_type < dtable::NpcTypes::kRecords) {
        if (const std::uint16_t s = npc_types_->word(m.npc_type, 0x3E)) shape_thumbnail(s, 0, 96);
    }
    changed |= ImGui::SliderInt(tr("Direction", "Direction"), &m.direction, 0, 7);
    changed |= ImGui::SliderInt(tr("Difficulté minimale", "Lowest difficulty"), &m.min_difficulty, 1, 4);
    changed |= ImGui::Checkbox(tr("Autre arme", "Other weapon"), &m.other_weapon);
    changed |= ImGui::Checkbox(tr("Apparaît quand le joueur arrive dans la zone", "Appears when the player comes near"),
                               &m.on_enter);
    help_marker(tr("Sinon, seulement quand un script le déclenche (renforts, PNJ qui sortent d'une porte...).",
                   "Otherwise only when a script triggers it (reinforcements, NPCs coming out of a door...)."));
    // What fires a place: switches and other TRIGGER users whose target is
    // MONSTER, and No Regret's MHATCHER egg (usecode 0x905), which fires
    // every MONSTER within 3200 units whose quality low is its id.
    const std::uint8_t link = place.quality_low();
    int fired = 0;
    std::string by;
    for (const LevelItem& i : level_.items) {
        if (i.quality_low() != link || !acts_through_trigger(i) || trigger_target(i) != kShapeMonster) continue;
        ++fired;
        if (by.size() < 60) by += (by.empty() ? "" : ", ") + class_name(i.shape);
    }
    int hatcher = -1;
    for (int t = 0; t < 0x100 && hatcher < 0; ++t) {
        if (class_name(static_cast<std::uint16_t>(kClassEggs + t)) == "MHATCHER") hatcher = t;
    }
    int eggs = 0;
    for (const LevelItem& i : level_.items) {
        eggs += hatcher >= 0 && family_of(i.shape) == kFamTriggerEgg && i.quality_low() == hatcher &&
                i.map_number == link && std::max(std::abs(i.x - place.x), std::abs(i.y - place.y)) <= 3200;
    }
    if (fired) ImGui::TextDisabled("%s %s", tr("Déclenché par :", "Fired by:"), by.c_str());
    if (eggs) {
        ImGui::TextDisabled("%d %s %s.", eggs, tr("œuf(s) MHATCHER le déclenche(nt), id", "MHATCHER egg(s) trigger it, id"),
                            hex(link, 2).c_str());
    }
    if (!m.on_enter && !fired && !eggs) {
        ImGui::TextColored(kTextWarning, "%s",
                           tr("Rien ne le déclenche : liez-lui un interrupteur (sélectionnez l'interrupteur, L, puis "
                              "cliquez la place).",
                              "Nothing fires it: link a switch to it (select the switch, L, then click the place)."));
        if (hatcher >= 0 && ImGui::Button(tr("Ajouter un œuf MHATCHER ici", "Add a MHATCHER egg here"))) {
            // Its zone: where the player sets the NPC off; move it there.
            LevelItem egg;
            egg.uid = new_uid();
            egg.shape = kShapeTriggerEgg;
            egg.x = place.x;
            egg.y = place.y;
            egg.z = place.z;
            egg.quality = static_cast<std::uint16_t>(hatcher);
            egg.map_number = link;
            set_hatch_zone(egg, 4, 4);
            deferred_items_.push_back(egg);
            message(tr("Œuf MHATCHER ajouté : déplacez-le où le joueur doit déclencher le PNJ.",
                       "MHATCHER egg added: move it where the player should set the NPC off."));
        }
    }
    changed |= activity_combo(tr("Activité au départ", "Starting activity"), m.activity[0]);
    changed |= activity_combo(tr("Activité en voyant le joueur", "Activity on seeing the player"), m.activity[1]);
    changed |= activity_combo(tr("Activité quand on tire", "Activity when shot at"), m.activity[2]);
    changed |= ImGui::SliderInt(tr("Fiche +0x12", "Record +0x12"), &m.record_12, 0, 15);
    ImGui::PopItemWidth();
    if (changed) {
        if (!edit_open_) {
            push_undo();
            edit_open_ = true;
        }
        write_monster_pair(place, kind, m);
        level_dirty_ = world_dirty_ = true;
    }
    if (ImGui::Button(tr("Sélectionner la paire", "Select the pair"))) {
        selection_ = {place.uid, kind.uid};
        primary_ = item.uid;
    }
}

void EditorApp::script_properties(const LevelItem& item, std::uint8_t fam) {
    const std::uint16_t cid = usecode_class_of(item, fam);
    if (!usecode_->has_class(cid)) {
        ImGui::TextDisabled("%s %s", tr("Aucune classe", "No class"), hex(cid, 3).c_str());
        if (fam == kFamMonsterEgg) {
            ImGui::TextWrapped("%s", tr("Le PNJ créé reçoit les événements de la classe de sa forme.",
                                        "The NPC made receives the events of its shape's class."));
        }
        return;
    }
    ImGui::Text("%s %s", hex(cid, 3).c_str(), usecode_->class_name(cid).c_str());
    for (std::uint32_t e = 0; e < usecode_->event_count(cid) && e < 0x20; ++e) {
        if (!usecode_->event_offset(cid, e)) continue;
        ImGui::BulletText("%02X %s", e, event_name(e));
        if (*event_help(e)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", event_help(e));
        }
    }
    if (item.quality_low() != 0 && links_by_quality(item)) {
        std::size_t linked = 0;
        for (const LevelItem& i : level_.items) {
            const std::uint8_t f = family_of(i.shape);
            if (i.uid != item.uid && i.quality_low() == item.quality_low() && f != kFamGlobEgg &&
                (usecode_->has_class(usecode_class_of(i, f)) || i.shape == kShapeMarker)) {
                ++linked;
            }
        }
        ImGui::Text("%s %s : %zu %s", tr("Qualité basse", "Quality low"), hex(item.quality_low(), 2).c_str(), linked,
                    tr("autre(s) objet(s) à script", "other scripted item(s)"));
    }
}

void EditorApp::contents_editor(LevelItem& item) {
    const LevelItem before = *level_.top_of(item.uid);
    bool changed = false;
    int remove = -1;
    for (std::size_t k = 0; k < item.contents.size(); ++k) {
        LevelItem& c = item.contents[k];
        ImGui::PushID(static_cast<int>(k));
        ImGui::SetNextItemWidth(70);
        changed |= input_u16(tr("forme", "shape"), c.shape);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        changed |= input_u16("q", c.quality);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40);
        changed |= input_u8(tr("pnj", "npc"), c.npc_number);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
        ImGui::PopID();
    }
    if (remove >= 0) {
        item.contents.erase(item.contents.begin() + remove);
        changed = true;
    }
    if (brush_.kind == BrushKind::Shape &&
        ImGui::Button(tr("Ajouter la forme du pinceau", "Add the brush's shape"))) {
        LevelItem c;
        c.uid = new_uid();
        c.shape = brush_.shape;
        c.frame = brush_.frame;
        item.contents.push_back(c);
        changed = true;
    }
    if (changed) {
        set_depths(item, level_.top_index(item.uid) ? 0 : 1);
        note_change(before);
    }
}

void EditorApp::glob_window() {
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Éditeur de globs", "Glob editor"), &glob_window_open_)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt(tr("Glob n°", "Glob #"), &glob_edit_index_)) {
        glob_edit_index_ = std::clamp(glob_edit_index_, 1, 0xFFFF);
        glob_edit_ = project_->glob(static_cast<std::size_t>(glob_edit_index_));
        glob_edit_dirty_ = false;
    }
    std::size_t uses = 0;
    for (const LevelItem& i : level_.items) {
        if (family_of(i.shape) == kFamGlobEgg && i.quality == glob_edit_index_) ++uses;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s", uses, tr("œuf(s) sur cette carte", "egg(s) on this map"));
    int w, h;
    if (SDL_Texture* t = glob_texture(static_cast<std::size_t>(glob_edit_index_), &w, &h)) {
        const float s = std::min(1.0f, 240.0f / std::max(w, h));
        ImGui::Image(tex_id(t), ImVec2(w * s, h * s));
    }
    ImGui::TextWrapped("%s", tr("x, y : demi-unités dans la case (0..255 = 0..511 unités) ; z : au-dessus de l'œuf. "
                                "Un glob est partagé par tous ses œufs, sur toutes les cartes.",
                                "x, y: half units in the cell (0..255 = 0..511 units); z: above the egg. A glob is "
                                "shared by all its eggs, on every map."));
    int remove = -1;
    if (ImGui::BeginTable("pieces", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders,
                          ImVec2(0, 220))) {
        ImGui::TableSetupColumn("x");
        ImGui::TableSetupColumn("y");
        ImGui::TableSetupColumn("z");
        ImGui::TableSetupColumn(tr("forme", "shape"));
        ImGui::TableSetupColumn(tr("image", "frame"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (std::size_t k = 0; k < glob_edit_.pieces.size(); ++k) {
            GlobPiece& p = glob_edit_.pieces[k];
            ImGui::PushID(static_cast<int>(k));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            glob_edit_dirty_ |= input_u8("##x", p.x, false);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            glob_edit_dirty_ |= input_u8("##y", p.y, false);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            glob_edit_dirty_ |= input_u8("##z", p.z, false);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            glob_edit_dirty_ |= input_u16("##s", p.shape);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            glob_edit_dirty_ |= input_u8("##f", p.frame, false);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(k);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        glob_edit_.pieces.erase(glob_edit_.pieces.begin() + remove);
        glob_edit_dirty_ = true;
    }
    if (brush_.kind == BrushKind::Shape && ImGui::Button(tr("Ajouter la forme du pinceau", "Add the brush's shape"))) {
        glob_edit_.pieces.push_back({128, 128, 0, brush_.shape, brush_.frame});
        glob_edit_dirty_ = true;
    }
    auto refresh = [&](std::size_t index) {
        glob_cache_.erase(index);
        if (auto it = glob_textures_.find(index); it != glob_textures_.end()) {
            if (it->second.first) SDL_DestroyTexture(it->second.first);
            glob_textures_.erase(it);
        }
        world_dirty_ = true;
    };
    ImGui::BeginDisabled(!glob_edit_dirty_);
    if (ImGui::Button(tr("Appliquer à ce glob", "Apply to this glob"))) {
        project_->store_glob(static_cast<std::size_t>(glob_edit_index_), glob_edit_);
        refresh(static_cast<std::size_t>(glob_edit_index_));
        glob_edit_dirty_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Enregistrer comme nouveau glob", "Save as a new glob"))) {
        const std::size_t index = project_->add_glob(glob_edit_);
        refresh(index);
        glob_edit_index_ = static_cast<int>(index);
        glob_edit_dirty_ = false;
        if (LevelItem* p = level_.find(primary_); p && family_of(p->shape) == kFamGlobEgg) {
            const LevelItem before = *p;
            p->quality = static_cast<std::uint16_t>(index);
            note_change(before);
        }
        message(std::string(tr("Nouveau glob ", "New glob ")) + std::to_string(index));
    }
    if (glob_edit_dirty_) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", tr("Modifié, non appliqué", "Changed, not applied"));
    ImGui::End();
}

void EditorApp::modal_popups() {
    if (want_open_map_) {
        ImGui::OpenPopup("open_map");
        want_open_map_ = false;
    }
    if (ImGui::BeginPopupModal("open_map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("Cartes de FIXED.DAT :", "Maps of FIXED.DAT:"));
        ImGui::BeginChild("maps", ImVec2(260, 300), ImGuiChildFlags_Borders);
        for (std::size_t m : project_->used_maps()) {
            const std::string label = std::string(tr("Carte ", "Map ")) + std::to_string(m);
            if (ImGui::Selectable(label.c_str(), open_map_choice_ == static_cast<int>(m),
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                open_map_choice_ = static_cast<int>(m);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    open_map(open_map_choice_);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
        if (ImGui::Button(tr("Ouvrir", "Open"))) {
            open_map(open_map_choice_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Annuler", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (want_generate_) {
        ImGui::OpenPopup("Crusader: No Limit###generate_level");
        want_generate_ = false;
    }
    if (ImGui::BeginPopupModal("Crusader: No Limit###generate_level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!generator_data_) generator_data_ = std::make_unique<gen::GameData>(gen::GameData::load(project_->data()));
        ImGui::TextUnformatted(tr("Un niveau procédural sur une carte libre : salles, couloirs, portes, verrous et\n"
                                  "leurs clés, ascenseurs entre deux étages, gardes, départ et téléporteur de sortie\n"
                                  "(qui ramène au départ).",
                                  "A procedural level on a free map: rooms, corridors, doors, locks and their keys,\n"
                                  "lifts between two storeys, guards, start and exit teleporter (back to the start)."));
        ImGui::TextDisabled("%s", tr("La graine est notée dans NOLIMIT.JSON du mod à l'enregistrement.",
                                     "The seed is recorded in the mod's NOLIMIT.JSON when saving."));
        ImGui::InputInt(tr("Graine", "Seed"), &generate_seed_);
        if (generate_seed_ < 0) generate_seed_ = 0;
        const auto& biomes = generator_data_->biomes;
        const std::string current = generate_biome_ < 0 || generate_biome_ >= static_cast<int>(biomes.size())
                                        ? std::string(tr("au hasard", "random"))
                                        : biomes[static_cast<std::size_t>(generate_biome_)].name();
        if (ImGui::BeginCombo(tr("Biome", "Biome"), current.c_str())) {
            if (ImGui::Selectable(tr("au hasard", "random"), generate_biome_ < 0)) generate_biome_ = -1;
            for (std::size_t i = 0; i < biomes.size(); ++i) {
                if (ImGui::Selectable(biomes[i].name().c_str(), generate_biome_ == static_cast<int>(i))) {
                    generate_biome_ = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SliderInt(tr("Salles (0 : au hasard)", "Rooms (0: random)"), &generate_rooms_, 0, 16);
        ImGui::SliderInt(tr("Verrous (-1 : au hasard)", "Locks (-1: random)"), &generate_locks_, -1, 4);
        ImGui::SliderInt(tr("Étages (-1 : au hasard)", "Storeys (-1: random)"), &generate_storeys_, -1, 2);
        ImGui::Checkbox(tr("Gardes", "Guards"), &generate_guards_);
        if (ImGui::Button(tr("Générer", "Generate"))) {
            generate_level();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Annuler", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (want_save_prefab_) {
        ImGui::OpenPopup("save_prefab");
        want_save_prefab_ = false;
    }
    if (ImGui::BeginPopupModal("save_prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%zu %s", selection_.size(), tr("objet(s) sélectionné(s)", "item(s) selected"));
        ImGui::SetNextItemWidth(280);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##pfname", tr("nom du prefab", "prefab name"), &prefab_name_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button(tr("Enregistrer", "Save"))) {
            save_prefab(prefab_name_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Annuler", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (want_quit_confirm_) {
        ImGui::OpenPopup("quit");
        want_quit_confirm_ = false;
    }
    if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("Des modifications ne sont pas enregistrées.", "Some changes are not saved."));
        if (ImGui::Button(tr("Enregistrer et quitter", "Save and quit"))) {
            save();
            if (!project_->dirty() && !level_dirty_) quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Quitter sans enregistrer", "Quit without saving"))) {
            quit_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Annuler", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorApp::problems_window() {
    if (!problems_open_) return;
    ImGui::SetNextWindowSize(ImVec2(620, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Vérification du niveau", "Level check"), &problems_open_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", tr("Ce qui ne marcherait pas dans le jeu, mis à jour à chaque modification.",
                                 "What would not work in the game, updated at each change."));
    if (problems_.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", tr("Aucun problème trouvé.", "No problem found."));
    }
    std::uint32_t show = 0;
    for (std::size_t k = 0; k < problems_.size(); ++k) {
        const Problem& p = problems_[k];
        ImGui::PushID(static_cast<int>(k));
        const ImVec4 colour = p.severity == Severity::Error     ? kTextError
                              : p.severity == Severity::Warning ? kTextWarning
                                                                : kTextNote;
        const char* tag = p.severity == Severity::Error     ? tr("Erreur", "Error")
                          : p.severity == Severity::Warning ? tr("Attention", "Warning")
                                                            : tr("Note", "Note");
        if (p.uid) {
            if (ImGui::SmallButton(tr("Voir", "Show"))) show = p.uid;
        } else {
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize(tr("Voir", "Show")).x + 2 * ImGui::GetStyle().FramePadding.x, 1));
        }
        ImGui::SameLine();
        ImGui::TextColored(colour, "%s", tag);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", p.text.c_str());
        ImGui::PopID();
    }
    if (const LevelItem* i = show ? level_.find(show) : nullptr) {
        select_only(i->uid);
        center_on(i->x, i->y, i->z);
    }
    ImGui::End();
}

void EditorApp::status_bar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kStatusHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kStatusHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##status", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const auto [wx, wy] = to_world(m.x, m.y, work_z_);
    ImGui::Text("x %d  y %d  z %d   %s x%.1f   %zu %s", wx, wy, work_z_, tr("zoom", "zoom"), zoom_, world_items_,
                tr("objets affichés", "items shown"));
    ImGui::SameLine();
    const std::size_t records = level_.record_count();
    // The busiest original map has 7 855 records (map 27); glob pieces of
    // the fast area need room in the table too.
    if (records > kItemTableSlots) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "  %s (%zu / %zu)",
                           tr("trop d'objets pour la table du jeu", "too many items for the game's table"), records,
                           kItemTableSlots);
        ImGui::SameLine();
    } else if (records > busiest_records_) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "  %s (%zu / %zu)",
                           tr("plus chargée que les cartes d'origine", "busier than the original maps"), records,
                           kItemTableSlots);
        ImGui::SameLine();
    }
    if (!problems_.empty()) {
        int errors = 0, warnings = 0;
        for (const Problem& p : problems_) {
            errors += p.severity == Severity::Error;
            warnings += p.severity == Severity::Warning;
        }
        ImGui::SameLine();
        const std::string label = std::to_string(errors) + tr(" erreur(s), ", " error(s), ") + std::to_string(warnings) +
                                  tr(" avertissement(s)", " warning(s)") + "##problems";
        ImGui::PushStyleColor(ImGuiCol_Text, errors ? kTextError : warnings ? kTextWarning : kTextNote);
        if (ImGui::SmallButton(label.c_str())) problems_open_ = true;
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  %s%s", project_->mod().string().c_str(),
                        (level_dirty_ || project_->dirty()) ? tr("  (modifié)", "  (modified)") : "");
    if (!message_.empty() && ImGui::GetTime() - message_time_ < 8.0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0.5f, 1), "   %s", message_.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace crusader::editor
