#pragma once
// Port addition: dynamic lighting of the isometric world (GameConfig::lighting,
// the lightview tool; docs/LIGHTVIEW.md).
//
// The world is drawn as usual (render.hpp); `capture` then works out, for
// each pixel, the item it shows and a 3D point and normal on that item: where
// the view ray through the pixel enters the item's box (top, +x or +y face),
// or, for characters and pixels off their box, a billboard through the box
// centre with a normal rounded from the sprite's silhouette, bumped by the
// sprite's own shading. `shade` lights those points with point lights (items
// with colour-cycled pixels, flashes and explosions, a lamp over the
// Silencer, extra ones) and shadows cast by the item boxes cut by their
// sprites. Pixels drawn over the world afterwards (status bar, cursor,
// menus) are left as they are.
//
// Lighting works in a "metric" space: the projection is a 2:1 dimetric view
// (camera 45 degrees round, 30 down), where one z unit is kZScale x/y units.
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
    bool show_normals = false;
    // Turned camera (reproject).
    float yaw = 0, pitch = 0;
    bool turn_actors = true;     // characters show the frame of the direction they face the camera with
    bool back_faces = true;      // far box faces made from the near ones
};

class WorldLighting {
public:
    WorldLighting(ShapeCache& shapes, const XformTable& xform, const AnimDat* anims = nullptr);
    ~WorldLighting();
    WorldLighting(const WorldLighting&) = delete;
    WorldLighting& operator=(const WorldLighting&) = delete;

    // The world just drawn on `screen` from `list` (in draw order), with the
    // view's top-left corner at (view_left, view_top).
    void capture(const World& world, const std::vector<DrawItem>& list, const Surface8& screen, int view_left,
                 int view_top, const Palette& palette, const LightingOptions& opt);
    bool captured() const { return width_ > 0; }
    void forget() { width_ = height_ = 0; }

    // The lit picture (0xAARRGGBB, `screen` size) of `screen`: its pixels
    // still showing the captured world are lit, the others shown as they are.
    // False (nothing written) when the capture does not fit `screen`.
    bool shade(const World& world, const Surface8& screen, const Palette& palette, const LightingOptions& opt,
               std::vector<PointLight> extra, std::uint32_t* argb);

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
    void probe(const World& world, int x, int y) const;

private:
    struct Decoded;
    struct Box;
    class Pool;

    const Decoded& decode(const ShapeFrame& frame);
    const Decoded& decoded(const ShapeFrame* frame) const { return *decoded_.at(frame); }
    void surface(const World& world, const DrawItem& d, float sx, float sy, int fx, int fy, bool bump, Vec3& p,
                 Vec3& n, int* face = nullptr) const;
    void gather_lights(const World& world, const LightingOptions& opt, std::vector<PointLight>& lights);
    void gather_occluders(const World& world);
    bool occluded(const Vec3& p, const Vec3& n, ObjId self, std::size_t light, bool sprite_cut,
                  const std::vector<std::uint32_t>& boxes) const;
    Vec3 shade_point(int c, const Vec3& p, const Vec3& n, ObjId self, const LightingOptions& opt,
                     const std::vector<std::uint32_t>& tile_lights,
                     const std::vector<std::vector<std::uint32_t>>* tile_boxes) const;
    std::uint32_t pack(const Vec3& c) const;
    void set_palette(const Palette& palette);
    std::pair<const ShapeFrame*, bool> turned_frame(const DrawItem& d, int steps);

    ShapeCache& shapes_;
    const XformTable& xform_;
    const AnimDat* anims_;
    std::unique_ptr<Pool> pool_;
    std::unordered_map<const ShapeFrame*, std::unique_ptr<Decoded>> decoded_;

    // Capture.
    std::vector<DrawItem> list_;
    int view_left_ = 0, view_top_ = 0, width_ = 0, height_ = 0;
    std::vector<std::uint8_t> world_;     // the world picture
    std::vector<std::int32_t> owner_;     // index in list_, -1 none
    std::vector<std::uint8_t> emissive_;  // flash / explosion pixel
    std::vector<Vec3> pos_, nrm_;

    // Shading.
    std::array<Vec3, 256> lin_{};
    std::array<float, 256> lum_{};
    std::array<std::uint32_t, 256> flat_{};  // palette colours as they are
    std::array<std::uint8_t, 4096> out_lut_{};
    std::vector<PointLight> lights_;
    std::vector<Box> boxes_;
    std::vector<std::vector<std::uint32_t>> per_light_;
    std::vector<std::uint32_t> lit_;  // last shaded picture, for reproject

    struct AnimPose {
        std::uint16_t action;
        int dir16;
        std::size_t index;
    };
    std::unordered_map<std::uint32_t, std::optional<AnimPose>> anim_of_frame_;
};

}  // namespace crusader
