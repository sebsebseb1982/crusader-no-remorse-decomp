#pragma once
// The level editor's window: an isometric view drawn by the game's own
// renderer, and Dear ImGui panels around it. See docs/EDITEUR.md.

#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "core/dtable.hpp"
#include "editor/item_semantics.hpp"
#include "editor/level.hpp"
#include "editor/project.hpp"
#include "engine/usecode/usecode_file.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace crusader::editor {

struct EditorOptions {
    std::filesystem::path data = "data";
    std::filesystem::path mod = "mods/mon-niveau";
    int map = 1;
    std::filesystem::path game_exe;  // crusader(.exe), next to the editor
    int select_shape = -1;           // selects the first item of this shape at start
    bool new_map = false;            // starts with Map > New map (then saves)
};

class EditorApp {
public:
    EditorApp(SDL_Renderer* renderer, EditorOptions options);
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // One frame of the UI (between ImGui::NewFrame and ImGui::Render).
    void frame();
    bool quit_requested() const { return quit_; }
    // Asks before quitting with unsaved changes; true when it may quit now.
    bool request_quit();

private:
    // --- Brush: what a click in the view creates.
    enum class BrushKind { None, Shape, Glob, TriggerEgg, TeleportArrival, TeleportDeparture, MonsterEgg };
    struct Brush {
        BrushKind kind = BrushKind::None;
        std::uint16_t shape = 0;
        std::uint8_t frame = 0;
        std::uint16_t value = 0;  // glob number, egg type, NPC type
    };

    // --- Data and level.
    void open_map(int map);
    void load_resources();
    void commit_map();  // level_ -> project_ (before a save or a map change)
    void save();
    void test_in_game();
    void new_empty_map();
    void copy_map_to_free_slot();

    // --- Undo.
    void push_undo();
    void note_change(const LevelItem& before);
    void undo();
    void redo();

    // --- World view.
    void rebuild_world();
    void render_view_if_needed();
    void center_on(std::int32_t x, std::int32_t y, std::int32_t z);
    void center_on_default();
    struct ScreenPoint {
        float x, y;
    };
    ScreenPoint to_screen(double ux, double uy, double uz) const;
    // Usecode point under a screen position, on the plane at height z.
    std::pair<std::int32_t, std::int32_t> to_world(float mx, float my, std::int32_t z) const;
    std::int32_t snap(std::int32_t v) const;
    void draw_overlays();
    void draw_box(const LevelItem& item, unsigned color, float thickness);
    void draw_zone(const LevelItem& egg, unsigned color);
    void handle_view_input();
    std::vector<std::uint32_t> pick_at(float mx, float my);

    // --- Edits.
    void place_brush(std::int32_t x, std::int32_t y);
    void delete_selection();
    void duplicate_selection();
    void move_selection(std::int32_t dx, std::int32_t dy, std::int32_t dz);
    void select_only(std::uint32_t uid);
    void select_same_quality_low(std::uint8_t q);
    void glob_from_selection();
    void explode_glob(std::uint32_t egg_uid);

    // --- Panels.
    void main_menu();
    void left_panel();
    void right_panel();
    void status_bar();
    void tab_objects();
    void tab_shapes();
    void tab_globs();
    void tab_eggs();
    void tab_classes();
    void properties(LevelItem& item);
    void semantic_properties(LevelItem& item, std::uint8_t family);
    void script_properties(const LevelItem& item, std::uint8_t family);
    void contents_editor(LevelItem& item);
    void glob_window();
    void modal_popups();

    // --- Helpers.
    std::uint8_t family_of(std::uint16_t shape) const;
    bool links_by_quality(const LevelItem& item) const;
    std::string shape_name(std::uint16_t shape) const;
    std::string class_name(std::uint16_t cid) const;
    int frame_count(std::uint16_t shape);
    SDL_Texture* frame_texture(std::uint16_t shape, std::uint8_t frame, int* w, int* h, int* xoff, int* yoff);
    SDL_Texture* glob_texture(std::size_t glob, int* w, int* h);
    const Glob& glob_cached(std::size_t index);
    void shape_thumbnail(std::uint16_t shape, std::uint8_t frame, float size);
    void message(std::string text);

    SDL_Renderer* renderer_;
    EditorOptions options_;
    std::unique_ptr<Project> project_;

    // Game resources.
    std::unique_ptr<FlexArchive> shapes_flex_;
    std::unique_ptr<ShapeCache> shapes_;
    std::vector<ShapeInfo> typeflags_;
    Palette palette_;
    std::optional<XformTable> xform_;
    std::unique_ptr<UsecodeFile> usecode_;
    std::optional<dtable::NpcTypes> npc_types_;

    // The map being edited.
    int map_ = 1;
    Level level_;
    bool level_dirty_ = false;  // changed since it was last stored in the project
    std::deque<Level> undo_, redo_;
    bool edit_open_ = false;  // a property widget is being edited (one undo step)
    std::set<std::uint32_t> selection_;
    std::uint32_t primary_ = 0;

    // The world drawn: rebuilt from level_ after each edit.
    std::unique_ptr<World> world_;
    std::unordered_map<ObjId, std::uint32_t> id_to_uid_;
    std::unordered_map<std::uint32_t, ObjId> uid_to_id_;
    std::vector<DrawItem> display_;
    bool world_dirty_ = true;
    bool view_dirty_ = true;
    std::size_t world_items_ = 0;

    // View: centre in projected pixels, zoom, cut height.
    double center_x_ = 0, center_y_ = 0;
    float zoom_ = 1.0f;
    float view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0;  // screen rect of the view
    int surface_w_ = 0, surface_h_ = 0;
    View view_corner_;
    SDL_Texture* view_texture_ = nullptr;
    int view_texture_w_ = 0, view_texture_h_ = 0;
    int z_cut_ = 255;
    int work_z_ = 0;
    int grid_ = 32;
    bool show_markers_ = true;
    bool show_invisible_ = true;
    bool show_globs_ = true;
    bool show_zones_ = true;
    bool show_links_ = true;
    bool pick_glob_pieces_ = false;

    // Mouse interaction.
    bool dragging_ = false;
    bool drag_moved_ = false;
    bool panning_ = false;
    float press_x_ = 0, press_y_ = 0;
    std::pair<std::int32_t, std::int32_t> drag_origin_{};
    std::map<std::uint32_t, Point3> drag_start_pos_;
    std::vector<std::uint32_t> last_pick_;
    std::size_t pick_cycle_ = 0;
    bool box_selecting_ = false;

    Brush brush_;

    // Palettes.
    std::string shape_filter_;
    int family_filter_ = -1;
    std::string object_filter_;
    bool objects_scripted_only_ = true;
    std::string class_filter_;

    // Glob window.
    bool glob_window_open_ = false;
    int glob_edit_index_ = 0;
    Glob glob_edit_;
    bool glob_edit_dirty_ = false;

    // Popups.
    bool want_open_map_ = false;
    bool want_quit_confirm_ = false;
    int open_map_choice_ = 1;
    std::string mod_path_text_;

    // Caches.
    std::unordered_map<std::uint32_t, SDL_Texture*> frame_textures_;
    struct FrameMeta {
        int w, h, xoff, yoff;
    };
    std::unordered_map<std::uint32_t, FrameMeta> frame_meta_;
    std::unordered_map<std::size_t, std::pair<SDL_Texture*, std::pair<int, int>>> glob_textures_;
    std::unordered_map<std::size_t, Glob> glob_cache_;

    std::string message_;
    double message_time_ = 0;
    bool quit_ = false;
};

}  // namespace crusader::editor
