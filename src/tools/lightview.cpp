// Light viewer: the world lit by the port's dynamic lighting
// (engine/world/lighting.hpp, GameConfig::lighting), and an experiment
// turning the camera around it. See docs/LIGHTVIEW.md.
//
// Usage: lightview [data_dir] [map] [cx cy]                       interactive
//        lightview [data_dir] --save SAVEGAME_FILE [...]          a saved game, centred on the Silencer
//        lightview [data_dir] [map] [cx cy] --png out.png [--mouse sx sy] [--drop sx sy]... [--ambient 0-4]
//                  [--orbit yaw pitch] [--flat] [--no-shadows] [--box-shadows] [--no-bump] [--no-glow] [--no-fixtures]
//                  [--alert] [--no-ceiling] [--ceiling-file MAP.json]
//                  [--no-lamp] [--no-smoke] [--no-liquids] [--fixed-actors] [--no-back-faces] [--normals] [--probe sx sy]...
//                  [--puff sx sy]... [--puff-scale K] [--rocket sx sy]... [--shot TYPE] [--ticks N] [--cpu] [--size WxH] [--bench N]
//                                                                 one frame to a PNG
//   Mouse            moving light on the surface under the pointer
//   Left click       drop a light there        Right click  remove the last one
//   Wheel            light height              C            light colour
//   S shadows  G shadows cut by sprites  B bump  E lights from glowing pixels  L Silencer's lamp  K lamps and screens
//   J  alert on / off (alarm lights)   O  ceiling lights on / off (ceiling_lights.hpp)   F5  read their files again
//   A ambient  N normals view  Tab original picture (unlit)
//   Q / W  turn the camera  Z / X  tilt it  R  back to the isometric view
//   T  characters' frames follow the camera  F  far box faces  P  an explosion's smoke at the pointer
//   M  smoke on / off   V  liquids glow and steam on / off   H  a BA-40 shot of the Silencer (muzzle flash)
//   Arrows / middle drag  scroll   PageUp/PageDown  previous/next map   Esc  quit

#define SDL_MAIN_HANDLED
#include "core/anim_dat.hpp"
#include "core/flex.hpp"
#include "core/palette.hpp"
#include "core/png_writer.hpp"
#include "core/typeflags.hpp"
#include "core/xform.hpp"
#include "engine/usecode/world_intrinsics.hpp"
#include "engine/world/ceiling_lights.hpp"
#include "engine/world/lighting.hpp"
#include "engine/world/projectile.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"
#include "platform/gl_lighting.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace crusader;

namespace {

int kW = 640;  // --size
int kH = 480;
constexpr int kScrollStep = 128;
constexpr float kAmbient[] = {0.08f, 0.22f, 0.45f, 0.85f, 1.0f};  // 3: the game's default
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
    std::uint32_t ticks = 0;  // the smoke's clock: one tick a frame
    bool ceiling = true;      // the map's ceiling lights
    std::vector<CeilingLight> ceiling_lights;
};

// The map's ceiling lights: the game's (ceiling_lights/mapNNN.json), or those
// of another map file of the same form (a plan of tools/lights).
std::vector<CeilingLight> load_ceiling(std::size_t map, const std::filesystem::path& file) {
    if (file.empty()) return ceiling_lights(static_cast<int>(map));
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string why;
    const auto doc = parse_json(text.str(), &why);
    if (!doc) {
        std::fprintf(stderr, "%s: %s\n", file.string().c_str(), why.c_str());
        return {};
    }
    return parse_ceiling_lights(*doc, ceiling_light_kinds());
}

// The time spent lighting (shade), for --bench.
std::uint64_t g_shade_ticks = 0;

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
    lighting.capture(world, list, screen, left, top, palette, v.opt, v.ticks++);
    argb.resize(kW * kH);
    if (!v.lit) {
        for (int i = 0; i < kW * kH; ++i) {
            const Rgb& c = palette.colors[screen.pixels[i]];
            argb[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
        }
        return;
    }
    std::vector<PointLight> extra = lights;
    if (v.ceiling) add_ceiling_lights(v.ceiling_lights, v.opt.ambient, extra);
    if (mouse) {
        if (auto p = lighting.light_spot(mouse->first, mouse->second, mouse_height))
            extra.push_back({*p, scaled(mouse_color, 1.6f), 900});
    }
    const auto t0 = SDL_GetPerformanceCounter();
    lighting.shade(world, screen, palette, v.opt, extra, argb.data());
    g_shade_ticks += SDL_GetPerformanceCounter() - t0;
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
    bool alert = false;
    int bench = 1;
    float puff_scale = 1;  // --puff-scale: an explosion of that size (the game: 0.8 to 2)
    int ticks = 0;  // --ticks: frames of smoke before the picture
    bool cpu = false;
    std::vector<std::pair<int, int>> puffs, rockets;
    int shot = -1;  // --shot: the Silencer fires this fire type (its muzzle flash)
    std::filesystem::path save;
    std::filesystem::path ceiling_file;
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
        } else if (arg("--ambient") && i + 1 < argc) v.ambient = std::clamp(std::stoi(argv[++i]), 0, 4);
        else if (arg("--flat")) v.lit = false;
        else if (arg("--no-shadows")) v.opt.shadows = false;
        else if (arg("--box-shadows")) v.opt.sprite_shadows = false;
        else if (arg("--no-bump")) v.opt.bump = false;
        else if (arg("--no-glow")) v.opt.glow_lights = v.opt.flash_lights = false;
        else if (arg("--no-fixtures")) v.opt.fixture_lights = false;
        else if (arg("--alert")) alert = true;
        else if (arg("--no-ceiling")) v.ceiling = false;
        else if (arg("--ceiling-file") && i + 1 < argc) ceiling_file = argv[++i];
        else if (arg("--no-lamp")) v.opt.player_lamp = false;
        else if (arg("--no-smoke")) v.opt.smoke = false;
        else if (arg("--no-liquids")) v.opt.liquids = false;
        else if (arg("--puff") && i + 2 < argc) puffs.push_back(pair());
        else if (arg("--puff-scale") && i + 1 < argc) puff_scale = std::stof(argv[++i]);
        else if (arg("--rocket") && i + 2 < argc) rockets.push_back(pair());
        else if (arg("--shot") && i + 1 < argc) shot = std::stoi(argv[++i], nullptr, 0);
        else if (arg("--ticks") && i + 1 < argc) ticks = std::max(0, std::stoi(argv[++i]));
        else if (arg("--cpu")) cpu = true;
        else if (arg("--size") && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &kW, &kH) != 2 || kW < 64 || kH < 64) kW = 640, kH = 480;
        }
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
        // The ceiling lights' files: next to the executable, else the sources'.
        {
            std::vector<std::filesystem::path> dirs;
            if (char* base = SDL_GetBasePath()) {
                dirs.push_back(std::filesystem::path(base) / "ceiling_lights");
                SDL_free(base);
            }
            dirs.push_back(CRUSADER_CEILING_LIGHTS_DIR);
            set_ceiling_lights_dirs(std::move(dirs));
        }
        WorldLighting lighting(shapes, xform, anims ? &*anims : nullptr);
        // On the GPU unless --cpu (or none can be used).
        std::unique_ptr<GlLighting> gpu;
        if (!cpu) {
            std::string why;
            gpu = GlLighting::create(&why);
            if (gpu) std::printf("GPU: %s\n", gpu->description().c_str());
            else std::printf("no GPU lighting (%s): processor\n", why.c_str());
        }
        lighting.set_gpu(gpu.get());

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
        if (alert) set_alert(*world, true);
        v.ceiling_lights = load_ceiling(map, ceiling_file);
        std::vector<PointLight> lights;
        std::vector<std::uint32_t> argb;
        float mouse_height = 40;
        int color = 0;
        // A shot of the Silencer the way he faces, from about the height of his gun.
        const auto fire = [&](int fire_type) {
            if (const Item* p = world->get(1); p && !p->off_map)
                lighting.shot_fired(1, p->x, p->y, p->z + 32, world->npc_direction(1), fire_type, v.ticks);
        };

        if (!png.empty()) {
            if (!drops.empty() || !puffs.empty() || !rockets.empty()) {
                // Surfaces first, then the lights and puffs dropped on them.
                render(*world, shapes, xform, palette, lighting, camera, v, lights, std::nullopt, mouse_height,
                       kLightColors[color], argb);
                for (std::size_t k = 0; k < drops.size(); ++k)
                    if (auto p = lighting.light_spot(drops[k].first, drops[k].second, mouse_height))
                        lights.push_back({*p, scaled(kLightColors[(k + 1) % 4], 1.4f), 800});
                for (const auto& [px, py] : puffs)
                    if (auto p = lighting.surface_at(px, py))
                        lighting.add_puff(Vec3{p->x, p->y, p->z + 16 * kLightZScale}, SmokePuff::kExplosion, puff_scale);
                // A rocket (projectile frame 0x11) hanging 30 z units over the surface.
                for (const auto& [px, py] : rockets)
                    if (auto p = lighting.surface_at(px, py)) {
                        world->create(kProjectileShape, 0x11);
                        const ObjId id = world->pop_to_coords(static_cast<std::int32_t>(p->x / 2),
                                                              static_cast<std::int32_t>(p->y / 2),
                                                              static_cast<std::int32_t>(p->z / kLightZScale) + 30);
                        if (Item* rocket = world->get(id)) rocket->sprite_only = true;
                    }
            }
            if (shot >= 0) fire(shot);
            for (int k = 0; k < ticks; ++k) {
                render(*world, shapes, xform, palette, lighting, camera, v, lights, mouse, mouse_height,
                       kLightColors[color], argb);
            }
            const auto t0 = SDL_GetPerformanceCounter();
            g_shade_ticks = 0;
            for (int k = 0; k < bench; ++k)
                render(*world, shapes, xform, palette, lighting, camera, v, lights, mouse, mouse_height,
                       kLightColors[color], argb);
            if (bench > 1) {
                const double freq = double(SDL_GetPerformanceFrequency());
                std::printf("%.1f ms per frame, %.2f ms of it lighting\n",
                            1000.0 * double(SDL_GetPerformanceCounter() - t0) / freq / bench,
                            1000.0 * double(g_shade_ticks) / freq / bench);
            }
            for (const auto& [px, py] : probes) lighting.probe(*world, px, py);
            write_png_argb(png, argb);
            std::printf("map %zu camera (%d, %d): %zu lights, %zu puffs, %s -> %s\n", map, camera.x, camera.y,
                        lighting.lights().size(), lighting.puffs_shown().size(), lighting.used_gpu() ? "GPU" : "CPU",
                        png.c_str());
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
                    case SDLK_k: v.opt.fixture_lights = !v.opt.fixture_lights; break;
                    case SDLK_j: set_alert(*world, !world->state.alert_active); break;
                    case SDLK_o: v.ceiling = !v.ceiling; break;
                    case SDLK_F5:
                        reload_ceiling_lights();
                        v.ceiling_lights = load_ceiling(map, ceiling_file);
                        break;
                    case SDLK_l: v.opt.player_lamp = !v.opt.player_lamp; break;
                    case SDLK_n: v.opt.show_normals = !v.opt.show_normals; break;
                    case SDLK_a: v.ambient = (v.ambient + 1) % 5; break;
                    case SDLK_c: color = (color + 1) % 4; break;
                    case SDLK_q: v.opt.yaw -= 5; break;
                    case SDLK_w: v.opt.yaw += 5; break;
                    case SDLK_z: v.opt.pitch -= 5; break;
                    case SDLK_x: v.opt.pitch += 5; break;
                    case SDLK_r: v.opt.yaw = v.opt.pitch = 0; break;
                    case SDLK_t: v.opt.turn_actors = !v.opt.turn_actors; break;
                    case SDLK_f: v.opt.back_faces = !v.opt.back_faces; break;
                    case SDLK_m: v.opt.smoke = !v.opt.smoke; break;
                    case SDLK_v: v.opt.liquids = !v.opt.liquids; break;
                    case SDLK_h: fire(0xB); break;
                    case SDLK_p:
                        if (pointer)
                            if (auto p = lighting.surface_at(pointer->first, pointer->second))
                                lighting.add_puff(Vec3{p->x, p->y, p->z + 16 * kLightZScale}, SmokePuff::kExplosion, puff_scale);
                        break;
                    case SDLK_PAGEUP:
                    case SDLK_PAGEDOWN:
                        map = next_map(fixed, map, ev.key.keysym.sym == SDLK_PAGEDOWN ? 1 : -1);
                        world = load_map(map);
                        camera = map_centre(*world);
                        lights.clear();
                        v.ceiling_lights = load_ceiling(map, ceiling_file);
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
                          "lightview  map %zu  (%d, %d)  lights %zu  %.1f ms %s  yaw %.0f pitch %.0f  [S]hadows %s "
                          "[A]mbient %d [Tab] %s",
                          map, camera.x, camera.y, lighting.lights().size(), ms, lighting.used_gpu() ? "GPU" : "CPU",
                          v.opt.yaw, v.opt.pitch,
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
