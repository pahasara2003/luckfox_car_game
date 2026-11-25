// Host self-check for the engine (depth, culling, lights, clipping, textures, fog, OBJ) and the game logic.
#include "game.h"
#include "objload.h"
#include <cassert>
#include <cstdio>
#include <fstream>
int main() {
    Renderer r(64, 64);
    r.clear(0);
    Mesh red = box(1, 1, 1, 0xF800), blue = box(1, 1, 1, 0x001F);
    Transform near_, far_; far_.pos = {0, 0, 3};
    r.draw(blue, far_);
    r.draw(red, near_);
    uint16_t mid = r.color[32 * 64 + 32];
    assert(mid != 0 && (mid & 0x1F) == 0);       // nearer red box wins over blue behind it
    assert(r.color[0] == 0);                       // corner stays background
    int n = 0; for (auto c : r.color) n += c != 0;
    assert(n > 100 && n < 64 * 64);
    // spotlight: floor pixel under the cone is brighter than one outside it
    Renderer s(64, 64);
    s.cam = {0, 2, -3}; s.sun = 0; s.ambient = 0.2f;
    SpotLight sp; sp.pos = {0, 3, 1}; sp.dir = {0, -1, 0}; s.spots.push_back(sp);
    s.clear(0);
    Transform ft; ft.pos = {0, 0, 1};
    s.draw(box(6, 0.1f, 6, 0xFFFF), ft);  // kept in front of the camera: no near-plane clipping
    // floor point (0,0,1) under the light projects to row ~59; row 48 is floor at z~3.5, outside the cone
    uint16_t lit = s.color[59 * 64 + 32], dark = s.color[48 * 64 + 32];
    std::printf("spot lit=%04x dark=%04x\n", lit, dark);
    assert(dark != 0 && (lit >> 11) > (dark >> 11));
    // OBJ: right-handed unit cube (CCW from outside), quads, a material, a negative index
    {
        std::ofstream("/tmp/t3d_cube.mtl") << "newmtl red\nKd 1 0 0\n";
        std::ofstream("/tmp/t3d_cube.obj") << "mtllib t3d_cube.mtl\nusemtl red\n"
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
            "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2/1/1 3/1/1 7/1/1 -3/1/1\n";
        Mesh c = loadObj("/tmp/t3d_cube.obj", 2);
        assert(c.t.size() == 12 && c.t[0].color == 0xF800);
        for (auto& p : c.v) assert(std::fabs(std::fabs(p.x) - 1) < 1e-5f);   // centred, scaled to size 2
        for (auto& t : c.t) {                                                  // every face normal points outward
            Vec3 a = c.v[t.a], b = c.v[t.b], d = c.v[t.c];
            Vec3 mid{(a.x + b.x + d.x) / 3, (a.y + b.y + d.y) / 3, (a.z + b.z + d.z) / 3};
            assert(dot(cross(b - a, d - a), mid) > 0);
        }
    }
    // rim: a sphere-ish box lit only by a backlight shows nothing on its camera-facing front
    {
        Renderer q(64, 64); q.ambient = 0; q.sun = 0; q.rim.strength = 2; q.rim.pos = {0, 0, 10};
        q.clear(0); q.draw(box(1, 1, 1, 0xFFFF), Transform());
        assert(q.color[32 * 64 + 32] == 0);
    }
    // near clipping: a floor reaching behind the camera still draws (before clipping it was dropped)
    {
        Renderer q(64, 64); q.cam = {0, 1, 0}; q.camPitch = 0.3f;
        q.clear(0);
        Mesh fl; fl.v = {{-10, 0, -10}, {-10, 0, 10}, {10, 0, 10}, {10, 0, -10}};
        fl.t = {{0, 1, 2, 0xFFFF, nullptr, {}}, {0, 2, 3, 0xFFFF, nullptr, {}}};
        q.draw(fl, Transform());
        assert(q.color[63 * 64 + 32] != 0 && q.color[0] == 0);
    }
    // textures: 2x2 checker on a box face, full light, nearest sampling, texel (0,0) at the face's top-left
    {
        Texture tex(2, 2);
        tex.at(0, 0) = 0xF800; tex.at(1, 0) = 0x07E0; tex.at(0, 1) = 0x001F; tex.at(1, 1) = 0xFFFF;
        Renderer q(64, 64); q.ambient = 1; q.sun = 0;
        q.clear(0); q.draw(box(4, 4, 1, 0xFFFF, &tex), Transform());
        assert(q.color[20 * 64 + 20] == 0xF800 && q.color[20 * 64 + 44] == 0x07E0);
        assert(q.color[44 * 64 + 20] == 0x001F && q.color[44 * 64 + 44] == 0xFFFF);
        bool threw = false;
        try { Texture bad(3, 2); } catch (const std::runtime_error&) { threw = true; }
        assert(threw);
    }
    // camera yaw: looking down +x puts an object at +x in the centre
    {
        Renderer q(64, 64); q.cam = {0, 0, 0}; q.camYaw = kPi / 2;
        q.clear(0);
        Transform t; t.pos = {5, 0, 0};
        q.draw(box(1, 1, 1, 0xFFFF), t);
        assert(q.color[32 * 64 + 32] != 0);
    }
    // fog: a distant white box tints towards the fog colour; beyond fogEnd it is skipped
    {
        Renderer q(64, 64); q.cam = {0, 0, 0}; q.ambient = 1; q.sun = 0;
        q.fogStart = 10; q.fogEnd = 60; q.fogColor = 0x001F;
        q.clear(0);
        Transform t; t.pos = {0, 0, 50};
        q.draw(box(10, 10, 1, 0xFFFF), t);
        uint16_t c = q.color[32 * 64 + 32];
        assert(c != 0 && (c >> 11) < 16 && (c & 31) == 31);
        q.clear(0); t.pos.z = 80;
        q.draw(box(10, 10, 1, 0xFFFF), t);
        assert(q.color[32 * 64 + 32] == 0);
    }
    // OBJ with vt + map_Kd texture (PPM)
    {
        std::ofstream("/tmp/t3d_tex.ppm", std::ios::binary) << "P6\n# c\n2 2\n255\n" << std::string("\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12);
        std::ofstream("/tmp/t3d_quad.mtl") << "newmtl t\nKd 1 1 1\nmap_Kd t3d_tex.ppm\n";
        std::ofstream("/tmp/t3d_quad.obj") << "mtllib t3d_quad.mtl\nusemtl t\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3 4/4\n";
        Mesh q = loadObj("/tmp/t3d_quad.obj");
        assert(q.t.size() == 2 && q.t[0].tex && q.textures.size() == 1);
        assert(q.t[0].tex->px[0] == 0xF800 && q.t[0].tex->px[3] == 0xFFFF);
        assert(q.t[0].uv[0].u == 0 && q.t[0].uv[0].v == 1);       // vt (0,0) is the image's bottom-left
    }
    {   // model pitch is about the model's own x: a long box turned to face +x, nose up, rises on the right
        Renderer pr(64, 64); pr.cam = {0, 0, -12}; pr.clear(0);
        Transform pt; pt.yaw = kPi / 2; pt.pitch = -0.5f;
        pr.draw(box(0.4f, 0.4f, 8, 0xFFFF), pt);
        auto top = [&](int x) { for (int y = 0; y < 64; ++y) if (pr.color[y * 64 + x]) return y; return 64; };
        assert(top(48) + 4 < top(16));
    }
    std::puts("engine3d ok");

    // game: the track closes on itself with even spacing
    Race race;
    const Track& tr = race.track;
    assert(tr.n() > 150);
    for (int i = 0; i < tr.n(); ++i) {
        float d = length(tr.at(i + 1) - tr.at(i));
        assert(d > 2.4f && d < 3.3f);
    }
    assert(race.trees.size() >= 50);
    // countdown holds everyone on the grid; the player starts last
    Vec3 start = race.cars[0].pos;
    Input go; go.up = true;
    for (int i = 0; i < 80; ++i) race.step(1 / 30.f, go);
    assert(race.time < 0 && length(race.cars[0].pos - start) == 0 && race.position(0) == 3);
    // throttle on the start straight: speeds up along the road, stays on the tarmac
    for (int i = 0; i < 20 + 90; ++i) race.step(1 / 30.f, go);
    assert(race.cars[0].speed > 25 && !race.cars[0].offroad && race.cars[0].lapsDone == 0);
    // full race on autopilot: every car finishes 3 laps, mostly on the road, in a plausible time
    Race auto_race;
    auto_race.cars[0].ai = true;
    std::vector<int> off(3, 0);
    int steps = 0;
    for (; steps < 30 * 400; ++steps) {
        auto_race.step(1 / 30.f, Input());
        for (int i = 0; i < 3; ++i) off[i] += auto_race.cars[i].offroad;
        bool all = true;
        for (auto& c : auto_race.cars) all = all && c.finishTime >= 0;
        if (all) break;
    }
    for (int i = 0; i < 3; ++i) {
        const Car& c = auto_race.cars[i];
        std::printf("car %d: finish %.1fs best lap %.1fs offroad %.1f%%\n", i, c.finishTime, c.bestLap, 100.f * off[i] / steps);
        assert(c.finishTime > 30 && c.lapsDone == 3 && c.bestLap > 10 && off[i] < steps / 20);
    }
    {   // terrain: the road follows `base` and no hill reaches into the road or onto the 5-unit ground mesh around it
        const Terrain& te = auto_race.terrain;
        float grade = 0, bank = 0, reach = tr.halfWidth + tr.kerb + 5 * 0.7072f * 2;   // road + verge + a mesh cell diagonal
        for (int i = 0; i < tr.n(); ++i) {
            Vec3 a = tr.at(i), b = tr.at(i + 1), rt = tr.right(i);
            grade = std::max(grade, std::fabs(Terrain::base(b.x, b.z) - Terrain::base(a.x, a.z)) / tr.spacing);
            Vec3 l = a - rt * (tr.halfWidth + tr.kerb), r2 = a + rt * (tr.halfWidth + tr.kerb);
            bank = std::max(bank, std::fabs(Terrain::base(l.x, l.z) - Terrain::base(r2.x, r2.z)) / (2 * (tr.halfWidth + tr.kerb)));
            for (float s = -reach; s <= reach; s += 0.5f) {
                Vec3 p = a + rt * s;
                assert(std::fabs(te.height(p.x, p.z) - Terrain::base(p.x, p.z)) < 1e-4f);
            }
        }
        float lo = 1e9, hi = -1e9;
        for (auto& p : tr.c) { lo = std::min(lo, Terrain::base(p.x, p.z)); hi = std::max(hi, Terrain::base(p.x, p.z)); }
        std::printf("terrain: road climbs %.1f units, steepest grade %.0f%%, steepest bank %.0f%%\n", hi - lo, 100 * grade, 100 * bank);
        assert(hi - lo > 6 && grade < 0.15f && bank < 0.15f);
        Car c = auto_race.cars[0];                     // cars sit on the ground and tilt nose-up when climbing
        assert(std::fabs(c.pos.y - te.height(c.pos.x, c.pos.z)) < 0.2f);
        for (auto& t : auto_race.trees) assert(std::fabs(t.y - te.height(t.x, t.z)) < 1e-4f);
    }
    std::printf("track: %d samples, %.0f units\n", tr.n(), tr.n() * tr.spacing);
    std::puts("game ok");
}
