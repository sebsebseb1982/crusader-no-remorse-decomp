#include "engine/world/casings.hpp"

#include "core/dtable.hpp"
#include "engine/world/actor_anim.hpp"

#include <algorithm>
#include <cmath>

namespace crusader {

namespace {

constexpr double kGravity = 0.32;  // units a tick, a tick
constexpr double kBounce = 0.3;    // of the speed down kept up, once

// splitmix32, as the marks': never the game's generators.
// @orig none: port-only casings
std::uint32_t mix(std::uint32_t v) {
    v += 0x9E3779B9u;
    v = (v ^ (v >> 16)) * 0x85EBCA6Bu;
    v = (v ^ (v >> 13)) * 0xC2B2AE35u;
    return v ^ (v >> 16);
}

int screen_x(double x, double y) { return static_cast<int>(std::floor((x - y) / 2)); }
int screen_y(double x, double y, double z) { return static_cast<int>(std::floor((x + y) / 4 - z)); }

}  // namespace

// @orig none: port-only casings
Ejection ejection_of(int fire_type) {
    if (mark_kind(fire_type) != MarkKind::Bullet) return Ejection::None;
    const dtable::FireType* ft = dtable::fire_type(fire_type);
    return ft && ft->projectiles() > 1 ? Ejection::Shell : Ejection::Casing;
}

// @orig none: port-only casings
double Casings::unit() {
    next_seed_ = mix(next_seed_);
    return (next_seed_ & 0xFFFF) / 65535.0;
}

// @orig none: port-only casings
void Casings::eject(const World& world, ObjId shooter, int dx, int dy, int dz, int dir, Ejection kind,
                    std::uint32_t now) {
    if (kind == Ejection::None) return;
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    const Item* item = world.get(shooter);
    if (!item) return;
    const World::Point at = world.location(*item);
    double fx = kDirectionDx[dir & 0xF], fy = kDirectionDy[dir & 0xF];
    const double length = std::sqrt(fx * fx + fy * fy);
    fx /= length;
    fy /= length;
    // Out of the right side (a quarter turn clockwise seen from above), a
    // little back, and up.
    const double rx = -fy, ry = fx;
    const double side = 1.2 + 1.0 * unit(), back = -0.2 + 0.9 * unit(), up = 2.2 + 1.2 * unit();
    Flying f;
    f.shooter = shooter;
    f.shell = kind == Ejection::Shell;
    f.x = at.x + dx * 0.6;
    f.y = at.y + dy * 0.6;
    f.z = at.z + dz;
    f.vx = rx * side - fx * back;
    f.vy = ry * side - fy * back;
    f.vz = up;
    f.ground = at.z;
    f.tick = now;
    next_seed_ = mix(next_seed_);
    f.seed = next_seed_;
    if (flying_.size() >= kMaxFlying) flying_.erase(flying_.begin());
    flying_.push_back(f);
}

// @orig none: port-only casings
void Casings::advance(const World& world, std::uint32_t now, DamageMarks& marks) {
    if (world.current_map() != map_) {
        clear();
        map_ = world.current_map();
    }
    std::erase_if(flying_, [&](Flying& f) {
        if (now < f.tick) return true;  // time went back: a load
        while (f.tick < now) {
            ++f.tick;
            ++f.age;
            f.x += f.vx;
            f.y += f.vy;
            f.z += f.vz;
            f.vz -= kGravity;
            if (f.z > f.ground) {
                if (f.age < kMaxTicks) continue;
                return true;
            }
            f.z = f.ground;
            if (f.bounces == 0 && f.vz < -1.0) {
                // A bounce, slowed down.
                ++f.bounces;
                f.vz = -f.vz * kBounce;
                f.vx *= 0.5;
                f.vy *= 0.5;
                continue;
            }
            marks.lay_casing(world,
                             {static_cast<std::int32_t>(std::lround(f.x)), static_cast<std::int32_t>(std::lround(f.y)),
                              f.ground},
                             f.shell, f.seed);
            return true;
        }
        return false;
    });
}

// @orig none: port-only casings
void Casings::draw(Surface8& screen, const DrawItem& d, const DamageMarks& marks) const {
    for (const Flying& f : flying_) {
        if (f.shooter != d.item->id) continue;
        const int cx = d.sx + screen_x(f.x, f.y) - screen_x(d.item->x, d.item->y);
        const int cy = d.sy + screen_y(f.x, f.y, f.z) - screen_y(d.item->x, d.item->y, d.item->z);
        // Spinning: a quarter turn every two ticks.
        static constexpr int kTurn[4][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}};
        const auto& t = kTurn[(f.seed + static_cast<std::uint32_t>(f.age / 2)) & 3];
        const auto put = [&](int x, int y, int shade) {
            if (x < 0 || y < 0 || x >= screen.width || y >= screen.height) return;
            std::uint8_t& p = screen.row(y)[x];
            p = marks.shaded(shade, p);
        };
        if (f.shell) {
            put(cx, cy, DamageMarks::kShellDark);
            put(cx + t[0], cy + t[1], DamageMarks::kShellRed);
            put(cx + 2 * t[0], cy + 2 * t[1], DamageMarks::kBrass);
        } else {
            put(cx, cy, DamageMarks::kBrassLight);
            put(cx + t[0], cy + t[1], DamageMarks::kBrass);
        }
    }
}

}  // namespace crusader
