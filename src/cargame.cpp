// Textured 3D car racing game for the Luckfox Pico + ILI9341 (320x240 landscape).
//
// usage: cargame [flip]                play; controls arrive on stdin (tools/drive.py sends them)
//        cargame demo [flip]           play with the player's car on autopilot (no controller needed)
//        cargame ppm SECONDS [KEYS]    headless: simulate SECONDS at 15 fps holding KEYS (any of u d l r; a = autopilot),
//                                      then print that frame as a PPM image on stdout
//        cargame bench FRAMES          headless: render FRAMES on autopilot and report the render time
// stdin protocol: one byte per change, '@' + bits (1 up, 2 down, 4 left, 8 right); 'r' restarts; 'q' quits.
#include "board_config.h"
#include "ILI9341.h"
#include "font5x7.h"
#include "game.h"
#include "objload.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <poll.h>
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
static int clamp255(float v) { return std::max(0, std::min(255, int(v))); }
static uint16_t rgbf(float r, float g, float b) { return rgb565(clamp255(r), clamp255(g), clamp255(b)); }

static TexPtr makeTex(int w, int h, std::function<uint16_t(int, int)> f) {
    auto t = std::make_shared<Texture>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) t->at(x, y) = f(x, y);
    return t;
}

struct Textures {
    TexPtr grass = makeTex(64, 64, [](int x, int y) {
        float n = noise(x, y, 1) * 0.6f + noise(x / 4, y / 4, 2) * 0.4f;
        return rgbf(40 + 35 * n, 105 + 60 * n, 30 + 25 * n);
    });
    TexPtr road = makeTex(64, 64, [](int x, int y) {                              // gravel with two wheel ruts
        float n = noise(x, y, 3) * 0.7f + noise(x / 3, y / 5, 8) * 0.3f;
        bool rut = (x >= 13 && x <= 22) || (x >= 41 && x <= 50);
        float k = rut ? 0.82f : 1.f;
        if (!rut && noise(x, y, 9) > 0.93f) return rgbf(150, 140, 120);                // loose stones
        return rgbf((118 + 34 * n) * k, (98 + 28 * n) * k, (72 + 20 * n) * k);
    });
    TexPtr verge = makeTex(16, 16, [](int x, int y) {                              // dirt (u = 0, road side) into grass
        float n = noise(x, y, 10);
        if (n * 1.2f < x / 15.f) return rgbf(50 + 40 * n, 115 + 50 * n, 35 + 20 * n);
        return rgbf(110 + 30 * n, 92 + 24 * n, 68 + 18 * n);
    });
    TexPtr kerb = makeTex(16, 16, [](int, int y) { return y < 8 ? rgbf(205, 35, 35) : rgbf(235, 235, 235); });
    TexPtr checker = makeTex(16, 16, [](int x, int y) { return ((x / 4 + y / 4) & 1) ? rgbf(240, 240, 240) : rgbf(20, 20, 20); });
    TexPtr bark = makeTex(16, 16, [](int x, int y) {
        float n = noise(x, 0, 4) * 0.7f + noise(x, y, 5) * 0.3f;
        return rgbf(75 + 40 * n, 50 + 25 * n, 30 + 15 * n);
    });
    TexPtr leaves = makeTex(32, 32, [](int x, int y) {
        float n = noise(x / 2, y / 2, 6) * 0.6f + noise(x, y, 7) * 0.4f;
        return rgbf(25 + 40 * n, 85 + 70 * n, 25 + 30 * n);
    });
};

// ---------------------------------------------------------------- meshes
// Quad p0..p3 (in order around its edge) facing `out`.
static void addQuad(Mesh& m, const Vec3 (&p)[4], const Vec2 (&uv)[4], const Texture* tex, Vec3 out) {
    int b = int(m.v.size());
    for (auto& q : p) m.v.push_back(q);
    if (dot(cross(p[1] - p[0], p[2] - p[0]), out) >= 0) {
        m.t.push_back({b, b + 1, b + 2, 0xFFFF, tex, {uv[0], uv[1], uv[2]}});
        m.t.push_back({b, b + 2, b + 3, 0xFFFF, tex, {uv[0], uv[2], uv[3]}});
    } else {
        m.t.push_back({b, b + 2, b + 1, 0xFFFF, tex, {uv[0], uv[2], uv[1]}});
        m.t.push_back({b, b + 3, b + 2, 0xFFFF, tex, {uv[0], uv[3], uv[2]}});
    }
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
static Mesh loadCar(const std::string& path, bool blue) {
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
        if (blue && r * 2 > g + 16 && r > b + 8) c = uint16_t(b << 11 | g << 5 | r);   // red paint -> blue
        if (r < 12 && g < 24 && b < 12) c = Renderer::mix(c, rgb565(150, 155, 170), 0.35f); // dark trim reads as a black blob on the TFT
        t.color = c;
        t.tex = nullptr;
    }
    m.textures.clear();
    return m;
}

struct World {
    Textures tx;
    std::vector<Mesh> ground;                     // terrain in 40x40 chunks, so chunks out of view are skipped
    std::vector<Vec3> groundAt;                   // chunk centres
    Mesh road, gantry, pine[2];
    Mesh cars[3];
    Transform gantryAt;

    World(const Race& race, const std::string& assets) {
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
                        m.t.push_back({a, d, c, 0xFFFF, tx.grass.get(), {uv(a), uv(d), uv(c)}});   // facing up
                        m.t.push_back({a, c, b, 0xFFFF, tx.grass.get(), {uv(a), uv(c), uv(b)}});
                    }
                ground.push_back(std::move(m));
                groundAt.push_back({cx + chunk / 2.f, race.terrain.height(cx + chunk / 2.f, cz + chunk / 2.f), cz + chunk / 2.f});
            }
        float hw = tr.halfWidth, kw = tr.kerb;
        auto onGround = [&](Vec3 p, float lift) { p.y = Terrain::base(p.x, p.z) + lift; return p; };
        road.rim = false;
        for (int i = 0; i < tr.n(); ++i) {                                          // gravel road, dirt verges both sides
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
        try {                                                                       // player red, AI blue and green
            cars[0] = loadCar(assets + "sedan-sports.obj", false);
            cars[1] = loadCar(assets + "sedan-sports.obj", true);
            cars[2] = loadCar(assets + "hatchback-sports.obj", false);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string(e.what()) + " (car models missing? run `make deploy`)");
        }
    }
};

// ---------------------------------------------------------------- HUD
static void text(std::vector<uint16_t>& fb, int x, int y, const std::string& s, uint16_t col, int sc) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = s[i];
        if (ch < 32 || ch > 126) continue;
        const uint8_t* g = font5x7 + (ch - 32) * 5;
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 7; ++cy)
                if (g[cx] & (1u << cy))
                    for (int sy = 0; sy < sc; ++sy)
                        for (int sx = 0; sx < sc; ++sx) {
                            int px = x + int(i) * 6 * sc + cx * sc + sx, py = y + cy * sc + sy;
                            if (px >= 0 && px < W && py >= 0 && py < H) fb[size_t(py) * W + px] = col;
                        }
    }
}
static int textWidth(const std::string& s, int sc) { return int(s.size()) * 6 * sc - sc; }
static void label(std::vector<uint16_t>& fb, int x, int y, const std::string& s, uint16_t col, int sc) {
    text(fb, x + 1, y + 1, s, 0, sc);                                               // drop shadow
    text(fb, x, y, s, col, sc);
}
static void centred(std::vector<uint16_t>& fb, int y, const std::string& s, uint16_t col, int sc) { label(fb, (W - textWidth(s, sc)) / 2, y, s, col, sc); }

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
    const uint16_t dots[3] = {rgb565(255, 60, 60), rgb565(80, 140, 255), rgb565(80, 220, 90)};
    for (int i = 2; i >= 0; --i) plot(race.cars[i].pos, dots[i], 1);
}

static void hud(std::vector<uint16_t>& fb, const Race& race) {
    const Car& me = race.cars[0];
    const uint16_t white = 0xFFFF, yellow = rgb565(255, 220, 60);
    int lap = std::max(1, std::min(race.laps, me.lapsDone + 1));
    label(fb, 6, 6, "LAP " + std::to_string(lap) + "/" + std::to_string(race.laps), white, 2);
    label(fb, 6, 24, clock(me.finishTime >= 0 ? me.finishTime : race.time), white, 2);
    if (me.bestLap > 0) label(fb, 6, 42, "BEST " + clock(me.bestLap), yellow, 1);
    std::string pos = "P" + std::to_string(race.position(0));
    label(fb, W - 6 - textWidth(pos, 3) - 18, 6, pos, yellow, 3);
    label(fb, W - 6 - 15, 13, "/" + std::to_string(race.cars.size()), white, 1);
    std::string kmh = std::to_string(int(std::fabs(me.speed) * 4 + 0.5f));
    label(fb, 6, H - 28, kmh, white, 3);
    label(fb, 8 + textWidth(kmh, 3), H - 14, "KM/H", white, 1);
    minimap(fb, race);
    if (race.time < 0) centred(fb, 90, std::to_string(int(-race.time) + 1), yellow, 7);
    else if (race.time < 1) centred(fb, 90, "GO!", rgb565(90, 255, 90), 7);
    if (me.finishTime >= 0) {
        int p = race.position(0);
        const char* th = p == 1 ? "ST" : p == 2 ? "ND" : p == 3 ? "RD" : "TH";
        centred(fb, 70, "FINISHED " + std::to_string(p) + th, yellow, 3);
        centred(fb, 100, "TIME " + clock(me.finishTime), white, 2);
        centred(fb, 122, "R TO RACE AGAIN", white, 1);
    }
}

// ---------------------------------------------------------------- frame rendering
// Sky panorama: a 360-degree picture of sky, clouds, snowy mountains and wooded hills, painted once at start-up
// and fixed to compass direction so it turns with the camera. Column = heading, row = elevation above horizon.
struct Sky {
    static const int kW = 1440, kH = 160;
    static constexpr float kTop = 0.56f, kBottom = -0.04f;       // elevation (radians) of the first and last row
    std::vector<uint16_t> px = std::vector<uint16_t>(size_t(kW) * kH);

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

    explicit Sky(uint16_t haze) {
        for (int x = 0; x < kW; ++x) {
            float u = float(x) / kW;
            float far = 0.035f + 0.11f * std::pow(fbm(u, 0.5f, 6, 20), 1.6f);        // snowy range
            float near = 0.012f + 0.04f * fbm(u, 3.5f, 10, 30);                      // wooded hills in front
            float snow = far - 0.025f - 0.01f * fbm(u, 7.5f, 40, 40);
            for (int y = 0; y < kH; ++y) {
                float e = kTop + (kBottom - kTop) * y / (kH - 1);
                uint16_t c;
                if (e < near) {
                    float n = fbm(u, e * 300, 160, 50);
                    c = Renderer::mix(rgbf(40 + 30 * n, 70 + 40 * n, 50 + 25 * n), haze, 0.35f + 0.4f * std::max(0.f, -e / 0.04f));
                } else if (e < far) {
                    float n = fbm(u, e * 120, 90, 60);
                    c = e > snow ? rgbf(225 + 25 * n, 230 + 22 * n, 240 + 15 * n) : rgbf(80 + 45 * n, 88 + 45 * n, 108 + 45 * n);
                    c = Renderer::mix(c, haze, 0.3f * std::max(0.f, 1 - (e - near) / 0.05f) + 0.12f);
                } else {
                    float t = std::min(1.f, e / 0.45f);
                    uint16_t sky = Renderer::mix(haze, rgb565(55, 110, 200), std::sqrt(t));
                    float d = fbm(u, e * 14, 12, 70) * 0.75f + fbm(u, e * 40, 40, 80) * 0.25f;   // stretched sideways
                    float cloud = std::max(0.f, std::min(1.f, (d - 0.52f) * 4)) * std::min(1.f, (e - far) / 0.04f);
                    float shade = 0.78f + 0.22f * std::min(1.f, std::max(0.f, (d - 0.52f) * 3));   // grey bellies, white tops
                    c = Renderer::mix(sky, rgbf(250 * shade, 250 * shade, 255 * shade), cloud);
                }
                px[size_t(y) * kW + x] = c;
            }
        }
    }

    void draw(Renderer& r) const {
        float hy = r.h / 2 - r.focal * std::tan(r.camPitch);
        for (int x = 0; x < r.w; ++x) {
            float a = r.camYaw + std::atan((x - r.w / 2) / r.focal);
            int col = (int(std::floor(a / (2 * kPi) * kW)) % kW + kW) % kW;
            for (int y = 0; y < r.h; ++y) {
                float e = (hy - y) / r.focal;
                int row = std::max(0, std::min(kH - 1, int((kTop - e) / (kTop - kBottom) * (kH - 1))));
                r.color[size_t(y) * r.w + x] = px[size_t(row) * kW + col];
            }
        }
        std::fill(r.depth.begin(), r.depth.end(), 0.f);
    }
};

struct Chase { float yaw = 0, pitch = 0; bool init = false; };

static void render(Renderer& r, const Sky& sky, const World& w, const Race& race, Chase& cam, float dt) {
    const Car& me = race.cars[0];
    if (!cam.init) { cam.yaw = me.yaw; cam.pitch = me.pitch; cam.init = true; }
    float k = std::min(1.f, 5 * dt);
    cam.yaw = wrapAngle(cam.yaw + wrapAngle(me.yaw - cam.yaw) * k);              // camera swings after the car
    cam.pitch += (me.pitch - cam.pitch) * k;
    Vec3 fwd{std::sin(cam.yaw), 0, std::cos(cam.yaw)};
    r.cam = me.pos - fwd * 6.8f + Vec3{0, 2.5f, 0};
    float ground = race.terrain.height(r.cam.x, r.cam.z, me.idx);
    r.cam.y = std::max(r.cam.y, ground + 1.2f);                                    // never inside a hill
    r.camYaw = cam.yaw;
    r.camPitch = 0.15f + cam.pitch * 0.7f;                                         // look down the slope with the car
    sky.draw(r);
    auto visible = [&](Vec3 p, float radius) {
        Vec3 d = p - r.cam;
        d.y = 0;
        return dot(d, d) < (r.fogEnd + radius) * (r.fogEnd + radius) && dot(d, fwd) > -radius;
    };
    for (size_t i = 0; i < race.cars.size(); ++i) {                                 // nearest first: less overdraw
        const Car& c = race.cars[i];
        if (!visible(c.pos, 3)) continue;
        Transform t;
        t.pos = c.pos;
        t.yaw = c.yaw;
        t.pitch = c.pitch;
        r.draw(w.cars[i], t);
    }
    r.draw(w.road, Transform());
    for (size_t i = 0; i < w.ground.size(); ++i)
        if (visible(w.groundAt[i], 29)) r.draw(w.ground[i], Transform());
    r.draw(w.gantry, w.gantryAt);
    for (size_t i = 0; i < race.trees.size(); ++i) {
        if (!visible(race.trees[i], 3)) continue;
        Transform t;
        t.pos = race.trees[i];
        r.draw(w.pine[i % 2], t);
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
    r.light = normalize({-0.4f, 0.85f, -0.35f});
    r.ambient = 0.5f;
    r.sun = 0.6f;
    r.fogColor = rgb565(185, 205, 225);
    r.fogStart = 45;
    r.fogEnd = 100;
    return r;
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    bool flip = false;
    for (int i = 1; i < argc; ++i) flip = flip || !std::strcmp(argv[i], "flip");
    try {
        Race race;
        std::string exe = argv[0];                                              // assets live next to the binary
        World world(race, exe.substr(0, exe.rfind('/') + 1) + "assets/cars/");
        Renderer r = makeRenderer();
        Sky sky(r.fogColor);
        Chase cam;

        if (mode == "ppm") {
            float seconds = argc > 2 ? std::atof(argv[2]) : 0;
            std::string keys = argc > 3 ? argv[3] : "";
            Input in;
            in.up = keys.find('u') != std::string::npos; in.down = keys.find('d') != std::string::npos;
            in.left = keys.find('l') != std::string::npos; in.right = keys.find('r') != std::string::npos;
            race.cars[0].ai = keys.find('a') != std::string::npos;                          // a = autopilot
            const float dt = 1 / 15.f;
            for (float t = 0; t < seconds; t += dt) { race.step(dt, in); render(r, sky, world, race, cam, dt); }
            render(r, sky, world, race, cam, dt);
            hud(r.color, race);
            writePpm(r.color);
            return 0;
        }
        if (mode == "bench") {
            int frames = argc > 2 ? std::atoi(argv[2]) : 200;
            race.cars[0].ai = true;
            race.time = 0;
            auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < frames; ++f) { race.step(1 / 15.f, Input()); render(r, sky, world, race, cam, 1 / 15.f); hud(r.color, race); }
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("render %.1f ms/frame (%.0f fps without display)\n", 1000 * s / frames, frames / s);
            return 0;
        }

        bool demo = mode == "demo";
        ILI9341 tft(board::spi, board::gpiochip, board::dc, board::reset, board::hz, board::cs);
        tft.init();
        tft.setRotation((board::rotation + (flip ? 3 : 1)) % 4);                   // landscape
        Presenter show(tft);
        RawStdin raw;
        if (demo) race.cars[0].ai = true;
        std::printf("READY\n");
        std::fflush(stdout);

        Input in;
        bool stdinOpen = true, quit = false, cruise = false;
        auto last = std::chrono::steady_clock::now(), report = last;
        int frames = 0;
        double renderSec = 0;
        while (!quit) {
            pollfd pf{0, POLLIN, 0};
            while (stdinOpen && poll(&pf, 1, 0) > 0) {
                unsigned char buf[64];
                ssize_t n = read(0, buf, sizeof buf);
                if (n <= 0) { stdinOpen = false; break; }                         // no controller: keep running
                for (ssize_t i = 0; i < n; ++i) {
                    unsigned char b = buf[i];
                    if (b >= 0x40 && b <= 0x4F) {
                        if (b & 1) cruise = true;                                     // tap up: throttle stays on
                        if (b & 2) cruise = false;                                    // down brakes and cancels it
                        in.up = (b & 1) || cruise; in.down = b & 2; in.left = b & 4; in.right = b & 8;
                    }
                    else if (b == 'r') { cruise = false; in.up = false; race.reset(); cam.init = false; if (demo) race.cars[0].ai = true; }
                    else if (b == 'q' || b == 3) quit = true;
                }
            }
            auto now = std::chrono::steady_clock::now();
            float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
            last = now;
            for (float left = dt; left > 1e-6f; left -= 1 / 30.f) race.step(std::min(left, 1 / 30.f), in);
            render(r, sky, world, race, cam, dt);
            hud(r.color, race);
            renderSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
            show.submit(r.color);
            ++frames;
            if (std::chrono::duration<double>(now - report).count() >= 2) {
                double s = std::chrono::duration<double>(now - report).count();
                const Car& me = race.cars[0];
                std::printf("%.1f fps  render %.0f ms  lap %d/%d  P%d  %d km/h\n", frames / s, 1000 * renderSec / frames,
                            std::max(1, std::min(race.laps, me.lapsDone + 1)), race.laps, race.position(0), int(std::fabs(me.speed) * 4));
                std::fflush(stdout);
                frames = 0; renderSec = 0; report = now;
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
