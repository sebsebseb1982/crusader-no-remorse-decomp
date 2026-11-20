// Light viewer: the world lit by the port's dynamic lighting
// (engine/world/lighting.hpp, GameConfig::lighting), and an experiment
// turning the camera around it. See docs/LIGHTVIEW.md.
//
// Usage: lightview [data_dir] [map] [cx cy]                       interactive
//        lightview [data_dir] --save SAVEGAME_FILE [...]          a saved game, centred on the Silencer
//        lightview [data_dir] [map] [cx cy] --png out.png [--mouse sx sy] [--drop sx sy]... [--ambient 0-3]
//                  [--orbit yaw pitch] [--flat] [--no-shadows] [--box-shadows] [--no-bump] [--no-glow]
//                  [--no-lamp] [--fixed-actors] [--no-back-faces] [--normals] [--probe sx sy]... [--bench N]
//                                                                 one frame to a PNG
//   Mouse            moving light on the surface under the pointer
//   Left click       drop a light there        Right click  remove the last one
//   Wheel            light height              C            light colour
//   S shadows  G shadows cut by sprites  B bump  E lights from glowing pixels  L Silencer's lamp
//   A ambient  N normals view  Tab original picture (unlit)
//   Q / W  turn the camera  Z / X  tilt it  R  back to the isometric view
//   T  characters' frames follow the camera  F  far box faces
//   Arrows / middle drag  scroll   PageUp/PageDown  previous/next map   Esc  quit

#define SDL_MAIN_HANDLED
#include "core/anim_dat.hpp"
#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/png_writer.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "engine/world/lighting.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace crusader;

namespace {

constexpr int kW = 640;
constexpr int kH = 480;
constexpr int kScrollStep = 128;
constexpr float kAmbient[] = {0.08f, 0.22f, 0.45f, 1.0f};
const Vec3 kLightColors[] = {{1.0f, 0.85f, 0.65f}, {0.55f, 0.75f, 1.0f}, {1.0f, 0.35f, 0.25f}, {0.4f, 1.0f, 0.5f}};

Vec3 scaled(Vec3 c, float k) { return {c.x * k, c.y * k, c.z * k}; }

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

std::size_t next_map(const FlexArchive& fixed, std::size_t map, int delta) {
    const std::size_t n = fixed.count();
    for (std::size_t tries = 0; tries < n; ++tries) {
        map = (map + n + static_cast<std::size_t>(delta % static_cast<int>(n))) % n;
        if (!fixed.entry(map).empty()) return map;
    }
    return map;
}

struct View3D {
    LightingOptions opt;
    bool lit = true;
    int ambient = 1;  // index in kAmbient
};

// Draws the world around `camera`, lit, into `argb` (kW x kH).
void render(const World& world, ShapeCache& shapes, const XformTable& xform, const Palette& palette,
            WorldLighting& lighting, const Camera& camera, View3D& v, const std::vector<PointLight>& lights,
            std::optional<std::pair<int, int>> mouse, float mouse_height, Vec3 mouse_color,
            std::vector<std::uint32_t>& argb) {
    v.opt.ambient = kAmbient[v.ambient];
    const int left = ((camera.x - camera.y) >> 2) - kW / 2;
    const int top = (((camera.x + camera.y) >> 3) - camera.z) - kH / 2;
    const auto list = build_display_list(world, shapes, View{left, top}, kW, kH);
    Surface8 screen(kW, kH);
    draw_display_list(screen, list, &xform);
    lighting.capture(world, list, screen, left, top, palette, v.opt);
    argb.resize(kW * kH);
    if (!v.lit) {
        for (int i = 0; i < kW * kH; ++i) {
            const Rgb& c = palette.colors[screen.pixels[i]];
            argb[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
        }
        return;
    }
    std::vector<PointLight> extra = lights;
    if (mouse) {
        if (auto p = lighting.light_spot(mouse->first, mouse->second, mouse_height))
            extra.push_back({*p, scaled(mouse_color, 1.6f), 900});
    }
    lighting.shade(world, screen, palette, v.opt, extra, argb.data());
    if (v.opt.yaw != 0 || v.opt.pitch != 0) lighting.reproject(world, camera, v.opt, argb.data());
}

void write_png_argb(const std::string& path, const std::vector<std::uint32_t>& argb) {
    std::vector<std::uint8_t> rgb(kW * kH * 3);
    for (int i = 0; i < kW * kH; ++i) {
        rgb[i * 3] = static_cast<std::uint8_t>(argb[i] >> 16);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(argb[i] >> 8);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>(argb[i]);
    }
    write_png_rgb(path, kW, kH, rgb.data());
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    std::filesystem::path data = "data";
    std::size_t map = 1;
    std::string png;
    std::optional<std::pair<int, int>> mouse;
    std::vector<std::pair<int, int>> probes, drops;
    int bench = 1;
    std::filesystem::path save;
    std::optional<Camera> forced;
    View3D v;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0; };
        const auto pair = [&] {
            const int a = std::stoi(argv[++i]);
            return std::pair{a, std::stoi(argv[++i])};
        };
        if (arg("--png") && i + 1 < argc) png = argv[++i];
        else if (arg("--mouse") && i + 2 < argc) mouse = pair();
        else if (arg("--drop") && i + 2 < argc) drops.push_back(pair());
        else if (arg("--probe") && i + 2 < argc) probes.push_back(pair());
        else if (arg("--bench") && i + 1 < argc) bench = std::max(1, std::stoi(argv[++i]));
        else if (arg("--save") && i + 1 < argc) save = argv[++i];
        else if (arg("--orbit") && i + 2 < argc) {
            v.opt.yaw = std::stof(argv[++i]);
            v.opt.pitch = std::stof(argv[++i]);
        } else if (arg("--ambient") && i + 1 < argc) v.ambient = std::stoi(argv[++i]) & 3;
        else if (arg("--flat")) v.lit = false;
        else if (arg("--no-shadows")) v.opt.shadows = false;
        else if (arg("--box-shadows")) v.opt.sprite_shadows = false;
        else if (arg("--no-bump")) v.opt.bump = false;
        else if (arg("--no-glow")) v.opt.glow_lights = v.opt.flash_lights = false;
        else if (arg("--no-lamp")) v.opt.player_lamp = false;
        else if (arg("--fixed-actors")) v.opt.turn_actors = false;
        else if (arg("--no-back-faces")) v.opt.back_faces = false;
        else if (arg("--normals")) v.opt.show_normals = true;
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
        std::optional<AnimDat> anims;
        try {
            anims = AnimDat::load(data / "STATIC/ANIM.DAT");
        } catch (const std::exception&) {
        }
        WorldLighting lighting(shapes, xform, anims ? &*anims : nullptr);

        auto load_map = [&](std::size_t m) {
            auto w = std::make_unique<World>(typeflags);
            w->load_fixed_map(fixed, m);
            w->expand_glob_eggs(globs);
            return w;
        };
        std::unique_ptr<World> world;
        Camera camera;
        if (!save.empty()) {
            // A saved game: its map with NPCs, centred on the Silencer.
            world = std::make_unique<World>(typeflags);
            world->load_saved_items(FlexArchive::open(save));
            world->expand_glob_eggs(globs);
            if (const Item* player = world->get(1)) {
                map = player->map_number;
                camera = {world_from_usecode(player->x), world_from_usecode(player->y), player->z};
            }
        } else {
            world = load_map(map);
            camera = map_centre(*world);
        }
        if (forced) camera = *forced;
        std::vector<PointLight> lights;
        std::vector<std::uint32_t> argb;
        float mouse_height = 40;
        int color = 0;

        if (!png.empty()) {
            if (!drops.empty()) {
                // Surfaces first, then the lights dropped on them.
                render(*world, shapes, xform, palette, lighting, camera, v, lights, std::nullopt, mouse_height,
                       kLightColors[color], argb);
                for (std::size_t k = 0; k < drops.size(); ++k)
                    if (auto p = lighting.light_spot(drops[k].first, drops[k].second, mouse_height))
                        lights.push_back({*p, scaled(kLightColors[(k + 1) % 4], 1.4f), 800});
            }
            const auto t0 = SDL_GetPerformanceCounter();
            for (int k = 0; k < bench; ++k)
                render(*world, shapes, xform, palette, lighting, camera, v, lights, mouse, mouse_height,
                       kLightColors[color], argb);
            if (bench > 1)
                std::printf("%.1f ms per frame\n",
                            1000.0 * (SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency() / bench);
            for (const auto& [px, py] : probes) lighting.probe(*world, px, py);
            write_png_argb(png, argb);
            std::printf("map %zu camera (%d, %d): %zu lights -> %s\n", map, camera.x, camera.y,
                        lighting.lights().size(), png.c_str());
            return 0;
        }

        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("lightview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW * 2,
                                              kH * 2, SDL_WINDOW_RESIZABLE);
        SDL_Renderer* sdl = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(sdl, kW, kH);
        SDL_Texture* tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        bool running = true, dragging = false;
        std::optional<std::pair<int, int>> pointer;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (auto p = lighting.light_spot(ev.button.x, ev.button.y, mouse_height))
                            lights.push_back({*p, scaled(kLightColors[color], 1.4f), 800});
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        if (!lights.empty()) lights.pop_back();
                    } else dragging = true;
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_MIDDLE) dragging = false;
                    break;
                case SDL_MOUSEMOTION:
                    pointer = std::pair{ev.motion.x, ev.motion.y};
                    if (dragging) {
                        const int dsx = -ev.motion.xrel, dsy = -ev.motion.yrel;
                        camera.x += 2 * dsx + 4 * dsy;
                        camera.y += 4 * dsy - 2 * dsx;
                    }
                    break;
                case SDL_MOUSEWHEEL: mouse_height = std::clamp(mouse_height + ev.wheel.y * 8.0f, 4.0f, 200.0f); break;
                case SDL_KEYDOWN:
                    switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_LEFT: camera.x -= kScrollStep; camera.y += kScrollStep; break;
                    case SDLK_RIGHT: camera.x += kScrollStep; camera.y -= kScrollStep; break;
                    case SDLK_UP: camera.x -= kScrollStep; camera.y -= kScrollStep; break;
                    case SDLK_DOWN: camera.x += kScrollStep; camera.y += kScrollStep; break;
                    case SDLK_TAB: v.lit = !v.lit; break;
                    case SDLK_s: v.opt.shadows = !v.opt.shadows; break;
                    case SDLK_g: v.opt.sprite_shadows = !v.opt.sprite_shadows; break;
                    case SDLK_b: v.opt.bump = !v.opt.bump; break;
                    case SDLK_e: v.opt.glow_lights = v.opt.flash_lights = !v.opt.glow_lights; break;
                    case SDLK_l: v.opt.player_lamp = !v.opt.player_lamp; break;
                    case SDLK_n: v.opt.show_normals = !v.opt.show_normals; break;
                    case SDLK_a: v.ambient = (v.ambient + 1) & 3; break;
                    case SDLK_c: color = (color + 1) % 4; break;
                    case SDLK_q: v.opt.yaw -= 5; break;
                    case SDLK_w: v.opt.yaw += 5; break;
                    case SDLK_z: v.opt.pitch -= 5; break;
                    case SDLK_x: v.opt.pitch += 5; break;
                    case SDLK_r: v.opt.yaw = v.opt.pitch = 0; break;
                    case SDLK_t: v.opt.turn_actors = !v.opt.turn_actors; break;
                    case SDLK_f: v.opt.back_faces = !v.opt.back_faces; break;
                    case SDLK_PAGEUP:
                    case SDLK_PAGEDOWN:
                        map = next_map(fixed, map, ev.key.keysym.sym == SDLK_PAGEDOWN ? 1 : -1);
                        world = load_map(map);
                        camera = map_centre(*world);
                        lights.clear();
                        break;
                    default: break;
                    }
                    break;
                default: break;
                }
            }
            const auto t0 = SDL_GetPerformanceCounter();
            render(*world, shapes, xform, palette, lighting, camera, v, lights, pointer, mouse_height,
                   kLightColors[color], argb);
            const double ms = 1000.0 * (SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
            SDL_UpdateTexture(tex, nullptr, argb.data(), kW * 4);
            SDL_RenderClear(sdl);
            SDL_RenderCopy(sdl, tex, nullptr, nullptr);
            SDL_RenderPresent(sdl);
            char title[256];
            std::snprintf(title, sizeof title,
                          "lightview  map %zu  (%d, %d)  lights %zu  %.1f ms  yaw %.0f pitch %.0f  [S]hadows %s "
                          "[A]mbient %d [Tab] %s",
                          map, camera.x, camera.y, lighting.lights().size(), ms, v.opt.yaw, v.opt.pitch,
                          v.opt.shadows ? "on" : "off", v.ambient, v.lit ? "lit" : "original");
            SDL_SetWindowTitle(window, title);
        }
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(sdl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
