// Minimal software 3D rasterizer: RGB565 framebuffer, 1/z depth buffer, near-plane clipping,
// perspective-correct textures, flat Lambert shading, fog, optional spot and rim lights.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

struct Vec3 { float x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(Vec3 v) { float l = std::sqrt(dot(v, v)); return l > 0 ? Vec3{v.x / l, v.y / l, v.z / l} : v; }

struct Vec2 { float u, v; };

inline uint16_t rgb565(int r, int g, int b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

// RGB565 texture; width and height must be powers of two (sampling wraps with a bit mask).
struct Texture {
    int w = 0, h = 0, shift = 0;
    float reflect = 1;                                 // spot-light gain: >1 for retroreflective paint
    std::vector<uint16_t> px;
    Texture(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_) {
        if (w < 1 || h < 1 || (w & (w - 1)) || (h & (h - 1))) throw std::runtime_error("texture size must be a power of two");
        while ((1 << shift) < w) ++shift;
    }
    uint16_t& at(int x, int y) { return px[size_t(y & (h - 1)) * w + (x & (w - 1))]; }
};

// uv in texture units (0..1 spans the texture once); tex == nullptr draws the flat color.
struct Tri { int a, b, c; uint16_t color; const Texture* tex; Vec2 uv[3]; };
struct Mesh {
    std::vector<Vec3> v;
    std::vector<Tri> t;
    bool rim = true;                                   // rim=false for ground planes seen at grazing angles
    float reflect = 1;                                 // spot-light gain for untextured faces
    std::vector<Vec3> tint;                            // optional, one per vertex: colour multiplier for textured faces
    std::vector<std::shared_ptr<Texture>> textures;    // keeps textures referenced by t alive
};

// Axis-aligned box centred at origin; faces wound counter-clockwise seen from outside.
// With a texture, each face shows it once: top/bottom map (x,z), sides (z,y), front/back (x,y), v=0 at the top.
inline Mesh box(float sx, float sy, float sz, uint16_t color, const Texture* tex = nullptr) {
    Mesh m;
    for (int i = 0; i < 8; ++i)
        m.v.push_back({(i & 1 ? sx : -sx) / 2, (i & 2 ? sy : -sy) / 2, (i & 4 ? sz : -sz) / 2});
    const int f[6][4] = {{0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}};
    const int axis[6][2] = {{0,1},{0,1},{0,2},{0,2},{2,1},{2,1}};    // which coordinates become (u, v) per face
    const float size[3] = {sx, sy, sz};
    for (int k = 0; k < 6; ++k) {
        Vec2 uv[4];
        for (int j = 0; j < 4; ++j) {
            const float* p = &m.v[f[k][j]].x;
            int a = axis[k][0], b = axis[k][1];
            uv[j] = {p[a] / size[a] + 0.5f, b == 1 ? 0.5f - p[b] / size[b] : p[b] / size[b] + 0.5f};
        }
        m.t.push_back({f[k][0], f[k][1], f[k][2], color, tex, {uv[0], uv[1], uv[2]}});
        m.t.push_back({f[k][0], f[k][2], f[k][3], color, tex, {uv[0], uv[2], uv[3]}});
    }
    return m;
}

// Appends src moved by `offset` into dst (to build one mesh from several boxes).
inline void append(Mesh& dst, const Mesh& src, Vec3 offset) {
    int base = int(dst.v.size());
    for (auto& p : src.v) dst.v.push_back(p + offset);
    for (auto t : src.t) { t.a += base; t.b += base; t.c += base; dst.t.push_back(t); }
    dst.textures.insert(dst.textures.end(), src.textures.begin(), src.textures.end());
}

// Coloured cone light: full strength inside cosInner, fading to zero at cosOuter.
struct SpotLight { Vec3 pos{0, 3, 0}; Vec3 dir{0, -1, 0}; float cosInner = 0.95f, cosOuter = 0.85f, intensity = 1.2f; Vec3 color{1, 1, 1}; float range = 0; };   // range 0: no falloff
// Backlight rim: brightens faces seen edge-on that also face the light at `pos`.
struct RimLight { Vec3 pos{0, 2, 5}; Vec3 color{0.5f, 0.7f, 1}; float strength = 0; };

struct Transform { Vec3 pos{0, 0, 0}; float yaw = 0, pitch = 0; };

class Renderer {
public:
    int w, h;
    std::vector<uint16_t> color;
    std::vector<float> depth;            // 1/z of the nearest surface so far; 0 = nothing drawn
    Vec3 cam{0, 0, -5};
    float camYaw = 0, camPitch = 0;      // yaw 0 looks down +z (positive turns towards +x); pitch > 0 looks down
    Vec3 light = normalize({-0.4f, 0.8f, -0.6f});
    float focal;
    float ambient = 0.25f, sun = 0.75f;
    std::vector<SpotLight> spots;
    RimLight rim;
    float fogStart = 0, fogEnd = 0;      // fog on when fogEnd > fogStart; triangles wholly beyond fogEnd are skipped
    uint16_t fogColor = 0;
    float nearZ = 0.1f;

    Renderer(int w_, int h_, float fovDeg = 60)
        : w(w_), h(h_), color(size_t(w_) * h_), depth(size_t(w_) * h_), focal(h_ / 2 / std::tan(fovDeg * 3.14159265f / 360)) {}

    void clear(uint16_t c) { std::fill(color.begin(), color.end(), c); std::fill(depth.begin(), depth.end(), 0.f); }

    // Vertical gradient from `top` down to the horizon line, `horizon` colour below it.
    void clearSky(uint16_t top, uint16_t horizon) {
        float hy = h / 2 - focal * std::tan(camPitch);
        for (int y = 0; y < h; ++y) {
            float t = hy > 0 ? std::min(1.f, std::max(0.f, y / hy)) : 1;
            std::fill(color.begin() + size_t(y) * w, color.begin() + size_t(y + 1) * w, mix(top, horizon, t));
        }
        std::fill(depth.begin(), depth.end(), 0.f);
    }

    Vec3 toView(Vec3 p) const {
        Vec3 d = p - cam;
        float x = d.x * cy_ - d.z * sy_, z = d.x * sy_ + d.z * cy_;
        return {x, d.y * cp_ + z * sp_, -d.y * sp_ + z * cp_};
    }

    void draw(const Mesh& m, const Transform& tr) {
        cy_ = std::cos(camYaw); sy_ = std::sin(camYaw); cp_ = std::cos(camPitch); sp_ = std::sin(camPitch);
        float cy = std::cos(tr.yaw), sy = std::sin(tr.yaw), cp = std::cos(tr.pitch), sp = std::sin(tr.pitch);
        world_.resize(m.v.size());
        view_.resize(m.v.size());
        for (size_t i = 0; i < m.v.size(); ++i) {
            Vec3 p = m.v[i];
            Vec3 r{p.x, p.y * cp - p.z * sp, p.y * sp + p.z * cp};                // pitch about the model's x (nose down > 0)
            r = {r.x * cy + r.z * sy, r.y, -r.x * sy + r.z * cy};                 // then yaw about y
            world_[i] = r + tr.pos;
            view_[i] = toView(world_[i]);
        }
        float tx = (w / 2.f) / focal, ty = (h / 2.f) / focal;
        bool fog = fogEnd > fogStart;
        for (auto& t : m.t) {
            Vec3 va = view_[t.a], vb = view_[t.b], vc = view_[t.c];
            if (va.z < nearZ && vb.z < nearZ && vc.z < nearZ) continue;
            if (fog && va.z > fogEnd && vb.z > fogEnd && vc.z > fogEnd) continue;
            // whole triangle outside one side plane of the view frustum
            if ((va.x > va.z * tx && vb.x > vb.z * tx && vc.x > vc.z * tx) || (-va.x > va.z * tx && -vb.x > vb.z * tx && -vc.x > vc.z * tx) ||
                (va.y > va.z * ty && vb.y > vb.z * ty && vc.y > vc.z * ty) || (-va.y > va.z * ty && -vb.y > vb.z * ty && -vc.y > vc.z * ty))
                continue;
            Vec3 a = world_[t.a], b = world_[t.b], c = world_[t.c];
            Vec3 n = normalize(cross(b - a, c - a));
            if (dot(n, a - cam) >= 0) continue;                                    // back-face cull
            bool tinted = t.tex && !m.tint.empty();
            Face f{n, t.color, t.tex, ambient + sun * std::max(0.f, dot(n, light)), m.rim, t.tex ? t.tex->reflect : m.reflect, tinted};
            const Vec3 one{1, 1, 1};
            V in[3] = {{va, a, t.uv[0], tinted ? m.tint[t.a] : one}, {vb, b, t.uv[1], tinted ? m.tint[t.b] : one}, {vc, c, t.uv[2], tinted ? m.tint[t.c] : one}};
            if (va.z >= nearZ && vb.z >= nearZ && vc.z >= nearZ) { raster(in[0], in[1], in[2], f); continue; }
            V out[4];                                                              // clip against z = nearZ
            int k = 0;
            for (int i = 0; i < 3; ++i) {
                const V& p = in[i]; const V& q = in[(i + 1) % 3];
                bool pin = p.p.z >= nearZ, qin = q.p.z >= nearZ;
                if (pin) out[k++] = p;
                if (pin != qin) out[k++] = lerp(p, q, (nearZ - p.p.z) / (q.p.z - p.p.z));
            }
            for (int i = 1; i + 1 < k; ++i) raster(out[0], out[i], out[i + 1], f);
        }
    }

    static uint16_t scale(uint16_t c, float s) { return shade(c, s, s, s); }
    // surface colour times light (fr,fg,fb), plus light added on top regardless of surface colour (ar,ag,ab)
    static uint16_t shade(uint16_t c, float fr, float fg, float fb, float ar = 0, float ag = 0, float ab = 0) {
        int r = std::min(31.f, (c >> 11) * fr + 31 * ar), g = std::min(63.f, ((c >> 5) & 63) * fg + 63 * ag), b = std::min(31.f, (c & 31) * fb + 31 * ab);
        return uint16_t(r << 11 | g << 5 | b);
    }
    static uint16_t mix(uint16_t a, uint16_t b, float t) {
        int k = int(t * 32);
        int r = a >> 11, g = (a >> 5) & 63, bl = a & 31;
        r += (((b >> 11) - r) * k) >> 5; g += ((((b >> 5) & 63) - g) * k) >> 5; bl += (((b & 31) - bl) * k) >> 5;
        return uint16_t(r << 11 | g << 5 | bl);
    }

private:
    struct V { Vec3 p, w; Vec2 uv; Vec3 tint; };   // view-space position, world position, texture coordinate, colour
    struct Face { Vec3 n; uint16_t color; const Texture* tex; float light; bool rim; float reflect; bool tinted; };
    float cy_ = 1, sy_ = 0, cp_ = 1, sp_ = 0;
    std::vector<Vec3> world_, view_;

    static V lerp(const V& a, const V& b, float t) {
        return {a.p + (b.p - a.p) * t, a.w + (b.w - a.w) * t, {a.uv.u + (b.uv.u - a.uv.u) * t, a.uv.v + (b.uv.v - a.uv.v) * t},
                a.tint + (b.tint - a.tint) * t};
    }

    void raster(const V& A, const V& B, const V& C, const Face& f) {
        float x0 = w / 2 + A.p.x * focal / A.p.z, y0 = h / 2 - A.p.y * focal / A.p.z;
        float x1 = w / 2 + B.p.x * focal / B.p.z, y1 = h / 2 - B.p.y * focal / B.p.z;
        float x2 = w / 2 + C.p.x * focal / C.p.z, y2 = h / 2 - C.p.y * focal / C.p.z;
        float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(area) < 1e-6f) return;
        int minx = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
        int maxx = std::min(w - 1, (int)std::ceil(std::max({x0, x1, x2})));
        int miny = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
        int maxy = std::min(h - 1, (int)std::ceil(std::max({y0, y1, y2})));
        if (minx > maxx || miny > maxy) return;
        float iz0 = 1 / A.p.z, iz1 = 1 / B.p.z, iz2 = 1 / C.p.z, inv = 1 / area;
        const Texture* tex = f.tex;
        // attributes pre-divided by z so they interpolate linearly in screen space
        float tw = tex ? tex->w : 0, th = tex ? tex->h : 0;
        float u0 = A.uv.u * tw * iz0, u1 = B.uv.u * tw * iz1, u2 = C.uv.u * tw * iz2;
        float v0 = A.uv.v * th * iz0, v1 = B.uv.v * th * iz1, v2 = C.uv.v * th * iz2;
        Vec3 p0 = A.w * iz0, p1 = B.w * iz1, p2 = C.w * iz2;
        bool lit = f.rim && rim.strength > 0;
        if (!lit && !spots.empty()) {                           // skip per-pixel lighting for triangles no beam can reach
            Vec3 mid = (A.w + B.w + C.w) * (1.f / 3);
            float rad = std::sqrt(std::max(dot(A.w - mid, A.w - mid), std::max(dot(B.w - mid, B.w - mid), dot(C.w - mid, C.w - mid))));
            for (auto& sp : spots) {
                Vec3 d = sp.pos - mid;
                if (sp.range <= 0 || dot(d, d) < (sp.range + rad) * (sp.range + rad)) { lit = true; break; }
            }
        }
        bool fog = fogEnd > fogStart;
        float fogInv = fog ? 1 / (fogEnd - fogStart) : 0;
        int s256 = std::min(511, int(f.light * 256));
        // tint times light, in 1/256ths; interpolated without perspective (colour varies slowly)
        Vec3 k0 = A.tint * (f.light * 256), k1 = B.tint * (f.light * 256), k2 = C.tint * (f.light * 256);
        uint16_t flat = scale(f.color, f.light);
        int tmaskx = tex ? tex->w - 1 : 0, tmasky = tex ? tex->h - 1 : 0, tshift = tex ? tex->shift : 0;
        const uint16_t* tpx = tex ? tex->px.data() : nullptr;
        float dw0 = (y1 - y2) * inv, dw1 = (y2 - y0) * inv;
        const float eps = -1e-5f;
        for (int y = miny; y <= maxy; ++y) {
            float py = y + 0.5f, px = minx + 0.5f;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv;
            bool inside = false;
            for (int x = minx; x <= maxx; ++x, w0 += dw0, w1 += dw1) {
                float w2 = 1 - w0 - w1;
                if (w0 < eps || w1 < eps || w2 < eps) { if (inside) break; continue; }   // convex: row span ended
                inside = true;
                float iz = w0 * iz0 + w1 * iz1 + w2 * iz2;
                size_t i = size_t(y) * w + x;
                if (iz <= depth[i]) continue;
                depth[i] = iz;
                uint16_t c = flat;
                float z = (tex || fog || lit) ? 1 / iz : 0;
                if (tex) {
                    int tu = int((w0 * u0 + w1 * u1 + w2 * u2) * z + 65536.f) & tmaskx;   // offset keeps negatives wrapping
                    int tv = int((w0 * v0 + w1 * v1 + w2 * v2) * z + 65536.f) & tmasky;
                    uint16_t t = tpx[(tv << tshift) | tu];
                    int sr = s256, sg = s256, sb = s256;
                    if (f.tinted) {
                        sr = int(w0 * k0.x + w1 * k1.x + w2 * k2.x); sg = int(w0 * k0.y + w1 * k1.y + w2 * k2.y); sb = int(w0 * k0.z + w1 * k1.z + w2 * k2.z);
                    }
                    int r = std::min(31, ((t >> 11) * sr) >> 8), g = std::min(63, (((t >> 5) & 63) * sg) >> 8), b = std::min(31, ((t & 31) * sb) >> 8);
                    c = uint16_t(r << 11 | g << 5 | b);
                }
                if (lit) c = lights(tex ? c : f.color, tex ? 1.f : f.light, f, (p0 * w0 + p1 * w1 + p2 * w2) * z);
                if (fog && z > fogStart) c = mix(c, fogColor, std::min(1.f, (z - fogStart) * fogInv));
                color[i] = c;
            }
        }
    }

    // Spot and rim lights for one pixel at world position p; `base` already carries `flat` light.
    uint16_t lights(uint16_t base, float flat, const Face& f, Vec3 p) const {
        float fr = flat, fg = flat, fb = flat, ar = 0, ag = 0, ab = 0;
        for (auto& sp : spots) {
            Vec3 d = sp.pos - p;
            float dd = dot(d, d), fall = 1;
            if (sp.range > 0) {                                    // cheap reject before the normalize
                if (dd >= sp.range * sp.range) continue;
                fall = 1 - dd / (sp.range * sp.range);
            }
            float along = -dot(d, sp.dir);                         // outside the cone or facing away: no sqrt needed
            if (along <= 0 || along * along <= sp.cosOuter * sp.cosOuter * dd || dot(f.n, d) <= 0) continue;
            float inv = 1 / std::sqrt(dd);
            float cd = along * inv, ndl = dot(f.n, d) * inv;
            float t = std::min(1.f, (cd - sp.cosOuter) / (sp.cosInner - sp.cosOuter));
            float k = sp.intensity * f.reflect * fall * ndl * t * t * (3 - 2 * t);
            fr += k * sp.color.x; fg += k * sp.color.y; fb += k * sp.color.z;
        }
        if (f.rim && rim.strength > 0) {
            float e = 1 - std::max(0.f, dot(f.n, normalize(cam - p)));
            float k = rim.strength * e * e * std::max(0.f, dot(f.n, normalize(rim.pos - p)));
            ar = k * rim.color.x; ag = k * rim.color.y; ab = k * rim.color.z;   // additive sheen
        }
        return shade(base, fr, fg, fb, ar, ag, ab);
    }
};
