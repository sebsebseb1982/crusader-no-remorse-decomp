#pragma once
// Port addition: dynamic lighting of the isometric world (GameConfig::lighting,
// the lightview tool; docs/LIGHTVIEW.md).
//
// The world is drawn as usual (render.hpp); `capture` then records which item
// shows at each pixel. A pixel's 3D point and normal come from that item:
// where the view ray through the pixel enters the item's box (top, +x or +y
// face), or, for characters and pixels off their box, a billboard through the
// box centre with a normal rounded from the sprite's silhouette, bumped by
// the sprite's own shading. `shade` lights those points with point lights
// (items with colour-cycled pixels, flashes and explosions, a lamp over the
// Silencer, extra ones) and shadows cast by the item boxes cut by their
// sprites, then adds the smoke of explosions and shots (volumes of noise,
// lit by the same lights). Pixels drawn over the world afterwards (status
// bar, cursor, menus) are left as they are.
//
// The shading runs on a GPU when one is given (LightingGpu, implemented by
// the platform layer with OpenGL), else on the processor's cores.
//
// Lighting works in a "metric" space: the projection is a 2:1 dimetric view
// (camera 45 degrees round, 30 down), where one z unit is kLightZScale x/y
// units.
//
// `reproject` (experiment, lightview only) shows the scene from a turned
// camera: every sprite pixel around the view becomes a lit 3D point.

#include "core/anim_dat.hpp"
#include "core/palette.hpp"
#include "core/surface.hpp"
#include "core/xform.hpp"
#include "engine/world/render.hpp"
#include "engine/world/world.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace crusader {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// One z unit in x/y units of the lighting space.
constexpr float kLightZScale = 3.266f;

struct PointLight {
    Vec3 pos;  // lighting space
    Vec3 color;
    float radius = 700;
    ObjId owner = 0;  // the item giving the light: never shadows it
};

struct LightingOptions {
    float ambient = 0.45f;
    bool shadows = true;
    bool sprite_shadows = true;  // occluders cut by their sprite (else whole boxes)
    bool bump = true;
    bool glow_lights = true;     // items with colour-cycled pixels give light
    bool flash_lights = true;    // flashes, explosions, shots give light
    bool player_lamp = true;     // a lamp over the Silencer (item 1)
    bool smoke = true;           // explosions and shots leave smoke
    bool show_normals = false;
    // Turned camera (reproject).
    float yaw = 0, pitch = 0;
    bool turn_actors = true;     // characters show the frame of the direction they face the camera with
    bool back_faces = true;      // far box faces made from the near ones
};

// A puff of smoke: a ball of noise that rises, grows and thins out.
struct SmokePuff {
    enum Kind : std::uint8_t { kExplosion, kShot, kTrail };
    Vec3 pos;                 // centre at birth, lighting space
    Vec3 drift;               // lighting-space units a second
    float radius0 = 40, radius1 = 80;  // at birth, at the end
    float density = 1;        // at birth
    float life = 3;           // seconds
    float seed = 0;
    std::uint32_t born = 0;   // game tick
    Kind kind = kShot;
};

// Smoke puff state at a time, as the shaders take it.
struct PuffState {
    Vec3 centre;
    float radius;
    float density;  // extinction at the centre, per lighting-space unit
    float heat;     // 0..1: glow of a young explosion
    float grey;     // albedo
    float seed;
    float time;     // seconds since birth (noise animation)
};

// Everything a GPU needs to shade a frame (WorldLighting::shade).
struct LitFrame {
    // Per pixel: the item index, or one of the codes.
    static constexpr std::uint16_t kFlat = 0xFFFF;      // drawn over the world: as it is
    static constexpr std::uint16_t kAmbient = 0xFFFE;   // background: ambient only
    static constexpr std::uint16_t kEmissive = 0xFFFD;  // flash, explosion: as it is, under the smoke
    static constexpr std::uint16_t kMaxItems = 0xFFF0;

    int width = 0, height = 0;
    int view_left = 0, view_top = 0;
    const std::uint8_t* screen = nullptr;  // palette indices
    std::vector<std::uint16_t> code;
    std::array<std::uint32_t, 256> palette{};  // 0x00RRGGBB
    // 16 floats an item: box (x_left, x, y_far, y), (z, z_top, flags, id),
    // atlas rectangle (x, y, w, h), frame rectangle (left, top, 0, 0) on screen.
    std::vector<float> items;
    // 12 floats a light: position, radius; colour, owner; first and count of its occluders.
    std::vector<float> lights;
    // 16 floats a box: lo, owner; hi, mirrored; atlas rectangle; sprite left, top, has sprite.
    std::vector<float> boxes;
    std::vector<std::int32_t> light_boxes;  // occluder indices, per light ranges
    // 12 floats a puff (PuffState: centre, radius, density, heat, grey, seed, time), far to near.
    std::vector<float> puffs;
    // Sprites to add to the atlas: RGBA, R opaque, G / B the silhouette normal + 128.
    struct Upload {
        int x, y, w, h;
        std::vector<std::uint8_t> rgba;
    };
    std::vector<Upload> uploads;
    bool atlas_reset = false;
    static constexpr int kAtlasSize = 4096;
    LightingOptions options;
    // Item flags.
    static constexpr int kActor = 1, kMirrored = 2;
};

// A GPU that shades frames (platform layer).
class LightingGpu {
public:
    virtual ~LightingGpu() = default;
    // The lit frame in `argb` (0xAARRGGBB). False on failure: the caller
    // shades it itself.
    virtual bool shade(const LitFrame& frame, std::uint32_t* argb) = 0;
};

class WorldLighting {
public:
    WorldLighting(ShapeCache& shapes, const XformTable& xform, const AnimDat* anims = nullptr);
    ~WorldLighting();
    WorldLighting(const WorldLighting&) = delete;
    WorldLighting& operator=(const WorldLighting&) = delete;

    // Shade on this GPU (null: on the processor).
    void set_gpu(LightingGpu* gpu) { gpu_ = gpu; }
    bool used_gpu() const { return used_gpu_; }

    // The world just drawn on `screen` from `list` (in draw order), with the
    // view's top-left corner at (view_left, view_top), at game tick `ticks`
    // (the smoke's clock).
    void capture(const World& world, const std::vector<DrawItem>& list, const Surface8& screen, int view_left,
                 int view_top, const Palette& palette, const LightingOptions& opt, std::uint32_t ticks);
    bool captured() const { return width_ > 0; }
    void forget() { width_ = height_ = 0; }

    // The lit picture (0xAARRGGBB, `screen` size) of `screen`: its pixels
    // still showing the captured world are lit, the others shown as they are.
    // False (nothing written) when the capture does not fit `screen`.
    bool shade(const World& world, const Surface8& screen, const Palette& palette, const LightingOptions& opt,
               std::vector<PointLight> extra, std::uint32_t* argb);

    // A puff of smoke of `kind` centred at `at` (lighting space), now.
    void add_puff(const Vec3& at, SmokePuff::Kind kind, float scale = 1);
    void clear_smoke() { puffs_.clear(); }
    std::size_t puff_count() const { return puffs_.size(); }

    // Experiment: the captured scene from a turned camera (opt.yaw, opt.pitch)
    // around `centre` (world units), lit as by the last shade. Holes in magenta.
    void reproject(const World& world, const Camera& centre, const LightingOptions& opt, std::uint32_t* argb);

    // Lighting-space point shown at a screen pixel, if any.
    std::optional<Vec3> surface_at(int x, int y) const;
    // Where a light goes for a pointer on a pixel: off the surface along its
    // normal, `height` z units up.
    std::optional<Vec3> light_spot(int x, int y, float height) const;
    const std::vector<PointLight>& lights() const { return lights_; }
    // Prints what a pixel shows and, light by light, what shadows it.
    void probe(const World& world, int x, int y);

private:
    struct Decoded;
    struct Box;
    class Pool;

    const Decoded& decode(const ShapeFrame& frame);
    const Decoded& decoded(const ShapeFrame* frame) const { return *decoded_.at(frame); }
    void surface(const DrawItem& d, bool actor, float sx, float sy, int fx, int fy, bool bump, Vec3& p, Vec3& n,
                 int* face = nullptr) const;
    void point_at(int x, int y, Vec3& p, Vec3& n) const;
    void geometry(const LightingOptions& opt);
    void gather_lights(const World& world, const LightingOptions& opt, std::vector<PointLight>& lights);
    void gather_occluders(const World& world);
    void spawn_smoke();
    void puff_states(std::vector<PuffState>& out) const;
    bool occluded(const Vec3& p, const Vec3& n, ObjId self, std::size_t light, bool sprite_cut,
                  const std::vector<std::uint32_t>& boxes) const;
    Vec3 shade_point(int c, const Vec3& p, const Vec3& n, ObjId self, const LightingOptions& opt,
                     const std::vector<std::uint32_t>& tile_lights,
                     const std::vector<std::vector<std::uint32_t>>* tile_boxes) const;
    Vec3 smoke_over(Vec3 colour, int px, int py, bool clip, float surface_s, const LightingOptions& opt,
                    const std::vector<std::uint32_t>& puffs) const;
    void shade_cpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb);
    bool shade_gpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb);
    void atlas_place(const Decoded& d, LitFrame& frame);
    std::uint32_t pack(const Vec3& c) const;
    void set_palette(const Palette& palette);
    std::pair<const ShapeFrame*, bool> turned_frame(const DrawItem& d, int steps);

    ShapeCache& shapes_;
    const XformTable& xform_;
    const AnimDat* anims_;
    LightingGpu* gpu_ = nullptr;
    bool used_gpu_ = false;
    std::unique_ptr<Pool> pool_;
    std::unordered_map<const ShapeFrame*, std::unique_ptr<Decoded>> decoded_;

    // Capture.
    std::vector<DrawItem> list_;
    int view_left_ = 0, view_top_ = 0, width_ = 0, height_ = 0;
    std::uint32_t ticks_ = 0;
    std::vector<std::uint8_t> world_;     // the world picture
    std::vector<std::int32_t> owner_;     // index in list_, -1 none
    std::vector<std::uint8_t> actor_;     // per list_ item: a character (billboard)
    std::vector<std::uint8_t> emissive_;  // flash / explosion pixel
    std::vector<Vec3> pos_, nrm_;         // CPU shading only

    // Shading.
    std::array<Vec3, 256> lin_{};
    std::array<float, 256> lum_{};
    std::array<std::uint32_t, 256> flat_{};  // palette colours as they are
    std::array<std::uint8_t, 4096> out_lut_{};
    std::vector<PointLight> lights_;
    std::vector<Box> boxes_;
    std::vector<std::vector<std::uint32_t>> per_light_;
    std::vector<std::uint32_t> lit_;  // last shaded picture, for reproject

    // Smoke.
    std::vector<SmokePuff> puffs_;
    std::vector<PuffState> puff_now_;  // far to near
    std::unordered_map<ObjId, std::uint16_t> sprites_seen_;  // sprite item -> shape
    std::uint32_t puff_serial_ = 0;

    // GPU atlas: where each decoded frame is (x, y), shelf packing.
    std::unordered_map<const Decoded*, std::pair<int, int>> atlas_;
    int shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
    bool atlas_reset_ = true;
    bool atlas_full_ = false;
    LitFrame frame_;

    struct AnimPose {
        std::uint16_t action;
        int dir16;
        std::size_t index;
    };
    std::unordered_map<std::uint32_t, std::optional<AnimPose>> anim_of_frame_;
};

// The smoke's noise (value noise, three octaves), shared with the shaders:
// 0..1 at lighting-space point p for a puff of seed `seed` at time t.
float smoke_noise(const Vec3& p, float seed, float t);

}  // namespace crusader
