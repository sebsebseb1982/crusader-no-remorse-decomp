#include "engine/world/lighting.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

namespace crusader {

namespace {

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 mul(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 1};
}
float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Camera basis of the isometric view, in lighting space.
const Vec3 kToViewer{0.6124f, 0.6124f, 0.5f};
const Vec3 kScreenRight{0.7071f, -0.7071f, 0.0f};
const Vec3 kScreenUp{-0.3536f, -0.3536f, 0.8660f};
// Screen pixels per lighting-space unit across the view: (x - y) / 4 = k (x - y) cos 45.
constexpr float kPixelScale = 0.35355f;
// Along a pixel's view ray, lighting-space point = A + B s, with s = x + y:
// A = (2 sx, -2 sx, -sy kLightZScale), B = (1/2, 1/2, kLightZScale / 8).
const Vec3 kRayStep{0.5f, 0.5f, kLightZScale / 8};
const float kRayStepLength = std::sqrt(dot(kRayStep, kRayStep));

constexpr int kTile = 16;
constexpr int kSmokeSteps = 8;
constexpr std::size_t kMaxPuffs = 160;

// Colour-cycled palette entries (palette_cycle.hpp): blinking lamps,
// screens, force fields, lasers.
bool glowing(int c) { return c >= 1 && c <= 14; }

// Characters, drawn as billboards: the NPC table (ids 1..255, the Silencer
// included, whose shape has no NPC typeflag) and NPC shapes created later.
bool is_actor(const Item& item, const ShapeInfo* info) {
    return is_npc_id(item.id) || (info && info->is(ShapeInfo::kNpc));
}

// Explosion and flash shapes (sprite_process.hpp, list DS:29C2).
bool is_blast_shape(std::uint16_t shape) { return shape == 0x31C || (shape >= 0x31F && shape <= 0x327); }

float srgb_to_linear(std::uint8_t c) { return std::pow(c / 255.0f, 2.2f); }

// Integer hash (also in the shaders).
// @orig none: port lighting
std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float lattice(std::int32_t x, std::int32_t y, std::int32_t z) {
    const std::uint32_t h = hash32(static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                                   static_cast<std::uint32_t>(z) * 83492791u);
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

// @orig none: port lighting
float value_noise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const std::int32_t ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy),
                       iz = static_cast<std::int32_t>(fz);
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    tz = tz * tz * (3 - 2 * tz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(lattice(ix, iy, iz), lattice(ix + 1, iy, iz), tx);
    const float x10 = lerp(lattice(ix, iy + 1, iz), lattice(ix + 1, iy + 1, iz), tx);
    const float x01 = lerp(lattice(ix, iy, iz + 1), lattice(ix + 1, iy, iz + 1), tx);
    const float x11 = lerp(lattice(ix, iy + 1, iz + 1), lattice(ix + 1, iy + 1, iz + 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

// A random number 0..1 for puff `serial`, draw `k` (smoke only: never the game's generators).
float puff_random(std::uint32_t serial, std::uint32_t k) {
    return static_cast<float>(hash32(serial * 2654435761u + k * 40503u) & 0xFFFFu) / 65535.0f;
}

}  // namespace

// @orig none: port lighting (smoke)
float smoke_noise(const Vec3& p, float seed, float t) {
    constexpr float kFeature = 1.0f / 45.0f;
    const float x = p.x * kFeature + seed * 17.3f, y = p.y * kFeature + seed * 9.1f,
                z = p.z * kFeature + seed * 5.7f - t * 0.35f;
    return 0.5f * value_noise(x, y, z) + 0.3f * value_noise(x * 2.03f, y * 2.03f, z * 2.03f) +
           0.2f * value_noise(x * 4.01f, y * 4.01f, z * 4.01f);
}

// A frame decoded once: colours (-1 transparent), a normal rounded from the
// silhouette (screen x / y, -127..127), and how many pixels of each colour.
struct WorldLighting::Decoded {
    int w = 0, h = 0;
    std::vector<std::int16_t> color;
    std::vector<std::int8_t> nx, ny;
    std::vector<std::pair<std::uint8_t, std::uint16_t>> histogram;
    int glow_pixels = 0;
};

// An occluder: the item's box, cut by its sprite. Seen from the camera the
// item covers only the sprite's opaque pixels, so a point of the box whose
// projection falls on a transparent pixel is empty (railings, grids and
// bodies cast their own outline, not a block).
struct WorldLighting::Box {
    Vec3 lo, hi;
    ObjId owner;
    const Decoded* sprite = nullptr;
    int left = 0, top = 0;  // frame rectangle, absolute screen coordinates
    bool mirrored = false;
};

// Worker threads kept for the whole run: run(n, f) calls f(0..n-1) spread
// over them and the caller, and returns when all are done.
class WorldLighting::Pool {
public:
    Pool() {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency()) - 1;
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { work(); });
    }
    // @orig none: port lighting
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    std::size_t size() const { return threads_.size() + 1; }

    // @orig none: port lighting
    void run(std::size_t n, const std::function<void(std::size_t)>& f) {
        if (n == 0) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = &f;
            count_ = n;
            next_ = 0;
            done_ = 0;
            ++generation_;
        }
        wake_.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&] { return done_ == count_; });
        task_ = nullptr;
    }

private:
    // @orig none: port lighting
    void drain() {
        for (std::size_t i; (i = next_++) < count_;) {
            (*task_)(i);
            if (++done_ == count_) {
                std::lock_guard<std::mutex> lock(mutex_);
                finished_.notify_all();
            }
        }
    }
    // @orig none: port lighting
    void work() {
        std::uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drain();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, finished_;
    const std::function<void(std::size_t)>* task_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0}, done_{0};
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

WorldLighting::WorldLighting(ShapeCache& shapes, const XformTable& xform, const AnimDat* anims)
    : shapes_(shapes), xform_(xform), anims_(anims), pool_(std::make_unique<Pool>()) {
    for (int i = 0; i < 4096; ++i)
        out_lut_[i] = static_cast<std::uint8_t>(std::lround(255.0 * std::pow(i / 4095.0, 1 / 2.2)));
}

WorldLighting::~WorldLighting() = default;

// @orig none: port lighting
const WorldLighting::Decoded& WorldLighting::decode(const ShapeFrame& f) {
    auto it = decoded_.find(&f);
    if (it != decoded_.end()) return *it->second;
    auto d = std::make_unique<Decoded>();
    d->w = f.width;
    d->h = f.height;
    const std::size_t n = static_cast<std::size_t>(d->w) * d->h;
    d->color.resize(n);
    d->nx.assign(n, 0);
    d->ny.assign(n, 0);
    std::array<std::uint16_t, 256> counts{};
    for (int y = 0; y < d->h; ++y)
        for (int x = 0; x < d->w; ++x) {
            const int c = f.pixel(x, y);
            d->color[y * d->w + x] = static_cast<std::int16_t>(c);
            if (c >= 0 && counts[c] < 0xFFFF) ++counts[c];
        }
    for (int c = 0; c < 256; ++c) {
        if (!counts[c]) continue;
        d->histogram.emplace_back(static_cast<std::uint8_t>(c), counts[c]);
        if (glowing(c)) d->glow_pixels += counts[c];
    }
    // Rows: the place between the first and last opaque pixels gives nx;
    // columns the same for ny, weaker (bodies are rounder across than along).
    for (int y = 0; y < d->h; ++y) {
        int a = d->w, b = -1;
        for (int x = 0; x < d->w; ++x)
            if (d->color[y * d->w + x] >= 0) a = std::min(a, x), b = std::max(b, x);
        for (int x = a; x <= b; ++x) {
            const float t = b > a ? (x - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d->nx[y * d->w + x] = static_cast<std::int8_t>((2 * t - 1) * 120);
        }
    }
    for (int x = 0; x < d->w; ++x) {
        int a = d->h, b = -1;
        for (int y = 0; y < d->h; ++y)
            if (d->color[y * d->w + x] >= 0) a = std::min(a, y), b = std::max(b, y);
        for (int y = a; y <= b; ++y) {
            const float t = b > a ? (y - a + 0.5f) / (b - a + 1.0f) : 0.5f;
            d->ny[y * d->w + x] = static_cast<std::int8_t>((1 - 2 * t) * 50);
        }
    }
    return *decoded_.emplace(&f, std::move(d)).first->second;
}

// 3D point and normal of the frame pixel (fx, fy) of an item drawn at
// absolute screen position (sx, sy). `face`: 0 top, 1 +x, 2 +y, 3 billboard.
// @orig none: port lighting
void WorldLighting::surface(const DrawItem& d, bool actor, float sx, float sy, int fx, int fy, bool bump, Vec3& p,
                            Vec3& n, int* face) const {
    const float xl = float(d.x_left), xh = float(d.x), yl = float(d.y_far), yh = float(d.y);
    const float zl = float(d.z), zh = float(d.z_top);
    // Points of the view ray: x = (s + 4sx)/2, y = (s - 4sx)/2, z = s/8 - sy.
    const float ux = 2 * xh - 4 * sx, uy = 2 * yh + 4 * sx, uz = 8 * (zh + sy);
    const float lo = std::max({2 * xl - 4 * sx, 2 * yl + 4 * sx, 8 * (zl + sy)});
    const float hi = std::min({ux, uy, uz});
    const Decoded& f = decoded(d.frame);
    float s;
    if (!actor && hi >= lo - 24) {
        s = hi;
        n = hi == uz ? Vec3{0, 0, 1} : hi == ux ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        if (face) *face = hi == uz ? 0 : hi == ux ? 1 : 2;
    } else {
        // Billboard through the box centre, rounded by the silhouette.
        s = (xl + xh + yl + yh) * 0.5f;
        float nx = f.nx[fy * f.w + fx] / 127.0f, ny = f.ny[fy * f.w + fx] / 127.0f;
        if (d.mirrored) nx = -nx;
        const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
        n = normalize(kScreenRight * nx + kScreenUp * ny + kToViewer * nz);
        if (face) *face = 3;
    }
    p = {(s + 4 * sx) * 0.5f, (s - 4 * sx) * 0.5f, (s / 8 - sy) * kLightZScale};
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
    const Vec3 tr = normalize(kScreenRight - n * dot(kScreenRight, n));
    const Vec3 tu = normalize(kScreenUp - n * dot(kScreenUp, n));
    constexpr float k = 2.5f;
    n = normalize(n - tr * (k * gx) + tu * (k * gy));
}

// The point and normal of an owned pixel.
// @orig none: port lighting
void WorldLighting::point_at(int x, int y, Vec3& p, Vec3& n) const {
    const std::int32_t o = owner_[static_cast<std::size_t>(y) * width_ + x];
    const DrawItem& d = list_[o];
    const int fx = d.mirrored ? d.right - 1 - x : x - d.left;
    surface(d, actor_[o], view_left_ + x + 0.5f, view_top_ + y + 0.5f, fx, y - d.top, true, p, n);
}

// @orig none: port lighting
void WorldLighting::capture(const World& world, const std::vector<DrawItem>& list, const Surface8& screen,
                            int view_left, int view_top, const Palette& palette, const LightingOptions& opt,
                            std::uint32_t ticks) {
    list_ = list;
    view_left_ = view_left;
    view_top_ = view_top;
    width_ = screen.width;
    height_ = screen.height;
    ticks_ = ticks;
    const std::size_t n = static_cast<std::size_t>(width_) * height_;
    world_ = screen.pixels;
    owner_.assign(n, -1);
    emissive_.assign(n, 0);
    set_palette(palette);
    actor_.resize(list_.size());
    for (std::size_t i = 0; i < list_.size(); ++i) {
        decode(*list_[i].frame);
        actor_[i] = is_actor(*list_[i].item, world.shape_info(list_[i].item->shape));
    }
    // Which item shows at each pixel, in the order they were drawn, by bands
    // of rows. Translucent items leave it to what is below; flashes and
    // explosions (drawn as sprites) only glow.
    const std::size_t bands = std::min<std::size_t>(pool_->size() * 2, static_cast<std::size_t>(height_));
    pool_->run(bands, [&](std::size_t band) {
        const int y0 = static_cast<int>(band * height_ / bands), y1 = static_cast<int>((band + 1) * height_ / bands);
        for (std::size_t i = 0; i < list_.size(); ++i) {
            const DrawItem& d = list_[i];
            if (d.bottom <= y0 || d.top >= y1) continue;
            const Decoded& f = decoded(d.frame);
            const bool translucent = d.shape_flags & ShapeInfo::kTranslucent;
            for (int y = std::max(d.top, y0); y < std::min(d.bottom, y1); ++y) {
                const int fy = y - d.top;
                for (int fx = 0; fx < f.w; ++fx) {
                    if (f.color[fy * f.w + fx] < 0) continue;
                    const int x = d.mirrored ? d.left + (f.w - 1 - fx) : d.left + fx;
                    if (x < 0 || x >= width_) continue;
                    const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                    if (d.item->sprite_only) {
                        emissive_[j] = 1;
                    } else if (!translucent) {
                        owner_[j] = static_cast<std::int32_t>(i);
                        emissive_[j] = 0;
                    }
                }
            }
        }
    });
    if (opt.smoke) spawn_smoke();
    else puffs_.clear();
}

// Position and normal of every owned pixel (processor shading).
// @orig none: port lighting
void WorldLighting::geometry(const LightingOptions& opt) {
    const std::size_t n = static_cast<std::size_t>(width_) * height_;
    pos_.resize(n);
    nrm_.resize(n);
    pool_->run(static_cast<std::size_t>(height_), [&](std::size_t row) {
        const int py = static_cast<int>(row);
        for (int px = 0; px < width_; ++px) {
            const std::size_t j = static_cast<std::size_t>(py) * width_ + px;
            const int o = owner_[j];
            if (o < 0) continue;
            const DrawItem& d = list_[o];
            const int fx = d.mirrored ? d.right - 1 - px : px - d.left;
            surface(d, actor_[o], view_left_ + px + 0.5f, view_top_ + py + 0.5f, fx, py - d.top, opt.bump, pos_[j],
                    nrm_[j]);
        }
    });
}

// New flashes, explosions and shots (sprites) leave smoke.
// @orig none: port lighting (smoke)
void WorldLighting::spawn_smoke() {
    std::unordered_map<ObjId, std::uint16_t> seen;
    for (const DrawItem& d : list_) {
        if (!d.item->sprite_only) continue;
        const ObjId id = d.item->id;
        const std::uint16_t shape = d.item->shape;
        seen[id] = shape;
        const auto before = sprites_seen_.find(id);
        const bool fresh = before == sprites_seen_.end() || before->second != shape;
        const Vec3 centre{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, (d.z + d.z_top) * 0.5f * kLightZScale};
        if (!is_blast_shape(shape)) {
            // A shot leaving the muzzle, an impact: a wisp where it shows
            // (these sprites keep their place); a trail behind bright shots
            // in flight (rockets).
            if (fresh) {
                add_puff(centre, SmokePuff::kShot);
                continue;
            }
            if (ticks_ % 4 != 0) continue;
            const Decoded& f = decoded(d.frame);
            int bright = 0;
            for (const auto& [c, k] : f.histogram)
                if (lum_[c] > 0.62f) bright += k;
            if (bright >= 20) add_puff(centre, SmokePuff::kTrail);
            continue;
        }
        {
            if (!fresh) continue;
            // Over the surface drawn under the anchor (these sprites are
            // moved up over the scene, sprite_process.hpp).
            std::optional<Vec3> at;
            for (int dy = 0; dy < 24 && !at; dy += 4) at = surface_at(d.sx, d.sy - dy);
            if (!at) at = Vec3{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, float(d.z) * kLightZScale};
            // Big shapes are explosions, small ones flashes and impacts.
            int area = 0;
            if (const Shape* s = shapes_.get(shape))
                for (const ShapeFrame& f : s->frames()) area = std::max(area, f.width * f.height);
            const bool blast = area > 3000;
            const Vec3 base = *at + Vec3{0, 0, (blast ? 16.0f : 10.0f) * kLightZScale};
            add_puff(base, blast ? SmokePuff::kExplosion : SmokePuff::kShot,
                     blast ? std::clamp(std::sqrt(float(area)) / 70.0f, 0.8f, 2.0f) : 1.0f);
        }
    }
    sprites_seen_ = std::move(seen);
    // Gone when their life is over.
    std::erase_if(puffs_, [&](const SmokePuff& p) { return (ticks_ - p.born) / 60.0f >= p.life; });
}

// @orig none: port lighting (smoke)
void WorldLighting::add_puff(const Vec3& at, SmokePuff::Kind kind, float scale) {
    const std::uint32_t serial = ++puff_serial_;
    auto rnd = [&](std::uint32_t k) { return puff_random(serial, k); };
    const int count = kind == SmokePuff::kExplosion ? 6 : kind == SmokePuff::kShot ? 2 : 1;
    for (int i = 0; i < count; ++i) {
        SmokePuff p;
        p.kind = kind;
        p.born = ticks_;
        p.seed = rnd(10 + i) * 100.0f;
        const float jx = rnd(20 + i) * 2 - 1, jy = rnd(30 + i) * 2 - 1, jz = rnd(40 + i);
        switch (kind) {
        case SmokePuff::kExplosion:
            p.pos = at + Vec3{jx * 60, jy * 60, jz * 40} * scale;
            p.drift = {jx * 12, jy * 12, 45 + rnd(50 + i) * 30};
            p.radius0 = (45 + rnd(60 + i) * 30) * scale;
            p.radius1 = (140 + rnd(70 + i) * 80) * scale;
            p.density = 0.03f;
            p.life = 4.5f + rnd(80 + i) * 2;
            break;
        case SmokePuff::kShot:
            p.pos = at + Vec3{jx * 10, jy * 10, jz * 10};
            p.drift = {jx * 8, jy * 8, 22 + jz * 10};
            p.radius0 = 20;
            p.radius1 = 75;
            p.density = 0.05f;
            p.life = 2.2f;
            break;
        case SmokePuff::kTrail:
            p.pos = at;
            p.drift = {jx * 4, jy * 4, 14};
            p.radius0 = 12;
            p.radius1 = 42;
            p.density = 0.016f;
            p.life = 1.2f;
            break;
        }
        if (puffs_.size() >= kMaxPuffs) puffs_.erase(puffs_.begin());
        puffs_.push_back(p);
    }
}

// The puffs now, far to near.
// @orig none: port lighting (smoke)
void WorldLighting::puff_states(std::vector<PuffState>& out) const {
    out.clear();
    for (const SmokePuff& p : puffs_) {
        const float age = (ticks_ - p.born) / 60.0f;
        const float u = std::clamp(age / p.life, 0.0f, 1.0f);
        PuffState s;
        s.centre = p.pos + p.drift * age;
        s.radius = p.radius0 + (p.radius1 - p.radius0) * (1 - (1 - u) * (1 - u));
        // Fades in over the first tenth of a second, thins out to nothing.
        s.density = p.density * std::min(1.0f, age / 0.1f) * std::pow(1 - u, 1.5f);
        s.heat = p.kind == SmokePuff::kExplosion ? std::max(0.0f, 1 - age / 0.9f) : 0.0f;
        s.grey = p.kind == SmokePuff::kExplosion ? 0.3f : p.kind == SmokePuff::kShot ? 0.62f : 0.7f;
        s.seed = p.seed;
        s.time = age;
        if (s.density > 1e-5f) out.push_back(s);
    }
    std::sort(out.begin(), out.end(),
              [](const PuffState& a, const PuffState& b) { return dot(a.centre, kToViewer) < dot(b.centre, kToViewer); });
}

// @orig none: port lighting
void WorldLighting::gather_lights(const World& world, const LightingOptions& opt, std::vector<PointLight>& lights) {
    for (const DrawItem& d : list_) {
        const Decoded& f = decoded(d.frame);
        if (d.item->sprite_only) {
            if (!opt.flash_lights) continue;
            // Flashes, explosions, shots: their bright pixels.
            int count = 0;
            Vec3 sum;
            for (const auto& [c, k] : f.histogram) {
                if (lum_[c] < 0.62f) continue;
                count += k;
                sum = sum + lin_[c] * float(k);
            }
            if (count < 4) continue;
            Vec3 col = sum * (1.0f / count);
            col = col * (1.0f / std::max({col.x, col.y, col.z, 0.05f}));
            // These sprites are moved up over the scene (sprite_process.hpp):
            // the light goes over the surface drawn under their anchor.
            std::optional<Vec3> at;
            for (int dy = 0; dy < 24 && !at; dy += 4) at = surface_at(d.sx, d.sy - dy);
            if (!at) at = Vec3{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, float(d.z) * kLightZScale};
            const float strength = std::min(2.2f, 0.7f + count / 120.0f);
            lights.push_back({*at + Vec3{0, 0, 24 * kLightZScale}, col * strength,
                              std::min(900.0f, 300.0f + count * 1.5f), d.item->id});
            continue;
        }
        if (!opt.glow_lights || f.glow_pixels < 6) continue;
        Vec3 sum;
        for (const auto& [c, k] : f.histogram)
            if (glowing(c)) sum = sum + lin_[c] * float(k);
        Vec3 col = sum * (1.0f / f.glow_pixels);
        col = col * (1.0f / std::max({col.x, col.y, col.z, 0.05f}));
        const float strength = std::min(1.2f, 0.35f + f.glow_pixels / 80.0f);
        const Vec3 centre{(d.x + d.x_left) * 0.5f, (d.y + d.y_far) * 0.5f, (d.z_top + 4.0f) * kLightZScale};
        lights.push_back({centre, col * strength, 260.0f + std::min(f.glow_pixels, 200) * 2.0f, d.item->id});
    }
    if (opt.player_lamp) {
        // A lamp just over the Silencer's head.
        const Item* p = world.get(1);
        if (p && !p->off_map && !p->parent) {
            const ShapeInfo* info = world.shape_info(p->shape);
            const float x = float(world_from_usecode(p->x)), y = float(world_from_usecode(p->y));
            const float half = info ? info->footpad_x * kFootpadXY * 0.5f : 32.0f;
            const float top = float(p->z + (info ? info->footpad_z * kFootpadZ : 40) + 12);
            lights.push_back({{x - half, y - half, top * kLightZScale}, Vec3{1.0f, 0.92f, 0.8f} * 1.1f, 650, p->id});
        }
    }
}

// Boxes that can shadow the lit area.
// @orig none: port lighting
void WorldLighting::gather_occluders(const World& world) {
    boxes_.clear();
    per_light_.assign(lights_.size(), {});
    if (lights_.empty()) return;
    float lox = 1e9f, loy = 1e9f, hix = -1e9f, hiy = -1e9f;
    for (const PointLight& l : lights_) {
        lox = std::min(lox, l.pos.x - l.radius);
        loy = std::min(loy, l.pos.y - l.radius);
        hix = std::max(hix, l.pos.x + l.radius);
        hiy = std::max(hiy, l.pos.y + l.radius);
    }
    world.for_each([&](const Item& item) {
        if (item.off_map || item.parent != 0 || item.sprite_only ||
            (item.flags & (kItemInvisible | kItemContained | kItemEthereal)))
            return;
        const ShapeInfo* info = world.shape_info(item.shape);
        if (!info || info->is(ShapeInfo::kEditor) || info->is(ShapeInfo::kTranslucent)) return;
        // Lasers, electrified floors: light, not matter.
        if (info->is(ShapeInfo::kDamaging)) return;
        if (!info->footpad_x || !info->footpad_y || !info->footpad_z) return;
        const bool mirrored = item.flags & kItemFlipped;
        const std::int32_t wx = world_from_usecode(item.x), wy = world_from_usecode(item.y);
        const float x = float(wx), y = float(wy);
        const float dx = float((mirrored ? info->footpad_y : info->footpad_x) * kFootpadXY);
        const float dy = float((mirrored ? info->footpad_x : info->footpad_y) * kFootpadXY);
        if (x < lox || x - dx > hix || y < loy || y - dy > hiy) return;
        Box b{{x - dx, y - dy, item.z * kLightZScale},
              {x, y, (item.z + info->footpad_z * kFootpadZ) * kLightZScale},
              item.id};
        if (const Shape* shape = shapes_.get(item.shape); shape && item.frame < shape->frames().size()) {
            const ShapeFrame& frame = shape->frames()[item.frame];
            const int sx = (wx - wy) >> 2, sy = ((wx + wy) >> 3) - item.z;
            b.sprite = &decode(frame);
            b.mirrored = mirrored;
            b.left = mirrored ? sx + frame.xoff - (frame.width - 1) : sx - frame.xoff;
            b.top = sy - frame.yoff;
        }
        boxes_.push_back(b);
    });
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        for (std::uint32_t i = 0; i < boxes_.size(); ++i) {
            const Box& b = boxes_[i];
            if (b.hi.x < L.pos.x - L.radius || b.lo.x > L.pos.x + L.radius || b.hi.y < L.pos.y - L.radius ||
                b.lo.y > L.pos.y + L.radius || b.hi.z < L.pos.z - L.radius || b.lo.z > L.pos.z + L.radius)
                continue;
            per_light_[l].push_back(i);
        }
    }
}

// Whether a box hides light `l` from point p: the segment from just off the
// surface to the light crosses an opaque part of it.
// @orig none: port lighting
bool WorldLighting::occluded(const Vec3& p, const Vec3& n, ObjId self, std::size_t light, bool sprite_cut,
                             const std::vector<std::uint32_t>& boxes) const {
    const PointLight& L = lights_[light];
    const Vec3 o = p + n * 3.0f;
    const Vec3 dir = L.pos - o;
    const float os[3] = {o.x, o.y, o.z};
    const float is[3] = {1 / (std::abs(dir.x) > 1e-6f ? dir.x : 1e-6f), 1 / (std::abs(dir.y) > 1e-6f ? dir.y : 1e-6f),
                         1 / (std::abs(dir.z) > 1e-6f ? dir.z : 1e-6f)};
    for (std::uint32_t bi : boxes) {
        const Box& b = boxes_[bi];
        if (b.owner == self || b.owner == L.owner) continue;
        // Ends excluded: a light or a surface touching a box is not hidden by it.
        float t0 = 1e-3f, t1 = 0.98f;
        const float lo[3] = {b.lo.x, b.lo.y, b.lo.z}, hi[3] = {b.hi.x, b.hi.y, b.hi.z};
        bool inside = true;
        for (int a = 0; a < 3 && inside; ++a) {
            float ta = (lo[a] - os[a]) * is[a], tb = (hi[a] - os[a]) * is[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            inside = t0 <= t1;
        }
        if (!inside) continue;
        if (!sprite_cut || !b.sprite) return true;
        const Decoded& f = *b.sprite;
        constexpr int kSamples = 5;
        for (int k = 0; k < kSamples; ++k) {
            const float t = t0 + (t1 - t0) * (k + 0.5f) / kSamples;
            const Vec3 q = o + dir * t;
            const int sx = static_cast<int>(std::floor((q.x - q.y) * 0.25f));
            const int sy = static_cast<int>(std::floor((q.x + q.y) * 0.125f - q.z / kLightZScale));
            const int fx = b.mirrored ? b.left + f.w - 1 - sx : sx - b.left;
            const int fy = sy - b.top;
            if (fx < 0 || fy < 0 || fx >= f.w || fy >= f.h) continue;
            if (f.color[fy * f.w + fx] >= 0) return true;
        }
    }
    return false;
}

// Linear colour of palette entry c at a point lit by the lights `ids`, with
// `boxes` (when given) the occluders of each, in the same order.
// @orig none: port lighting
Vec3 WorldLighting::shade_point(int c, const Vec3& p, const Vec3& n, ObjId self, const LightingOptions& opt,
                                const std::vector<std::uint32_t>& ids,
                                const std::vector<std::vector<std::uint32_t>>* boxes) const {
    const float a = opt.ambient;
    Vec3 light{a, a * 1.02f, a * 1.12f};
    for (std::size_t k = 0; k < ids.size(); ++k) {
        const PointLight& L = lights_[ids[k]];
        const Vec3 to = L.pos - p;
        const float d2 = dot(to, to);
        if (d2 >= L.radius * L.radius) continue;
        const float ndl = dot(n, to) / std::max(std::sqrt(d2), 1e-3f);
        if (ndl <= 0) continue;
        const float f = 1 - d2 / (L.radius * L.radius);
        const float w = ndl * f * f;
        if (opt.shadows &&
            occluded(p, n, self, ids[k], opt.sprite_shadows, boxes ? (*boxes)[k] : per_light_[ids[k]]))
            continue;
        light = light + L.color * w;
    }
    Vec3 col = mul(lin_[c], light);
    if (glowing(c)) col = col + lin_[c];
    return col;
}

// The puffs `ids` (far to near) over a pixel of linear colour `colour`;
// with `clip`, only in front of its surface (s = x + y of its point).
// @orig none: port lighting (smoke)
Vec3 WorldLighting::smoke_over(Vec3 colour, int px, int py, bool clip, float surface_s, const LightingOptions& opt,
                               const std::vector<std::uint32_t>& ids) const {
    const float sx = view_left_ + px + 0.5f, sy = view_top_ + py + 0.5f;
    const Vec3 a{2 * sx, -2 * sx, -sy * kLightZScale};
    const float bb = dot(kRayStep, kRayStep);
    for (std::uint32_t id : ids) {
        const PuffState& puff = puff_now_[id];
        // The ray's closest point to the centre, then the half chord (no
        // b^2 - 4ac: these numbers are far too big for floats).
        const Vec3 d = a - puff.centre;
        const float mid = -dot(kRayStep, d) / bb;
        const Vec3 e = d + kRayStep * mid;
        const float perp2 = dot(e, e), r2 = puff.radius * puff.radius;
        if (perp2 >= r2) continue;
        const float half = std::sqrt((r2 - perp2) / bb);
        float s0 = mid - half;
        const float s1 = mid + half;
        if (clip) s0 = std::max(s0, surface_s);
        if (s1 <= s0) continue;
        const float ds = (s1 - s0) / kSmokeSteps;
        const float len = ds * kRayStepLength;
        // Far to near.
        for (int i = 0; i < kSmokeSteps; ++i) {
            const Vec3 p = a + kRayStep * (s0 + (i + 0.5f) * ds);
            const float dist = std::sqrt(dot(p - puff.centre, p - puff.centre)) / puff.radius;
            const float shape = 1 - smoothstep(0.35f, 1.0f, dist);
            if (shape <= 0) continue;
            const float noise = std::clamp((smoke_noise(p, puff.seed, puff.time) - 0.3f) * 1.8f, 0.0f, 1.0f);
            const float sigma = puff.density * shape * noise;
            if (sigma <= 0) continue;
            const float keep = std::exp(-sigma * len);
            Vec3 light{opt.ambient, opt.ambient * 1.02f, opt.ambient * 1.12f};
            for (const PointLight& L : lights_) {
                const Vec3 to = L.pos - p;
                const float d2 = dot(to, to);
                if (d2 >= L.radius * L.radius) continue;
                const float f = 1 - d2 / (L.radius * L.radius);
                light = light + L.color * (f * f * 0.8f);
            }
            const Vec3 glow = Vec3{1.0f, 0.45f, 0.12f} * (puff.heat * puff.heat * 2.5f * shape);
            const Vec3 radiance = light * puff.grey + glow;
            colour = colour * keep + radiance * (1 - keep);
        }
    }
    return colour;
}

// The palette shown (fades, colour cycling): linear colours and luminance.
// @orig none: port lighting
void WorldLighting::set_palette(const Palette& palette) {
    for (int i = 0; i < 256; ++i) {
        const Rgb& c = palette.colors[i];
        lin_[i] = {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)};
        lum_[i] = std::sqrt(lin_[i].x * 0.3f + lin_[i].y * 0.59f + lin_[i].z * 0.11f);
        flat_[i] = 0xFF000000u | std::uint32_t{c.r} << 16 | std::uint32_t{c.g} << 8 | c.b;
    }
}

std::uint32_t WorldLighting::pack(const Vec3& c) const {
    auto q = [&](float v) { return std::uint32_t{out_lut_[static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 4095)]}; };
    return 0xFF000000u | q(c.x) << 16 | q(c.y) << 8 | q(c.z);
}

// @orig none: port lighting
bool WorldLighting::shade(const World& world, const Surface8& screen, const Palette& palette,
                          const LightingOptions& opt, std::vector<PointLight> extra, std::uint32_t* argb) {
    if (!captured() || screen.width != width_ || screen.height != height_) return false;
    set_palette(palette);
    lights_ = std::move(extra);
    gather_lights(world, opt, lights_);
    gather_occluders(world);
    if (opt.smoke) puff_states(puff_now_);
    else puff_now_.clear();
    used_gpu_ = gpu_ && !opt.show_normals && shade_gpu(screen, opt, argb);
    if (!used_gpu_) shade_cpu(screen, opt, argb);
    lit_.assign(argb, argb + static_cast<std::size_t>(width_) * height_);
    return true;
}

// @orig none: port lighting
void WorldLighting::shade_cpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb) {
    geometry(opt);
    // The puffs' screen circles (absolute screen coordinates).
    struct Disc {
        float x, y, r;
    };
    std::vector<Disc> discs;
    for (const PuffState& p : puff_now_) {
        const float x = p.centre.x, y = p.centre.y, z = p.centre.z / kLightZScale;
        discs.push_back({(x - y) * 0.25f, (x + y) * 0.125f - z, p.radius * kPixelScale});
    }
    const int tiles_x = (width_ + kTile - 1) / kTile, tiles_y = (height_ + kTile - 1) / kTile;
    pool_->run(static_cast<std::size_t>(tiles_x) * tiles_y, [&](std::size_t t) {
        const int x0 = static_cast<int>(t % tiles_x) * kTile, y0 = static_cast<int>(t / tiles_x) * kTile;
        const int x1 = std::min(x0 + kTile, width_), y1 = std::min(y0 + kTile, height_);
        // The tile's lit points: their box, the lights reaching it, and for
        // each the occluders between.
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                if (owner_[j] < 0) continue;
                const Vec3& p = pos_[j];
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        std::vector<std::uint32_t> ids;
        std::vector<std::vector<std::uint32_t>> boxes;
        if (lo.x <= hi.x) {
            for (std::uint32_t l = 0; l < lights_.size(); ++l) {
                const PointLight& L = lights_[l];
                const float dx = std::max({lo.x - L.pos.x, 0.0f, L.pos.x - hi.x});
                const float dy = std::max({lo.y - L.pos.y, 0.0f, L.pos.y - hi.y});
                const float dz = std::max({lo.z - L.pos.z, 0.0f, L.pos.z - hi.z});
                if (dx * dx + dy * dy + dz * dz >= L.radius * L.radius) continue;
                ids.push_back(l);
                auto& list = boxes.emplace_back();
                if (!opt.shadows) continue;
                const Vec3 a{std::min(lo.x, L.pos.x) - 4, std::min(lo.y, L.pos.y) - 4, std::min(lo.z, L.pos.z) - 4};
                const Vec3 b{std::max(hi.x, L.pos.x) + 4, std::max(hi.y, L.pos.y) + 4, std::max(hi.z, L.pos.z) + 4};
                for (std::uint32_t bi : per_light_[l]) {
                    const Box& box = boxes_[bi];
                    if (box.hi.x < a.x || box.lo.x > b.x || box.hi.y < a.y || box.lo.y > b.y || box.hi.z < a.z ||
                        box.lo.z > b.z)
                        continue;
                    list.push_back(bi);
                }
            }
        }
        // The puffs over the tile.
        std::vector<std::uint32_t> puffs;
        const float tx0 = float(view_left_ + x0), ty0 = float(view_top_ + y0);
        const float tx1 = float(view_left_ + x1), ty1 = float(view_top_ + y1);
        for (std::uint32_t i = 0; i < discs.size(); ++i) {
            const Disc& d = discs[i];
            const float cx = std::clamp(d.x, tx0, tx1), cy = std::clamp(d.y, ty0, ty1);
            if ((cx - d.x) * (cx - d.x) + (cy - d.y) * (cy - d.y) < d.r * d.r) puffs.push_back(i);
        }
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
                const int c = screen.pixels[j];
                if (c != world_[j]) {
                    // Drawn over the world (status bar, cursor, menus): as it is.
                    argb[j] = flat_[c];
                    continue;
                }
                const bool lit = owner_[j] >= 0 && !emissive_[j];
                if (opt.show_normals && lit) {
                    const Vec3& n = nrm_[j];
                    argb[j] = 0xFF000000u | std::uint32_t((n.x * 0.5f + 0.5f) * 255) << 16 |
                              std::uint32_t((n.y * 0.5f + 0.5f) * 255) << 8 | std::uint32_t((n.z * 0.5f + 0.5f) * 255);
                    continue;
                }
                Vec3 col;
                if (emissive_[j]) col = lin_[c];  // a flash: as it is
                else if (!lit) col = lin_[c] * opt.ambient;
                else col = shade_point(c, pos_[j], nrm_[j], list_[owner_[j]].item->id, opt, ids, &boxes);
                if (!puffs.empty())
                    col = smoke_over(col, x, y, lit, lit ? pos_[j].x + pos_[j].y : 0.0f, opt, puffs);
                argb[j] = pack(col);
            }
    });
}

// Where a decoded frame is in the GPU's atlas, added (to upload) if new.
// @orig none: port lighting (GPU)
void WorldLighting::atlas_place(const Decoded& d, LitFrame& frame) {
    if (atlas_.count(&d)) return;
    constexpr int kSize = LitFrame::kAtlasSize;
    if (d.w > kSize || d.h > kSize || d.w <= 0 || d.h <= 0) return;
    if (shelf_x_ + d.w > kSize) {
        shelf_y_ += shelf_h_;
        shelf_x_ = 0;
        shelf_h_ = 0;
    }
    if (shelf_y_ + d.h > kSize) {
        atlas_full_ = true;
        return;
    }
    atlas_[&d] = {shelf_x_, shelf_y_};
    LitFrame::Upload up{shelf_x_, shelf_y_, d.w, d.h,
                        std::vector<std::uint8_t>(static_cast<std::size_t>(d.w) * d.h * 4)};
    for (int i = 0; i < d.w * d.h; ++i) {
        up.rgba[i * 4] = d.color[i] >= 0 ? 255 : 0;
        up.rgba[i * 4 + 1] = static_cast<std::uint8_t>(d.nx[i] + 128);
        up.rgba[i * 4 + 2] = static_cast<std::uint8_t>(d.ny[i] + 128);
        up.rgba[i * 4 + 3] = 255;
    }
    frame.uploads.push_back(std::move(up));
    shelf_x_ += d.w;
    shelf_h_ = std::max(shelf_h_, d.h);
}

// The frame for the GPU: per pixel codes, items, lights, occluders, puffs.
// @orig none: port lighting (GPU)
bool WorldLighting::shade_gpu(const Surface8& screen, const LightingOptions& opt, std::uint32_t* argb) {
    if (list_.size() >= LitFrame::kMaxItems) return false;
    LitFrame& f = frame_;
    for (int attempt = 0; attempt < 2; ++attempt) {
        f.uploads.clear();
        f.atlas_reset = atlas_reset_;
        atlas_full_ = false;
        if (atlas_reset_) {
            atlas_.clear();
            shelf_x_ = shelf_y_ = shelf_h_ = 0;
            atlas_reset_ = false;
        }
        f.items.assign(list_.size() * 16, 0.0f);
        for (std::size_t i = 0; i < list_.size(); ++i) {
            const DrawItem& d = list_[i];
            const Decoded& dec = decoded(d.frame);
            atlas_place(dec, f);
            const auto it = atlas_.find(&dec);
            const std::pair<int, int> at = it != atlas_.end() ? it->second : std::pair{0, 0};
            float* o = &f.items[i * 16];
            o[0] = float(d.x_left), o[1] = float(d.x), o[2] = float(d.y_far), o[3] = float(d.y);
            o[4] = float(d.z), o[5] = float(d.z_top);
            o[6] = float((actor_[i] ? LitFrame::kActor : 0) | (d.mirrored ? LitFrame::kMirrored : 0));
            o[7] = float(d.item->id);
            o[8] = float(at.first), o[9] = float(at.second), o[10] = float(dec.w), o[11] = float(dec.h);
            o[12] = float(view_left_ + d.left), o[13] = float(view_top_ + d.top);
        }
        f.boxes.assign(boxes_.size() * 16, 0.0f);
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            const Box& b = boxes_[i];
            float* o = &f.boxes[i * 16];
            o[0] = b.lo.x, o[1] = b.lo.y, o[2] = b.lo.z, o[3] = float(b.owner);
            o[4] = b.hi.x, o[5] = b.hi.y, o[6] = b.hi.z, o[7] = b.mirrored ? 1.0f : 0.0f;
            if (!b.sprite) continue;
            atlas_place(*b.sprite, f);
            if (const auto it = atlas_.find(b.sprite); it != atlas_.end()) {
                o[8] = float(it->second.first), o[9] = float(it->second.second);
                o[10] = float(b.sprite->w), o[11] = float(b.sprite->h);
                o[12] = float(b.left), o[13] = float(b.top), o[14] = 1.0f;
            }
        }
        if (!atlas_full_) break;
        atlas_reset_ = true;  // full: start it again with this frame's sprites
    }
    if (atlas_full_) return false;
    f.width = width_;
    f.height = height_;
    f.view_left = view_left_;
    f.view_top = view_top_;
    f.screen = screen.pixels.data();
    f.options = opt;
    for (int i = 0; i < 256; ++i) f.palette[i] = flat_[i] & 0xFFFFFFu;
    f.code.resize(static_cast<std::size_t>(width_) * height_);
    pool_->run(static_cast<std::size_t>(height_), [&](std::size_t row) {
        for (std::size_t j = row * width_; j < (row + 1) * width_; ++j) {
            if (screen.pixels[j] != world_[j]) f.code[j] = LitFrame::kFlat;
            else if (emissive_[j]) f.code[j] = LitFrame::kEmissive;
            else if (owner_[j] < 0) f.code[j] = LitFrame::kAmbient;
            else f.code[j] = static_cast<std::uint16_t>(owner_[j]);
        }
    });
    f.lights.assign(lights_.size() * 12, 0.0f);
    f.light_boxes.clear();
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        float* o = &f.lights[l * 12];
        o[0] = L.pos.x, o[1] = L.pos.y, o[2] = L.pos.z, o[3] = L.radius;
        o[4] = L.color.x, o[5] = L.color.y, o[6] = L.color.z, o[7] = float(L.owner);
        o[8] = float(f.light_boxes.size()), o[9] = float(per_light_[l].size());
        for (std::uint32_t b : per_light_[l]) f.light_boxes.push_back(static_cast<std::int32_t>(b));
    }
    f.puffs.assign(puff_now_.size() * 12, 0.0f);
    for (std::size_t i = 0; i < puff_now_.size(); ++i) {
        const PuffState& p = puff_now_[i];
        float* o = &f.puffs[i * 12];
        o[0] = p.centre.x, o[1] = p.centre.y, o[2] = p.centre.z, o[3] = p.radius;
        o[4] = p.density, o[5] = p.heat, o[6] = p.grey, o[7] = p.seed, o[8] = p.time;
    }
    if (!gpu_->shade(f, argb)) {
        atlas_reset_ = true;  // the GPU may have lost it
        return false;
    }
    return true;
}

// @orig none: port lighting
std::optional<Vec3> WorldLighting::surface_at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return std::nullopt;
    if (owner_[static_cast<std::size_t>(y) * width_ + x] < 0) return std::nullopt;
    Vec3 p, n;
    point_at(x, y, p, n);
    return p;
}

// @orig none: port lighting
std::optional<Vec3> WorldLighting::light_spot(int x, int y, float height) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return std::nullopt;
    if (owner_[static_cast<std::size_t>(y) * width_ + x] < 0) return std::nullopt;
    Vec3 p, n;
    point_at(x, y, p, n);
    const Vec3 off = std::abs(n.z) > 0.7f ? Vec3{} : normalize(Vec3{n.x, n.y, 0}) * 40.0f;
    return p + off + Vec3{0, 0, height * kLightZScale};
}

// @orig none: port lighting (diagnostics)
void WorldLighting::probe(const World& world, int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const std::size_t j = static_cast<std::size_t>(y) * width_ + x;
    if (owner_[j] < 0) {
        std::printf("probe (%d, %d): %s\n", x, y, emissive_[j] ? "flash" : "background");
        return;
    }
    const DrawItem& d = list_[owner_[j]];
    Vec3 p, n;
    point_at(x, y, p, n);
    std::printf("probe (%d, %d): item %u shape %u frame %u box x[%d..%d] y[%d..%d] z[%d..%d]\n"
                "  surface (%.0f %.0f %.1f) normal (%.2f %.2f %.2f)\n",
                x, y, d.item->id, d.item->shape, d.item->frame, d.x_left, d.x, d.y_far, d.y, d.z, d.z_top, p.x, p.y,
                p.z / kLightZScale, n.x, n.y, n.z);
    for (std::size_t l = 0; l < lights_.size(); ++l) {
        const PointLight& L = lights_[l];
        std::printf("  light %zu (item %u) at (%.0f %.0f %.1f) distance %.0f\n", l, L.owner, L.pos.x, L.pos.y,
                    L.pos.z / kLightZScale, std::sqrt(dot(L.pos - p, L.pos - p)));
        for (std::uint32_t bi : per_light_[l]) {
            if (!occluded(p, n, d.item->id, l, true, {bi})) continue;
            const Box& b = boxes_[bi];
            const Item* it = world.get(b.owner);
            std::printf("    blocked by item %u shape %u frame %u box (%.0f %.0f %.1f)-(%.0f %.0f %.1f)\n", b.owner,
                        it ? it->shape : 0, it ? it->frame : 0, b.lo.x, b.lo.y, b.lo.z / kLightZScale, b.hi.x,
                        b.hi.y, b.hi.z / kLightZScale);
        }
    }
}

// The frame a character shows when it faces `steps` sixteenths of a turn
// further: the animation frame (action, direction, index) that gives its
// current frame is found in ANIM.DAT, then taken in the new direction. The
// current frame when no animation has it.
// @orig none: port lighting (experiment)
std::pair<const ShapeFrame*, bool> WorldLighting::turned_frame(const DrawItem& d, int steps) {
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
                        found = AnimPose{a, static_cast<int>(action->sixteen_directions() ? dir : dir * 2), i};
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

// Every pixel of every sprite around the view (not only those left on top)
// becomes a lit point, splatted with a depth buffer from the turned camera.
// Characters face it with the frame of their direction; the far faces of
// boxes, never drawn, repeat the near ones mirrored through the box.
// @orig none: port lighting (experiment)
void WorldLighting::reproject(const World& world, const Camera& centre, const LightingOptions& opt,
                              std::uint32_t* argb) {
    constexpr float kDeg = 3.14159265f / 180.0f;
    const float phi = (45.0f + opt.yaw) * kDeg, theta = (30.0f + opt.pitch) * kDeg;
    const Vec3 f{std::cos(theta) * std::cos(phi), std::cos(theta) * std::sin(phi), std::sin(theta)};
    const Vec3 r{std::sin(phi), -std::cos(phi), 0.0f};
    const Vec3 u{-std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
    const Vec3 c{float(centre.x), float(centre.y), centre.z * kLightZScale};
    const int w = width_, h = height_;
    constexpr int kMargin = 320;
    const auto around =
        build_display_list(world, shapes_, View{view_left_ - kMargin, view_top_ - kMargin}, w + 2 * kMargin,
                           h + 2 * kMargin);
    const int steps = static_cast<int>(std::lround(opt.yaw / 22.5f));
    std::vector<std::pair<const ShapeFrame*, bool>> sprite(around.size());
    std::vector<std::uint8_t> actor(around.size());
    for (std::size_t k = 0; k < around.size(); ++k) {
        const DrawItem& d = around[k];
        actor[k] = is_actor(*d.item, world.shape_info(d.item->shape));
        sprite[k] = {d.frame, d.mirrored};
        if (actor[k] && opt.turn_actors) sprite[k] = turned_frame(d, steps);
        decode(*d.frame);
        decode(*sprite[k].first);
    }
    std::fill(argb, argb + static_cast<std::size_t>(w) * h, 0xFFFF00FFu);
    std::vector<float> depth(static_cast<std::size_t>(w) * h, -1e30f);
    std::vector<std::mutex> row_locks(h);
    std::vector<std::uint32_t> all(lights_.size());
    for (std::uint32_t l = 0; l < all.size(); ++l) all[l] = l;
    auto splat = [&](float sx, float sy, float z, int col, const Vec3& p, const Vec3& n, ObjId self, bool flash) {
        const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
        if (x0 < -1 || y0 < -1 || x0 >= w || y0 >= h) return;
        std::uint32_t value = 0;
        bool shaded = false;
        for (int dy = 0; dy < 2; ++dy) {
            const int y = y0 + dy;
            if (y < 0 || y >= h) continue;
            std::lock_guard<std::mutex> lock(row_locks[y]);
            for (int dx = 0; dx < 2; ++dx) {
                const int x = x0 + dx;
                if (x < 0 || x >= w) continue;
                const std::size_t j = static_cast<std::size_t>(y) * w + x;
                if (z <= depth[j]) continue;
                if (!shaded) {
                    value = flash ? pack(lin_[col]) : pack(shade_point(col, p, n, self, opt, all, nullptr));
                    shaded = true;
                }
                depth[j] = z;
                argb[j] = value;
            }
        }
    };
    pool_->run(around.size(), [&](std::size_t k) {
        const DrawItem& d = around[k];
        if (d.shape_flags & ShapeInfo::kTranslucent) return;
        if (actor[k] || d.item->sprite_only) {
            // A sprite facing the turned camera, standing on the box centre.
            const ShapeFrame& frame = *sprite[k].first;
            const bool mirrored = sprite[k].second;
            const Decoded& fr = decoded(&frame);
            const float xc = (d.x + d.x_left) * 0.5f, yc = (d.y + d.y_far) * 0.5f;
            const Vec3 base{xc, yc, d.z * kLightZScale};
            // Where the centre falls in the sprite, from its anchor (the box corner).
            const float cx = (xc - yc) * 0.25f - (d.x - d.y) * 0.25f;
            const float cy = (xc + yc) * 0.125f - (d.x + d.y) * 0.125f;
            const Vec3 q0 = base - c;
            const float base_z = dot(q0, f);
            const float csx = kPixelScale * dot(q0, r) + w / 2.0f, csy = -kPixelScale * dot(q0, u) + h / 2.0f;
            for (int fy = 0; fy < fr.h; ++fy)
                for (int fx = 0; fx < fr.w; ++fx) {
                    const int col = fr.color[fy * fr.w + fx];
                    if (col < 0) continue;
                    const float ox = (mirrored ? frame.xoff - fx : fx - frame.xoff) - cx;
                    const float oy = (fy - frame.yoff) - cy;
                    float nx = fr.nx[fy * fr.w + fx] / 127.0f, ny = fr.ny[fy * fr.w + fx] / 127.0f;
                    if (mirrored) nx = -nx;
                    const float nz = std::sqrt(std::max(0.05f, 1 - nx * nx - ny * ny));
                    const Vec3 n = normalize(r * nx + u * ny + f * nz);
                    const Vec3 p = base + r * (ox / kPixelScale) - u * (oy / kPixelScale) + f * (nz * 12.0f);
                    splat(csx + ox, csy + oy, base_z + nz * 12.0f, col, p, n, d.item->id, d.item->sprite_only);
                }
            return;
        }
        const Decoded& fr = decoded(d.frame);
        // DrawItem screen positions are relative to the wider view.
        const int left = d.left + view_left_ - kMargin, top = d.top + view_top_ - kMargin;
        for (int fy = 0; fy < fr.h; ++fy)
            for (int fx = 0; fx < fr.w; ++fx) {
                const int col = fr.color[fy * fr.w + fx];
                if (col < 0) continue;
                const int ax = d.mirrored ? left + fr.w - 1 - fx : left + fx;
                Vec3 p, n;
                int face = 0;
                surface(d, false, ax + 0.5f, top + fy + 0.5f, fx, fy, opt.bump, p, n, &face);
                const Vec3 q = p - c;
                splat(kPixelScale * dot(q, r) + w / 2.0f, -kPixelScale * dot(q, u) + h / 2.0f, dot(q, f), col, p, n,
                      d.item->id, false);
                if (!opt.back_faces || (face != 1 && face != 2)) continue;
                // The far side of the box, never drawn: the near side mirrored through it.
                Vec3 pb = p, nb = n;
                if (face == 1) {
                    pb.x = d.x_left - (p.x - d.x);
                    nb.x = -n.x;
                } else {
                    pb.y = d.y_far - (p.y - d.y);
                    nb.y = -n.y;
                }
                const Vec3 qb = pb - c;
                splat(kPixelScale * dot(qb, r) + w / 2.0f, -kPixelScale * dot(qb, u) + h / 2.0f, dot(qb, f), col, pb,
                      nb, d.item->id, false);
            }
    });
}

}  // namespace crusader
