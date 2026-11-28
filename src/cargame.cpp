// Textured 3D car racing game for the Luckfox Pico + ILI9341 (320x240 landscape).
//
// usage: cargame [flip]                play; controls arrive on stdin (tools/drive.py sends them)
//        cargame demo [flip]           play with the player's car on autopilot (no controller needed)
//        cargame ppm SECONDS [KEYS] [TIME WEATHER ROAD]
//                                      headless: simulate SECONDS at 15 fps holding KEYS (any of u d l r n; a = autopilot),
//                                      then print that frame as a PPM image on stdout
//        cargame sim                   host: play like on the board, but write each frame to stdout as the panel's
//                                      SPI bytes (big-endian RGB565, 320x240) at most 24 fps; status goes to stderr
//        cargame bench FRAMES [TIME WEATHER ROAD]
//                                      headless: render FRAMES on autopilot and report the render time
// stdin protocol: '@' + bits (1 up, 2 down, 4 left, 8 right, 16 nitrous); 'r' restarts; 't'/'c'/'g' cycle conditions; 'q' quits.
#include "board_config.h"
#include "ILI9341.h"
#include "font_hud.h"
#include "game.h"
#include "objload.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static const int W = 320, H = 240;
typedef std::shared_ptr<Texture> TexPtr;

// ---------------------------------------------------------------- procedural textures
static float noise(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
}
// Smooth value noise in 0..1, one lattice cell per unit.
static float smoothNoise(float x, float y, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float top = noise(xi, yi, seed) + (noise(xi + 1, yi, seed) - noise(xi, yi, seed)) * fx;
    float bot = noise(xi, yi + 1, seed) + (noise(xi + 1, yi + 1, seed) - noise(xi, yi + 1, seed)) * fx;
    return top + (bot - top) * fy;
}
static int clamp255(float v) { return std::max(0, std::min(255, int(v))); }
static uint16_t rgbf(float r, float g, float b) { return rgb565(clamp255(r), clamp255(g), clamp255(b)); }

static void paintTex(TexPtr& tex, int size, const std::function<uint16_t(int, int)>& paint) {
    if (!tex) tex = std::make_shared<Texture>(size, size); // meshes keep pointers to the texture objects
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) tex->at(x, y) = paint(x, y);
}

struct Textures {
    static const int kWalls = 8, kTowers = 3, kRoofs = 3;
    TexPtr grass, road, verge, kerb, checker, bark, foliage, leaves, water, rock, wall[kWalls], tower[kTowers], roof[kRoofs];
    Vec3 wallColor[kWalls];                                                         // average wall colour, for the gables
    Textures(Weather weather, RoadSurface surface) {
        setGround(weather, surface);
        paintTex(kerb, 16, [](int, int y) { return y < 8 ? rgbf(205, 35, 35) : rgbf(235, 235, 235); });
        kerb->reflect = 9;                                                          // kerbs shine back in headlights
        paintTex(checker, 16, [](int x, int y) { return ((x / 4 + y / 4) & 1) ? rgbf(240, 240, 240) : rgbf(20, 20, 20); });
        paintTex(bark, 16, [](int x, int y) {
            float n = noise(x, 0, 4) * 0.7f + noise(x, y, 5) * 0.3f;
            return rgbf(75 + 40 * n, 50 + 25 * n, 30 + 15 * n);
        });
        paintBuildings(false);
        const Vec3 roofColor[kRoofs] = {{176, 74, 48}, {74, 80, 92}, {120, 84, 56}};   // clay tiles, slate, shingles
        for (int k = 0; k < kRoofs; ++k) {
            Vec3 rc = roofColor[k];
            paintTex(roof[k], 16, [k, rc](int x, int y) {
                int row = y / 4, tile = (x + (row & 1) * 2) / 4;
                float v = 0.85f + 0.3f * noise(tile, row, 70 + k) + 0.06f * noise(x, y, 73);
                if (y % 4 == 3) v *= 0.55f;                                          // shadow under each course
                if (k == 0 && (x + (row & 1) * 2) % 4 == 0) v *= 0.75f;
                return rgbf(rc.x * v, rc.y * v, rc.z * v);
            });
        }
    }

    // Lit window glass at night: a warm orange the night tint leaves alone (see kLit), so it glows.
    static uint16_t litWindow(int x, int y) { float n = noise(x, y, 34); return rgbf(255, 150 + 30 * n, 30 + 10 * n); }
    static bool kLit(uint16_t c) {                       // r high, g about 0.45..0.7 of r, b tiny: only lit windows
        int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        return r >= 10 && b * 5 <= r && g >= r * 9 / 10 && g <= r * 14 / 10;
    }
    // Facades: day windows reflect the sky, night windows glow.
    void paintBuildings(bool night) {
        // House walls: one 32x32 tile is one storey of facade with a window. Brick, wooden planks, stone or plaster.
        struct Wall { int kind; Vec3 color; uint16_t shutter; };   // kind: 0 brick, 1 planks, 2 stone, 3 plaster
        const Wall walls[kWalls] = {{0, {168, 74, 52}, 0}, {0, {128, 86, 62}, 0}, {1, {232, 230, 220}, rgb565(52, 92, 60)},
                                    {1, {112, 150, 182}, 0xFFFF}, {1, {150, 106, 66}, 0}, {2, {160, 152, 136}, 0},
                                    {3, {234, 214, 170}, rgb565(70, 100, 150)}, {3, {226, 170, 156}, 0xFFFF}};
        for (int k = 0; k < kWalls; ++k) {
            const Wall wl = walls[k];
            Vec3 sum{0, 0, 0};
            paintTex(wall[k], 32, [&](int x, int y) {
                uint16_t c;
                if (x >= 10 && x <= 21 && y >= 7 && y <= 21) {                       // window: frame, mullions, sky in the glass
                    bool frame = x == 10 || x == 21 || y == 7 || y == 21 || x == 15 || x == 16 || y == 14;
                    float sky = 1 - (y - 7) / 14.f + 0.25f * ((x + y) % 9 < 2);
                    c = frame ? rgbf(236, 234, 226) : night ? litWindow(x, y) : rgbf(60 + 70 * sky, 82 + 80 * sky, 112 + 90 * sky);
                } else if (wl.shutter && (x == 7 || x == 8 || x == 23 || x == 24) && y >= 7 && y <= 21) {
                    c = Renderer::scale(wl.shutter, (y & 1) ? 0.8f : 1.f);
                } else if (y == 22 && x >= 9 && x <= 22) {
                    c = rgbf(200, 198, 190);                                         // sill
                } else {
                    float n = noise(x, y, 30 + k), v = 1;
                    if (wl.kind == 0) {                                              // bricks 8x4, every other row offset, mortar
                        int row = y / 4, bx = (x + (row & 1) * 4) / 8;
                        v = (y % 4 == 3 || (x + (row & 1) * 4) % 8 == 7) ? 1.45f : 0.82f + 0.3f * noise(bx, row, 60 + k) + 0.08f * n;
                    } else if (wl.kind == 1) {                                       // horizontal boards with dark seams
                        v = y % 4 == 3 ? 0.62f : 0.9f + 0.1f * noise(x / 6, y / 4, 61 + k) + 0.06f * n;
                    } else if (wl.kind == 2) {                                       // stone courses of uneven blocks
                        int row = y / 5, bx = (x + row * 3) / (5 + row % 3);
                        v = (y % 5 == 4 || (x + row * 3) % (5 + row % 3) == 0) ? 0.6f : 0.8f + 0.35f * noise(bx, row, 62 + k) + 0.1f * n;
                    } else {
                        v = 0.92f + 0.08f * n + 0.05f * smoothNoise(x / 5.f, y / 5.f, 63 + k);
                    }
                    c = rgbf(wl.color.x * v, wl.color.y * v, wl.color.z * v);
                }
                sum = sum + Vec3{float((c >> 11) << 3), float(((c >> 5) & 63) << 2), float((c & 31) << 3)};
                return c;
            });
            wallColor[k] = sum * (1.f / 1024);
        }
        for (int k = 0; k < kTowers; ++k)                                           // tower blocks: concrete, brick, glass
            paintTex(tower[k], 32, [k, night](int x, int y) {
                float n = noise(x, y, 31 + k);
                bool glass = k == 2 ? (x % 16 != 0 && y % 16 != 0) : (x >= 3 && x <= 28 && y >= 6 && y <= 20);
                if (glass && night) return noise(x / 8, y / 16, 33 + k) > 0.4f ? litWindow(x, y) : rgbf(28, 32, 40);   // most flats lit
                if (glass) {
                    float sky = 1 - (y % 16) / 20.f + 0.3f * (((x + y) % 11) < 2);
                    return rgbf(55 + 60 * sky, 85 + 70 * sky, 115 + 80 * sky);
                }
                if (k == 1) return (y % 4 == 3 || (x + (y / 4 & 1) * 4) % 8 == 7) ? rgbf(190, 180, 170) : rgbf(150 + 30 * n, 82 + 15 * n, 60 + 10 * n);
                if (k == 2) return rgbf(70, 74, 80);
                return y >= 22 && y <= 24 ? rgbf(130, 128, 124) : rgbf(196 + 20 * n, 192 + 20 * n, 184 + 18 * n);
            });
    }

    void setRoad(Weather weather, RoadSurface surface) {
        paintTex(road, 64, [weather, surface](int x, int y) {
            float n = noise(x, y, 3) * 0.7f + noise(x / 3, y / 5, 8) * 0.3f;
            if (surface == RoadSurface::Asphalt) {
                float wet = weather == Weather::Rain ? 0.62f : 1.f;
                bool line = x < 2 || x > 61 || (x >= 31 && x <= 32 && y < 24);
                return line ? rgbf(213 * wet, 211 * wet, 193 * wet)
                            : rgbf((42 + 12 * n) * wet, (44 + 11 * n) * wet, (47 + 11 * n) * wet);
            }
            bool rut = (x >= 13 && x <= 22) || (x >= 41 && x <= 50);
            float wet = weather == Weather::Rain ? 0.68f : 1.f;
            if (!rut && noise(x, y, 9) > 0.93f) return rgbf(150 * wet, 140 * wet, 120 * wet);
            float k = (rut ? 0.82f : 1.f) * wet;
            return rgbf((118 + 34 * n) * k, (98 + 28 * n) * k, (72 + 20 * n) * k);
        });
    }

    void setGround(Weather weather, RoadSurface surface) {
        setRoad(weather, surface);
        paintTex(grass, 64, [](int x, int y) {                                     // grey detail: the colour map colours it
            float n = noise(x, y, 1) * 0.6f + noise(x / 4, y / 4, 2) * 0.4f;
            return rgbf(170 + 70 * n, 170 + 70 * n, 170 + 70 * n);
        });
        paintTex(verge, 16, [weather](int x, int y) {
            float n = noise(x, y, 10);
            if (weather == Weather::Snow) return rgbf(195 + 35 * n, 205 + 30 * n, 215 + 25 * n);
            if (n * 1.2f < x / 15.f) return rgbf(95 + 40 * n, 140 + 45 * n, 45 + 25 * n);
            return rgbf(110 + 30 * n, 92 + 24 * n, 68 + 18 * n);
        });
        paintTex(foliage, 32, [weather](int x, int y) {
            float n = noise(x / 2, y / 2, 13) * 0.6f + noise(x, y, 14) * 0.4f;
            if (weather == Weather::Snow && n > y / 48.f) return rgbf(168 + 60 * n, 181 + 55 * n, 188 + 52 * n);
            return rgbf(40 + 40 * n, 95 + 50 * n, 20 + 20 * n);
        });
        paintTex(water, 32, [weather](int x, int y) {
            float n = noise(x, y, 40), ripple = noise(x / 6, y, 41) > 0.82f ? 1.f : 0.f;   // short glints across the flow
            if (weather == Weather::Snow) return noise(x, y / 8, 42) > 0.93f ? rgbf(150, 175, 195) : rgbf(190 + 20 * n, 212 + 15 * n, 228 + 12 * n);
            float k = weather == Weather::Rain ? 0.7f : 1.f;
            return rgbf((38 + 14 * n + 90 * ripple) * k, (92 + 16 * n + 90 * ripple) * k, (138 + 18 * n + 80 * ripple) * k);
        });
        paintTex(rock, 16, [weather](int x, int y) {
            float n = noise(x, y, 47) * 0.6f + noise(x / 3, y / 3, 48) * 0.4f;
            if (weather == Weather::Snow && n > 0.5f) return rgbf(220, 226, 234);
            return rgbf(112 + 50 * n, 110 + 46 * n, 104 + 40 * n);
        });
        paintTex(leaves, 32, [weather](int x, int y) {
            float n = noise(x / 2, y / 2, 6) * 0.6f + noise(x, y, 7) * 0.4f;
            if (weather == Weather::Snow && n > y / 48.f) return rgbf(165 + 60 * n, 178 + 55 * n, 185 + 52 * n);
            return rgbf(12 + 25 * n, 55 + 45 * n, 45 + 30 * n);
        });
    }
};

// ---------------------------------------------------------------- meshes
// Quad p0..p3 (in order around its edge) facing `out`.
static void addQuad(Mesh& m, const Vec3 (&p)[4], const Vec2 (&uv)[4], const Texture* tex, Vec3 out, uint16_t color = 0xFFFF) {
    int b = int(m.v.size());
    for (auto& q : p) m.v.push_back(q);
    if (dot(cross(p[1] - p[0], p[2] - p[0]), out) >= 0) {
        m.t.push_back({b, b + 1, b + 2, color, tex, {uv[0], uv[1], uv[2]}});
        m.t.push_back({b, b + 2, b + 3, color, tex, {uv[0], uv[2], uv[3]}});
    } else {
        m.t.push_back({b, b + 2, b + 1, color, tex, {uv[0], uv[2], uv[1]}});
        m.t.push_back({b, b + 3, b + 2, color, tex, {uv[0], uv[3], uv[2]}});
    }
}
// Flat-coloured triangle facing `out`.
static void addTri(Mesh& m, Vec3 a, Vec3 b, Vec3 c, uint16_t color, Vec3 out) {
    int k = int(m.v.size());
    m.v.push_back(a); m.v.push_back(b); m.v.push_back(c);
    if (dot(cross(b - a, c - a), out) >= 0) m.t.push_back({k, k + 1, k + 2, color, nullptr, {}});
    else m.t.push_back({k, k + 2, k + 1, color, nullptr, {}});
}
// Flat-coloured cylinder around the y axis, centred on the origin.
static Mesh cylinder(float r, float h, int sides, uint16_t side, uint16_t cap) {
    Mesh m;
    const Vec2 none[4] = {};
    for (int i = 0; i < sides; ++i) {
        float a0 = 2 * kPi * i / sides, a1 = 2 * kPi * (i + 1) / sides;
        Vec3 p0{r * std::sin(a0), 0, r * std::cos(a0)}, p1{r * std::sin(a1), 0, r * std::cos(a1)}, up{0, h / 2, 0};
        Vec3 q[4] = {p0 - up, p1 - up, p1 + up, p0 + up};
        addQuad(m, q, none, nullptr, (p0 + p1) * 0.5f, side);
        addTri(m, up, p0 + up, p1 + up, cap, {0, 1, 0});
        addTri(m, up * -1, p0 - up, p1 - up, cap, {0, -1, 0});
    }
    return m;
}

// Cone with `sides` faces around the y axis, base at y=0 (with a cap) and tip at y=h.
static Mesh cone(float r, float h, int sides, const Texture* tex) {
    Mesh m;
    m.v.push_back({0, h, 0});
    m.v.push_back({0, 0, 0});
    for (int i = 0; i < sides; ++i) m.v.push_back({r * std::sin(2 * kPi * i / sides), 0, r * std::cos(2 * kPi * i / sides)});
    for (int i = 0; i < sides; ++i) {
        int a = 2 + i, b = 2 + (i + 1) % sides;
        float u0 = float(i) / sides, u1 = float(i + 1) / sides;
        m.t.push_back({0, a, b, 0xFFFF, tex, {{(u0 + u1) / 2, 0}, {u0, 1.5f}, {u1, 1.5f}}});   // facing out
        m.t.push_back({1, b, a, 0xFFFF, tex, {{0.5f, 0.5f}, {u1, 0}, {u0, 0}}});
    }
    return m;
}

// Kenney car model: turned to face +z with its wheels on y=0, its colour-palette texture baked into flat
// triangle colours (each triangle sits inside one palette swatch; also cheaper to draw).
static Mesh loadCar(const std::string& path, int paint) {
    Mesh m = loadObj(path, 4.2f);
    float minY = 1e9f;
    for (auto& v : m.v) minY = std::min(minY, v.y);
    for (auto& v : m.v) v = {-v.x, v.y - minY, -v.z};                             // models face -z once loaded
    for (auto& t : m.t) {
        if (!t.tex) continue;
        float u = (t.uv[0].u + t.uv[1].u + t.uv[2].u) / 3, v = (t.uv[0].v + t.uv[1].v + t.uv[2].v) / 3;
        int x = int(std::floor(u * t.tex->w)) & (t.tex->w - 1), y = int(std::floor(v * t.tex->h)) & (t.tex->h - 1);
        uint16_t c = t.tex->px[size_t(y) * t.tex->w + x];
        int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        if (paint && r * 2 > g + 16 && r > b + 8) {
            const int colors[6][3] = {{255, 80, 60}, {55, 120, 255}, {55, 210, 85},
                                      {240, 190, 35}, {45, 205, 210}, {190, 90, 220}};
            c = rgb565(colors[paint][0] * r / 31, colors[paint][1] * r / 31, colors[paint][2] * r / 31);
        }
        if (r < 12 && g < 24 && b < 12) c = Renderer::mix(c, rgb565(150, 155, 170), 0.35f); // dark trim reads as a black blob on the TFT
        t.color = c;
        t.tex = nullptr;
    }
    m.textures.clear();
    return m;
}

struct Bounds { float front = 1.9f, rear = -1.9f, halfWidth = 0.9f, lampY = 0.6f; };   // where a car's lamps sit

struct World {
    Textures tx;
    std::vector<Mesh> ground;                     // terrain in 40x40 chunks, so chunks out of view are skipped
    std::vector<Vec3> groundAt;                   // chunk centres
    Mesh road, gantry, pine[4], marker, flag[3], water, bridge, coin, tank, flame[2], tunnelIn, tunnelOut;
    std::vector<Mesh> buildings;                  // one per race.blocks entry (railings stay empty)
    std::vector<Vec3> porch;                      // each house's porch lamp, in its block's frame (towers: none)
    std::vector<std::vector<Vec3>> windows;       // centres of each house's road-facing windows, in its block's frame
    std::vector<Transform> markersAt, flagsAt;
    Transform bridgeAt;
    std::vector<Vec3> tunnelLamps;                // ceiling lamps
    Vec3 tunnelMid{0, 0, 0};
    Mesh cars[6];
    Bounds carBounds[6];
    Transform gantryAt;

    World(const Race& race, const std::string& assets) : tx(race.weather, race.surface) {
        const Track& tr = race.track;
        const Vec3 up{0, 1, 0};
        const int cell = 5, chunk = 40, half = 160;
        for (int cz = -half; cz < half; cz += chunk)
            for (int cx = -half; cx < half; cx += chunk) {
                Mesh m;
                m.rim = false;
                const int n = chunk / cell + 1;
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i) {
                        float x = float(cx + i * cell), z = float(cz + j * cell);
                        m.v.push_back({x, race.terrain.height(x, z), z});
                    }
                for (int j = 0; j + 1 < n; ++j)
                    for (int i = 0; i + 1 < n; ++i) {
                        int a = j * n + i, b = a + 1, c = a + n + 1, d = a + n;
                        auto uv = [&](int k) { return Vec2{m.v[k].x / 6, m.v[k].z / 6}; };
                        const Texture* g = tx.grass.get();
                        auto dry = [&](int k) { return m.v[k].y > Terrain::waterLevel(m.v[k].x, m.v[k].z) - 0.05f; };
                        if (dry(a) || dry(d) || dry(c)) m.t.push_back({a, d, c, 0xFFFF, g, {uv(a), uv(d), uv(c)}});   // facing up; the lake
                        if (dry(a) || dry(c) || dry(b)) m.t.push_back({a, c, b, 0xFFFF, g, {uv(a), uv(c), uv(b)}});   // bed under water is skipped
                    }
                ground.push_back(std::move(m));
                groundAt.push_back({cx + chunk / 2.f, race.terrain.height(cx + chunk / 2.f, cz + chunk / 2.f), cz + chunk / 2.f});
            }
        float hw = tr.halfWidth, kw = tr.kerb;
        auto onGround = [&](Vec3 p, float lift) { p.y = Terrain::base(p.x, p.z) + lift; return p; };
        road.rim = false;
        for (int i = 0; i < tr.n(); ++i) {                                          // road, dirt verges both sides
            Vec3 c0 = tr.at(i), c1 = tr.at(i + 1), r0 = tr.right(i), r1 = tr.right(i + 1);
            float v0 = i * tr.spacing / 8, v1 = (i + 1) * tr.spacing / 8;
            Vec3 p[4] = {onGround(c0 - r0 * hw, 0.1f), onGround(c0 + r0 * hw, 0.1f), onGround(c1 + r1 * hw, 0.1f), onGround(c1 - r1 * hw, 0.1f)};
            Vec2 uv[4] = {{0, v0}, {1, v0}, {1, v1}, {0, v1}};
            addQuad(road, p, uv, tx.road.get(), up);
            for (float s : {-1.f, 1.f}) {                                           // red-white kerb, then a dirt verge
                const float kerbW = 0.9f;
                Vec3 k[4] = {onGround(c0 + r0 * (s * hw), 0.1f), onGround(c0 + r0 * (s * (hw + kerbW)), 0.1f),
                             onGround(c1 + r1 * (s * (hw + kerbW)), 0.1f), onGround(c1 + r1 * (s * hw), 0.1f)};
                Vec2 kuv[4] = {{0, v0 * 2}, {1, v0 * 2}, {1, v1 * 2}, {0, v1 * 2}};
                addQuad(road, k, kuv, tx.kerb.get(), up);
                Vec3 g[4] = {k[1], onGround(c0 + r0 * (s * (hw + kw)), 0.1f), onGround(c1 + r1 * (s * (hw + kw)), 0.1f), k[2]};
                Vec2 guv[4] = {{0, v0 * 4}, {1, v0 * 4}, {1, v1 * 4}, {0, v1 * 4}};
                addQuad(road, g, guv, tx.verge.get(), up);
            }
        }
        Vec3 s0 = tr.at(0), r = tr.right(0), d = tr.dir(0);                         // chequered start line
        Vec3 line[4] = {onGround(s0 - r * hw - d * 0.8f, 0.14f), onGround(s0 + r * hw - d * 0.8f, 0.14f),
                        onGround(s0 + r * hw + d * 0.8f, 0.14f), onGround(s0 - r * hw + d * 0.8f, 0.14f)};
        Vec2 luv[4] = {{0, 0}, {5, 0}, {5, 0.4f}, {0, 0.4f}};
        addQuad(road, line, luv, tx.checker.get(), up);
        // start gantry: two posts and a chequered banner, built along local x then turned to face the track
        float span = hw + kw + 1;
        append(gantry, box(0.4f, 6.5f, 0.4f, rgb565(200, 200, 205)), {-span, 2.75f, 0});   // posts reach below a banked road
        append(gantry, box(0.4f, 6.5f, 0.4f, rgb565(200, 200, 205)), {span, 2.75f, 0});
        append(gantry, box(2 * span + 0.4f, 1.2f, 0.3f, 0xFFFF, tx.checker.get()), {0, 5.4f, 0});
        gantryAt.pos = onGround(tr.at(0), 0);
        gantryAt.yaw = std::atan2(d.x, d.z);
        // pine trees, a tall and a short one
        for (int k = 0; k < 2; ++k) {
            float s = k ? 0.75f : 1.f;
            append(pine[k], box(0.5f, 2 * s, 0.5f, 0xFFFF, tx.bark.get()), {0, s, 0});
            append(pine[k], cone(2.4f * s, 3.4f * s, 7, tx.leaves.get()), {0, 1.4f * s, 0});
            append(pine[k], cone(1.9f * s, 3.0f * s, 7, tx.leaves.get()), {0, 3.2f * s, 0});
            append(pine[k], cone(1.3f * s, 2.6f * s, 7, tx.leaves.get()), {0, 4.9f * s, 0});
        }
        auto flip = [](Mesh m) { for (auto& v : m.v) v.y = -v.y; for (auto& t : m.t) std::swap(t.b, t.c); return m; };
        auto blob = [&](float r, float h, int sides, const Texture* tex) {             // double cone: low-poly ball
            Mesh m; append(m, cone(r, h, sides, tex), {0, 0, 0}); append(m, flip(cone(r, h * 0.8f, sides, tex)), {0, 0, 0}); return m;
        };
        append(pine[2], box(0.45f, 3.2f, 0.45f, 0xFFFF, tx.bark.get()), {0, 1.6f, 0});   // broadleaf tree
        append(pine[2], blob(2.6f, 2.2f, 7, tx.foliage.get()), {0, 4.2f, 0});
        append(pine[2], blob(1.7f, 1.6f, 6, tx.foliage.get()), {0.9f, 5.6f, 0.4f});
        const uint16_t flagCol[3] = {rgb565(230, 40, 40), rgb565(250, 200, 30), rgb565(40, 110, 230)};
        for (int k = 0; k < 3; ++k) {
            append(flag[k], box(0.12f, 4.5f, 0.12f, rgb565(210, 210, 215)), {0, 2.25f, 0});
            append(flag[k], box(0.05f, 1.0f, 1.6f, flagCol[k]), {0, 3.9f, 0.85f});
        }
        for (int i = -3; i <= 3; ++i) {                                                  // flags lining the start straight
            if (i == 0) continue;
            for (float side : {-1.f, 1.f}) {
                int k = (i + tr.n()) % tr.n();
                Transform t;
                t.pos = tr.at(k) + tr.right(k) * (side * (hw + kw + 2.5f));
                t.pos.y = race.terrain.height(t.pos.x, t.pos.z);
                t.yaw = std::atan2(tr.dir(k).x, tr.dir(k).z) + 1.2f;
                flagsAt.push_back(t);
            }
        }
        append(marker, box(0.28f, 1.4f, 0.28f, rgb565(242, 238, 223)), {0, 0.5f, 0});
        marker.reflect = 6;                                                         // roadside posts too
        append(marker, box(0.31f, 0.28f, 0.31f, rgb565(210, 54, 35)), {0, 0.97f, 0});
        auto roadside = [&](int i, float side, float setback) {
            Vec3 outward = tr.right(i) * side;
            Transform t;
            t.pos = tr.at(i) + outward * setback;
            t.yaw = std::atan2(-outward.x, -outward.z);
            return t;
        };
        for (int i = 4; i < tr.n() - 4; i += 8) {
            if (std::abs(i - race.bridge) * tr.spacing < race.bridgeLen / 2 + 3) continue;   // the bridge has railings
            for (float side : {-1.f, 1.f}) {
                Transform t = roadside(i, side, hw + kw + 1.5f);
                t.pos.y = race.terrain.height(t.pos.x, t.pos.z);
                markersAt.push_back(t);
            }
        }
        buildScenery(race);
        colourGround(race, race.weather);
        try {
            cars[0] = loadCar(assets + "sedan-sports.obj", 0);
            cars[1] = loadCar(assets + "race.obj", 1);
            cars[2] = loadCar(assets + "hatchback-sports.obj", 2);
            cars[3] = loadCar(assets + "race-future.obj", 3);
            cars[4] = loadCar(assets + "suv.obj", 4);
            cars[5] = loadCar(assets + "police.obj", 5);
            for (int i = 0; i < 6; ++i) {
                Bounds& b = carBounds[i];
                b.front = -1e9f; b.rear = 1e9f; b.halfWidth = 0;
                float top = -1e9f, bottom = 1e9f;
                for (const Vec3& v : cars[i].v) {
                    b.front = std::max(b.front, v.z); b.rear = std::min(b.rear, v.z);
                    b.halfWidth = std::max(b.halfWidth, std::fabs(v.x));
                    top = std::max(top, v.y); bottom = std::min(bottom, v.y);
                }
                b.lampY = bottom + 0.38f * (top - bottom);
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(e.what()) + " (car models missing? run `make deploy`)");
        }
    }

    // Lake, river, houses, towers, the bridge, rocks, coins and nitrous tanks.
    void buildScenery(const Race& race) {
        const Track& tr = race.track;
        const Vec3 up{0, 1, 0};
        auto wpt = [](float x, float z) { return Vec3{x, Terrain::waterLevel(x, z), z}; };
        auto wuv = [](Vec3 p) { return Vec2{p.x / 8, p.z / 8}; };
        water.rim = false;
        const float reach = Terrain::kRiverHalf + Terrain::kBank;                   // the banks hide whatever is past the shore
        std::vector<Vec3> line;                                                     // river centreline every ~6 units
        const auto& R = Terrain::riverPts();
        for (int i = 0; i + 1 < Terrain::kRiverPts; ++i) {
            Vec3 a{R[i][0], 0, R[i][1]}, b{R[i + 1][0], 0, R[i + 1][1]};
            int steps = std::max(1, int(length(b - a) / 6));
            for (int s = 0; s < steps; ++s) line.push_back(a + (b - a) * (float(s) / steps));
        }
        line.push_back({R[Terrain::kRiverPts - 1][0], 0, R[Terrain::kRiverPts - 1][1]});
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            auto side = [&](size_t k) {
                Vec3 d = normalize(line[std::min(k + 1, line.size() - 1)] - line[k > 0 ? k - 1 : 0]);
                return Vec3{d.z, 0, -d.x} * reach;
            };
            Vec3 s0 = side(i), s1 = side(i + 1), a = line[i], b = line[i + 1];
            Vec3 q[4] = {wpt(a.x - s0.x, a.z - s0.z), wpt(a.x + s0.x, a.z + s0.z), wpt(b.x + s1.x, b.z + s1.z), wpt(b.x - s1.x, b.z - s1.z)};
            Vec2 uv[4] = {wuv(q[0]), wuv(q[1]), wuv(q[2]), wuv(q[3])};
            addQuad(water, q, uv, tx.water.get(), up);
        }
        const int segs = 20;
        Vec3 lc = wpt(Terrain::kLakeX, Terrain::kLakeZ);
        for (int i = 0; i < segs; ++i) {                                             // lake: a fan out past the shore
            float r = Terrain::kLakeR + Terrain::kBank, a0 = 2 * kPi * i / segs, a1 = 2 * kPi * (i + 1) / segs;
            Vec3 p0 = wpt(lc.x + r * std::sin(a0), lc.z + r * std::cos(a0)), p1 = wpt(lc.x + r * std::sin(a1), lc.z + r * std::cos(a1));
            int k = int(water.v.size());
            water.v.push_back(lc); water.v.push_back(p0); water.v.push_back(p1);
            water.t.push_back({k, k + 1, k + 2, 0xFFFF, tx.water.get(), {wuv(lc), wuv(p0), wuv(p1)}});   // +z then +x: facing up
        }
        porch.assign(race.blocks.size(), Vec3{0, -1, 0});
        windows.assign(race.blocks.size(), {});
        for (size_t i = 0; i < race.blocks.size(); ++i) buildings.push_back(building(race.blocks[i], int(i)));
        {   // steel truss bridge, built along +z
            const float L = race.bridgeLen, X = tr.halfWidth + tr.kerb + 0.3f;
            const uint16_t steel = rgb565(186, 52, 40), concrete = rgb565(178, 176, 170);
            for (float s : {-1.f, 1.f}) {
                append(bridge, box(0.7f, 3.2f, L, rgb565(120, 116, 108)), {s * X, -1.5f, 0});   // girder down into the water
                append(bridge, box(0.5f, 0.9f, L, concrete), {s * X, 0.45f, 0});                 // parapet
                append(bridge, box(0.35f, 0.35f, L, steel), {s * X, 4.6f, 0});                   // top chord
                for (int k = -3; k <= 3; ++k) append(bridge, box(0.3f, 3.7f, 0.3f, steel), {s * X, 2.75f, k * (L / 2 - 0.3f) / 3});
            }
            for (float z : {-L / 2 + 0.3f, L / 2 - 0.3f}) append(bridge, box(2 * X + 0.3f, 0.45f, 0.35f, steel), {0, 4.6f, z});   // portals
            Vec3 c = tr.at(race.bridge), d = tr.dir(race.bridge), a = c - d * (L / 2), b = c + d * (L / 2);
            bridgeAt.pos = {c.x, Terrain::base(c.x, c.z), c.z};
            bridgeAt.yaw = std::atan2(d.x, d.z);
            bridgeAt.pitch = -std::atan2(Terrain::base(b.x, b.z) - Terrain::base(a.x, a.z), L);   // follows the road's slope
        }
        {   // tunnel through a hill: an arched concrete liner, a grassy mound over it, a lit floor, built along the bend of the road
            static const float arch[6][2] = {{6.8f, 0}, {6.8f, 2.6f}, {5.7f, 3.9f}, {3.8f, 4.8f}, {1.8f, 5.2f}, {0, 5.3f}};     // wall to crown
            static const float hill[6][2] = {{12, -0.3f}, {10.6f, 2.2f}, {8.6f, 4.4f}, {6.0f, 6.2f}, {3, 7.3f}, {0, 7.8f}};
            const int lo = race.tunnelLo, hi = race.tunnelHi, P = 11;
            const uint16_t concrete[2] = {rgb565(172, 170, 162), rgb565(150, 148, 142)}, lip = rgb565(205, 202, 192);
            auto ring = [&](int i, bool outer) {                                    // one cross-section, left wall to right wall
                std::vector<Vec3> pts;
                float f = outer ? smooth01(std::min(i - lo, hi - i) / 4.f) : 0;     // the mound melts into the hillside at both mouths
                for (int j = 0; j < P; ++j) {
                    int h = 5 - std::abs(j - 5);
                    float sgn = j < 5 ? -1.f : 1.f, x = arch[h][0] + (hill[h][0] - arch[h][0]) * f, y = arch[h][1] + (hill[h][1] - arch[h][1]) * f;
                    Vec3 c = tr.at(i) + tr.right(i) * (sgn * x);
                    pts.push_back({c.x, Terrain::base(c.x, c.z) + 0.1f + y, c.z});
                }
                return pts;
            };
            const Vec2 none[4] = {};
            for (int i = lo; i < hi; ++i) {
                std::vector<Vec3> a = ring(i, false), b = ring(i + 1, false), oa = ring(i, true), ob = ring(i + 1, true);
                Vec3 axis = tr.at(i) + Vec3{0, Terrain::base(tr.at(i).x, tr.at(i).z) + 2.6f, 0};
                for (int j = 0; j + 1 < P; ++j) {
                    Vec3 q[4] = {a[j], a[j + 1], b[j + 1], b[j]};
                    addQuad(tunnelIn, q, none, nullptr, axis - (q[0] + q[2]) * 0.5f, concrete[i & 1]);
                    Vec3 o[4] = {oa[j], oa[j + 1], ob[j + 1], ob[j]};
                    Vec2 uv[4] = {{o[0].x / 6, o[0].z / 6}, {o[1].x / 6, o[1].z / 6}, {o[2].x / 6, o[2].z / 6}, {o[3].x / 6, o[3].z / 6}};
                    addQuad(tunnelOut, o, uv, tx.grass.get(), (o[0] + o[2]) * 0.5f - axis);
                }
                float v0 = i * tr.spacing / 8, v1 = (i + 1) * tr.spacing / 8;      // floor over the road: dimmer than the open road outside
                Vec3 c0 = tr.at(i), c1 = tr.at(i + 1), r0 = tr.right(i), r1 = tr.right(i + 1);
                auto fl = [&](Vec3 c, Vec3 r, float x) { Vec3 p = c + r * x; return Vec3{p.x, Terrain::base(p.x, p.z) + 0.14f, p.z}; };
                Vec3 pv[4] = {fl(c0, r0, -5), fl(c0, r0, 5), fl(c1, r1, 5), fl(c1, r1, -5)};
                Vec2 rv[4] = {{0, v0}, {1, v0}, {1, v1}, {0, v1}};
                addQuad(tunnelIn, pv, rv, tx.road.get(), {0, 1, 0});
                for (float s : {-1.f, 1.f}) {
                    Vec3 k[4] = {fl(c0, r0, s * 5), fl(c0, r0, s * 5.9f), fl(c1, r1, s * 5.9f), fl(c1, r1, s * 5)};
                    Vec2 kv[4] = {{0, v0 * 2}, {1, v0 * 2}, {1, v1 * 2}, {0, v1 * 2}};
                    addQuad(tunnelIn, k, kv, tx.kerb.get(), {0, 1, 0});
                    Vec3 g[4] = {k[1], fl(c0, r0, s * 6.8f), fl(c1, r1, s * 6.8f), k[2]};
                    Vec2 gv[4] = {{0, v0 * 4}, {1, v0 * 4}, {1, v1 * 4}, {0, v1 * 4}};
                    addQuad(tunnelIn, g, gv, tx.verge.get(), {0, 1, 0});
                }
                if (i > lo && i < hi && i % 2 == 0) tunnelLamps.push_back({a[5].x, a[5].y - 0.35f, a[5].z});
            }
            for (int end = 0; end < 2; ++end) {                                     // a pale concrete frame round each mouth
                int i = end ? hi : lo;
                Vec3 out = tr.dir(i) * (end ? 1.f : -1.f), axis = tr.at(i) + Vec3{0, Terrain::base(tr.at(i).x, tr.at(i).z) + 2.6f, 0};
                std::vector<Vec3> in = ring(i, false), fr;
                for (const Vec3& p : in) fr.push_back(axis + (p - axis) * 1.14f + Vec3{0, 0.15f, 0});
                for (int j = 0; j + 1 < P; ++j) {
                    Vec3 q[4] = {in[j], in[j + 1], fr[j + 1], fr[j]};
                    addQuad(tunnelOut, q, none, nullptr, out, lip);
                }
            }
            tunnelMid = tr.at((lo + hi) / 2);
        }
        auto flip = [](Mesh m) { for (auto& v : m.v) v.y = -v.y; for (auto& t : m.t) std::swap(t.b, t.c); return m; };
        append(pine[3], cone(0.9f, 1.1f, 6, tx.rock.get()), {0, 0, 0});            // boulders, about the trees' collision size
        append(pine[3], flip(cone(0.9f, 0.8f, 6, tx.rock.get())), {0, 0, 0});
        append(pine[3], cone(0.55f, 0.6f, 5, tx.rock.get()), {0.8f, 0, 0.4f});
        coin = cylinder(0.62f, 0.16f, 8, rgb565(214, 150, 20), rgb565(255, 212, 50));
        coin.reflect = 5;
        append(tank, cylinder(0.38f, 1.2f, 8, rgb565(40, 110, 235), rgb565(40, 110, 235)), {0, 0, 0});
        append(tank, cylinder(0.39f, 0.18f, 8, 0xFFFF, 0xFFFF), {0, 0.25f, 0});                 // white band
        Mesh dome = cone(0.38f, 0.3f, 8, nullptr);
        for (auto& t : dome.t) t.color = rgb565(200, 205, 215);                                 // silver dome
        append(tank, dome, {0, 0.6f, 0});
        append(tank, box(0.14f, 0.3f, 0.14f, rgb565(220, 40, 40)), {0, 1.0f, 0});              // valve
        tank.reflect = 5;
        for (int k = 0; k < 2; ++k) {                                               // nitrous plasma, long and short for flicker
            const float L = k ? 0.8f : 1.4f;                                        // white-hot core, blue-violet sheath, orange tip
            struct Layer { float r, len; Vec3 c; };
            for (Layer l : {Layer{0.26f, 0.70f * L, {80, 90, 255}}, Layer{0.19f, 0.95f * L, {70, 140, 255}}, Layer{0.08f, 1.25f * L, {225, 245, 255}}}) {
                Mesh m = cone(l.r, l.len, 6, nullptr);
                for (auto& t : m.t) t.color = rgb565(int(l.c.x), int(l.c.y), int(l.c.z));
                append(flame[k], m, {0, 0, 0});
            }
            Mesh tip = cone(0.1f, 0.5f * L, 6, nullptr);                            // a faint orange wisp beyond the sheath
            for (auto& t : tip.t) t.color = rgb565(255, 140, 50);
            append(flame[k], tip, {0, 1.0f * L, 0});
        }
    }

    // Terrain colour map, by place: lush valleys, dry grass up the hills, rock on steep slopes, sand at the water,
    // slow meadow-to-heath patches everywhere; snow and rain recolour it. Multiplies the grey grass detail texture.
    void colourGround(const Race& race, Weather weather) {
        auto mix = [](Vec3 a, Vec3 b, float t) { return a + (b - a) * std::max(0.f, std::min(1.f, t)); };
        const Vec3 lush{62, 120, 42}, meadow{108, 152, 54}, heath{122, 128, 60}, dry{170, 160, 88}, rock{128, 118, 104}, sand{206, 186, 138};
        for (Mesh& m : ground) {
            m.tint.resize(m.v.size());
            for (size_t i = 0; i < m.v.size(); ++i) {
                Vec3 p = m.v[i];
                float hx = race.terrain.height(p.x + 2.5f, p.z) - race.terrain.height(p.x - 2.5f, p.z);
                float hz = race.terrain.height(p.x, p.z + 2.5f) - race.terrain.height(p.x, p.z - 2.5f);
                float slope = std::hypot(hx, hz) / 5, rise = p.y - Terrain::base(p.x, p.z);
                Vec3 c = mix(lush, meadow, smoothNoise(p.x / 35, p.z / 35, 80) * 1.6f - 0.3f);
                c = mix(c, heath, smoothNoise(p.x / 22, p.z / 22, 81) * 2.5f - 1.6f);
                c = mix(c, dry, (rise - 4) / 8 + 0.3f * (smoothNoise(p.x / 12, p.z / 12, 82) - 0.5f));
                c = mix(c, rock, (slope - 0.45f) / 0.35f);
                c = mix(c, sand, Terrain::wet(p.x, p.z) * 3);
                if (weather == Weather::Snow) c = mix(Vec3{226, 232, 242}, rock, (slope - 0.6f) / 0.3f);
                else if (weather == Weather::Rain) c = c * 0.82f;
                m.tint[i] = c * (1.f / 205);                                        // the detail texture averages ~205
            }
        }
        tunnelOut.tint.resize(tunnelOut.v.size());                                   // the hill over the tunnel: lush at the foot, dry on top
        for (size_t i = 0; i < tunnelOut.v.size(); ++i) {
            Vec3 p = tunnelOut.v[i];
            Vec3 c = mix(lush, dry, (p.y - Terrain::base(p.x, p.z)) / 9);
            if (weather == Weather::Snow) c = Vec3{226, 232, 242};
            else if (weather == Weather::Rain) c = c * 0.82f;
            tunnelOut.tint[i] = c * (1.f / 205);
        }
    }

    // A house or tower of its own: size from its block, storeys, walls, roof and chimney picked from its index.
    Mesh building(const Block& b, int i) {
        Mesh m;
        if (b.kind == Block::Railing) return m;
        auto pick = [&](int what, int n) { return std::min(n - 1, int(noise(i, what, 90) * n)); };
        int wallStyle = (i * 5 + 3) % Textures::kWalls;                             // neighbours never share a wall or roof
        bool tower = b.kind == Block::Tower;
        int storeys = tower ? 4 + pick(1, 3) : 1 + (noise(i, 1, 91) > 0.55f);
        float h = 3 * storeys, hx = b.hx, hz = b.hz;
        const Texture* wallTex = tower ? tx.tower[pick(2, Textures::kTowers)].get() : tx.wall[wallStyle].get();
        const Vec2 none[4] = {};
        for (int f = 0; f < 4; ++f) {                                               // walls, footing 1.5 below ground
            float len = f < 2 ? 2 * hx : 2 * hz, sgn = f % 2 ? 1.f : -1.f;
            float tiles = std::max(1.f, std::round(len / (tower ? 3.f : 3.4f)));
            Vec3 a, c, out;
            if (f < 2) { a = {-hx, 0, sgn * hz}; c = {hx, 0, sgn * hz}; out = {0, 0, sgn}; }
            else { a = {sgn * hx, 0, -hz}; c = {sgn * hx, 0, hz}; out = {sgn, 0, 0}; }
            Vec3 q[4] = {a + Vec3{0, -1.5f, 0}, c + Vec3{0, -1.5f, 0}, c + Vec3{0, h, 0}, a + Vec3{0, h, 0}};
            float vb = storeys + 0.5f;
            Vec2 uv[4] = {{0, vb}, {tiles, vb}, {tiles, 0}, {0, 0}};
            addQuad(m, q, uv, wallTex, out);
        }
        if (tower) {
            append(m, box(2 * hx + 0.4f, 0.5f, 2 * hz + 0.4f, rgb565(112, 112, 116)), {0, h + 0.25f, 0});
            append(m, box(hx * 0.6f, 1.6f, hz * 0.6f, rgb565(150, 150, 154)), {hx * 0.3f, h + 1.3f, -hz * 0.2f});
            append(m, box(2 * hx + 0.1f, 0.4f, 2 * hz + 0.1f, rgb565(150, 146, 140)), {0, 3, 0});   // ledge over the shops
            return m;
        }
        const Texture* roofTex = tx.roof[(i + i / 3) % Textures::kRoofs].get();
        float rh = 1.4f + 1.6f * noise(i, 4, 90), rx = hx + 0.35f, rz = hz + 0.55f, slope = std::hypot(rz, rh);
        Vec3 e[4] = {{-rx, h, -rz}, {rx, h, -rz}, {rx, h, rz}, {-rx, h, rz}}, r0{-rx, h + rh, 0}, r1{rx, h + rh, 0};
        Vec2 ruv[4] = {{0, slope / 2}, {2 * rx / 2, slope / 2}, {2 * rx / 2, 0}, {0, 0}};
        Vec3 front[4] = {e[3], e[2], r1, r0}, back[4] = {e[0], e[1], r1, r0}, under[4] = {e[0], e[1], e[2], e[3]};
        addQuad(m, front, ruv, roofTex, {0, 1, 1});
        addQuad(m, back, ruv, roofTex, {0, 1, -1});
        addQuad(m, under, none, nullptr, {0, -1, 0}, rgb565(70, 60, 55));
        Vec3 wc = tx.wallColor[wallStyle];
        Vec3 g[2][3] = {{{-hx, h, -hz}, {-hx, h, hz}, {-hx, h + rh * hz / rz, 0}}, {{hx, h, -hz}, {hx, h, hz}, {hx, h + rh * hz / rz, 0}}};
        addTri(m, g[0][0], g[0][1], g[0][2], rgbf(wc.x, wc.y, wc.z), {-1, 0, 0});     // gables in the wall colour
        addTri(m, g[1][0], g[1][1], g[1][2], rgbf(wc.x, wc.y, wc.z), {1, 0, 0});
        float tileW = 2 * hx / std::max(1.f, std::round(2 * hx / 3.4f));
        append(m, box(1.1f, 2.2f, 0.12f, rgb565(96, 62, 40)), {-hx + tileW, 1.1f, hz + 0.05f});   // door between windows, facing the road
        append(m, box(1.5f, 0.15f, 0.7f, rgb565(80, 76, 72)), {-hx + tileW, 2.35f, hz + 0.35f});   // porch canopy
        porch[i] = {-hx + tileW + 0.75f, 2.0f, hz + 0.2f};
        for (int f = 0; f < 4; ++f) {                                               // window centres on all four walls, 0.3 out
            float len = f < 2 ? 2 * hx : 2 * hz, step = len / std::max(1.f, std::round(len / 3.4f)), sgn = f % 2 ? 1.f : -1.f;
            for (int s = 0; s < storeys; ++s)                                       // 1.4 below each storey's top
                for (float t = -len / 2 + step / 2; t < len / 2; t += step)
                    windows[i].push_back(f < 2 ? Vec3{t, h - 3 * s - 1.4f, sgn * (hz + 0.3f)} : Vec3{sgn * (hx + 0.3f), h - 3 * s - 1.4f, t});
        }
        if (noise(i, 5, 90) > 0.35f)
            append(m, box(0.6f, 1.8f, 0.6f, rgb565(126, 66, 52)), {hx * (noise(i, 6, 90) - 0.5f), h + rh * 0.7f, -hz * 0.35f});   // chimney
        return m;
    }
};

// ---------------------------------------------------------------- HUD
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
    if (a == 0) return bg;
    if (a >= 252) return fg;
    int br = (bg >> 11) & 0x1F;
    int bg_g = (bg >> 5) & 0x3F;
    int bb = bg & 0x1F;
    int fr = (fg >> 11) & 0x1F;
    int fg_g = (fg >> 5) & 0x3F;
    int fb_b = fg & 0x1F;
    int r = br + (((fr - br) * a + 128) >> 8);
    int g = bg_g + (((fg_g - bg_g) * a + 128) >> 8);
    int b = bb + (((fb_b - bb) * a + 128) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t blendDarken565(uint16_t bg, uint8_t a) {
    if (a <= 6) return bg;
    int br = (bg >> 11) & 0x1F;
    int bg_g = (bg >> 5) & 0x3F;
    int bb = bg & 0x1F;
    int r = br - ((br * a + 128) >> 8);
    int g = bg_g - ((bg_g * a + 128) >> 8);
    int b = bb - ((bb * a + 128) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int textWidth(const HudFont& font, const std::string& s) {
    int w = 0;
    for (char c : s) {
        unsigned char ch = (unsigned char)c;
        if (ch >= 32 && ch <= 126) w += font.glyphs[ch - 32].advance;
    }
    return w;
}

static void drawText(std::vector<uint16_t>& fb, const HudFont& font, int x, int y, const std::string& s, uint16_t col, bool outline = true) {
    if (s.empty()) return;

    if (outline) {
        static const struct { int8_t dx, dy; uint8_t a; } kOffsets[] = {
            {1, 1, 230}   // one crisp drop shadow; a soft halo all round blurs small text
        };
        int cur_x = x;
        for (char c : s) {
            unsigned char ch = (unsigned char)c;
            if (ch < 32 || ch > 126) continue;
            const HudGlyph& g = font.glyphs[ch - 32];
            if (g.w > 0 && g.h > 0) {
                const uint8_t* bmp = font.bitmaps + g.offset;
                for (const auto& off : kOffsets) {
                    for (int gy = 0; gy < g.h; ++gy) {
                        int py = y + g.top + gy + off.dy;
                        if (py < 0 || py >= H) continue;
                        size_t row_idx = size_t(py) * W;
                        const uint8_t* row = bmp + gy * g.w;
                        for (int gx = 0; gx < g.w; ++gx) {
                            int px = cur_x + g.left + gx + off.dx;
                            if (px < 0 || px >= W) continue;
                            uint8_t a = (uint16_t(row[gx]) * off.a) >> 8;
                            if (a > 6) {
                                fb[row_idx + px] = blendDarken565(fb[row_idx + px], a);
                            }
                        }
                    }
                }
            }
            cur_x += g.advance;
        }
    }

    int cur_x = x;
    for (char c : s) {
        unsigned char ch = (unsigned char)c;
        if (ch < 32 || ch > 126) continue;
        const HudGlyph& g = font.glyphs[ch - 32];
        if (g.w > 0 && g.h > 0) {
            const uint8_t* bmp = font.bitmaps + g.offset;
            for (int gy = 0; gy < g.h; ++gy) {
                int py = y + g.top + gy;
                if (py < 0 || py >= H) continue;
                size_t row_idx = size_t(py) * W;
                const uint8_t* row = bmp + gy * g.w;
                for (int gx = 0; gx < g.w; ++gx) {
                    int px = cur_x + g.left + gx;
                    if (px < 0 || px >= W) continue;
                    uint8_t a = row[gx];
                    if (a > 4) {
                        fb[row_idx + px] = blend565(fb[row_idx + px], col, a);
                    }
                }
            }
        }
        cur_x += g.advance;
    }
}

static void centred(std::vector<uint16_t>& fb, const HudFont& font, int y, const std::string& s, uint16_t col) {
    drawText(fb, font, (W - textWidth(font, s)) / 2, y, s, col, true);
}

static std::string clock(float t) {
    if (t < 0) t = 0;
    char b[16];
    std::snprintf(b, sizeof b, "%d:%04.1f", int(t) / 60, std::fmod(t, 60.f));
    return b;
}

static void minimap(std::vector<uint16_t>& fb, const Race& race) {
    const int S = 70, X0 = W - S - 4, Y0 = H - S - 4;
    for (int y = Y0; y < Y0 + S; ++y)                                               // darkened panel
        for (int x = X0; x < X0 + S; ++x) fb[size_t(y) * W + x] = (fb[size_t(y) * W + x] >> 1) & 0x7BEF;
    float lo = -125, span = 250;
    auto plot = [&](Vec3 p, uint16_t col, int r) {
        int mx = X0 + int((p.x - lo) / span * S), my = Y0 + S - 1 - int((p.z - lo) / span * S);
        for (int y = my - r; y <= my + r; ++y)
            for (int x = mx - r; x <= mx + r; ++x)
                if (x >= X0 && x < X0 + S && y >= Y0 && y < Y0 + S) fb[size_t(y) * W + x] = col;
    };
    for (auto& p : race.track.c) plot(p, rgb565(200, 200, 200), 0);
    const uint16_t dots[6] = {rgb565(255, 60, 60), rgb565(80, 140, 255), rgb565(80, 220, 90),
                              rgb565(255, 210, 50), rgb565(80, 220, 220), rgb565(190, 90, 220)};
    for (int i = int(race.cars.size()) - 1; i >= 0; --i) plot(race.cars[i].pos, dots[i], 1);
}

static const float kNewRaceDelay = 6;                                           // seconds on the finish screen before the menu

static void hud(std::vector<uint16_t>& fb, const Race& race) {
    const Car& me = race.cars[0];
    const uint16_t white = 0xFFFF;
    const uint16_t labelColor = rgb565(180, 200, 220);
    const uint16_t gold = rgb565(255, 215, 60);

    // Top-left: Lap, race time, and best lap
    int lap = std::max(1, std::min(race.laps, me.lapsDone + 1));
    std::string lapStr = std::to_string(lap) + "/" + std::to_string(race.laps);
    drawText(fb, fontSmall, 8, 7, "LAP", labelColor);
    drawText(fb, fontMedium, 8 + textWidth(fontSmall, "LAP") + 5, 5, lapStr, white);

    std::string timeStr = clock(me.finishTime >= 0 ? me.finishTime : race.time);
    drawText(fb, fontMedium, 8, 22, timeStr, white);

    if (me.bestLap > 0) {
        drawText(fb, fontSmall, 8, 41, "BEST", gold);
        drawText(fb, fontSmall, 8 + textWidth(fontSmall, "BEST") + 5, 41, clock(me.bestLap), white);
    }

    // Top-right: Position x/N (uses race.cars.size())
    int p = race.position(0);
    int totalCars = int(race.cars.size());
    std::string pStr = std::to_string(p);
    std::string slashCars = "/" + std::to_string(totalCars);
    int wSlash = textWidth(fontSmall, slashCars);
    int wPos = textWidth(fontLarge, pStr);
    int wLbl = textWidth(fontSmall, "POS");
    int xSlash = W - 8 - wSlash;
    int xPos = xSlash - 3 - wPos;
    int xLbl = xPos - 4 - wLbl;
    drawText(fb, fontSmall, xLbl, 13, "POS", labelColor);
    drawText(fb, fontLarge, xPos, 5, pStr, p == 1 ? gold : white);
    drawText(fb, fontSmall, xSlash, 13, slashCars, labelColor);

    // Top-centre: coins collected
    std::string coins = std::to_string(me.coins);
    int cx = (W - textWidth(fontMedium, coins) - 16) / 2 + 6;
    for (int y = -6; y <= 6; ++y)
        for (int x = -6; x <= 6; ++x)
            if (x * x + y * y <= 36) fb[size_t(13 + y) * W + cx + x] = x * x + y * y > 20 ? rgb565(190, 130, 20) : x + y < -3 ? rgb565(255, 240, 150) : gold;
    drawText(fb, fontMedium, cx + 10, 5, coins, white);

    // Bottom-left: nitrous gauge above the speedometer
    int nx = 8 + textWidth(fontSmall, "N2O") + 5, ny = H - 45, nw = 64;
    drawText(fb, fontSmall, 8, ny - 4, "N2O", labelColor);
    for (int y = ny; y < ny + 6; ++y)
        for (int x = nx; x < nx + nw; ++x) {
            uint16_t& px = fb[size_t(y) * W + x];
            bool full = x - nx < int(me.nitro * nw + 0.5f);
            px = full ? (me.boosting && (int(race.time * 12) & 1) ? white : rgb565(60, 200, 255)) : (px >> 1) & 0x7BEF;
        }

    // Bottom-left: Speedometer
    std::string kmh = std::to_string(int(std::fabs(me.speed) * 4 + 0.5f));
    drawText(fb, fontLarge, 8, H - 31, kmh, white);
    int wKmh = textWidth(fontLarge, kmh);
    drawText(fb, fontSmall, 8 + wKmh + 4, H - 21, "KM/H", labelColor);

    // Bottom-right: Minimap
    minimap(fb, race);

    // Center: Countdown / GO!
    if (race.time < 0) {
        std::string cd = std::to_string(int(-race.time) + 1);
        centred(fb, fontHuge, 72, cd, gold);
    } else if (race.time < 1.0f) {
        centred(fb, fontHuge, 72, "GO!", rgb565(60, 245, 100));
    }

    // Finish screen
    if (me.finishTime >= 0) {
        const char* th = p == 1 ? "ST" : p == 2 ? "ND" : p == 3 ? "RD" : "TH";
        std::string finTitle = "FINISHED " + std::to_string(p) + th;
        centred(fb, fontLarge, 50, finTitle, p == 1 ? gold : white);
        centred(fb, fontMedium, 80, "TIME " + clock(me.finishTime), white);
        centred(fb, fontSmall, 104, "NEW RACE IN " + std::to_string(std::max(1, int(std::ceil(kNewRaceDelay - (race.time - me.finishTime))))), labelColor);
    }
}

// ---------------------------------------------------------------- frame rendering
enum class TimeOfDay { Night, Morning, Day, Evening };
// Colours are what ends up on screen: the sky is painted pre-divided by `tint`, which then colours the whole frame.
struct Daylight {
    const char* name;
    Vec3 horizon, zenith, glow, disc, tint, cloudLit, cloudShade;   // glow: horizon colour towards the sun or moon
    float heading, elevation, ambient, sun, glowStrength, cover;
    uint16_t fog;
};
static const Daylight kDaylight[] = {
    {"night",   {26, 44, 84},   {4, 9, 26},     {58, 82, 130},  {232, 238, 250}, {0.58f, 0.80f, 1.40f},
                {92, 108, 140}, {12, 17, 32},   -0.28f, 0.30f, 0.42f, 0.62f, 0.8f, 0.40f, rgb565(26, 38, 56)},
    {"morning", {192, 206, 222},{86, 138, 204}, {255, 192, 138},{255, 238, 196}, {1.05f, 0.98f, 0.92f},
                {255, 228, 204},{146, 146, 172}, 0.25f, 0.14f, 0.50f, 0.62f, 1.0f, 0.40f, rgb565(104, 124, 128)},
    {"day",     {172, 204, 234},{38, 96, 190},  {236, 242, 248},{255, 250, 236}, {1, 1, 1},
                {255, 255, 255},{164, 175, 194},-0.38f, 0.38f, 0.60f, 0.66f, 0.4f, 0.45f, rgb565(92, 120, 112)},
    {"evening", {196, 128, 126},{46, 58, 124},  {255, 158, 58}, {255, 204, 118}, {1.16f, 0.98f, 0.80f},
                {255, 172, 104},{92, 68, 100},  -0.18f, 0.09f, 0.54f, 0.72f, 1.0f, 0.50f, rgb565(110, 86, 78)}
};
static Vec3 weatherTint(Weather w) {
    return w == Weather::Rain ? Vec3{0.92f, 0.97f, 1.04f} : w == Weather::Snow ? Vec3{0.96f, 1.0f, 1.06f} : Vec3{1, 1, 1};
}

static const char* timeName(TimeOfDay t) { return kDaylight[int(t)].name; }
static const char* weatherName(Weather w) { return w == Weather::Sunny ? "sunny" : w == Weather::Rain ? "rain" : "snow"; }
static const char* roadName(RoadSurface s) { return s == RoadSurface::Asphalt ? "asphalt" : "gravel"; }

// ---------------------------------------------------------------- start menu
// A row of option cards over a still of the grid. Left/right pick a card, middle changes it (or starts on GO).
static void menu(std::vector<uint16_t>& fb, const std::vector<uint16_t>& scene, int sel, TimeOfDay tod, Weather weather,
                 RoadSurface surface, const Texture& road, const Texture& checker) {
    fb = scene;
    const int TW = 60, TH = 44, CW = 72, CH = 92, Y0 = H - CH - 6;
    auto px = [&](int x, int y) -> uint16_t& { return fb[size_t(y) * W + x]; };
    auto disc = [&](int tx, int ty, float cx, float cy, float rad, uint16_t col) {
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad) px(tx + x, ty + y) = col;
    };
    auto cloud = [&](int tx, int ty, uint16_t col) {
        disc(tx, ty, 20, 20, 9, col); disc(tx, ty, 32, 15, 11, col); disc(tx, ty, 43, 21, 8, col);
        for (int y = 20; y < 29; ++y) for (int x = 20; x < 44; ++x) px(tx + x, ty + y) = col;
    };
    auto toV = [](Vec3 c) { return rgb565(clamp255(c.x), clamp255(c.y), clamp255(c.z)); };
    auto upper = [](std::string s) { for (char& c : s) c = char(std::toupper((unsigned char)c)); return s; };
    centred(fb, fontLarge, 10, "RACE SETUP", rgb565(255, 215, 60));
    centred(fb, fontSmall, Y0 - 16, "LEFT/RIGHT: PICK   MIDDLE: CHANGE", rgb565(220, 230, 240));
    const char* labels[4] = {"TIME", "WEATHER", "ROAD", "START"};
    std::string values[4] = {upper(timeName(tod)), upper(weatherName(weather)), upper(roadName(surface)), "GO!"};
    for (int i = 0; i < 4; ++i) {
        int x0 = 7 + i * (CW + 6), tx = x0 + (CW - TW) / 2, ty = Y0 + 22;
        for (int y = Y0; y < Y0 + CH; ++y)                                          // darkened card
            for (int x = x0; x < x0 + CW; ++x) {
                bool edge = x < x0 + 2 || x >= x0 + CW - 2 || y < Y0 + 2 || y >= Y0 + CH - 2;
                px(x, y) = edge ? (i == sel ? rgb565(255, 215, 60) : rgb565(90, 100, 115)) : (px(x, y) >> 2) & 0x39E7;
            }
        drawText(fb, fontSmall, x0 + (CW - textWidth(fontSmall, labels[i])) / 2, Y0 + 6, labels[i], rgb565(180, 200, 220));
        if (i == 0) {                                                               // sky colours of that time, with its sun or moon
            const Daylight& d = kDaylight[int(tod)];
            for (int y = 0; y < TH; ++y)
                for (int x = 0; x < TW; ++x) px(tx + x, ty + y) = toV(d.zenith + (d.horizon - d.zenith) * (y / float(TH - 10)));
            float sx = tod == TimeOfDay::Morning ? 14 : tod == TimeOfDay::Evening ? 46 : 38;
            float sy = tod == TimeOfDay::Day ? 12 : tod == TimeOfDay::Night ? 11 : 30;
            if (tod != TimeOfDay::Day) disc(tx, ty, sx, sy + 3, 11, toV(d.glow));
            disc(tx, ty, sx, sy, 6, toV(d.disc));
            if (tod == TimeOfDay::Night) {
                disc(tx, ty, sx + 3, sy - 2, 5, toV(d.zenith));                    // crescent
                for (int s = 0; s < 9; ++s) px(tx + 4 + s * 6, ty + 4 + (s * 7) % 20) = 0xFFFF;
            }
            uint16_t land = tod == TimeOfDay::Night ? rgb565(16, 30, 22) : rgb565(52, 104, 46);
            for (int y = TH - 10; y < TH; ++y) for (int x = 0; x < TW; ++x) px(tx + x, ty + y) = land;
        } else if (i == 1) {                                                        // sun, rain or snow
            uint16_t bg = weather == Weather::Sunny ? rgb565(90, 160, 230) : weather == Weather::Rain ? rgb565(70, 80, 92) : rgb565(150, 162, 176);
            for (int y = 0; y < TH; ++y) for (int x = 0; x < TW; ++x) px(tx + x, ty + y) = bg;
            if (weather == Weather::Sunny) {
                for (int a = 0; a < 8; ++a)
                    for (float r = 13; r < 19; r += 0.5f)
                        px(tx + 30 + int(r * std::cos(a * kPi / 4)), ty + 22 + int(r * std::sin(a * kPi / 4))) = rgb565(255, 220, 80);
                disc(tx, ty, 30, 22, 10, rgb565(255, 220, 80));
            } else if (weather == Weather::Rain) {
                cloud(tx, ty - 4, rgb565(170, 176, 186));
                for (int s = 0; s < 7; ++s)
                    for (int k = 0; k < 6; ++k) px(tx + 16 + s * 5 - k / 2, ty + 30 + k + (s & 1) * 4) = rgb565(120, 180, 255);
            } else {
                cloud(tx, ty - 4, rgb565(236, 240, 246));
                for (int s = 0; s < 8; ++s) disc(tx, ty, 14 + s * 4.5f, 33 + (s & 1) * 5, 1.5f, 0xFFFF);
            }
        } else {                                                                    // the game's own road or chequered-flag texture
            const Texture& t = i == 2 ? road : checker;
            for (int y = 0; y < TH; ++y)
                for (int x = 0; x < TW; ++x) px(tx + x, ty + y) = t.px[size_t(y * t.h / TH) * t.w + x * t.w / TW];
        }
        const HudFont& vf = i == 3 ? fontMedium : fontSmall;
        drawText(fb, vf, x0 + (CW - textWidth(vf, values[i])) / 2, Y0 + 72, values[i], i == 3 ? rgb565(60, 245, 100) : 0xFFFF);
    }
}
// Sky panorama: a 360-degree picture of sky, clouds, snowy mountains and wooded hills, painted when conditions change
// and fixed to compass direction so it turns with the camera. Column = heading, row = elevation above horizon.
struct Sky {
    static const int kW = 1440, kH = 160;
    static constexpr float kTop = 0.56f, kBottom = -0.10f;       // elevation (radians) of the first and last row
    struct Sample {
        uint16_t slopeColor[2] = {};   // mountain shaded from the left or right
        uint8_t haze = 0, cloud = 0, foot = 0;   // zero haze identifies a sample above the mountains; foot: fade into the fog
        bool forest = false;
    };
    std::vector<Sample> samples = std::vector<Sample>(size_t(kW) * kH);
    std::vector<uint16_t> px = std::vector<uint16_t>(size_t(kW) * kH);
    std::vector<uint16_t> tint;

    // Smooth value noise that wraps every `period` cells in x, summed over octaves.
    static float vnoise(float x, float y, int period, uint32_t seed) {
        int xi = int(std::floor(x)), yi = int(std::floor(y));
        float fx = x - xi, fy = y - yi;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        auto n = [&](int i, int j) { return noise(((i % period) + period) % period, j, seed); };
        float top = n(xi, yi) + (n(xi + 1, yi) - n(xi, yi)) * fx, bot = n(xi, yi + 1) + (n(xi + 1, yi + 1) - n(xi, yi + 1)) * fx;
        return top + (bot - top) * fy;
    }
    static float fbm(float u, float y, int period, uint32_t seed) {    // u in 0..1 around the horizon
        float sum = 0, amp = 0.5f;
        for (int o = 0; o < 5; ++o, amp *= 0.5f, period *= 2, y *= 2) sum += amp * vnoise(u * period, y, period, seed + o);
        return sum / 0.97f;
    }

    Sky() {
        // ridged noise gives sharp crests; each range is a height profile around the horizon
        auto ridge = [&](float u, int period, uint32_t seed) {
            float sum = 0, amp = 0.55f;
            for (int o = 0; o < 5; ++o, amp *= 0.48f, period *= 2) sum += amp * (1 - std::fabs(2 * vnoise(u * period, 0.5f, period, seed + o) - 1));
            return sum;
        };
        struct Range { float base, height; int period; uint32_t seed; Vec3 rock; float haze, snowLine; };
        const Range ranges[3] = {{0.03f, 0.26f, 9, 100, {96, 108, 140}, 0.10f, 0.15f},   // far, tall, snowy
                                 {0.02f, 0.13f, 15, 200, {84, 92, 106}, 0.05f, 0.11f},    // middle rock
                                 {0.01f, 0.06f, 24, 300, {46, 72, 50}, 0.02f, 9.f}};     // near forest
        float h[3][kW], dh[3][kW];
        for (int k = 0; k < 3; ++k) {
            for (int x = 0; x < kW; ++x) {
                float r = ridge(float(x) / kW, ranges[k].period, ranges[k].seed);
                h[k][x] = ranges[k].base + ranges[k].height * std::pow(std::max(0.f, r - 0.2f) / 0.8f, 1.2f);
            }
            for (int x = 0; x < kW; ++x) dh[k][x] = (h[k][(x + 6) % kW] - h[k][(x + kW - 6) % kW]) * kW / 12 / (2 * kPi);   // wide slope: no per-column stripes
        }
        for (int x = 0; x < kW; ++x) {
            float u = float(x) / kW;
            for (int y = 0; y < kH; ++y) {
                float e = kTop + (kBottom - kTop) * y / (kH - 1);
                Sample& sample = samples[size_t(y) * kW + x];
                int k = 2;
                while (k >= 0 && e >= h[k][x]) --k;
                if (k < 0) {
                    float cloud = fbm(u, e * 10, 10, 70) * 0.7f + fbm(u, e * 34, 36, 80) * 0.3f;
                    sample.cloud = uint8_t(cloud * 255);
                    continue;
                }
                const Range& range = ranges[k];
                float depth = (h[k][x] - e) / (h[k][x] - kBottom);
                float depthCubed = depth * depth * depth;
                sample.haze = uint8_t((range.haze + 0.5f * depthCubed * depthCubed) * 255);
                sample.foot = uint8_t(0.5f * depthCubed * depthCubed * 255);
                sample.forest = k == 2;
                float n = fbm(u, e * 60, 260, range.seed + 9);
                float snow = range.snowLine + 0.03f * (fbm(u, e * 40, 120, range.seed + 7) - 0.5f);
                for (int dir = 0; dir < 2; ++dir) {
                    float side = dir ? 1.f : -1.f;
                    float gully = fbm(u + e * 0.5f * side, e * 10, 150, range.seed + 5);   // streaks run down the slope
                    float rock = fbm(u + 0.13f, e * 30, 200, range.seed + 11);
                    float slope = std::max(0.f, std::min(1.f, 0.55f + 1.4f * side * dh[k][x] + 0.6f * (gully - 0.5f) + 0.25f * (rock - 0.5f)));
                    Vec3 color = range.rock * (0.35f + 0.9f * slope) * (0.88f + 0.24f * n);
                    if (e > snow + 0.04f * (gully - 0.5f))
                        color = (Vec3{122, 141, 168} + Vec3{82, 68, 51} * slope) * (0.92f + 0.08f * n);
                    sample.slopeColor[dir] = rgbf(color.x, color.y, color.z);
                }
            }
        }
    }

    void paint(uint16_t haze, TimeOfDay tod, Weather weather) {
        auto lerp = [](Vec3 a, Vec3 b, float t) { return a + (b - a) * t; };
        auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
        const Daylight& light = kDaylight[int(tod)];
        const bool night = tod == TimeOfDay::Night, overcast = weather != Weather::Sunny;
        const float sunU = light.heading - std::floor(light.heading), sunE = light.elevation;
        const float cosE = std::cos(sunE);
        const float rangeShade = night ? (overcast ? 0.22f : 0.32f)
                               : weather == Weather::Rain ? 0.62f : weather == Weather::Snow ? 0.85f : 1.f;
        Vec3 wt = weatherTint(weather);
        Vec3 tn{light.tint.x * wt.x, light.tint.y * wt.y, light.tint.z * wt.z};
        float sat = (night ? 0.55f : 1.f) * (weather == Weather::Rain ? 0.75f : weather == Weather::Snow ? 0.85f : 1.f);
        bool neutral = sat > 0.99f && std::fabs(tn.x - 1) + std::fabs(tn.y - 1) + std::fabs(tn.z - 1) < 0.01f;
        tint.resize(neutral ? 0 : 65536);
        for (size_t i = 0; i < tint.size(); ++i) {   // colour of the light, and less colour in dim or wet light
            if (Textures::kLit(uint16_t(i))) { tint[i] = uint16_t(i); continue; }   // lit windows keep their glow
            float r = float((i >> 11) << 3), g = float(((i >> 5) & 63) << 2), b = float((i & 31) << 3);
            float l = 0.3f * r + 0.59f * g + 0.11f * b;
            tint[i] = rgbf(std::min(255.f, (l + (r - l) * sat) * tn.x), std::min(255.f, (l + (g - l) * sat) * tn.y),
                           std::min(255.f, (l + (b - l) * sat) * tn.z));
        }

        // overcast: a textured cloud deck whose brightness follows the time of day
        float dayLevel = night ? 0.13f : tod == TimeOfDay::Day ? 1.f : tod == TimeOfDay::Morning ? 0.82f : 0.72f;
        Vec3 deckLit = weather == Weather::Rain ? Vec3{112, 120, 132} : Vec3{214, 218, 226};
        Vec3 deckShade = weather == Weather::Rain ? Vec3{40, 46, 56} : Vec3{134, 142, 156};
        Vec3 deckLow = weather == Weather::Rain ? Vec3{86, 94, 104} : Vec3{176, 184, 196};   // brighter band at the horizon
        if (!night && tod != TimeOfDay::Day) {   // a hint of warm light through the cloud
            deckLit = lerp(deckLit, light.glow, 0.18f);
            deckLow = lerp(deckLow, light.glow, 0.25f);
        }
        deckLit = deckLit * dayLevel; deckShade = deckShade * dayLevel; deckLow = deckLow * dayLevel;

        float sunDx[kW];
        float fromRight[kW];
        for (int x = 0; x < kW; ++x) {
            float delta = float(x) / kW - sunU;
            if (delta > 0.5f) delta -= 1.f;
            if (delta < -0.5f) delta += 1.f;
            sunDx[x] = delta * 2 * kPi * cosE;
            fromRight[x] = 0.5f - 0.5f * std::max(-1.f, std::min(1.f, std::sin(delta * 2 * kPi) * 4));   // blend sides, no seam
        }
        auto density = [&](int x, int y) {
            y = std::max(0, std::min(kH - 1, y));
            return samples[size_t(y) * kW + ((x % kW) + kW) % kW].cloud / 255.f;
        };
        for (int y = 0; y < kH; ++y) {
            float e = kTop + (kBottom - kTop) * y / (kH - 1);
            float t = std::pow(std::max(0.f, e / kTop), 0.55f);
            Vec3 skyColor = lerp(light.horizon, light.zenith, t);
            float low = std::pow(1 - t, 1.6f);
            float cloudFade = clamp01((e - 0.04f) / 0.10f);
            for (int x = 0; x < kW; ++x) {
                size_t i = size_t(y) * kW + x;
                const Sample& sample = samples[i];
                float dx = sunDx[x], dy = e - sunE;
                if (sample.haze) {   // mountains: lit like the track, then lost in air the colour of the horizon
                    uint16_t col = Renderer::mix(sample.slopeColor[0], sample.slopeColor[1], fromRight[x]);
                    if (weather == Weather::Snow && sample.forest) col = Renderer::mix(col, rgb565(150, 162, 170), 0.6f);
                    Vec3 m{float((col >> 11) << 3), float(((col >> 5) & 63) << 2), float((col & 31) << 3)};
                    m = Vec3{m.x * tn.x, m.y * tn.y, m.z * tn.z} * rangeShade;
                    float snowy = clamp01((m.x + m.y + m.z) / (3 * 255.f) * 2.2f - 1.0f);    // bright snow catches the low sun
                    if (!night && !overcast && tod != TimeOfDay::Day) m = lerp(m, light.glow * 1.0f, snowy * 0.35f);
                    Vec3 air = overcast ? deckLow : lerp(light.horizon, light.glow, clamp01(light.glowStrength * std::exp(-std::fabs(dx) * 1.4f)));
                    float aerial = (sample.haze - sample.foot) / 255.f * (overcast ? 1.f : 0.7f) + (overcast ? 0.35f : 0.f);
                    m = lerp(m, air, clamp01(aerial));
                    Vec3 fog{float((haze >> 11) << 3) * tn.x, float(((haze >> 5) & 63) << 2) * tn.y, float((haze & 31) << 3) * tn.z};
                    m = lerp(m, fog, clamp01(sample.foot / 255.f * 1.6f));
                    px[i] = rgbf(std::min(255.f, m.x / tn.x), std::min(255.f, m.y / tn.y), std::min(255.f, m.z / tn.z));
                    continue;
                }
                float ang = std::sqrt(dx * dx + dy * dy);
                float d = sample.cloud / 255.f;
                Vec3 c;
                if (overcast) {
                    float v = clamp01((d - 0.30f) * 1.8f);
                    float lit = clamp01(0.5f + (d - density(x + 5, y - 3)) * 4);   // soft relief in the deck
                    c = lerp(deckShade, deckLit, 0.55f * v + 0.45f * lit);
                    c = lerp(c, deckLow, low * 0.85f);
                } else {
                    c = skyColor;
                    float side = std::exp(-std::fabs(dx) * 1.4f);                // warm horizon on the sun's side
                    c = lerp(c, light.glow, clamp01(light.glowStrength * side * low));
                    if (std::fabs(dx) < 1.2f) {
                        float halo = (night ? 0.22f : 0.55f) * std::exp(-ang * (night ? 9.f : 5.f));
                        float disc = clamp01(((night ? 0.040f : 0.032f) - ang) * 250);
                        c = lerp(c, light.disc, clamp01(halo + disc));
                    }
                    if (night && e > 0.03f) {
                        float u = float(x) / kW;
                        float band = std::exp(-std::pow((e - 0.30f - 0.10f * std::sin(u * 2 * kPi)) / 0.07f, 2.f));
                        float milky = band * fbm(u, e * 22, 40, 55);
                        c = c + Vec3{20, 22, 34} * milky;
                        float n = noise(x, y, 901);
                        float edge = 0.990f - 0.008f * milky;
                        if (n > edge) {
                            float b = (n - edge) / (1 - edge);
                            c = lerp(c, noise(x, y, 903) > 0.8f ? Vec3{255, 226, 196} : Vec3{214, 228, 255}, 0.35f + 0.65f * b * b);
                        }
                    }
                    float cover = clamp01((d - (1 - light.cover)) * 3.4f) * cloudFade;
                    if (cover > 0) {
                        float len = std::max(ang, 1e-4f);
                        int sx = x - int(std::lround(dx / len * 4)), sy = y + int(std::lround(dy / len * 4));
                        float lit = clamp01(0.55f + (d - density(sx, sy)) * 5);       // edges facing the sun catch light
                        Vec3 cc = lerp(light.cloudShade, light.cloudLit, lit);
                        cc = lerp(cc, light.disc, clamp01(std::exp(-ang * 3.5f) * (1.2f - d)) * 0.7f);   // silver lining
                        c = lerp(c, cc, std::min(1.f, cover));
                    }
                }
                // pre-divide by the frame tint so the sky keeps these colours on screen
                px[i] = rgbf(std::min(255.f, c.x / tn.x), std::min(255.f, c.y / tn.y), std::min(255.f, c.z / tn.z));
            }
        }
    }

    void draw(Renderer& r) const {
        float cp = std::cos(r.camPitch), sp = std::sin(r.camPitch);
        float bottomSlope = std::tan(kBottom), edge = (r.w - 1) * 0.5f;
        const int span = 8;
        for (int y = 0; y < r.h; ++y) {
            float viewY = r.h / 2 - (y + 0.5f);
            float up = viewY * cp - r.focal * sp, forward = viewY * sp + r.focal * cp;
            auto line = r.color.begin() + size_t(y) * r.w;
            if (up <= bottomSlope * std::sqrt(edge * edge + forward * forward)) {
                std::fill_n(line, r.w, r.fogColor);
                continue;
            }
            auto panorama = [&](int x) {
                float right = x + 0.5f - r.w / 2;
                float heading = r.camYaw + std::atan2(right, forward);
                float elevation = std::atan2(up, std::sqrt(right * right + forward * forward));
                return Vec2{(heading / (2 * kPi) + 1) * kW,
                            (kTop - elevation) * ((kH - 1) / (kTop - kBottom))};
            };
            // shortcut: interpolate angles across eight pixels, reduce the span if the camera FOV grows.
            Vec2 from = panorama(0);
            for (int x = 0; x < r.w; x += span) {
                Vec2 to = panorama(x + span);
                float du = (to.u - from.u) / span, dv = (to.v - from.v) / span;
                for (int i = 0; i < span && x + i < r.w; ++i) {
                    int col = int(from.u + du * i) % kW;
                    int row = std::max(0, std::min(kH - 1, int(from.v + dv * i)));
                    line[x + i] = px[size_t(row) * kW + col];
                }
                from = to;
            }
        }
        std::fill(r.depth.begin(), r.depth.end(), 0.f);
    }
};

struct Chase { float yaw = 0, pitch = 0, boost = 0; bool init = false; };

static void render(Renderer& r, const Sky& sky, const World& w, const Race& race, TimeOfDay tod, Chase& cam, float dt) {
    const Car& me = race.cars[0];
    if (!cam.init) { cam.yaw = me.yaw; cam.pitch = me.pitch; cam.init = true; }
    float k = std::min(1.f, 5 * dt);
    float travel = me.yaw + 0.6f * std::atan2(me.vy, std::max(std::fabs(me.speed), 5.f));   // in a drift, look where it goes
    cam.yaw = wrapAngle(cam.yaw + wrapAngle(travel - cam.yaw) * k);              // camera swings after the car
    cam.pitch += (me.pitch - cam.pitch) * k;
    Vec3 fwd{std::sin(cam.yaw), 0, std::cos(cam.yaw)};
    r.cam = me.pos - fwd * 6.8f + Vec3{0, 2.5f, 0};
    float ground = race.terrain.height(r.cam.x, r.cam.z, me.idx);
    r.cam.y = std::max(r.cam.y, ground + 1.2f);                                    // never inside a hill
    r.camYaw = cam.yaw;
    const bool inTunnel = me.idx > race.tunnelLo && me.idx < race.tunnelHi;
    r.camPitch = 0.15f + cam.pitch * 0.7f;                                         // look down the slope with the car
    cam.boost += (float(me.boosting) - cam.boost) * std::min(1.f, 3 * dt);
    r.focal = (H / 2) / std::tan((55 + 12 * cam.boost) * kPi / 360);               // nitrous widens the view: a rush of speed
    sky.draw(r);
    auto visible = [&](Vec3 p, float radius) {
        Vec3 d = p - r.cam;
        d.y = 0;
        return dot(d, d) < (r.fogEnd + radius) * (r.fogEnd + radius) && dot(d, fwd) > -radius;
    };
    auto nearbyLamp = [&](Vec3 p) { Vec3 d = p - r.cam; return dot(d, d) < 55 * 55 && dot(d, fwd) > 0; };
    for (size_t i = 0; i < race.cars.size(); ++i) {                                 // nearest first: less overdraw
        const Car& c = race.cars[i];
        if (!visible(c.pos, 3)) continue;
        Transform t;
        t.pos = c.pos;
        t.yaw = c.yaw;
        t.pitch = c.pitch;
        r.draw(w.cars[i], t);
    }
    struct Lamp { Vec3 pos; Vec3 color; float size; float halo; };   // halo > 0: a soft glow that wide (world units), no core
    std::vector<Lamp> lamps;
    if (tod == TimeOfDay::Night) {
        const Vec3 tn = kDaylight[int(tod)].tint, wt = weatherTint(race.weather);
        const Vec3 white{1.f / (tn.x * wt.x), 0.95f / (tn.y * wt.y), 0.8f / (tn.z * wt.z)};   // warm white after the blue night tint
        std::vector<std::pair<float, int>> near;
        for (size_t i = 0; i < race.cars.size(); ++i) {
            const Car& c = race.cars[i];
            Vec3 d = c.pos - r.cam;
            float dist = std::sqrt(dot(d, d));
            if (dist > r.fogEnd || !visible(c.pos, 3)) continue;
            near.push_back({i == 0 ? -1.f : dist, int(i)});
            Vec3 f{std::sin(c.yaw), -std::sin(c.pitch), std::cos(c.yaw)}, rt{f.z, 0, -f.x};
            Vec3 view = normalize(c.pos - r.cam);
            float front = -dot(f, view), back = dot(f, view);
            const Bounds& bx = w.carBounds[i];
            for (float side : {-1.f, 1.f}) {
                Vec3 across = rt * (side * bx.halfWidth * 0.72f);
                if (front > 0) lamps.push_back({c.pos + f * (bx.front + 0.08f) + across + Vec3{0, bx.lampY, 0}, Vec3{255, 245, 220} * front, 1.f, 0});
                if (back > 0) lamps.push_back({c.pos + f * (bx.rear - 0.08f) + across + Vec3{0, bx.lampY + 0.05f, 0}, Vec3{255, 30, 20} * back, 0.8f, 0});
            }
        }
        std::sort(near.begin(), near.end());
        for (size_t k = 0; k < near.size() && k < 2; ++k) {          // the player and the nearest opponent light the road
            const Car& c = race.cars[near[k].second];
            bool mine = near[k].second == 0;
            Vec3 carFwd{std::sin(c.yaw), 0, std::cos(c.yaw)}, carRight{carFwd.z, 0, -carFwd.x};
            {   // one wide beam per car: each beam costs a light test on every road pixel
                float side = 0;
                SpotLight beam;
                beam.pos = c.pos + carFwd * 1.9f + carRight * side + Vec3{0, 0.67f, 0};
                beam.dir = normalize(carFwd + Vec3{0, -0.2f - c.pitch, 0});
                beam.cosInner = mine ? 0.95f : 0.95f;
                beam.cosOuter = 0.78f;
                beam.intensity = mine ? 12.f : 7.f;
                beam.range = mine ? 36 : 26;
                beam.color = white;
                r.spots.push_back(beam);
            }
        }
    }
    if (me.boosting) {                                                // the plasma lights the road behind the car
        Vec3 f{std::sin(me.yaw), -std::sin(me.pitch), std::cos(me.yaw)};
        SpotLight glow;
        glow.pos = me.pos + f * (w.carBounds[0].rear - 0.8f) + Vec3{0, 0.6f, 0};
        glow.dir = normalize(f * -0.5f + Vec3{0, -1, 0});
        glow.cosInner = 0.5f;
        glow.cosOuter = 0.2f;
        glow.intensity = 2.2f * (0.7f + 0.6f * noise(int(race.time * 40), 0, 77));
        glow.range = 11;
        glow.color = {0.45f, 0.5f, 1.f};
        r.spots.push_back(glow);
    }
    r.draw(w.road, Transform());
    r.draw(w.water, Transform());                                     // before the ground: the beds behind it fail the depth test
    for (size_t i = 0; i < w.ground.size(); ++i)
        if (visible(w.groundAt[i], 29)) r.draw(w.ground[i], Transform());
    r.draw(w.gantry, w.gantryAt);
    for (const Transform& t : w.markersAt)
        if (visible(t.pos, 1)) r.draw(w.marker, t);
    if (visible(w.tunnelMid, 30)) {                                   // the hill, then the liner, dim inside; beams light its walls too
        r.draw(w.tunnelOut, Transform());
        float a = r.ambient, s = r.sun;
        r.ambient = a * 0.55f;
        r.sun = s * 0.12f;
        r.draw(w.tunnelIn, Transform());
        r.ambient = a;
        r.sun = s;
    }
    r.spots.clear();                                    // beams light the track, terrain, gantry and posts only
    for (size_t i = 0; i < race.trees.size(); ++i) {
        if (!visible(race.trees[i], 3)) continue;
        Transform t;
        t.pos = race.trees[i];
        r.draw(w.pine[i % 4], t);
    }
    for (const Block& b : race.blocks) {
        if (b.kind == Block::Railing || !visible(b.pos, 8)) continue;
        Transform t;
        t.pos = b.pos;
        t.yaw = b.yaw;
        size_t i = &b - &race.blocks[0];
        r.draw(w.buildings[i], t);
        if (tod == TimeOfDay::Night && w.porch[i].y > 0) {          // porch lamp beside the door
            Vec3 p = w.porch[i], f{std::sin(b.yaw), 0, std::cos(b.yaw)}, rt{f.z, 0, -f.x};
            lamps.push_back({b.pos + rt * p.x + Vec3{0, p.y, 0} + f * (p.z + 0.4f), {255, 170, 70}, 0.7f, 0});
            lamps.push_back({b.pos + rt * p.x + Vec3{0, p.y, 0} + f * (p.z + 0.6f), {255, 150, 60}, 0, 1.6f});
            if (dot(b.pos - r.cam, b.pos - r.cam) < 70 * 70)         // warm glow out of each window (hidden when the wall faces away)
                for (const Vec3& wv : w.windows[i]) lamps.push_back({b.pos + rt * wv.x + Vec3{0, wv.y, 0} + f * wv.z, {150, 95, 35}, 0, 0.9f});
        }
    }
    if (visible(w.bridgeAt.pos, race.bridgeLen / 2 + 2)) r.draw(w.bridge, w.bridgeAt);
    for (const Vec3& p : w.tunnelLamps)                               // warm ceiling lamps, day and night
        if (nearbyLamp(p)) lamps.push_back({p, {255, 214, 150}, 1.6f, 0});
    auto nearby = [&](Vec3 p, float range) { Vec3 d = p - r.cam; d.y = 0; return dot(d, d) < range * range && dot(d, fwd) > -2; };
    for (size_t i = 0; i < race.pickups.size(); ++i) {                // coins spin on edge, each a step behind the last; tanks turn slowly
        const Pickup& p = race.pickups[i];
        if (p.taken || !nearby(p.pos, 70)) continue;
        Transform t;
        t.pos = p.pos;
        t.yaw = race.time * (p.nitro ? 1.5f : 3.f) + i * 0.7f;
        t.pitch = p.nitro ? 0 : kPi / 2;
        r.draw(p.nitro ? w.tank : w.coin, t);
        float pulse = 1 + 0.18f * std::sin(race.time * 5 + i * 0.9f);
        Vec3 toCam = normalize(r.cam - p.pos);                        // in front of the pickup, so its own body never hides the glow
        lamps.push_back({p.pos + toCam * 0.7f, p.nitro ? Vec3{70, 190, 255} : Vec3{255, 196, 50}, 0, (p.nitro ? 1.5f : 1.3f) * pulse});
    }
    for (size_t i = 0; i < w.flagsAt.size(); ++i)
        if (nearby(w.flagsAt[i].pos, 80)) r.draw(w.flag[i % 3], w.flagsAt[i]);
    if (me.boosting) {                                                // nitrous plasma from both exhausts
        Vec3 f{std::sin(me.yaw), -std::sin(me.pitch), std::cos(me.yaw)}, rt{f.z, 0, -f.x};
        int tick = int(race.time * 40);
        float flick = 0.7f + 0.6f * noise(tick, 0, 77);
        float a = r.ambient, s = r.sun;
        r.ambient = 1.5f;                                              // glows: bright whatever the light
        r.sun = 0;
        for (float side : {-0.4f, 0.4f}) {
            Vec3 nozzle = me.pos + f * w.carBounds[0].rear + rt * side + Vec3{0, 0.4f, 0};
            Transform t;
            t.pos = nozzle;
            t.yaw = me.yaw;
            t.pitch = -kPi / 2;                                       // cone tip points out behind the car
            r.draw(w.flame[flick > 1 ? 0 : 1], t);
            lamps.push_back({nozzle - f * 0.15f, {150, 190, 255}, 1.2f * flick, 0});
            lamps.push_back({nozzle - f * 0.9f, {110, 80, 255}, 0, 2.6f * flick});        // violet bloom round the plume
            for (int k = 1; k <= 4; ++k) {                            // shock diamonds: bright beads along the jet, flickering apart
                float wob = 0.85f + 0.3f * noise(tick + k, int(side * 5), 78);
                lamps.push_back({nozzle - f * (0.42f * k * flick * wob), {170, 210, 255}, 0.75f - 0.1f * k, 0});
            }
        }
        r.ambient = a;
        r.sun = s;
    }
    if (!sky.tint.empty())
        for (uint16_t& pixel : r.color) pixel = sky.tint[pixel];
    for (const Lamp& l : lamps) {                                     // after the tint, so lamps keep their colour
        Vec3 v = r.toView(l.pos);
        if (v.z < 0.8f) continue;
        float sx = W / 2 + v.x * r.focal / v.z, sy = H / 2 - v.y * r.focal / v.z;   // (lamps and nitrous flames)
        int cx = int(sx), cy = int(sy);
        if (cx < 0 || cx >= W || cy < 0 || cy >= H || 1 / v.z < r.depth[size_t(cy) * W + cx] * 0.97f) continue;   // hidden
        if (l.halo > 0) {                                             // soft additive glow, brightest in the middle
            float rad = std::min(40.f, l.halo * r.focal / v.z), r2 = rad * rad;
            for (int y = std::max(0, int(sy - rad)); y <= std::min(H - 1, int(sy + rad)); ++y)
                for (int x = std::max(0, int(sx - rad)); x <= std::min(W - 1, int(sx + rad)); ++x) {
                    float d2 = (x + 0.5f - sx) * (x + 0.5f - sx) + (y + 0.5f - sy) * (y + 0.5f - sy);
                    if (d2 >= r2) continue;
                    float a = 1 - d2 / r2;
                    a = 0.55f * a * a;
                    uint16_t& px = r.color[size_t(y) * W + x];
                    px = rgbf(std::min(255.f, ((px >> 11) << 3) + l.color.x * a), std::min(255.f, (((px >> 5) & 63) << 2) + l.color.y * a),
                              std::min(255.f, ((px & 31) << 3) + l.color.z * a));
                }
            continue;
        }
        float core = std::max(0.8f, std::min(3.2f, 0.14f * l.size * r.focal / v.z)), radius = core * 4.f;
        for (int y = int(sy - radius); y <= int(sy + radius); ++y) {
            if (y < 0 || y >= H) continue;
            for (int x = int(sx - radius); x <= int(sx + radius); ++x) {
                if (x < 0 || x >= W) continue;
                float d = std::sqrt((x + 0.5f - sx) * (x + 0.5f - sx) + (y + 0.5f - sy) * (y + 0.5f - sy));
                float a = d < core ? 1.f : std::exp(-(d - core) / core * 1.6f) * 0.7f;
                if (a < 0.03f) continue;
                uint16_t& px = r.color[size_t(y) * W + x];
                float pr = float((px >> 11) << 3), pg = float(((px >> 5) & 63) << 2), pb = float((px & 31) << 3);
                Vec3 c = d < core ? Vec3{255, 255, 255} * 0.6f + l.color * 0.4f : l.color;   // white-hot centre
                px = rgbf(std::min(255.f, pr + c.x * a), std::min(255.f, pg + c.y * a), std::min(255.f, pb + c.z * a));
            }
        }
    }
    if (cam.boost > 0.2f) {                                           // speed streaks round the edge of the screen
        int tick = int(race.time * 30);
        for (int i = 0; i < 16; ++i) {
            float ang = 2 * kPi * noise(i, tick, 91), dist = 0.62f + 0.3f * noise(i, tick, 92), len = 0.10f + 0.12f * noise(i, tick, 93);
            for (int s = 0; s < 14; ++s) {
                float d = dist + len * s / 14.f;
                int x = int(W / 2 + std::cos(ang) * d * W * 0.62f), y = int(H / 2 + std::sin(ang) * d * H * 0.62f);
                if (x < 0 || x >= W || y < 0 || y >= H) break;
                uint16_t& px = r.color[size_t(y) * W + x];
                px = Renderer::mix(px, rgb565(190, 215, 255), cam.boost * (0.25f + 0.45f * s / 14.f));
            }
        }
    }
    if (race.weather != Weather::Sunny && !inTunnel) {
        bool rain = race.weather == Weather::Rain;
        uint16_t col = rain ? rgb565(143, 162, 181) : rgb565(223, 230, 239);
        int count = rain ? 96 : 70;
        float seconds = race.time + 3.f;
        for (int i = 0; i < count; ++i) {
            float size = noise(i, 0, 712);
            int fall = int(seconds * (rain ? 150 + 140 * size : 21 + 37 * size));
            int y = (int(noise(i, 0, 711) * H) + fall) % H;
            int drift = rain ? fall / 4 : int(9 * std::sin(seconds * 1.9f + i));
            int x = (int(noise(i, 0, 710) * W) + drift + W) % W;
            int len = rain ? 5 + int(7 * size) : 1 + int(1.7f * size);
            for (int j = 0; j < len && x + j / 4 < W && y + j < H; ++j) {
                uint16_t& pixel = r.color[size_t(y + j) * W + x + j / 4];
                pixel = Renderer::mix(pixel, col, rain ? 0.30f + 0.34f * size : 0.54f + 0.43f * size);
            }
        }
    }
}

static void writePpm(const std::vector<uint16_t>& fb) {
    std::printf("P6\n%d %d\n255\n", W, H);
    for (uint16_t c : fb) {
        unsigned char px[3] = {uint8_t((c >> 11) * 255 / 31), uint8_t(((c >> 5) & 63) * 255 / 63), uint8_t((c & 31) * 255 / 31)};
        std::fwrite(px, 1, 3, stdout);
    }
}

// ---------------------------------------------------------------- display thread
// Sends finished frames to the panel on its own thread, so the next frame renders while SPI is busy.
class Presenter {
public:
    explicit Presenter(ILI9341& t) : tft_(t), th_([this] { run(); }) {}
    ~Presenter() {
        { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
        cv_.notify_all();
        th_.join();
    }
    void submit(const std::vector<uint16_t>& frame) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !pending_ || !err_.empty(); });
        if (!err_.empty()) throw std::runtime_error(err_);
        buf_ = frame;
        pending_ = true;
        cv_.notify_all();
    }
private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return pending_ || quit_; });
            if (!pending_) return;
            lk.unlock();
            std::string e;
            try { tft_.pushImage(0, 0, W, H, buf_.data()); } catch (const std::exception& ex) { e = ex.what(); }
            lk.lock();
            pending_ = false;
            if (!e.empty()) { err_ = e; cv_.notify_all(); return; }
            cv_.notify_all();
        }
    }
    ILI9341& tft_;
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<uint16_t> buf_;
    bool pending_ = false, quit_ = false;
    std::string err_;
    std::thread th_;                                                                // last: starts after the members above
};

// Raw stdin for the controller's bytes; restores the terminal on exit.
struct RawStdin {
    termios saved{};
    bool tty = false;
    RawStdin() {
        if (isatty(0) && tcgetattr(0, &saved) == 0) {
            termios raw = saved;
            cfmakeraw(&raw);
            tty = tcsetattr(0, TCSANOW, &raw) == 0;
        }
    }
    ~RawStdin() { if (tty) tcsetattr(0, TCSANOW, &saved); }
};

static Renderer makeRenderer() {
    Renderer r(W, H, 55);
    r.fogStart = 45;
    r.fogEnd = 100;
    return r;
}

static void setSky(Renderer& r, Sky& sky, TimeOfDay tod, Weather weather) {
    const Daylight& light = kDaylight[int(tod)];
    float az = light.heading * 2 * kPi, h = std::cos(light.elevation);
    r.light = {std::sin(az) * h, std::sin(light.elevation), std::cos(az) * h};
    r.ambient = light.ambient;
    r.sun = light.sun;
    r.fogColor = light.fog;
    if (weather != Weather::Sunny) {
        bool rain = weather == Weather::Rain;
        r.ambient *= rain ? 0.85f : 1.12f;
        r.sun *= rain ? 0.25f : 0.35f;
        uint16_t fog = rain ? rgb565(60, 68, 76) : rgb565(140, 150, 162);
        if (tod == TimeOfDay::Night) fog = Renderer::scale(fog, rain ? 0.30f : 0.22f);
        else if (tod != TimeOfDay::Day) fog = Renderer::scale(fog, 0.82f);
        r.fogColor = Renderer::mix(r.fogColor, fog, 0.75f);
    }
    sky.paint(r.fogColor, tod, weather);
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    bool flip = false;
    for (int i = 1; i < argc; ++i) flip = flip || !std::strcmp(argv[i], "flip");
    try {
        Race race;
        TimeOfDay tod = TimeOfDay::Day;
        std::string keys;
        for (int i = (mode == "ppm" || mode == "bench" ? 3 : 1); i < argc; ++i) {
            std::string a = argv[i];
            if (a == "night") tod = TimeOfDay::Night;
            else if (a == "morning") tod = TimeOfDay::Morning;
            else if (a == "day") tod = TimeOfDay::Day;
            else if (a == "evening" || a == "golden") tod = TimeOfDay::Evening;
            else if (a == "sunny") race.weather = Weather::Sunny;
            else if (a == "rain") race.weather = Weather::Rain;
            else if (a == "snow") race.weather = Weather::Snow;
            else if (a == "asphalt") race.surface = RoadSurface::Asphalt;
            else if (a == "gravel") race.surface = RoadSurface::Gravel;
            else if (mode == "ppm" && i == 3) keys = a;
        }
        std::string exe = argv[0];                                              // assets live next to the binary
        std::string assets = exe.substr(0, exe.rfind('/') + 1) + "assets/cars/";
        World world(race, assets);
        Renderer r = makeRenderer();
        Sky sky;
        setSky(r, sky, tod, race.weather);
        world.tx.paintBuildings(tod == TimeOfDay::Night);
        Chase cam;

        if (mode == "ppm") {
            float seconds = argc > 2 ? std::atof(argv[2]) : 0;
            Input in;
            in.up = keys.find('u') != std::string::npos; in.down = keys.find('d') != std::string::npos;
            in.left = keys.find('l') != std::string::npos; in.right = keys.find('r') != std::string::npos;
            in.nitro = keys.find('n') != std::string::npos;
            race.cars[0].ai = keys.find('a') != std::string::npos;                          // a = autopilot
            const float dt = 1 / 15.f;
            for (float t = 0; t < seconds; t += dt) { race.step(dt, in); render(r, sky, world, race, tod, cam, dt); }
            render(r, sky, world, race, tod, cam, dt);
            hud(r.color, race);
            writePpm(r.color);
            return 0;
        }
        if (mode == "menu") {                                                   // ./cargame menu [sel] [conditions] > menu.ppm
            render(r, sky, world, race, tod, cam, 0);
            std::vector<uint16_t> scene = r.color;
            menu(r.color, scene, argc > 2 ? std::atoi(argv[2]) % 4 : 3, tod, race.weather, race.surface, *world.tx.road, *world.tx.checker);
            writePpm(r.color);
            return 0;
        }
        if (mode == "bench") {
            int frames = argc > 2 ? std::atoi(argv[2]) : 200;
            race.cars[0].ai = true;
            race.time = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < frames; ++f) { race.step(1 / 15.f, Input()); render(r, sky, world, race, tod, cam, 1 / 15.f); hud(r.color, race); }
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("render %.1f ms/frame (%.0f fps without display)\n", 1000 * s / frames, frames / s);
            return 0;
        }

        bool demo = mode == "demo", sim = mode == "sim";
        FILE* status = sim ? stderr : stdout;                                     // sim: stdout carries frames
        std::unique_ptr<ILI9341> tft;
        std::unique_ptr<Presenter> show;
        std::vector<uint8_t> spiBytes;
        auto nextFrame = std::chrono::steady_clock::now();
        if (!sim) {
            tft.reset(new ILI9341(board::spi, board::gpiochip, board::dc, board::reset, board::hz, board::cs));
            tft->init();
            tft->setRotation((board::rotation + (flip ? 1 : 3)) % 4);             // landscape, turned 180° for this mounting
            show.reset(new Presenter(*tft));
        }
        auto present = [&](const std::vector<uint16_t>& fb) {
            if (show) return show->submit(fb);
            std::this_thread::sleep_until(nextFrame);                               // pace like the board's SPI
            nextFrame = std::max(nextFrame + std::chrono::microseconds(41667), std::chrono::steady_clock::now());
            spiBytes.resize(fb.size() * 2);
            for (size_t i = 0; i < fb.size(); ++i) { spiBytes[2 * i] = fb[i] >> 8; spiBytes[2 * i + 1] = uint8_t(fb[i]); }  // as pushImage sends them
            if (std::fwrite(spiBytes.data(), 1, spiBytes.size(), stdout) != spiBytes.size() || std::fflush(stdout)) throw std::runtime_error("viewer closed");
        };
        RawStdin raw;
        // Three push buttons to GND (Pico Mini B pads): left GPIO1_C7, nitrous GPIO1_D2, right GPIO1_D3.
        int buttons = -1;
        if (!sim) {
            int chip = open(board::gpiochip, O_RDONLY);
            gpiohandle_request req{};
            req.lineoffsets[0] = 23; req.lineoffsets[1] = 26; req.lineoffsets[2] = 27;
            req.lines = 3;
            req.flags = GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_ACTIVE_LOW | GPIOHANDLE_REQUEST_BIAS_PULL_UP;
            std::strcpy(req.consumer_label, "cargame-buttons");
            if (chip >= 0 && ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &req) == 0) buttons = req.fd;
            else std::fprintf(status, "buttons: GPIO lines 23/26/27 unavailable (%s), keyboard only\n", std::strerror(errno));
            if (chip >= 0) close(chip);
        }
        if (demo) race.cars[0].ai = true;
        std::fprintf(status, "READY\n");
        std::fflush(status);

        Input in, kb;                                                           // kb: keyboard state from stdin
        bool stdinOpen = true, quit = false, cruise = false;
        bool inMenu = !demo, sceneStale = true;
        int sel = 3, prevBtn = 0;                                                  // menu card; GO is picked first
        std::vector<uint16_t> scene;
        auto last = std::chrono::steady_clock::now(), report = last;
        int frames = 0;
        double renderSec = 0;
        while (!quit) {
            TimeOfDay oldTime = tod;
            Weather oldWeather = race.weather;
            RoadSurface oldSurface = race.surface;
            pollfd pf{0, POLLIN, 0};
            while (stdinOpen && poll(&pf, 1, 0) > 0) {
                unsigned char buf[64];
                ssize_t n = read(0, buf, sizeof buf);
                if (n <= 0) { stdinOpen = false; break; }                         // no controller: keep running
                for (ssize_t i = 0; i < n; ++i) {
                    unsigned char b = buf[i];
                    if (b >= 0x40 && b <= 0x5F) {
                        if (b & 1) cruise = true;                                     // tap up: throttle stays on
                        if (b & 2) cruise = false;                                    // down brakes and cancels it
                        kb.up = (b & 1) || cruise; kb.down = b & 2; kb.left = b & 4; kb.right = b & 8; kb.nitro = b & 16;
                    }
                    else if (b == 'r') { cruise = false; kb.up = false; race.reset(); cam.init = false; if (demo) race.cars[0].ai = true; }
                    else if (b == 't') tod = TimeOfDay((int(tod) + 1) % 4);
                    else if (b == 'c') race.weather = Weather((int(race.weather) + 1) % 3);
                    else if (b == 'g') race.surface = race.surface == RoadSurface::Gravel ? RoadSurface::Asphalt : RoadSurface::Gravel;
                    else if (b == 'q' || b == 3) quit = true;
                }
            }
            if (quit) break;
            in = kb;
            gpiohandle_data bv{};
            bool btnOk = buttons >= 0 && ioctl(buttons, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &bv) == 0;
            if (btnOk) {
                bool L = bv.values[0], M = bv.values[1], R = bv.values[2];
                in.down |= L && R;                                                // both: brake, then reverse
                in.up = !in.down;                                                 // otherwise the throttle is always on
                in.left |= L && !R; in.right |= R && !L; in.nitro |= M;
            }
            int btn = in.left | in.right << 1 | in.nitro << 2 | kb.up << 3, press = btn & ~prevBtn;
            prevBtn = btn;
            bool start = false;
            if (inMenu) {
                if (press & 1) sel = (sel + 3) % 4;
                if (press & 2) sel = (sel + 1) % 4;
                if (press & 4) {
                    if (sel == 0) tod = TimeOfDay((int(tod) + 1) % 4);
                    else if (sel == 1) race.weather = Weather((int(race.weather) + 1) % 3);
                    else if (sel == 2) race.surface = race.surface == RoadSurface::Gravel ? RoadSurface::Asphalt : RoadSurface::Gravel;
                    else start = true;
                }
                start |= (press & 8) != 0;                                           // keyboard Up starts too
                if (press) {
                    std::fprintf(status, "menu: press %s card %d %s/%s/%s%s\n", press & 1 ? "L" : press & 2 ? "R" : press & 4 ? "M" : "up", sel,
                                 timeName(tod), weatherName(race.weather), roadName(race.surface), start ? " -> race" : "");
                    std::fflush(status);
                }
            }
            bool skyChanged = oldTime != tod || oldWeather != race.weather;
            if (skyChanged) setSky(r, sky, tod, race.weather);
            if (oldTime != tod) world.tx.paintBuildings(tod == TimeOfDay::Night);
            if (oldWeather != race.weather) { world.tx.setGround(race.weather, race.surface); world.colourGround(race, race.weather); }
            else if (oldSurface != race.surface) world.tx.setRoad(race.weather, race.surface);
            auto now = std::chrono::steady_clock::now();
            float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
            last = now;
            if (start) { inMenu = false; cruise = false; kb.up = false; race.reset(); cam.init = false; }
            if (inMenu) {
                if (sceneStale || skyChanged || oldSurface != race.surface) {          // still of the grid in the chosen conditions
                    render(r, sky, world, race, tod, cam, 0);
                    scene = r.color;
                    sceneStale = false;
                }
                menu(r.color, scene, sel, tod, race.weather, race.surface, *world.tx.road, *world.tx.checker);
                present(r.color);
                continue;
            }
            const Car& me0 = race.cars[0];
            if (me0.finishTime >= 0 && race.time - me0.finishTime > kNewRaceDelay) {   // finish screen over: back to the menu
                race.reset(); cam.init = false;
                if (demo) race.cars[0].ai = true;
                else inMenu = sceneStale = true;
                std::fprintf(status, "race over: back to the menu\n");
                std::fflush(status);
            }
            for (float left = dt; left > 1e-6f; left -= 1 / 30.f) race.step(std::min(left, 1 / 30.f), in);
            render(r, sky, world, race, tod, cam, dt);
            hud(r.color, race);
            renderSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
            present(r.color);
            ++frames;
            auto shown = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(shown - report).count();
            if (s >= 2 || skyChanged || oldSurface != race.surface) {
                const Car& me = race.cars[0];
                std::fprintf(status, "%.1f fps render %.0f ms %s/%s/%s lap %d/%d P%d %d km/h btn %s\n", frames / s, 1000 * renderSec / frames,
                            timeName(tod), weatherName(race.weather), roadName(race.surface),
                            std::max(1, std::min(race.laps, me.lapsDone + 1)), race.laps, race.position(0), int(std::fabs(me.speed) * 4),
                            !btnOk ? "none" : bv.values[0] ? (bv.values[2] ? "L+R" : "L") : bv.values[2] ? "R" : bv.values[1] ? "M" : "-");
                std::fflush(status);
                frames = 0; renderSec = 0; report = shown;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
