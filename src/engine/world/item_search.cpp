// Usecode `loop` searches (ITEMFIND.C). The original builds a search object
// on the script's stack and advances it at each `loopnext`; here the matching
// items are collected when the loop starts, in the same order.

#include "engine/world/world.hpp"

#include <algorithm>
#include <array>

namespace crusader {

namespace {

// Opcodes of the loop filter (ITEMFIND.C 010C). Values are pushed as 16-bit
// words; comparisons are signed. `!` is a bitwise NOT, as in the original.
enum : std::uint8_t {
    kPushFalse = 0x00,
    kPushTrue = 0x01,
    kNot = '!',
    kPushId = '#',
    kEnd = '$',
    kPushImmediate = '%',  // followed by a u16
    kAnd = '&',
    kLessEqual = '(',
    kGreaterEqual = ')',
    kPushQuality = '*',
    kOr = '+',
    kPushFamily = ':',  // 14 for shapes >= 0x800
    kLess = '<',
    kEqual = '=',
    kGreater = '>',
    kPushFlags = '?',
    kPushShape = '@',
    kPushFrame = '`',
    // 'A'..'_': shape in a list of (op - 0x40) u16; 'a'..0x7F: frame in a list of (op - 0x60) u16.
};

constexpr std::size_t kScriptSize = 0x20;  // bytes kept in the search object
constexpr std::size_t kStackSize = 0x20;

}  // namespace

// @orig 10C8:010C
bool World::matches_loop_script(const Item& item, std::span<const std::uint8_t> script) const {
    if (item.sprite_only) return false;  // port-only sprite items are not items
    // The search object holds 32 bytes; past the given script they are zero.
    std::array<std::uint8_t, kScriptSize + 2> code{};
    std::copy_n(script.begin(), std::min(script.size(), kScriptSize), code.begin());
    auto word = [&](std::size_t at) { return static_cast<std::uint16_t>(code[at] | code[at + 1] << 8); };

    std::array<std::uint16_t, kStackSize> stack{};
    std::size_t sp = 0;
    bool broken = false;  // overflow or underflow: a fatal assertion in the original
    auto push = [&](std::uint16_t v) {
        if (sp < kStackSize) stack[sp++] = v;
        else broken = true;
    };
    auto binary = [&](auto op) {
        if (sp < 2) {
            broken = true;
            return;
        }
        const auto a = static_cast<std::int16_t>(stack[sp - 2]), b = static_cast<std::int16_t>(stack[sp - 1]);
        stack[sp - 2] = static_cast<std::uint16_t>(op(a, b));
        --sp;
    };

    std::size_t pc = 0;
    while (pc < kScriptSize && !broken) {
        const std::uint8_t op = code[pc++];
        if (op == kEnd) break;
        switch (op) {
            case kPushFalse: push(0); break;
            case kPushTrue: push(1); break;
            case kPushId: push(item.id); break;
            case kPushQuality: push(item.quality); break;
            case kPushFlags: push(item.flags); break;
            case kPushShape: push(item.shape); break;
            case kPushFrame: push(item.frame); break;
            case kPushFamily: {
                const ShapeInfo* info = item.shape < 0x800 ? shape_info(item.shape) : nullptr;
                push(item.shape < 0x800 ? (info ? info->family : 0) : 14);
                break;
            }
            case kPushImmediate:
                push(word(pc));
                pc += 2;
                break;
            case kNot:
                if (sp == 0) broken = true;
                else stack[sp - 1] = static_cast<std::uint16_t>(~stack[sp - 1]);
                break;
            case kAnd: binary([](int a, int b) { return a & b; }); break;
            case kOr: binary([](int a, int b) { return a | b; }); break;
            case kLess: binary([](int a, int b) { return a < b; }); break;
            case kGreater: binary([](int a, int b) { return a > b; }); break;
            case kEqual: binary([](int a, int b) { return a == b; }); break;
            case kLessEqual: binary([](int a, int b) { return a <= b; }); break;
            case kGreaterEqual: binary([](int a, int b) { return a >= b; }); break;
            default:
                if (op >= 0x41 && op <= 0x5F) {  // shape list
                    const std::size_t n = op - 0x40u;
                    bool found = false;
                    for (std::size_t i = 0; i < n && pc + 2 * i + 1 < code.size(); ++i) {
                        found |= word(pc + 2 * i) == item.shape;
                    }
                    push(found);
                    pc += 2 * n;
                } else if (op >= 0x61 && op <= 0x7F) {  // frame list
                    const std::size_t n = op - 0x60u;
                    bool found = false;
                    for (std::size_t i = 0; i < n && pc + 2 * i + 1 < code.size(); ++i) {
                        found |= word(pc + 2 * i) == item.frame;
                    }
                    push(found);
                    pc += 2 * n;
                }
                break;  // other bytes are ignored
        }
    }
    if (broken) return false;
    return sp == 0 || stack[sp - 1] != 0;
}

// @orig 10C8:0771 10C8:0A8E
std::vector<ObjId> World::area_search(ObjId centre, std::uint16_t range, std::span<const std::uint8_t> script) const {
    std::vector<ObjId> found;
    // The original reads the centre's x/y in the item arrays (INTER.C, loop
    // type 2), which a destroyed item's free slot keeps. MISS5 destroys its
    // egg, then looks around it for the door its contact comes out by: the
    // door stayed shut and the contact behind it, unseen (issue #16).
    const Item* c = get(centre);
    if (!c) {
        const auto last = stale_.find(centre);
        if (last == stale_.end()) return found;
        c = &last->second;
    }
    // Rectangle around the centre's own x/y (16-bit, ITEMFIND.C 0771):
    // negative low bounds become 0, negative high bounds 0x7FFF.
    const auto cx = static_cast<std::int16_t>(c->x), cy = static_cast<std::int16_t>(c->y);
    const auto r = static_cast<std::int16_t>(range);
    auto low = [](int v) -> std::uint16_t { return static_cast<std::int16_t>(v) < 0 ? 0 : static_cast<std::uint16_t>(v); };
    auto high = [](int v) -> std::uint16_t {
        return static_cast<std::int16_t>(v) < 0 ? 0x7FFF : static_cast<std::uint16_t>(v);
    };
    const std::uint16_t x0 = low(cx - r), y0 = low(cy - r), x1 = high(cx + r), y1 = high(cy + r);

    for (int gy = y0 >> kCellShift; gy <= (y1 >> kCellShift); ++gy) {
        for (int gx = x0 >> kCellShift; gx <= (x1 >> kCellShift); ++gx) {
            for (ObjId id : cell(gx, gy)) {
                const Item& i = *get(id);
                const auto ix = static_cast<std::uint16_t>(i.x), iy = static_cast<std::uint16_t>(i.y);
                if (ix < x0 || ix > x1 || iy < y0 || iy > y1) continue;
                if (matches_loop_script(i, script)) found.push_back(id);
            }
        }
    }
    return found;
}

namespace {

// Scripts recognised by ITEMFIND.C's fast paths (0771 / 1099): "$" alone
// (surface searches only), "@ % n = $", "% n @ = $" and the same with ':',
// or a shape list followed by '$'. They test the same conditions, except that
// a surface search on a fast path does not skip the item itself.
// @orig 10C8:0771 10C8:1099
bool is_fast_path(std::span<const std::uint8_t> s, bool surface) {
    if (s.empty()) return false;
    if (s[0] == '$') return surface;
    if (s[0] >= 'A' && s[0] <= '_') {
        const std::size_t end = 2 * (s[0] - 0x40u) + 1;
        return end < s.size() && s[end] == '$';
    }
    if (s.size() != 6 || s[4] != '=' || s[5] != '$') return false;
    return (s[1] == '%' && (s[0] == '@' || s[0] == ':')) || (s[0] == '%' && (s[3] == '@' || s[3] == ':'));
}

}  // namespace

// @orig 10C8:0F73
std::vector<ObjId> World::surface_search(ObjId item_id, int mode, std::span<const std::uint8_t> script,
                                         std::vector<std::uint8_t>* sides) const {
    std::vector<ObjId> found;
    const Item* item = get(item_id);
    if (!item) return found;
    // Height in z units; a flat shape counts as 1 unless it is fixed.
    auto height = [this](const Item& i) {
        const ShapeInfo* info = shape_info(i.shape);
        const int fz = info ? info->footpad_z : 0;
        return fz == 0 && !(info && info->is(ShapeInfo::kFixed)) ? 1 : fz * 8;
    };
    auto footpad = [this](const Item& i) {
        const ShapeInfo* info = shape_info(i.shape);
        return info ? std::pair<int, int>{info->footpad_x, info->footpad_y} : std::pair<int, int>{0, 0};
    };
    const auto ix = static_cast<std::uint16_t>(item->x), iy = static_cast<std::uint16_t>(item->y);
    const auto top = static_cast<std::uint8_t>(item->z + height(*item));
    const auto bottom = static_cast<std::uint8_t>(item->z);
    const auto [fx, fy] = footpad(*item);

    // Candidate rectangle (ITEMFIND.C 0F73): from just inside the item's far
    // corner minus its footprint to 0x100 beyond its x/y.
    auto low = [](int v) -> std::uint16_t { return static_cast<std::int16_t>(v) < 0 ? 0 : static_cast<std::uint16_t>(v); };
    auto high = [](int v) -> std::uint16_t {
        return static_cast<std::int16_t>(v) < 0 ? 0x7FFF : static_cast<std::uint16_t>(v);
    };
    const std::uint16_t x0 = low(ix - fx * 16 + 1), y0 = low(iy - fy * 16 + 1);
    const std::uint16_t x1 = high(ix + 0x100), y1 = high(iy + 0x100);
    const bool fast = is_fast_path(script, true);

    for (int gy = y0 >> kCellShift; gy <= (y1 >> kCellShift); ++gy) {
        for (int gx = x0 >> kCellShift; gx <= (x1 >> kCellShift); ++gx) {
            for (ObjId id : cell(gx, gy)) {
                if (id == item_id && !fast) continue;
                const Item& c = *get(id);
                const auto cx = static_cast<std::uint16_t>(c.x), cy = static_cast<std::uint16_t>(c.y);
                if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
                const auto [cfx, cfy] = footpad(c);
                // The candidate's near corner must not be beyond the item's far one.
                if (static_cast<std::uint16_t>(cx - cfx * 16 + 1) > ix ||
                    static_cast<std::uint16_t>(cy - cfy * 16 + 1) > iy) {
                    continue;
                }
                const bool above = (mode & 1) && static_cast<std::uint8_t>(c.z) == top;
                const bool below = (mode & 2) && static_cast<std::uint8_t>(c.z + height(c)) == bottom;
                if ((above || below) && matches_loop_script(c, script)) {
                    found.push_back(id);
                    if (sides) sides->push_back(above ? 1 : 2);
                }
            }
        }
    }
    return found;
}

// @orig 10C8:0DEF
std::vector<ObjId> World::container_search(ObjId container, std::span<const std::uint8_t> script) const {
    std::vector<ObjId> found;
    const Item* c = get(container);
    // ITEM.C 0DF6: only families 6 and 7 have contents.
    if (!c) return found;
    for (ObjId id : c->contents) {
        if (const Item* i = get(id); i && matches_loop_script(*i, script)) found.push_back(id);
    }
    return found;
}

}  // namespace crusader
