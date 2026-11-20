// Light viewer: prototype of a "2.5D in a 3D renderer" look for the world.
//
// The isometric sprites are drawn as usual (same display list and order as
// the game), and each opaque pixel gets a 3D position and a normal: the point
// where the view ray through that pixel enters the item's world box (top,
// +x or +y face), or, for NPCs and pixels outside their box, a vertical
// billboard through the box centre with a normal rounded from the sprite's
// silhouette. The picture is then lit per pixel by point lights, with
// shadows cast by the item boxes cut by their sprites.
//
// Experiment (--orbit, Q/W/Z/X): every pixel of every sprite around the view
// becomes a lit 3D point, splatted from a turned camera. Characters face the
// camera with the frame of the direction they show it (ANIM.DAT); the far
// faces of boxes, never drawn, repeat the near ones. See docs/LIGHTVIEW.md.
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
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace crusader;

namespace {

constexpr int kW = 640;
constexpr int kH = 480;
constexpr int kScrollStep = 128;

// The projection (sx = (x - y) / 4, sy = (x + y) / 8 - z) is a 2:1 dimetric
// view: camera 45 degrees round and 30 degrees down. One z unit is then
// kZScale x/y units: lighting works in that "metric" space (x, y, z * kZScale).
constexpr float kZScale = 3.266f;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : V3{0, 0, 1};
}

// Camera basis in metric space.
const V3 kToViewer{0.6124f, 0.6124f, 0.5f};
const V3 kScreenRight{0.7071f, -0.7071f, 0.0f};
const V3 kScreenUp{-0.3536f, -0.3536f, 0.8660f};

// A frame decoded once: colour (-1 transparent) and a normal rounded from the
// silhouette (screen x / y components, -127..127).
struct DecodedFrame {
    int w = 0, h = 0;
    std::vector<std::int16_t> color;
    std::vector<std::int8_t> nx, ny;
};

const DecodedFrame& decode(std::unordered_map<const ShapeFrame*, DecodedFrame>& cache, const ShapeFrame& f) {
    auto it = cache.find(&f);
    if (it != cache.end()) return it->second;
    DecodedFrame d;
    d.w = f.width;
    d.h = f.height;
    const std::size_t n = static_cast<std::size_t>(d.w) * d.h;
    d.color.resize(n);
    d.nx.assign(n, 0);
    d.ny.assign(n, 0);
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x) d.color[y * d.w + x] = static_cast<std::int16_t>(f.pixel(x, y));
    // Rows: position between the first and last opaque pixels gives nx;
    // columns the same for ny (weaker: bodies are rounder across than along).
    for (int y = 0; y < d.h; ++y) {
        int a = d.w, b = -1;
        for (int x = 0; x < d.w; ++x)
            if (d.color[y * d.w + x] >= 0) a = std::min(a, x), b = std::max(b, x);
        if (b < a) continue;
        for (int x = a; x <= b; ++x) {
            const float t = b > a ? (x - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d.nx[y * d.w + x] = static_cast<std::int8_t>((2 * t - 1) * 120);
        }
    }
    for (int x = 0; x < d.w; ++x) {
        int a = d.h, b = -1;
        for (int y = 0; y < d.h; ++y)
            if (d.color[y * d.w + x] >= 0) a = std::min(a, y), b = std::max(b, y);
        if (b < a) continue;
        for (int y = a; y <= b; ++y) {
            const float t = b > a ? (y - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d.ny[y * d.w + x] = static_cast<std::int8_t>((1 - 2 * t) * 50);
        }
    }
    return cache.emplace(&f, std::move(d)).first->second;
}

struct Light {
    V3 pos;  // metric
    V3 color;
    float radius = 700;
    ObjId owner = 0;  // item giving the light: never shadows it
};

// An occluder: the item's box, cut by its sprite. Seen from the camera the
// item covers only the sprite's opaque pixels, so a point of the box whose
// projection falls on a transparent pixel is empty (railings, grids and
// bodies cast their own outline, not a block).
struct Box {
    V3 lo, hi;  // metric
    ObjId owner;
    const DecodedFrame* sprite = nullptr;
    int left = 0, top = 0;  // frame rectangle, absolute screen coordinates
    bool mirrored = false;
};

// Glowing palette entries: the colour-cycled ones (1..14: blinking lamps,
// screens, force fields).
bool glowing(int c) { return c >= 1 && c <= 14; }

// Characters, drawn as billboards: the NPC table (ids 1..255, the Silencer
// included, whose shape has no NPC typeflag) and NPC shapes created later.
bool is_actor(const Item& item, const ShapeInfo* info) {
    return is_npc_id(item.id) || (info && info->is(ShapeInfo::kNpc));
}

// Segment o -> o + dir (t in 0..1) against a box: the part inside it.
bool segment_box(const V3& o, const V3& inv, const Box& b, float& t0, float& t1) {
    // Ends excluded: a light or a surface touching a box is not hidden by it.
    t0 = 1e-3f;
    t1 = 0.98f;
    const float os[3] = {o.x, o.y, o.z}, is[3] = {inv.x, inv.y, inv.z};
    const float lo[3] = {b.lo.x, b.lo.y, b.lo.z}, hi[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int a = 0; a < 3; ++a) {
        float ta = (lo[a] - os[a]) * is[a], tb = (hi[a] - os[a]) * is[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

// Whether the segment crosses an opaque part of the occluder.
bool blocks(const V3& o, const V3& dir, const V3& inv, const Box& b, bool cut_by_sprite) {
    float t0, t1;
    if (!segment_box(o, inv, b, t0, t1)) return false;
    if (!cut_by_sprite || !b.sprite) return true;
    const DecodedFrame& f = *b.sprite;
    constexpr int kSamples = 5;
    for (int k = 0; k < kSamples; ++k) {
        const float t = t0 + (t1 - t0) * (k + 0.5f) / kSamples;
        const V3 p = o + dir * t;
        const int sx = static_cast<int>(std::floor((p.x - p.y) * 0.25f));
        const int sy = static_cast<int>(std::floor((p.x + p.y) * 0.125f - p.z / kZScale));
        const int fx = b.mirrored ? b.left + f.w - 1 - sx : sx - b.left;
        const int fy = sy - b.top;
        if (fx < 0 || fy < 0 || fx >= f.w || fy >= f.h) continue;
        if (f.color[fy * f.w + fx] >= 0) return true;
    }
    return false;
}

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

template <class F>
void parallel_rows(int h, F&& f) {
    const int n = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (int t = 0; t < n; ++t)
        threads.emplace_back([&, t] {
            for (int y = t; y < h; y += n) f(y);
        });
    for (auto& th : threads) th.join();
}

struct Options {
    bool lit = true;
    bool shadows = true;
    bool bump = true;
    bool glow_lights = true;
    bool sprite_shadows = true;  // occluders cut by their sprite (else whole boxes)
    bool player_lamp = true;     // a light over the Silencer (item 1)
    bool show_normals = false;
    float yaw = 0, pitch = 0;  // degrees, reprojection experiment
    bool turn_actors = true;   // NPC frames follow the turned camera
    bool back_faces = true;    // far box faces made from the near ones
    int ambient = 1;  // index in kAmbient
};
constexpr float kAmbient[] = {0.08f, 0.22f, 0.45f, 1.0f};

const V3 kLightColors[] = {{1.0f, 0.85f, 0.65f}, {0.55f, 0.75f, 1.0f}, {1.0f, 0.35f, 0.25f}, {0.4f, 1.0f, 0.5f}};

class LitRenderer {
public:
    LitRenderer(ShapeCache& shapes, const XformTable& xform, const Palette& palette, const AnimDat* anims)
        : shapes_(shapes), xform_(xform), anims_(anims) {
        for (int i = 0; i < 256; ++i) {
            const Rgb& c = palette.colors[i];
            lin_[i] = {to_lin(c.r), to_lin(c.g), to_lin(c.b)};
            srgb_[i] = c;
            lum_[i] = std::sqrt(0.3f * lin_[i].x + 0.59f * lin_[i].y + 0.11f * lin_[i].z);
        }
        for (int i = 0; i < 4096; ++i)
            out_lut_[i] = static_cast<std::uint8_t>(std::lround(255.0 * std::pow(i / 4095.0, 1 / 2.2)));
        idx_.resize(kW * kH);
        owner_.resize(kW * kH);
        pos_.resize(kW * kH);
        nrm_.resize(kW * kH);
        rgb_.resize(kW * kH * 3);
    }

    // Draws the view; `mouse` (screen) carries the moving light when set.
    void render(const World& world, const Camera& camera, const Options& opt, std::vector<Light> lights,
                std::optional<std::pair<int, int>> mouse, float mouse_height, V3 mouse_color) {
        view_left_ = ((camera.x - camera.y) >> 2) - kW / 2;
        view_top_ = (((camera.x + camera.y) >> 3) - camera.z) - kH / 2;
        list_ = build_display_list(world, shapes_, View{view_left_, view_top_}, kW, kH);
        composite(world);
        geometry(world, opt);
        if (opt.glow_lights) add_glow_lights(lights);
        if (opt.player_lamp) add_player_lamp(world, lights);
        if (mouse) {
            if (auto p = light_spot(mouse->first, mouse->second, mouse_height))
                lights.push_back({*p, mouse_color * 1.6f, 900});
        }
        lights_used_ = lights;
        occluders(world, lights);
        shade(opt, lights);
        orbited_ = opt.yaw != 0 || opt.pitch != 0;
        if (orbited_) reproject(world, camera, opt, lights);
    }

    // Experiment: the scene seen from a camera turned by `yaw` degrees around
    // the vertical and tilted by `pitch`. Every pixel of every sprite around
    // the view (not only those left on top) is a lit 3D point, splatted with
    // a depth buffer. What no sprite ever showed (the far faces of walls and
    // objects) is left as holes, in magenta.
    void reproject(const World& world, const Camera& camera, const Options& opt, const std::vector<Light>& lights) {
        constexpr float kDeg = 3.14159265f / 180.0f;
        const float phi = (45.0f + opt.yaw) * kDeg, theta = (30.0f + opt.pitch) * kDeg;
        const V3 f{std::cos(theta) * std::cos(phi), std::cos(theta) * std::sin(phi), std::sin(theta)};
        const V3 r{std::sin(phi), -std::cos(phi), 0.0f};
        const V3 u{-std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
        // Scale of the original projection: (x - y) / 4 = scale * (x - y) * cos 45.
        constexpr float kScale = 0.35355f;
        const V3 c{float(camera.x), float(camera.y), camera.z * kZScale};
        // Items around the view too: turning brings them in.
        constexpr int kMargin = 320;
        const auto around = build_display_list(world, shapes_, View{view_left_ - kMargin, view_top_ - kMargin},
                                               kW + 2 * kMargin, kH + 2 * kMargin);
        // NPCs: the frame of the direction they show to the turned camera
        // (16 directions, 22.5 degrees each).
        const int steps = static_cast<int>(std::lround(opt.yaw / 22.5f));
        std::vector<std::pair<const ShapeFrame*, bool>> sprite(around.size());
        for (std::size_t k = 0; k < around.size(); ++k) {
            const DrawItem& d = around[k];
            sprite[k] = {d.frame, d.mirrored};
            const ShapeInfo* info = world.shape_info(d.item->shape);
            if (is_actor(*d.item, info) && opt.turn_actors) sprite[k] = turned_frame(d, steps);
            decode(cache_, *d.frame);
            decode(cache_, *sprite[k].first);
        }
        out_.assign(kW * kH * 3, 0);
        for (int i = 0; i < kW * kH; ++i) out_[i * 3] = 255, out_[i * 3 + 2] = 255;
        std::vector<float> depth(kW * kH, -1e30f);
        std::vector<std::mutex> row_locks(kH);
        // Depth-tested 2 x 2 splat (no cracks where the turned view stretches
        // the surface), shaded only when it shows.
        auto splat = [&](float sx, float sy, float z, int col, const V3& p, const V3& n, ObjId self) {
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            if (x0 < -1 || y0 < -1 || x0 >= kW || y0 >= kH) return;
            std::uint8_t rgb[3];
            bool shaded = false;
            for (int dy = 0; dy < 2; ++dy) {
                const int y = y0 + dy;
                if (y < 0 || y >= kH) continue;
                std::lock_guard<std::mutex> lock(row_locks[y]);
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = x0 + dx;
                    if (x < 0 || x >= kW) continue;
                    const int j = y * kW + x;
                    if (z <= depth[j]) continue;
                    if (!shaded) {
                        if (opt.show_normals) write_normal(rgb, n);
                        else if (opt.lit) write(rgb, shade_point(col, p, n, self, opt, lights));
                        else write(rgb, lin_[col]);
                        shaded = true;
                    }
                    depth[j] = z;
                    std::copy_n(rgb, 3, &out_[j * 3]);
                }
            }
        };
        // One task per item, spread over the threads.
        const int nthreads = std::max(1u, std::thread::hardware_concurrency());
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < nthreads; ++t)
            threads.emplace_back([&] {
                for (std::size_t k; (k = next++) < around.size();) {
                    const DrawItem& d = around[k];
                    if (d.shape_flags & ShapeInfo::kTranslucent) continue;
                    const ShapeInfo* info = world.shape_info(d.item->shape);
                    if (is_actor(*d.item, info)) {
                        // A sprite facing the turned camera, standing on the box centre.
                        const ShapeFrame& frame = *sprite[k].first;
                        const bool mirrored = sprite[k].second;
                        const DecodedFrame& fr = cache_.at(&frame);
                        const float xc = (d.x + d.x_left) * 0.5f, yc = (d.y + d.y_far) * 0.5f;
                        const V3 centre{xc, yc, d.z * kZScale};
                        // Where the centre falls in the sprite, from its anchor (the box corner).
                        const float cx = (xc - yc) * 0.25f - (d.x - d.y) * 0.25f;
                        const float cy = (xc + yc) * 0.125f - (d.x + d.y) * 0.125f;
                        const V3 q0 = centre - c;
                        const float base_z = dot(q0, f);
                        const float csx = kScale * dot(q0, r) + kW / 2.0f, csy = -kScale * dot(q0, u) + kH / 2.0f;
                        for (int fy = 0; fy < fr.h; ++fy)
                            for (int fx = 0; fx < fr.w; ++fx) {
                                const int col = fr.color[fy * fr.w + fx];
                                if (col < 0) continue;
                                const float ox = (mirrored ? frame.xoff - fx : fx - frame.xoff) - cx;
                                const float oy = (fy - frame.yoff) - cy;
                                float nx = fr.nx[fy * fr.w + fx] / 127.0f, ny = fr.ny[fy * fr.w + fx] / 127.0f;
                                if (mirrored) nx = -nx;
                                const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
                                const V3 n = normalize(r * nx + u * ny + f * nz);
                                const V3 p = centre + r * (ox / kScale) - u * (oy / kScale) + f * (nz * 12.0f);
                                splat(csx + ox, csy + oy, base_z + nz * 12.0f, col, p, n, d.item->id);
                            }
                        continue;
                    }
                    const DecodedFrame& fr = cache_.at(d.frame);
                    // DrawItem screen positions are relative to the wider view.
                    const int left = d.left + view_left_ - kMargin, top = d.top + view_top_ - kMargin;
                    for (int fy = 0; fy < fr.h; ++fy)
                        for (int fx = 0; fx < fr.w; ++fx) {
                            const int col = fr.color[fy * fr.w + fx];
                            if (col < 0) continue;
                            const int ax = d.mirrored ? left + fr.w - 1 - fx : left + fx;
                            V3 p, n;
                            int face = 0;
                            surface(world, d, ax + 0.5f, top + fy + 0.5f, fx, fy, opt.bump, p, n, &face);
                            const V3 q = p - c;
                            splat(kScale * dot(q, r) + kW / 2.0f, -kScale * dot(q, u) + kH / 2.0f, dot(q, f), col, p,
                                  n, d.item->id);
                            if (!opt.back_faces || (face != 1 && face != 2)) continue;
                            // The far side of the box, never drawn: the near side's
                            // pixels, mirrored through the box.
                            V3 pb = p, nb = n;
                            if (face == 1) {
                                pb.x = d.x_left - (p.x - d.x);
                                nb.x = -n.x;
                            } else {
                                pb.y = d.y_far - (p.y - d.y);
                                nb.y = -n.y;
                            }
                            const V3 qb = pb - c;
                            splat(kScale * dot(qb, r) + kW / 2.0f, -kScale * dot(qb, u) + kH / 2.0f, dot(qb, f), col,
                                  pb, nb, d.item->id);
                        }
                }
            });
        for (auto& th : threads) th.join();
    }

    // The frame an NPC shows when it faces `steps` sixteenths of a turn
    // further: the animation frame (action, direction, index) that gives its
    // current frame is found in ANIM.DAT, then taken in the new direction.
    // The current frame when no animation has it.
    std::pair<const ShapeFrame*, bool> turned_frame(const DrawItem& d, int steps) {
        const std::pair<const ShapeFrame*, bool> same{d.frame, d.mirrored};
        if (!anims_ || steps % 16 == 0) return same;
        const std::uint16_t shape = d.item->shape, frame = d.item->frame;
        const std::uint32_t key = std::uint32_t{shape} << 17 | std::uint32_t{frame} << 1 | (d.mirrored ? 1u : 0u);
        auto it = anim_of_frame_.find(key);
        if (it == anim_of_frame_.end()) {
            std::optional<AnimPose> found;
            for (std::uint16_t a = 0; a < AnimDat::kActions && !found; ++a) {
                const AnimAction* action = anims_->action(shape, a);
                if (!action) continue;
                for (std::size_t dir = 0; dir < action->directions() && !found; ++dir)
                    for (std::size_t i = 0; i < action->frame_count(); ++i) {
                        const AnimFrame& af = action->frames[dir * action->frame_count() + i];
                        if (af.frame == frame && af.mirrored == d.mirrored) {
                            const int dir16 = static_cast<int>(action->sixteen_directions() ? dir : dir * 2);
                            found = AnimPose{a, dir16, i};
                            break;
                        }
                    }
            }
            it = anim_of_frame_.emplace(key, found).first;
        }
        if (!it->second) return same;
        const AnimPose& pose = *it->second;
        const int dir = ((pose.dir16 - steps) % 16 + 16) % 16;
        const AnimAction* action = anims_->action(shape, pose.action);
        const AnimFrame* af = action ? action->frame(dir, pose.index) : nullptr;
        const Shape* sh = shapes_.get(shape);
        if (!af || !sh || af->frame >= sh->frames().size()) return same;
        return {&sh->frames()[af->frame], af->mirrored};
    }

    // Metric position of the surface drawn at a screen pixel.
    std::optional<V3> surface_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= kW || y >= kH || owner_[y * kW + x] < 0) return std::nullopt;
        return pos_[y * kW + x];
    }

    // Where a light goes for a pointer on a screen pixel: off the surface
    // (along its normal) and `height` z units up.
    std::optional<V3> light_spot(int x, int y, float height) const {
        auto p = surface_at(x, y);
        if (!p) return std::nullopt;
        const V3 n = nrm_[y * kW + x];
        const V3 off = std::abs(n.z) > 0.7f ? V3{} : normalize(V3{n.x, n.y, 0}) * 40.0f;
        return *p + off + V3{0, 0, height * kZScale};
    }

    const std::vector<std::uint8_t>& rgb() const { return orbited_ ? out_ : rgb_; }
    std::size_t drawn() const { return list_.size(); }
    std::size_t light_count() const { return lights_used_.size(); }

    // Prints what the pixel shows and, per light, what shadows it.
    void probe(const World& world, int x, int y) const {
        const int i = y * kW + x;
        if (owner_[i] < 0) {
            std::printf("probe (%d, %d): background\n", x, y);
            return;
        }
        const DrawItem& d = list_[owner_[i]];
        const V3 p = pos_[i], n = nrm_[i];
        std::printf("probe (%d, %d): item %u shape %u frame %u box x[%d..%d] y[%d..%d] z[%d..%d]\n"
                    "  surface (%.0f %.0f %.1f) normal (%.2f %.2f %.2f)\n",
                    x, y, d.item->id, d.item->shape, d.item->frame, d.x_left, d.x, d.y_far, d.y, d.z, d.z_top, p.x,
                    p.y, p.z / kZScale, n.x, n.y, n.z);
        for (std::size_t l = 0; l < lights_used_.size(); ++l) {
            const Light& L = lights_used_[l];
            const V3 o = p + n * 3.0f;
            const V3 dir = L.pos - o;
            const V3 inv{1 / dir.x, 1 / dir.y, 1 / dir.z};
            std::printf("  light %zu at (%.0f %.0f %.1f) distance %.0f\n", l, L.pos.x, L.pos.y, L.pos.z / kZScale,
                        std::sqrt(dot(dir, dir)));
            for (std::uint32_t bi : per_light_[l]) {
                const Box& b = boxes_[bi];
                if (b.owner == d.item->id || b.owner == L.owner || !blocks(o, dir, inv, b, true)) continue;
                const Item* it = world.get(b.owner);
                const ShapeInfo* info = it ? world.shape_info(it->shape) : nullptr;
                std::printf("    blocked by item %u shape %u frame %u flags 0x%x typeflags 0x%x box (%.0f %.0f %.1f)-(%.0f "
                            "%.0f %.1f)\n",
                            b.owner, it ? it->shape : 0, it ? it->frame : 0, it ? it->flags : 0,
                            info ? info->flags : 0, b.lo.x, b.lo.y, b.lo.z / kZScale, b.hi.x, b.hi.y,
                            b.hi.z / kZScale);
            }
        }
    }

private:
    static float to_lin(std::uint8_t c) { return std::pow(c / 255.0f, 2.2f); }

    // Same order and blending as draw_display_list, keeping which item owns
    // each pixel (translucent items do not take it).
    void composite(const World& world) {
        std::fill(idx_.begin(), idx_.end(), std::uint8_t{0});
        std::fill(owner_.begin(), owner_.end(), -1);
        for (std::size_t i = 0; i < list_.size(); ++i) {
            const DrawItem& d = list_[i];
            const DecodedFrame& f = decode(cache_, *d.frame);
            const bool translucent = d.shape_flags & ShapeInfo::kTranslucent;
            for (int fy = 0; fy < f.h; ++fy) {
                const int y = d.top + fy;
                if (y < 0 || y >= kH) continue;
                for (int fx = 0; fx < f.w; ++fx) {
                    const int c = f.color[fy * f.w + fx];
                    if (c < 0) continue;
                    const int x = d.mirrored ? d.left + (f.w - 1 - fx) : d.left + fx;
                    if (x < 0 || x >= kW) continue;
                    std::uint8_t& out = idx_[y * kW + x];
                    if (translucent) {
                        out = xform_.blend(static_cast<std::uint8_t>(c), out);
                    } else {
                        out = static_cast<std::uint8_t>(c);
                        owner_[y * kW + x] = static_cast<std::int32_t>(i);
                    }
                }
            }
        }
        (void)world;
    }

    // Position and normal of every owned pixel.
    // 3D point and normal of the frame pixel (fx, fy) of an item, drawn at
    // absolute screen position (sx, sy): where the view ray enters the item's
    // box, or a billboard through the box centre for NPCs and pixels off the box.
    // `face` (when given) gets which: 0 top, 1 +x, 2 +y, 3 billboard.
    void surface(const World& world, const DrawItem& d, float sx, float sy, int fx, int fy, bool bump, V3& p,
                 V3& n, int* face = nullptr) const {
        const float xl = float(d.x_left), xh = float(d.x), yl = float(d.y_far), yh = float(d.y);
        const float zl = float(d.z), zh = float(d.z_top);
        // Points of the view ray: x = (s + 4sx)/2, y = (s - 4sx)/2, z = s/8 - sy.
        const float ux = 2 * xh - 4 * sx, uy = 2 * yh + 4 * sx, uz = 8 * (zh + sy);
        const float lo = std::max({2 * xl - 4 * sx, 2 * yl + 4 * sx, 8 * (zl + sy)});
        const float hi = std::min({ux, uy, uz});
        const ShapeInfo* info = world.shape_info(d.item->shape);
        const bool npc = is_actor(*d.item, info);
        const DecodedFrame& f = cache_.at(d.frame);
        float s;
        if (!npc && hi >= lo - 24) {
            s = hi;
            n = hi == uz ? V3{0, 0, 1} : hi == ux ? V3{1, 0, 0} : V3{0, 1, 0};
            if (face) *face = hi == uz ? 0 : hi == ux ? 1 : 2;
        } else {
            if (face) *face = 3;
            s = (xl + xh + yl + yh) * 0.5f;
            float nx = f.nx[fy * f.w + fx] / 127.0f, ny = f.ny[fy * f.w + fx] / 127.0f;
            if (d.mirrored) nx = -nx;
            const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
            n = normalize(kScreenRight * nx + kScreenUp * ny + kToViewer * nz);
        }
        p = {(s + 4 * sx) * 0.5f, (s - 4 * sx) * 0.5f, (s / 8 - sy) * kZScale};
        if (!bump) return;
        // Bumps from the sprite's own shading (brighter = raised), within the frame.
        const int c = f.color[fy * f.w + fx];
        auto l = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= f.w || y >= f.h) return lum_[c];
            const int v = f.color[y * f.w + x];
            return v < 0 ? lum_[c] : lum_[v];
        };
        float gx = (l(fx + 1, fy) - l(fx - 1, fy)) * 0.5f;
        const float gy = (l(fx, fy + 1) - l(fx, fy - 1)) * 0.5f;
        if (d.mirrored) gx = -gx;
        const V3 tr = normalize(kScreenRight - n * dot(kScreenRight, n));
        const V3 tu = normalize(kScreenUp - n * dot(kScreenUp, n));
        constexpr float k = 2.5f;
        n = normalize(n - tr * (k * gx) + tu * (k * gy));
    }

    // Position and normal of every owned pixel.
    void geometry(const World& world, const Options& opt) {
        parallel_rows(kH, [&](int py) {
            for (int px = 0; px < kW; ++px) {
                const int o = owner_[py * kW + px];
                if (o < 0) continue;
                const DrawItem& d = list_[o];
                const int fx = d.mirrored ? d.right - 1 - px : px - d.left;
                const int fy = py - d.top;
                surface(world, d, view_left_ + px + 0.5f, view_top_ + py + 0.5f, fx, fy, opt.bump,
                        pos_[py * kW + px], nrm_[py * kW + px]);
            }
        });
    }

    // A light over each item with enough colour-cycled (glowing) pixels.
    void add_glow_lights(std::vector<Light>& lights) {
        for (const DrawItem& d : list_) {
            const DecodedFrame& f = decode(cache_, *d.frame);
            int count = 0;
            V3 sum;
            for (std::int16_t c : f.color)
                if (glowing(c)) ++count, sum = sum + V3{srgb_[c].r / 255.0f, srgb_[c].g / 255.0f, srgb_[c].b / 255.0f};
            if (count < 6) continue;
            V3 col = sum * (1.0f / count);
            const float m = std::max({col.x, col.y, col.z, 0.2f});
            col = col * (1.0f / m);
            const float strength = std::min(1.2f, 0.35f + count / 80.0f);
            const V3 centre{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, (d.z_top + 4.0f) * kZScale};
            lights.push_back({centre, col * strength, 260.0f + std::min(count, 200) * 2.0f, d.item->id});
        }
    }

    // A lamp just over the Silencer's head, when he is on the map.
    void add_player_lamp(const World& world, std::vector<Light>& lights) const {
        const Item* p = world.get(1);
        if (!p || p->off_map || p->parent) return;
        const ShapeInfo* info = world.shape_info(p->shape);
        const float x = float(world_from_usecode(p->x)), y = float(world_from_usecode(p->y));
        const float half = info ? info->footpad_x * kFootpadXY * 0.5f : 32.0f;
        const float top = float(p->z + (info ? info->footpad_z * kFootpadZ : 40) + 12);
        lights.push_back({{x - half, y - half, top * kZScale}, V3{1.0f, 0.92f, 0.8f} * 1.3f, 650, p->id});
    }

    // Boxes that can shadow the lit area (wider than the view).
    void occluders(const World& world, const std::vector<Light>& lights) {
        boxes_.clear();
        if (lights.empty()) return;
        V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const Light& l : lights) {
            lo = {std::min(lo.x, l.pos.x - l.radius), std::min(lo.y, l.pos.y - l.radius), -1e9f};
            hi = {std::max(hi.x, l.pos.x + l.radius), std::max(hi.y, l.pos.y + l.radius), 1e9f};
        }
        world.for_each([&](const Item& item) {
            if (item.off_map || item.parent != 0 ||
                (item.flags & (kItemInvisible | kItemContained | kItemEthereal)))
                return;
            const ShapeInfo* info = world.shape_info(item.shape);
            if (!info || info->is(ShapeInfo::kEditor) || info->is(ShapeInfo::kTranslucent)) return;
            // Lasers, electrified floors: light, not matter.
            if (info->is(ShapeInfo::kDamaging)) return;
            if (!info->footpad_x || !info->footpad_y || !info->footpad_z) return;
            const bool mirrored = item.flags & kItemFlipped;
            const float x = float(world_from_usecode(item.x)), y = float(world_from_usecode(item.y));
            const float dx = float((mirrored ? info->footpad_y : info->footpad_x) * kFootpadXY);
            const float dy = float((mirrored ? info->footpad_x : info->footpad_y) * kFootpadXY);
            Box b{{x - dx, y - dy, item.z * kZScale}, {x, y, (item.z + info->footpad_z * kFootpadZ) * kZScale},
                  item.id};
            if (b.hi.x < lo.x || b.lo.x > hi.x || b.hi.y < lo.y || b.lo.y > hi.y) return;
            if (const Shape* shape = shapes_.get(item.shape); shape && item.frame < shape->frames().size()) {
                const ShapeFrame& frame = shape->frames()[item.frame];
                const std::int32_t wx = world_from_usecode(item.x), wy = world_from_usecode(item.y);
                const int sx = (wx - wy) >> 2, sy = ((wx + wy) >> 3) - item.z;
                b.sprite = &decode(cache_, frame);
                b.mirrored = mirrored;
                b.left = mirrored ? sx + frame.xoff - (frame.width - 1) : sx - frame.xoff;
                b.top = sy - frame.yoff;
            }
            boxes_.push_back(b);
        });
        per_light_.assign(lights.size(), {});
        for (std::size_t l = 0; l < lights.size(); ++l) {
            const Light& L = lights[l];
            for (std::uint32_t i = 0; i < boxes_.size(); ++i) {
                const Box& b = boxes_[i];
                if (b.hi.x < L.pos.x - L.radius || b.lo.x > L.pos.x + L.radius || b.hi.y < L.pos.y - L.radius ||
                    b.lo.y > L.pos.y + L.radius || b.hi.z < L.pos.z - L.radius || b.lo.z > L.pos.z + L.radius)
                    continue;
                per_light_[l].push_back(i);
            }
        }
    }

    // Linear colour of palette entry c at a point lit by the lights.
    V3 shade_point(int c, const V3& p, const V3& n, ObjId self, const Options& opt,
                   const std::vector<Light>& lights) const {
        const float ambient = kAmbient[opt.ambient];
        V3 light{ambient, ambient * 1.02f, ambient * 1.12f};
        for (std::size_t l = 0; l < lights.size(); ++l) {
            const Light& L = lights[l];
            const V3 to = L.pos - p;
            const float d2 = dot(to, to);
            if (d2 >= L.radius * L.radius) continue;
            const float d = std::sqrt(d2);
            const float ndl = dot(n, to) / std::max(d, 1e-3f);
            if (ndl <= 0) continue;
            const float f = 1 - d2 / (L.radius * L.radius);
            float k = ndl * f * f;
            if (opt.shadows) {
                const V3 o = p + n * 3.0f;
                const V3 dir = L.pos - o;
                const V3 inv{1 / (std::abs(dir.x) > 1e-6f ? dir.x : 1e-6f),
                             1 / (std::abs(dir.y) > 1e-6f ? dir.y : 1e-6f),
                             1 / (std::abs(dir.z) > 1e-6f ? dir.z : 1e-6f)};
                for (std::uint32_t bi : per_light_[l]) {
                    const Box& b = boxes_[bi];
                    if (b.owner == self || b.owner == L.owner) continue;
                    if (blocks(o, dir, inv, b, opt.sprite_shadows)) {
                        k = 0;
                        break;
                    }
                }
            }
            light = light + L.color * k;
        }
        V3 col{lin_[c].x * light.x, lin_[c].y * light.y, lin_[c].z * light.z};
        if (glowing(c)) col = col + lin_[c];
        return col;
    }

    void shade(const Options& opt, const std::vector<Light>& lights) {
        const float ambient = kAmbient[opt.ambient];
        parallel_rows(kH, [&](int py) {
            for (int px = 0; px < kW; ++px) {
                const int i = py * kW + px;
                std::uint8_t* out = &rgb_[i * 3];
                const int c = idx_[i];
                if (!opt.lit || owner_[i] < 0) {
                    // Background, translucent-only pixels: as drawn.
                    write(out, lin_[c] * (opt.lit ? ambient : 1.0f));
                    continue;
                }
                const V3 n = nrm_[i];
                if (opt.show_normals) {
                    write_normal(out, n);
                    continue;
                }
                write(out, shade_point(c, pos_[i], n, list_[owner_[i]].item->id, opt, lights));
            }
        });
    }

    static void write_normal(std::uint8_t* out, const V3& n) {
        out[0] = static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255);
        out[1] = static_cast<std::uint8_t>((n.y * 0.5f + 0.5f) * 255);
        out[2] = static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255);
    }

    void write(std::uint8_t* out, V3 c) const {
        auto q = [&](float v) { return out_lut_[static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 4095)]; };
        out[0] = q(c.x);
        out[1] = q(c.y);
        out[2] = q(c.z);
    }

    ShapeCache& shapes_;
    const AnimDat* anims_;
    struct AnimPose {
        std::uint16_t action;
        int dir16;
        std::size_t index;
    };
    std::unordered_map<std::uint32_t, std::optional<AnimPose>> anim_of_frame_;
    const XformTable& xform_;
    std::array<V3, 256> lin_{};
    std::array<Rgb, 256> srgb_{};
    std::array<float, 256> lum_{};
    std::array<std::uint8_t, 4096> out_lut_{};
    std::unordered_map<const ShapeFrame*, DecodedFrame> cache_;
    std::vector<DrawItem> list_;
    int view_left_ = 0, view_top_ = 0;
    std::vector<std::uint8_t> idx_;
    std::vector<std::int32_t> owner_;
    std::vector<V3> pos_, nrm_;
    std::vector<std::uint8_t> rgb_;
    std::vector<std::uint8_t> out_;  // reprojected view
    bool orbited_ = false;
    std::vector<Box> boxes_;
    std::vector<std::vector<std::uint32_t>> per_light_;
    std::vector<Light> lights_used_;
};

}  // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    std::filesystem::path data = "data";
    std::size_t map = 1;
    std::string png;
    std::optional<std::pair<int, int>> mouse;
    std::vector<std::pair<int, int>> probes;
    std::vector<std::pair<int, int>> drops;
    int bench = 1;
    std::filesystem::path save;
    std::optional<Camera> forced;
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) png = argv[++i];
        else if (std::strcmp(argv[i], "--mouse") == 0 && i + 2 < argc) {
            const int mx = std::stoi(argv[++i]);
            mouse = std::pair{mx, std::stoi(argv[++i])};
        } else if (std::strcmp(argv[i], "--drop") == 0 && i + 2 < argc) {
            const int dx = std::stoi(argv[++i]);
            drops.emplace_back(dx, std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) bench = std::max(1, std::stoi(argv[++i]));
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) save = argv[++i];
        else if (std::strcmp(argv[i], "--orbit") == 0 && i + 2 < argc) {
            opt.yaw = std::stof(argv[++i]);
            opt.pitch = std::stof(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--probe") == 0 && i + 2 < argc) {
            const int px = std::stoi(argv[++i]);
            probes.emplace_back(px, std::stoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--box-shadows") == 0) opt.sprite_shadows = false;
        else if (std::strcmp(argv[i], "--fixed-actors") == 0) opt.turn_actors = false;
        else if (std::strcmp(argv[i], "--no-back-faces") == 0) opt.back_faces = false;
        else if (std::strcmp(argv[i], "--no-lamp") == 0) opt.player_lamp = false;
        else if (std::strcmp(argv[i], "--flat") == 0) opt.lit = false;
        else if (std::strcmp(argv[i], "--no-shadows") == 0) opt.shadows = false;
        else if (std::strcmp(argv[i], "--no-bump") == 0) opt.bump = false;
        else if (std::strcmp(argv[i], "--no-glow") == 0) opt.glow_lights = false;
        else if (std::strcmp(argv[i], "--normals") == 0) opt.show_normals = true;
        else if (std::strcmp(argv[i], "--ambient") == 0 && i + 1 < argc) opt.ambient = std::stoi(argv[++i]) & 3;
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
        std::optional<AnimDat> anims;
        try {
            anims = AnimDat::load(data / "STATIC/ANIM.DAT");
        } catch (const std::exception&) {
        }
        LitRenderer renderer(shapes, xform, palette, anims ? &*anims : nullptr);
        std::vector<Light> lights;
        float mouse_height = 40;
        int color = 0;

        if (!png.empty()) {
            if (!drops.empty()) {
                // Surfaces first, then the lights dropped on them.
                renderer.render(*world, camera, opt, lights, std::nullopt, mouse_height, kLightColors[color]);
                for (std::size_t k = 0; k < drops.size(); ++k)
                    if (auto p = renderer.light_spot(drops[k].first, drops[k].second, mouse_height))
                        lights.push_back({*p, kLightColors[(k + 1) % 4] * 1.4f, 800});
            }
            const auto t0 = SDL_GetPerformanceCounter();
            for (int k = 0; k < bench; ++k)
                renderer.render(*world, camera, opt, lights, mouse, mouse_height, kLightColors[color]);
            if (bench > 1)
                std::printf("%.1f ms per frame\n",
                            1000.0 * (SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency() / bench);
            for (const auto& [px, py] : probes) renderer.probe(*world, px, py);
            write_png_rgb(png, kW, kH, renderer.rgb().data());
            std::printf("map %zu camera (%d, %d): %zu items, %zu lights -> %s\n", map, camera.x, camera.y,
                        renderer.drawn(), renderer.light_count(), png.c_str());
            return 0;
        }

        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        SDL_Window* window = SDL_CreateWindow("lightview", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW * 2,
                                              kH * 2, SDL_WINDOW_RESIZABLE);
        SDL_Renderer* sdl = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        SDL_RenderSetLogicalSize(sdl, kW, kH);
        SDL_Texture* tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        bool running = true, dragging = false;
        std::optional<std::pair<int, int>> pointer;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_QUIT: running = false; break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (auto p = renderer.light_spot(ev.button.x, ev.button.y, mouse_height))
                            lights.push_back({*p, kLightColors[color] * 1.4f, 800});
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
                    case SDLK_TAB: opt.lit = !opt.lit; break;
                    case SDLK_s: opt.shadows = !opt.shadows; break;
                    case SDLK_b: opt.bump = !opt.bump; break;
                    case SDLK_e: opt.glow_lights = !opt.glow_lights; break;
                    case SDLK_g: opt.sprite_shadows = !opt.sprite_shadows; break;
                    case SDLK_l: opt.player_lamp = !opt.player_lamp; break;
                    case SDLK_q: opt.yaw -= 5; break;
                    case SDLK_w: opt.yaw += 5; break;
                    case SDLK_z: opt.pitch -= 5; break;
                    case SDLK_x: opt.pitch += 5; break;
                    case SDLK_r: opt.yaw = opt.pitch = 0; break;
                    case SDLK_t: opt.turn_actors = !opt.turn_actors; break;
                    case SDLK_f: opt.back_faces = !opt.back_faces; break;
                    case SDLK_n: opt.show_normals = !opt.show_normals; break;
                    case SDLK_a: opt.ambient = (opt.ambient + 1) & 3; break;
                    case SDLK_c: color = (color + 1) % 4; break;
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
            renderer.render(*world, camera, opt, lights, pointer, mouse_height, kLightColors[color]);
            const double ms = 1000.0 * (SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
            SDL_UpdateTexture(tex, nullptr, renderer.rgb().data(), kW * 3);
            SDL_RenderClear(sdl);
            SDL_RenderCopy(sdl, tex, nullptr, nullptr);
            SDL_RenderPresent(sdl);
            char title[256];
            std::snprintf(title, sizeof title,
                          "lightview  map %zu  (%d, %d)  lights %zu  %.1f ms  [S]hadows %s [B]ump %s [E]glow %s "
                          "[A]mbient %d [Tab] %s",
                          map, camera.x, camera.y, renderer.light_count(), ms, opt.shadows ? "on" : "off",
                          opt.bump ? "on" : "off", opt.glow_lights ? "on" : "off", opt.ambient,
                          opt.lit ? "lit" : "original");
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
