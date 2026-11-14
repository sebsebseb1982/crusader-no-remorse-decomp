// Map viewer: renders a FIXED.DAT map with the isometric renderer.
//
// Usage: mapview [data_dir] [map]                      interactive
//        mapview [data_dir] [map] --png out.png [cx cy]   one frame to a PNG
//        mapview [data_dir] [map] --pick x y [cx cy]      items under a screen pixel
//        mapview [data_dir] --save N [...]                state of SAVEGAME/SAVEGAME.N, centred on the player
//        mapview [data_dir] [map] --find S                items of shape S, on the map or contained
//        mapview [data_dir] [map] --item N                item N and the items it contains
//   Right click: prints the items under the cursor (draw order, boxes).
//   Arrows / mouse drag  scroll        PageUp/PageDown  previous/next map
//   Home                 recentre      Esc              quit

#define SDL_MAIN_HANDLED
#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/png_writer.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"
#include "platform/sdl_presenter.hpp"

#include <SDL.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

using namespace crusader;

namespace {

constexpr int kScreenW = 640;
constexpr int kScreenH = 480;
constexpr int kScrollStep = 128;  // world units

// Centre of the densest 2048x2048 world-unit cell: maps are large and mostly
// empty, and a few far-away records skew any average.
Camera map_centre(const World& world) {
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> cells;
    world.for_each([&](const Item& i) {
        if (!i.parent) ++cells[{world_from_usecode(i.x) / 2048, world_from_usecode(i.y) / 2048}];
    });
    if (cells.empty()) return {};
    const auto best = std::max_element(cells.begin(), cells.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    return {best->first.first * 2048 + 1024, best->first.second * 2048 + 1024, 0};
}

void print_pick(const std::vector<DrawItem>& list, int x, int y) {
    const auto hits = pick(list, x, y);
    std::printf("pixel (%d, %d): %zu item(s), bottom first\n", x, y, hits.size());
    for (std::size_t i : hits) {
        const DrawItem& d = list[i];
        std::printf("  #%-4zu id %-5u shape %4u frame %3u flags 0x%04X  box x[%d..%d] y[%d..%d] z[%d..%d]%s\n", i,
                    d.item->id, d.item->shape, d.item->frame, d.item->flags, d.x_left, d.x, d.y_far, d.y, d.z,
                    d.z_top, d.mirrored ? " mirrored" : "");
    }
}

std::size_t next_map(const FlexArchive& fixed, std::size_t map, int delta) {
    const std::size_t n = fixed.count();
    for (std::size_t tries = 0; tries < n; ++tries) {
        map = (map + n + static_cast<std::size_t>(delta % static_cast<int>(n))) % n;
        if (!fixed.entry(map).empty()) return map;
    }
    return map;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    std::filesystem::path data = "data";
    std::size_t map = 1;
    std::string png;
    std::optional<std::pair<int, int>> pick_at;
    bool show_cycles = false;
    std::optional<int> save_slot;
    std::optional<Camera> forced;
    std::optional<int> find_shape;
    std::optional<int> show_item;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) png = argv[++i];
        else if (std::strcmp(argv[i], "--cycles") == 0) show_cycles = true;
        else if (std::strcmp(argv[i], "--find") == 0 && i + 1 < argc) find_shape = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--item") == 0 && i + 1 < argc) show_item = std::stoi(argv[++i], nullptr, 0);
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) save_slot = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--pick") == 0 && i + 2 < argc) {
            const int px = std::stoi(argv[++i]);
            pick_at = std::pair{px, std::stoi(argv[++i])};
        }
        else pos.emplace_back(argv[i]);
    }
    if (pos.size() > 0) data = pos[0];
    if (pos.size() > 1) map = std::stoul(pos[1]);
    if (pos.size() > 3) forced = Camera{std::stoi(pos[2]), std::stoi(pos[3]), 0};

    try {
        const auto fixed = FlexArchive::open(data / "STATIC/FIXED.DAT");
        const auto shapes_flex = FlexArchive::open(data / "STATIC/SHAPES.FLX");
        const auto globs = FlexArchive::open(data / "STATIC/GLOB.FLX");
        const auto typeflags = load_typeflags(data / "STATIC/TYPEFLAG.DAT");
        const auto palette = Palette::load(data / "STATIC/GAMEPAL.PAL");
        const auto xform = XformTable::load(data / "STATIC/XFORMPAL.DAT");
        ShapeCache shapes(shapes_flex, ShapeLayout::World);

        auto load_map = [&](std::size_t m) {
            auto w = std::make_unique<World>(typeflags);
            w->load_fixed_map(fixed, m);
            w->expand_glob_eggs(globs);
            return w;
        };
        std::unique_ptr<World> world;
        Camera camera;
        if (save_slot) {
            // A saved game holds the whole state of the current map.
            const auto save = FlexArchive::open(data / "SAVEGAME" / ("SAVEGAME." + std::to_string(*save_slot)));
            world = std::make_unique<World>(typeflags);
            world->load_saved_items(save);
            world->expand_glob_eggs(globs);
            const Item* player = world->get(1);
            if (player) {
                map = player->map_number;
                camera = {world_from_usecode(player->x), world_from_usecode(player->y), player->z};
            }
        } else {
            world = load_map(map);
            camera = map_centre(*world);
        }
        if (forced) camera = *forced;
        if (find_shape) {
            world->for_each([&](const Item& i) {
                if (i.shape != *find_shape) return;
                if (!i.parent)
                    std::printf("id %-5u frame %3u  quality %u  flags 0x%04x  camera %d %d  z %d\n", i.id, i.frame,
                                i.quality, i.flags,
                                world_from_usecode(i.x), world_from_usecode(i.y), i.z);
                else if (const Item* p = world->get(i.parent))
                    std::printf("id %-5u frame %3u  quality %u  flags 0x%04x  in %u (shape %u, camera %d %d  z %d)\n",
                                i.id, i.frame, i.quality, i.flags, i.parent, p->shape, world_from_usecode(p->x),
                                world_from_usecode(p->y), p->z);
                else
                    std::printf("id %-5u frame %3u  quality %u  flags 0x%04x  in missing item %u\n", i.id, i.frame,
                                i.quality, i.flags, i.parent);
            });
            return 0;
        }
        if (show_item) {
            auto print = [&](const Item& i, const char* indent) {
                std::printf("%sid %-5u shape %4u frame %3u  quality %u  npc %u  map %u  flags 0x%04x  parent %u  "
                            "camera %d %d  z %d\n",
                            indent, i.id, i.shape, i.frame, i.quality, i.npc_number, i.map_number, i.flags, i.parent,
                            world_from_usecode(i.x), world_from_usecode(i.y), i.z);
            };
            const Item* item = world->get(static_cast<ObjId>(*show_item));
            if (!item) {
                std::printf("no item %d\n", *show_item);
                return 1;
            }
            print(*item, "");
            world->for_each([&](const Item& i) {
                if (i.parent == item->id) print(i, "  ");
            });
            return 0;
        }
        Surface8 screen(kScreenW, kScreenH);
        RenderStats stats;

        std::vector<DrawItem> list;
        auto render = [&] {
            screen.clear(0);
            list = build_display_list(*world, shapes, camera, kScreenW, kScreenH, &stats);
            draw_display_list(screen, list, &xform);
        };

        if (show_cycles) {
            render();
            std::printf("%zu cycle edges; samples (first drawn -> should be drawn first):\n", stats.cycle_edges);
            for (const auto& [first, dep] : stats.cycle_samples) {
                for (const Item* it : {first, dep}) {
                    const ShapeInfo* si = world->shape_info(it->shape);
                    const int fx = si ? si->footpad_x : 0, fy = si ? si->footpad_y : 0, fz = si ? si->footpad_z : 0;
                    const int x = world_from_usecode(it->x), y = world_from_usecode(it->y);
                    std::printf("   id %-5u shape %4u box x[%d..%d] y[%d..%d] z[%d..%d]%s\n", it->id, it->shape,
                                x - fx * kFootpadXY, x, y - fy * kFootpadXY, y, it->z, it->z + fz * kFootpadZ,
                                it == first ? "  ->" : "");
                }
            }
            return 0;
        }

        if (pick_at) {
            render();
            print_pick(list, pick_at->first, pick_at->second);
            if (png.empty()) return 0;
        }

        if (!png.empty()) {
            render();
            write_png(png, screen, palette);
            std::printf("map %zu (%zu items) camera (%d, %d): %zu items drawn, %zu with missing shape/frame, "
                        "%zu cycle edges -> %s\n",
                        map, world->item_count(), camera.x, camera.y, stats.drawn, stats.missing_shape,
                        stats.cycle_edges, png.c_str());
            return 0;
        }

        SdlPresenter presenter("mapview", kScreenW, kScreenH, 2);
        bool running = true, dragging = false;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                    case SDL_QUIT: running = false; break;
                    case SDL_MOUSEBUTTONDOWN:
                        if (ev.button.button == SDL_BUTTON_RIGHT) print_pick(list, ev.button.x, ev.button.y);
                        else dragging = true;
                        break;
                    case SDL_MOUSEBUTTONUP: dragging = false; break;
                    case SDL_MOUSEMOTION:
                        if (dragging) {
                            // Inverse projection of a screen delta at constant z.
                            const int dsx = -ev.motion.xrel, dsy = -ev.motion.yrel;
                            camera.x += 2 * dsx + 4 * dsy;
                            camera.y += 4 * dsy - 2 * dsx;
                        }
                        break;
                    case SDL_KEYDOWN:
                        switch (ev.key.keysym.sym) {
                            case SDLK_ESCAPE: running = false; break;
                            case SDLK_LEFT: camera.x -= kScrollStep; camera.y += kScrollStep; break;
                            case SDLK_RIGHT: camera.x += kScrollStep; camera.y -= kScrollStep; break;
                            case SDLK_UP: camera.x -= kScrollStep; camera.y -= kScrollStep; break;
                            case SDLK_DOWN: camera.x += kScrollStep; camera.y += kScrollStep; break;
                            case SDLK_HOME: camera = map_centre(*world); break;
                            case SDLK_PAGEUP:
                            case SDLK_PAGEDOWN:
                                map = next_map(fixed, map, ev.key.keysym.sym == SDLK_PAGEDOWN ? 1 : -1);
                                world = load_map(map);
                                camera = map_centre(*world);
                                break;
                            default: break;
                        }
                        break;
                    default: break;
                }
            }
            render();
            presenter.set_title("map " + std::to_string(map) + "  camera (" + std::to_string(camera.x) + ", " +
                                std::to_string(camera.y) + ")  items drawn " + std::to_string(stats.drawn));
            presenter.present(screen, palette);
        }
        SDL_Quit();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        // Also visible when launched without a console.
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "mapview", e.what(), nullptr);
        return 1;
    }
}
