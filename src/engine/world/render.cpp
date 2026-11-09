#include "engine/world/render.hpp"

#include <algorithm>
#include <optional>

namespace crusader {

// @orig none: shape cache
const Shape* ShapeCache::get(std::uint16_t shape) {
    auto it = cache_.find(shape);
    if (it == cache_.end()) {
        std::optional<Shape> parsed;
        const auto data = archive_.entry(shape);
        if (!data.empty()) {
            try {
                parsed = Shape::parse(data, layout_);
            } catch (const std::exception&) {
                parsed.reset();
            }
        }
        it = cache_.emplace(shape, std::move(parsed)).first;
    }
    return it->second ? &*it->second : nullptr;
}

namespace {

// World units (2 x usecode). Same rounding as the original (SHAPHAND.C,
// usecode units): the difference or sum is shifted, not each term.
int project_x(std::int32_t x, std::int32_t y) { return (x - y) >> 2; }
int project_y(std::int32_t x, std::int32_t y, std::int32_t z) { return ((x + y) >> 3) - z; }

bool screen_overlap(const DrawItem& a, const DrawItem& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// A box in the original's display units (DLIST 11A0:095D): x and y are the
// item's usecode x / 2 (world / 4), footpads * 8; a shape of height 0 is 1
// high unless it is fixed (TYPEFLAG byte 0 bit 0).
struct SortBox {
    int x, y, z, dx, dy, dz;
};

// @orig 11A0:095D (the box part of the display entry)
SortBox sort_box(const DrawItem& d) {
    int dz = d.z_top - d.z;
    if (dz == 0 && !(d.shape_flags & ShapeInfo::kFixed)) dz = 1;
    return {d.x >> 2, d.y >> 2, d.z, (d.x - d.x_left) >> 2, (d.y - d.y_far) >> 2, dz};
}

enum class Order { kNone, kOtherFirst, kThisFirst };

// Draw order of two items (DLIST 11A0:0000, US 0035:0000). First whether
// their screen hexagons overlap, tested on the three screen axes x - z,
// y - z and x - y (strictly: touching boxes get no constraint). Then the
// first axis on which the boxes are apart (x, y, then z) decides; boxes
// that intersect are ordered by the sum x + y + 2z of their far bottom
// corners, the smaller first. Equal sums leave them unordered. No shape
// property (roof, land, flat, translucent) takes part.
// @orig 11A0:0000
Order compare(const DrawItem& self, const DrawItem& other) {
    const SortBox a = sort_box(self), b = sort_box(other);
    const bool overlap = a.x - a.z - a.dx - a.dz < b.x - b.z && b.x - b.z - b.dx - b.dz < a.x - a.z &&
                         a.y - a.z - a.dy - a.dz < b.y - b.z && b.y - b.z - b.dy - b.dz < a.y - a.z &&
                         a.x - a.y - a.dx < b.x - b.y + b.dy && b.x - b.y - b.dx < a.x - a.y + a.dy;
    if (!overlap) return Order::kNone;
    if (b.x <= a.x - a.dx) return Order::kOtherFirst;
    if (a.x <= b.x - b.dx) return Order::kThisFirst;
    if (b.y <= a.y - a.dy) return Order::kOtherFirst;
    if (a.y <= b.y - b.dy) return Order::kThisFirst;
    if (b.z + b.dz <= a.z && b.z < a.z + a.dz) return Order::kOtherFirst;
    if (a.z + a.dz <= b.z && a.z < b.z + b.dz) return Order::kThisFirst;
    const int far_a = a.x - a.dx + a.y - a.dy + 2 * a.z;
    const int far_b = b.x - b.dx + b.y - b.dy + 2 * b.z;
    if (far_b < far_a) return Order::kOtherFirst;
    if (far_a < far_b) return Order::kThisFirst;
    return Order::kNone;
}

}  // namespace

// The edge that 1198:08A0 adds between two display entries.
// @orig 1198:08A0
std::optional<bool> draw_before(const DrawItem& a, const DrawItem& b) {
    if (!screen_overlap(a, b)) return std::nullopt;
    switch (compare(a, b)) {
    case Order::kThisFirst: return true;
    case Order::kOtherFirst: return false;
    case Order::kNone: break;
    }
    return std::nullopt;
}

std::vector<DrawItem> build_display_list(const World& world, ShapeCache& shapes, const Camera& camera, int w,
                                         int h, RenderStats* stats) {
    return build_display_list(world, shapes, View{project_x(camera.x, camera.y) - w / 2,
                                                  project_y(camera.x, camera.y, camera.z) - h / 2},
                              w, h, stats);
}

// @orig ?: display list (DLIST.C), not located
std::vector<DrawItem> build_display_list(const World& world, ShapeCache& shapes, const View& view, int w, int h,
                                         RenderStats* stats) {
    RenderStats local;
    RenderStats& st = stats ? *stats : local;
    st = {};
    // Screen = projection - view corner.
    const int cam_sx = view.left + w / 2, cam_sy = view.top + h / 2;

    std::vector<DrawItem> items;
    world.for_each([&](const Item& item) {
        if (item.off_map || item.parent != 0 || (item.flags & (kItemInvisible | kItemContained | kItemEthereal))) return;
        const ShapeInfo* info = world.shape_info(item.shape);
        if (info && info->is(ShapeInfo::kEditor)) return;  // editor-only markers
        const Shape* shape = shapes.get(item.shape);
        if (!shape || item.frame >= shape->frames().size()) {
            ++st.missing_shape;
            return;
        }
        DrawItem d{};
        d.item = &item;
        d.frame = &shape->frames()[item.frame];
        d.mirrored = item.flags & kItemFlipped;
        d.x = world_from_usecode(item.x);
        d.y = world_from_usecode(item.y);
        d.z = item.z;
        const int fx = info ? info->footpad_x : 0;
        const int fy = info ? info->footpad_y : 0;
        // A mirrored item has its footpad x/y swapped.
        d.x_left = d.x - (d.mirrored ? fy : fx) * kFootpadXY;
        d.y_far = d.y - (d.mirrored ? fx : fy) * kFootpadXY;
        d.z_top = d.z + (info ? info->footpad_z : 0) * kFootpadZ;
        d.sx = project_x(d.x, d.y) - cam_sx + w / 2;
        d.sy = project_y(d.x, d.y, d.z) - cam_sy + h / 2;
        d.sx_left = project_x(d.x_left, d.y) - cam_sx + w / 2;
        d.shape_flags = info ? info->flags : 0;
        const int fw = d.frame->width, fh = d.frame->height;
        d.left = d.mirrored ? d.sx + d.frame->xoff - (fw - 1) : d.sx - d.frame->xoff;
        d.top = d.sy - d.frame->yoff;
        d.right = d.left + fw;
        d.bottom = d.top + fh;
        if (d.right <= 0 || d.bottom <= 0 || d.left >= w || d.top >= h) return;
        items.push_back(d);
    });
    st.candidates = items.size();

    // Dependency graph between screen-overlapping items, then DFS topological
    // order (cycles, which the original also had to break, are cut by the DFS).
    // Ties are broken by id: the order of unconstrained items follows from the
    // order of this list, and must not change as the camera moves.
    std::sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.left != b.left) return a.left < b.left;
        return a.item->id < b.item->id;
    });
    const std::size_t n = items.size();
    std::vector<std::vector<std::uint32_t>> before(n);  // before[i]: items to draw before i
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n && items[j].left < items[i].right; ++j) {
            if (!screen_overlap(items[i], items[j])) continue;
            const Order order = compare(items[i], items[j]);
            if (order == Order::kThisFirst) before[j].push_back(static_cast<std::uint32_t>(i));
            else if (order == Order::kOtherFirst) before[i].push_back(static_cast<std::uint32_t>(j));
        }
    }
    std::vector<std::uint8_t> state(n, 0);  // 0 new, 1 visiting, 2 done
    std::vector<DrawItem> ordered;
    ordered.reserve(n);
    std::vector<std::pair<std::uint32_t, std::size_t>> stack;
    // Visit in depth order so unconstrained items keep a sensible order: low z
    // first, flat before thick (the original's list order, ScummVM's
    // SortItem::listLessThan), then by depth and, for a total order, by id.
    std::vector<std::uint32_t> roots(n);
    for (std::uint32_t i = 0; i < n; ++i) roots[i] = i;
    std::sort(roots.begin(), roots.end(), [&](std::uint32_t a, std::uint32_t b) {
        const auto& A = items[a];
        const auto& B = items[b];
        if (A.z != B.z) return A.z < B.z;
        const bool flat_a = A.z == A.z_top, flat_b = B.z == B.z_top;
        if (flat_a != flat_b) return flat_a;
        if (A.x + A.y != B.x + B.y) return A.x + A.y < B.x + B.y;
        return A.item->id < B.item->id;
    });
    for (std::uint32_t root : roots) {
        if (state[root]) continue;
        stack.emplace_back(root, 0);
        state[root] = 1;
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < before[node].size()) {
                const std::uint32_t dep = before[node][next++];
                if (state[dep] == 0) {
                    state[dep] = 1;
                    stack.emplace_back(dep, 0);
                } else if (state[dep] == 1) {
                    ++st.cycle_edges;  // dep is an ancestor: constraint cannot hold
                    if (st.cycle_samples.size() < 12) st.cycle_samples.emplace_back(items[node].item, items[dep].item);
                }
            } else {
                state[node] = 2;
                ordered.push_back(items[node]);
                stack.pop_back();
            }
        }
    }
    st.drawn = ordered.size();
    return ordered;
}

// @orig ?: item under the mouse, not located
std::vector<std::size_t> pick(const std::vector<DrawItem>& list, int x, int y) {
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const DrawItem& d = list[i];
        if (x < d.left || x >= d.right || y < d.top || y >= d.bottom) continue;
        const int fy = y - d.top;
        const int fx = d.mirrored ? (d.right - 1 - x) : (x - d.left);
        if (d.frame->pixel(fx, fy) >= 0) hits.push_back(i);
    }
    return hits;
}

// @orig 11A0:02E4 (blitter choice; the walk over the list is not located)
void draw_display_list(Surface8& screen, const std::vector<DrawItem>& list, const XformTable* xform,
                       const std::function<void(Surface8&, const DrawItem&)>& after) {
    for (const DrawItem& d : list) {
        // TYPEFLAG translucent shapes (windows, force fields...) go through
        // the translucent blitter (asm_blit_4, mirrored asm_blit_5).
        const bool translucent = d.shape_flags & ShapeInfo::kTranslucent;
        d.frame->draw(screen, d.sx, d.sy, d.mirrored, translucent ? xform : nullptr);
        if (after) after(screen, d);
    }
}

}  // namespace crusader
