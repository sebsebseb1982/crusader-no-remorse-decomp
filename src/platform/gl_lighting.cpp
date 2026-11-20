#include "platform/gl_lighting.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace crusader {

namespace {

#if defined(_WIN32)
#define CRU_GLAPI __stdcall
#else
#define CRU_GLAPI
#endif

using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLchar = char;
using GLfloat = float;
using GLubyte = unsigned char;

// The constants used (OpenGL 3.3 / ES 3.0 values).
constexpr GLenum kTexture2D = 0x0DE1, kTextureMinFilter = 0x2801, kTextureMagFilter = 0x2800, kNearest = 0x2600;
constexpr GLenum kTextureWrapS = 0x2802, kTextureWrapT = 0x2803, kClampToEdge = 0x812F;
constexpr GLenum kUnpackAlignment = 0x0CF5, kPackAlignment = 0x0D05;
constexpr GLenum kRgba = 0x1908, kRgba8 = 0x8058, kRgba32f = 0x8814;
constexpr GLenum kR8ui = 0x8232, kR16ui = 0x8234, kR32i = 0x8235, kRedInteger = 0x8D94;
constexpr GLenum kUnsignedByte = 0x1401, kUnsignedShort = 0x1403, kInt = 0x1404, kFloat = 0x1406;
constexpr GLenum kTexture0 = 0x84C0, kFragmentShader = 0x8B30, kVertexShader = 0x8B31;
constexpr GLenum kCompileStatus = 0x8B81, kLinkStatus = 0x8B82, kInfoLogLength = 0x8B84;
constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
constexpr GLenum kTriangles = 0x0004, kVersion = 0x1F02, kRenderer = 0x1F01;

// The data textures are 1024 texels wide.
constexpr int kDataWidth = 1024;

const char* const kVertexShaderSource = R"(
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The same computations as WorldLighting's processor shading (lighting.cpp).
const char* const kFragmentShaderSource = R"(
uniform highp usampler2D uScreen;
uniform highp usampler2D uCode;
uniform highp sampler2D uPalette;
uniform highp sampler2D uItems;
uniform highp sampler2D uLights;
uniform highp sampler2D uBoxes;
uniform highp isampler2D uLightBoxes;
uniform highp sampler2D uPuffs;
uniform highp sampler2D uAtlas;
uniform int uViewX, uViewY, uWidth, uHeight, uLightCount, uPuffCount, uFlags;
uniform float uAmbient;
out vec4 outColor;

const float K = 3.266;
const vec3 TO_VIEWER = vec3(0.6124, 0.6124, 0.5);
const vec3 RIGHT = vec3(0.7071, -0.7071, 0.0);
const vec3 UP = vec3(-0.3536, -0.3536, 0.8660);
const vec3 RAY_STEP = vec3(0.5, 0.5, K / 8.0);
const int SHADOWS = 1, SPRITE_CUT = 2, BUMP = 4, SMOKE = 8;

vec4 fetchf(highp sampler2D t, int i) { return texelFetch(t, ivec2(i & 1023, i >> 10), 0); }
vec3 srgb(uint c) { return texelFetch(uPalette, ivec2(int(c), 0), 0).bgr; }
vec3 lin(uint c) { return pow(srgb(c), vec3(2.2)); }
float lum(uint c) { vec3 l = lin(c); return sqrt(l.r * 0.3 + l.g * 0.59 + l.b * 0.11); }
uint screenAt(ivec2 q) { return texelFetch(uScreen, q, 0).r; }
uint codeAt(ivec2 q) { return texelFetch(uCode, q, 0).r; }

// Bump: the luminance of a neighbour of the same item.
float neighbour(ivec2 q, uint code, float own) {
    if (q.x < 0 || q.y < 0 || q.x >= uWidth || q.y >= uHeight || codeAt(q) != code) return own;
    return lum(screenAt(q));
}

void surfacePoint(int item, ivec2 pix, out vec3 p, out vec3 n) {
    vec4 a = fetchf(uItems, item * 4);
    vec4 b = fetchf(uItems, item * 4 + 1);
    vec4 at = fetchf(uItems, item * 4 + 2);
    vec4 fr = fetchf(uItems, item * 4 + 3);
    int flags = int(b.z);
    float sx = float(uViewX + pix.x) + 0.5, sy = float(uViewY + pix.y) + 0.5;
    float ux = 2.0 * a.y - 4.0 * sx, uy = 2.0 * a.w + 4.0 * sx, uz = 8.0 * (b.y + sy);
    float lo = max(max(2.0 * a.x - 4.0 * sx, 2.0 * a.z + 4.0 * sx), 8.0 * (b.x + sy));
    float hi = min(min(ux, uy), uz);
    float s;
    if ((flags & 1) == 0 && hi >= lo - 24.0) {
        s = hi;
        n = hi == uz ? vec3(0.0, 0.0, 1.0) : (hi == ux ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0));
    } else {
        // Billboard through the box centre, rounded by the silhouette.
        s = (a.x + a.y + a.z + a.w) * 0.5;
        int ax = uViewX + pix.x, ay = uViewY + pix.y;
        int fx = (flags & 2) != 0 ? int(fr.x) + int(at.z) - 1 - ax : ax - int(fr.x);
        int fy = ay - int(fr.y);
        vec4 t = texelFetch(uAtlas, ivec2(int(at.x) + fx, int(at.y) + fy), 0);
        float nx = (t.g * 255.0 - 128.0) / 127.0, ny = (t.b * 255.0 - 128.0) / 127.0;
        if ((flags & 2) != 0) nx = -nx;
        float nz = sqrt(max(0.05, 1.0 - nx * nx - ny * ny));
        n = normalize(RIGHT * nx + UP * ny + TO_VIEWER * nz);
    }
    p = vec3((s + 4.0 * sx) * 0.5, (s - 4.0 * sx) * 0.5, (s / 8.0 - sy) * K);
    if ((uFlags & BUMP) != 0) {
        uint code = codeAt(pix);
        float own = lum(screenAt(pix));
        float gx = (neighbour(pix + ivec2(1, 0), code, own) - neighbour(pix - ivec2(1, 0), code, own)) * 0.5;
        float gy = (neighbour(pix + ivec2(0, 1), code, own) - neighbour(pix - ivec2(0, 1), code, own)) * 0.5;
        vec3 tr = normalize(RIGHT - n * dot(RIGHT, n));
        vec3 tu = normalize(UP - n * dot(UP, n));
        n = normalize(n - tr * (2.5 * gx) + tu * (2.5 * gy));
    }
}

// Whether a box hides the light from p (boxes cut by their sprites).
bool occluded(vec3 p, vec3 n, float self, vec3 lpos, float lowner, int first, int count) {
    vec3 o = p + n * 3.0;
    vec3 dir = lpos - o;
    vec3 inv = vec3(1.0 / (abs(dir.x) > 1e-6 ? dir.x : 1e-6), 1.0 / (abs(dir.y) > 1e-6 ? dir.y : 1e-6),
                    1.0 / (abs(dir.z) > 1e-6 ? dir.z : 1e-6));
    for (int k = 0; k < count; ++k) {
        int i = first + k;
        int bi = texelFetch(uLightBoxes, ivec2(i & 1023, i >> 10), 0).r;
        vec4 b0 = fetchf(uBoxes, bi * 4), b1 = fetchf(uBoxes, bi * 4 + 1);
        if (b0.w == self || b0.w == lowner) continue;
        vec3 ta = (b0.xyz - o) * inv, tb = (b1.xyz - o) * inv;
        vec3 tmin = min(ta, tb), tmax = max(ta, tb);
        float t0 = max(max(max(tmin.x, tmin.y), tmin.z), 1e-3);
        float t1 = min(min(min(tmax.x, tmax.y), tmax.z), 0.98);
        if (t0 > t1) continue;
        if ((uFlags & SPRITE_CUT) == 0) return true;
        vec4 at = fetchf(uBoxes, bi * 4 + 2), fr = fetchf(uBoxes, bi * 4 + 3);
        if (fr.z == 0.0) return true;
        for (int s = 0; s < 5; ++s) {
            float t = t0 + (t1 - t0) * (float(s) + 0.5) / 5.0;
            vec3 q = o + dir * t;
            int sx = int(floor((q.x - q.y) * 0.25)), sy = int(floor((q.x + q.y) * 0.125 - q.z / K));
            int fx = b1.w != 0.0 ? int(fr.x) + int(at.z) - 1 - sx : sx - int(fr.x);
            int fy = sy - int(fr.y);
            if (fx < 0 || fy < 0 || fx >= int(at.z) || fy >= int(at.w)) continue;
            if (texelFetch(uAtlas, ivec2(int(at.x) + fx, int(at.y) + fy), 0).r > 0.5) return true;
        }
    }
    return false;
}

// The smoke's noise (smoke_noise in lighting.cpp).
uint hash32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
float lattice(ivec3 c) {
    uint h = hash32(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u);
    return float(h & 0xFFFFFFu) / 16777215.0;
}
float valueNoise(vec3 v) {
    vec3 f = floor(v);
    ivec3 i = ivec3(f);
    vec3 t = v - f;
    t = t * t * (3.0 - 2.0 * t);
    float x00 = mix(lattice(i), lattice(i + ivec3(1, 0, 0)), t.x);
    float x10 = mix(lattice(i + ivec3(0, 1, 0)), lattice(i + ivec3(1, 1, 0)), t.x);
    float x01 = mix(lattice(i + ivec3(0, 0, 1)), lattice(i + ivec3(1, 0, 1)), t.x);
    float x11 = mix(lattice(i + ivec3(0, 1, 1)), lattice(i + ivec3(1, 1, 1)), t.x);
    return mix(mix(x00, x10, t.y), mix(x01, x11, t.y), t.z);
}
float smokeNoise(vec3 p, float seed, float time, float feature) {
    vec3 v = p * (1.0 / feature) + vec3(seed * 17.3, seed * 9.1, seed * 5.7 - time * 0.35);
    return 0.5 * valueNoise(v) + 0.3 * valueNoise(v * 2.03) + 0.2 * valueNoise(v * 4.01);
}

vec3 lightAt(vec3 p) {
    vec3 light = vec3(uAmbient, uAmbient * 1.02, uAmbient * 1.12);
    for (int l = 0; l < uLightCount; ++l) {
        vec4 L0 = fetchf(uLights, l * 3), L1 = fetchf(uLights, l * 3 + 1);
        vec3 to = L0.xyz - p;
        float d2 = dot(to, to), r2 = L0.w * L0.w;
        if (d2 >= r2) continue;
        float f = 1.0 - d2 / r2;
        light += L1.rgb * (f * f * 0.8);
    }
    return light;
}

// The puffs (far to near) over a pixel; with clip, only in front of its surface.
vec3 smokeOver(vec3 colour, ivec2 pix, bool clip, float surfaceS) {
    float sx = float(uViewX + pix.x) + 0.5, sy = float(uViewY + pix.y) + 0.5;
    vec3 A = vec3(2.0 * sx, -2.0 * sx, -sy * K);
    float bb = dot(RAY_STEP, RAY_STEP);
    float stepLength = sqrt(bb);
    for (int i = 0; i < uPuffCount; ++i) {
        vec4 q0 = fetchf(uPuffs, i * 3), q1 = fetchf(uPuffs, i * 3 + 1), q2 = fetchf(uPuffs, i * 3 + 2);
        vec3 d = A - q0.xyz;
        float mid = -dot(RAY_STEP, d) / bb;
        vec3 e = d + RAY_STEP * mid;
        float perp2 = dot(e, e), r2 = q0.w * q0.w;
        if (perp2 >= r2) continue;
        float half_ = sqrt((r2 - perp2) / bb);
        float s0 = mid - half_, s1 = mid + half_;
        if (clip) s0 = max(s0, surfaceS);
        if (s1 <= s0) continue;
        float ds = (s1 - s0) / 8.0;
        float len = ds * stepLength;
        for (int k = 0; k < 8; ++k) {
            vec3 p = A + RAY_STEP * (s0 + (float(k) + 0.5) * ds);
            float dist = length(p - q0.xyz) / q0.w;
            float shape = 1.0 - smoothstep(0.35, 1.0, dist);
            if (shape <= 0.0) continue;
            float noise = clamp((smokeNoise(p, q1.w, q2.x, q2.y) - 0.3) * 1.8, 0.0, 1.0);
            float sigma = q1.x * shape * noise;
            if (sigma <= 0.0) continue;
            float keep = exp(-sigma * len);
            vec3 glow = vec3(1.0, 0.45, 0.12) * (q1.y * q1.y * 2.5 * shape);
            vec3 radiance = lightAt(p) * q1.z + glow;
            colour = colour * keep + radiance * (1.0 - keep);
        }
    }
    return colour;
}

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    uint code = codeAt(pix);
    uint c = screenAt(pix);
    if (code == 0xFFFFu) {
        // Drawn over the world: as it is.
        vec3 o = srgb(c);
        outColor = vec4(o.b, o.g, o.r, 1.0);
        return;
    }
    vec3 col;
    vec3 p = vec3(0.0), n = vec3(0.0, 0.0, 1.0);
    bool lit = code < 0xFFF0u;
    if (code == 0xFFFDu) {
        col = lin(c);
    } else if (!lit) {
        col = lin(c) * uAmbient;
    } else {
        surfacePoint(int(code), pix, p, n);
        float self = fetchf(uItems, int(code) * 4 + 1).w;
        vec3 light = vec3(uAmbient, uAmbient * 1.02, uAmbient * 1.12);
        for (int l = 0; l < uLightCount; ++l) {
            vec4 L0 = fetchf(uLights, l * 3), L1 = fetchf(uLights, l * 3 + 1), L2 = fetchf(uLights, l * 3 + 2);
            vec3 to = L0.xyz - p;
            float d2 = dot(to, to), r2 = L0.w * L0.w;
            if (d2 >= r2) continue;
            float ndl = dot(n, to) / max(sqrt(d2), 1e-3);
            if (ndl <= 0.0) continue;
            float f = 1.0 - d2 / r2;
            if ((uFlags & SHADOWS) != 0 && occluded(p, n, self, L0.xyz, L1.w, int(L2.x), int(L2.y))) continue;
            light += L1.rgb * (ndl * f * f);
        }
        col = lin(c) * light;
        if (c >= 1u && c <= 14u) col += lin(c);
    }
    if ((uFlags & SMOKE) != 0 && uPuffCount > 0) col = smokeOver(col, pix, lit, p.x + p.y);
    vec3 o = pow(clamp(col, 0.0, 1.0), vec3(1.0 / 2.2));
    outColor = vec4(o.b, o.g, o.r, 1.0);
}
)";

}  // namespace

struct GlLighting::Impl {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    bool es = false;

#define CRU_GL_FUNCTIONS(X)                                                                                        \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                        \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                               \
    X(void, BindTexture, (GLenum, GLuint))                                                                          \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))               \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))            \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                                 \
    X(void, PixelStorei, (GLenum, GLint))                                                                           \
    X(void, ActiveTexture, (GLenum))                                                                                \
    X(GLuint, CreateShader, (GLenum))                                                                               \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                    \
    X(void, CompileShader, (GLuint))                                                                                \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                                  \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                 \
    X(void, DeleteShader, (GLuint))                                                                                 \
    X(GLuint, CreateProgram, ())                                                                                    \
    X(void, AttachShader, (GLuint, GLuint))                                                                         \
    X(void, LinkProgram, (GLuint))                                                                                  \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                                 \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                \
    X(void, DeleteProgram, (GLuint))                                                                                \
    X(void, UseProgram, (GLuint))                                                                                   \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                           \
    X(void, Uniform1i, (GLint, GLint))                                                                              \
    X(void, Uniform1f, (GLint, GLfloat))                                                                            \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                                    \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                           \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                      \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                          \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                                     \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                             \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                                    \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                           \
    X(void, BindVertexArray, (GLuint))                                                                              \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                                   \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                                    \
    X(GLenum, GetError, ())                                                                                         \
    X(const GLubyte*, GetString, (GLenum))

#define CRU_GL_DECLARE(ret, name, args) ret(CRU_GLAPI* name) args = nullptr;
    CRU_GL_FUNCTIONS(CRU_GL_DECLARE)
#undef CRU_GL_DECLARE

    GLuint program = 0, vao = 0, fbo = 0, target = 0;
    int target_w = 0, target_h = 0;
    // Textures, by unit.
    enum Unit { kScreen, kCode, kPalette, kItems, kLights, kBoxes, kLightBoxes, kPuffs, kAtlas, kUnits };
    GLuint tex[kUnits] = {};
    int tex_w[kUnits] = {}, tex_h[kUnits] = {};
    GLint u_view_x = -1, u_view_y = -1, u_width = -1, u_height = -1, u_lights = -1, u_puffs = -1, u_flags = -1,
          u_ambient = -1;
    std::vector<float> scratch;
    std::vector<std::int32_t> scratch_int;

    // @orig none: port lighting (GPU)
    bool load(std::string* error) {
        bool ok = true;
#define CRU_GL_LOAD(ret, name, args)                                                                               \
    name = reinterpret_cast<ret(CRU_GLAPI*) args>(SDL_GL_GetProcAddress("gl" #name));                             \
    if (!name) {                                                                                                   \
        ok = false;                                                                                                \
        if (error) *error = "missing gl" #name;                                                                    \
    }
        CRU_GL_FUNCTIONS(CRU_GL_LOAD)
#undef CRU_GL_LOAD
        return ok;
    }

    // @orig none: port lighting (GPU)
    GLuint compile(GLenum kind, const char* body, std::string* error) {
        const char* header = es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                : "#version 330 core\n";
        const GLuint s = CreateShader(kind);
        const GLchar* parts[2] = {header, body};
        ShaderSource(s, 2, parts, nullptr);
        CompileShader(s);
        GLint status = 0;
        GetShaderiv(s, kCompileStatus, &status);
        if (!status) {
            GLint length = 0;
            GetShaderiv(s, kInfoLogLength, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            GetShaderInfoLog(s, length, nullptr, log.data());
            if (error) *error = "shader: " + log;
            DeleteShader(s);
            return 0;
        }
        return s;
    }

    // @orig none: port lighting (GPU)
    bool build(std::string* error) {
        const GLuint vs = compile(kVertexShader, kVertexShaderSource, error);
        if (!vs) return false;
        const GLuint fs = compile(kFragmentShader, kFragmentShaderSource, error);
        if (!fs) {
            DeleteShader(vs);
            return false;
        }
        program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        DeleteShader(vs);
        DeleteShader(fs);
        GLint status = 0;
        GetProgramiv(program, kLinkStatus, &status);
        if (!status) {
            GLint length = 0;
            GetProgramiv(program, kInfoLogLength, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            GetProgramInfoLog(program, length, nullptr, log.data());
            if (error) *error = "link: " + log;
            return false;
        }
        UseProgram(program);
        const char* samplers[kUnits] = {"uScreen", "uCode",   "uPalette", "uItems", "uLights",
                                        "uBoxes",  "uLightBoxes", "uPuffs", "uAtlas"};
        for (int i = 0; i < kUnits; ++i) Uniform1i(GetUniformLocation(program, samplers[i]), i);
        u_view_x = GetUniformLocation(program, "uViewX");
        u_view_y = GetUniformLocation(program, "uViewY");
        u_width = GetUniformLocation(program, "uWidth");
        u_height = GetUniformLocation(program, "uHeight");
        u_lights = GetUniformLocation(program, "uLightCount");
        u_puffs = GetUniformLocation(program, "uPuffCount");
        u_flags = GetUniformLocation(program, "uFlags");
        u_ambient = GetUniformLocation(program, "uAmbient");
        GenVertexArrays(1, &vao);
        GenFramebuffers(1, &fbo);
        GenTextures(kUnits, tex);
        GenTextures(1, &target);
        for (GLuint t : tex) {
            BindTexture(kTexture2D, t);
            TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
            TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
            TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        }
        BindTexture(kTexture2D, tex[kAtlas]);
        TexImage2D(kTexture2D, 0, kRgba8, LitFrame::kAtlasSize, LitFrame::kAtlasSize, 0, kRgba, kUnsignedByte,
                   nullptr);
        tex_w[kAtlas] = tex_h[kAtlas] = LitFrame::kAtlasSize;
        return GetError() == 0;
    }

    // A texture of `w` x `h` texels, (re)made when its size changes, then filled.
    // @orig none: port lighting (GPU)
    void upload(Unit unit, GLint internal, GLenum format, GLenum type, int w, int h, const void* data) {
        ActiveTexture(kTexture0 + unit);
        BindTexture(kTexture2D, tex[unit]);
        if (tex_w[unit] != w || tex_h[unit] != h) {
            TexImage2D(kTexture2D, 0, internal, w, h, 0, format, type, data);
            tex_w[unit] = w;
            tex_h[unit] = h;
        } else {
            TexSubImage2D(kTexture2D, 0, 0, 0, w, h, format, type, data);
        }
    }

    // Floats, 4 a texel, in rows of kDataWidth texels (at least one row).
    // @orig none: port lighting (GPU)
    void upload_floats(Unit unit, const std::vector<float>& data) {
        const std::size_t texels = std::max<std::size_t>(data.size() / 4, 1);
        const int rows = static_cast<int>((texels + kDataWidth - 1) / kDataWidth);
        // Rows grow by powers of two, so that the texture is seldom remade.
        int alloc = 1;
        while (alloc < rows) alloc *= 2;
        scratch.assign(static_cast<std::size_t>(alloc) * kDataWidth * 4, 0.0f);
        std::copy(data.begin(), data.end(), scratch.begin());
        upload(unit, kRgba32f, kRgba, kFloat, kDataWidth, alloc, scratch.data());
    }

    // @orig none: port lighting (GPU)
    void upload_ints(Unit unit, const std::vector<std::int32_t>& data) {
        const std::size_t texels = std::max<std::size_t>(data.size(), 1);
        const int rows = static_cast<int>((texels + kDataWidth - 1) / kDataWidth);
        int alloc = 1;
        while (alloc < rows) alloc *= 2;
        scratch_int.assign(static_cast<std::size_t>(alloc) * kDataWidth, 0);
        std::copy(data.begin(), data.end(), scratch_int.begin());
        upload(unit, kR32i, kRedInteger, kInt, kDataWidth, alloc, scratch_int.data());
    }

    // @orig none: port lighting (GPU)
    void release() {
        if (!context) return;
        SDL_GL_MakeCurrent(window, context);
        if (program) DeleteProgram(program);
        if (vao) DeleteVertexArrays(1, &vao);
        if (fbo) DeleteFramebuffers(1, &fbo);
        DeleteTextures(kUnits, tex);
        if (target) DeleteTextures(1, &target);
        SDL_GL_DeleteContext(context);
        context = nullptr;
    }
};

// @orig none: port lighting (GPU)
std::unique_ptr<GlLighting> GlLighting::create(std::string* error) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        if (error) *error = SDL_GetError();
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    // OpenGL 3.3 core, else OpenGL ES 3.0 (handhelds).
    for (const bool es : {false, true}) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 0 : 3);
        impl->window = SDL_CreateWindow("crusader lighting", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 16,
                                        16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_SKIP_TASKBAR);
        if (impl->window) impl->context = SDL_GL_CreateContext(impl->window);
        if (impl->context) {
            impl->es = es;
            break;
        }
        if (impl->window) SDL_DestroyWindow(impl->window);
        impl->window = nullptr;
    }
    SDL_GL_ResetAttributes();
    if (!impl->context) {
        if (error) *error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }
    SDL_GL_MakeCurrent(impl->window, impl->context);
    if (!impl->load(error) || !impl->build(error)) {
        impl->release();
        SDL_DestroyWindow(impl->window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }
    std::unique_ptr<GlLighting> gl(new GlLighting(std::move(impl)));
    const GLubyte* version = gl->impl_->GetString(kVersion);
    const GLubyte* renderer = gl->impl_->GetString(kRenderer);
    gl->description_ = std::string(version ? reinterpret_cast<const char*>(version) : "?") + " / " +
                       (renderer ? reinterpret_cast<const char*>(renderer) : "?");
    return gl;
}

GlLighting::GlLighting(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GlLighting::~GlLighting() {
    if (!impl_) return;
    impl_->release();
    if (impl_->window) SDL_DestroyWindow(impl_->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// @orig none: port lighting (GPU)
bool GlLighting::shade(const LitFrame& f, std::uint32_t* argb) {
    Impl& g = *impl_;
    if (SDL_GL_MakeCurrent(g.window, g.context) != 0) return false;
    g.GetError();  // clear
    g.UseProgram(g.program);
    g.PixelStorei(kUnpackAlignment, 1);
    g.PixelStorei(kPackAlignment, 4);
    // The picture, the items at each pixel, the palette (0x00RRGGBB, read as B, G, R).
    g.upload(Impl::kScreen, kR8ui, kRedInteger, kUnsignedByte, f.width, f.height, f.screen);
    g.upload(Impl::kCode, kR16ui, kRedInteger, kUnsignedShort, f.width, f.height, f.code.data());
    g.upload(Impl::kPalette, kRgba8, kRgba, kUnsignedByte, 256, 1, f.palette.data());
    g.upload_floats(Impl::kItems, f.items);
    g.upload_floats(Impl::kLights, f.lights);
    g.upload_floats(Impl::kBoxes, f.boxes);
    g.upload_ints(Impl::kLightBoxes, f.light_boxes);
    g.upload_floats(Impl::kPuffs, f.puffs);
    g.ActiveTexture(kTexture0 + Impl::kAtlas);
    g.BindTexture(kTexture2D, g.tex[Impl::kAtlas]);
    for (const LitFrame::Upload& up : f.uploads)
        g.TexSubImage2D(kTexture2D, 0, up.x, up.y, up.w, up.h, kRgba, kUnsignedByte, up.rgba.data());
    // The output picture.
    if (g.target_w != f.width || g.target_h != f.height) {
        g.ActiveTexture(kTexture0 + Impl::kUnits);
        g.BindTexture(kTexture2D, g.target);
        g.TexImage2D(kTexture2D, 0, kRgba8, f.width, f.height, 0, kRgba, kUnsignedByte, nullptr);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.BindFramebuffer(kFramebuffer, g.fbo);
        g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, g.target, 0);
        if (g.CheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) return false;
        g.target_w = f.width;
        g.target_h = f.height;
    }
    g.BindFramebuffer(kFramebuffer, g.fbo);
    g.Viewport(0, 0, f.width, f.height);
    const LightingOptions& o = f.options;
    g.Uniform1i(g.u_view_x, f.view_left);
    g.Uniform1i(g.u_view_y, f.view_top);
    g.Uniform1i(g.u_width, f.width);
    g.Uniform1i(g.u_height, f.height);
    g.Uniform1i(g.u_lights, static_cast<GLint>(f.lights.size() / 12));
    g.Uniform1i(g.u_puffs, static_cast<GLint>(f.puffs.size() / 12));
    g.Uniform1i(g.u_flags, (o.shadows ? 1 : 0) | (o.sprite_shadows ? 2 : 0) | (o.bump ? 4 : 0) | (o.smoke ? 8 : 0));
    g.Uniform1f(g.u_ambient, o.ambient);
    g.BindVertexArray(g.vao);
    g.DrawArrays(kTriangles, 0, 3);
    // Rows come back from the bottom of the framebuffer, which is the top of
    // the picture (the shader takes gl_FragCoord.y as the row).
    g.ReadPixels(0, 0, f.width, f.height, kRgba, kUnsignedByte, argb);
    g.BindFramebuffer(kFramebuffer, 0);
    return g.GetError() == 0;
}

}  // namespace crusader
