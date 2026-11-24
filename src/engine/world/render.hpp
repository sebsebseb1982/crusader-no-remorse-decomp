#pragma once
// Isometric world rendering (DISPLAY.C / SPRITE.C / DLIST.C).
//
// Coordinates: world x/y = 2 * usecode x/y (what FIXED.DAT and getX store);
// z is unscaled. An item's location is the corner of its box nearest to the
// viewer at the bottom; the box extends towards -x, -y and +z by the shape's
// footpad (TYPEFLAG.DAT) * 32 for x/y and * 8 for z (checked against tile and
// wall frame sizes). Projection of a world point:
//   sx = x/4 - y/4        sy = x/8 + y/8 - z
// A frame is drawn with its anchor (xoff, yoff) on the projected location.
//
// Draw order: items whose screen rectangles overlap are ordered by the
// original's comparator (DLIST 11A0:0000): boxes apart on x, y, then z are
// drawn back to front; intersecting boxes by the depth of their far bottom
// corner (x + y + 2z). The constraints are then topologically sorted.

#include "core/flex.hpp"
#include "core/shape.hpp"
#include "core/surface.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace crusader {

constexpr int kFootpadXY = 32;  // world units per footpad unit (x, y)
constexpr int kFootpadZ = 8;    // world units per footpad unit (z)

// Item flags (Item::flags) used by rendering.
enum ItemFlag : std::uint16_t {
    kItemInvisible = 0x0010,
    kItemFlipped = 0x0020,  // drawn mirrored
};

struct Camera {
    std::int32_t x = 0;  // world units
    std::int32_t y = 0;
    std::int32_t z = 0;
};

inline std::int32_t world_from_usecode(std::int32_t v) { return v * 2; }

// Parses shapes of an archive on first use.
class ShapeCache {
public:
    ShapeCache(const FlexArchive& archive, ShapeLayout layout) : archive_(archive), layout_(layout) {}
    const Shape* get(std::uint16_t shape);

private:
    const FlexArchive& archive_;
    ShapeLayout layout_;
    std::unordered_map<std::uint16_t, std::optional<Shape>> cache_;
};

struct DrawItem {
    const Item* item;
    const ShapeFrame* frame;
    int sx, sy;  // projected location on screen
    bool mirrored;
    // World box.
    std::int32_t x, y, z, x_left, y_far, z_top;
    // Screen rectangle.
    int left, top, right, bottom;
    int sx_left;                 // screen x of the box's left corner (x_left, y)
    std::uint32_t shape_flags;   // ShapeInfo::flags, for tie-breaking
};

struct RenderStats {
    std::size_t candidates = 0;   // items in view
    std::size_t drawn = 0;
    std::size_t missing_shape = 0;
    std::size_t cycle_edges = 0;  // ordering constraints dropped to break cycles
    // First dropped constraints: (item drawn first, item that should have been).
    std::vector<std::pair<const Item*, const Item*>> cycle_samples;
};

// Ordering constraint between two items: true when `a` must be drawn before
// `b`, false for the reverse, nullopt when they do not cover each other.
std::optional<bool> draw_before(const DrawItem& a, const DrawItem& b);

// Top-left corner of the view in projected coordinates (pixels).
struct View {
    int left = 0, top = 0;
};

// Port addition (glass debris, glass_debris.hpp): sprites that are not items,
// drawn and ordered among them, and items left out. A sprite has a box in
// world units (its location the corner nearest to the viewer at the bottom,
// as an item's) and `proxy` stands for it in the display list.
struct ExtraSprite {
    const Item* proxy = nullptr;
    const ShapeFrame* frame = nullptr;
    bool mirrored = false;
    std::int32_t x = 0, y = 0, z = 0, x_left = 0, y_far = 0, z_top = 0;
};
struct DisplayExtras {
    std::vector<ExtraSprite> sprites;
    std::vector<ObjId> hidden;  // sorted
};

// Items of `world` visible on a w x h screen whose top-left corner is `view`,
// in draw order; with `extras`, its sprites among them and its hidden items
// left out.
std::vector<DrawItem> build_display_list(const World& world, ShapeCache& shapes, const View& view, int w, int h,
                                         RenderStats* stats = nullptr, const DisplayExtras* extras = nullptr);
// Port addition (lighting): the items whose sprite falls on that screen
// widened by `margin` pixels on each side, placed as in the display list
// (screen coordinates from `view`), unsorted.
std::vector<DrawItem> items_around_view(const World& world, ShapeCache& shapes, const View& view, int w, int h,
                                        int margin);
// Same, centred on `camera` (world units).
std::vector<DrawItem> build_display_list(const World& world, ShapeCache& shapes, const Camera& camera, int w,
                                         int h, RenderStats* stats = nullptr);

// Items of translucent shapes are blended through `xform` (opaque without it).
// `after` runs right after each item is drawn (overlays drawn with it).
void draw_display_list(Surface8& screen, const std::vector<DrawItem>& list, const XformTable* xform = nullptr,
                       const std::function<void(Surface8&, const DrawItem&)>& after = {});

// Indices (in draw order) of the items with an opaque pixel at screen (x, y).
std::vector<std::size_t> pick(const std::vector<DrawItem>& list, int x, int y);

}  // namespace crusader
