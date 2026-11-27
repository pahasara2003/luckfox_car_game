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
        Mesh tinted = box(4, 4, 1, 0xFFFF, &tex);                 // per-vertex tint multiplies the texture: white texel turns half red
        tinted.tint.assign(tinted.v.size(), Vec3{0.5f, 0, 0});
        q.clear(0); q.draw(tinted, Transform());
        assert(q.color[44 * 64 + 44] == rgb565(123, 0, 0) || q.color[44 * 64 + 44] == uint16_t(15 << 11));
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
    {   // road choice and weather have a predictable grip order
        Race grip;
        float lastAsphalt = 4, lastGravel = 4;
        for (Weather weather : {Weather::Sunny, Weather::Rain, Weather::Snow}) {
            grip.weather = weather;
            grip.surface = RoadSurface::Asphalt;
            float asphalt = grip.roadGrip();
            grip.surface = RoadSurface::Gravel;
            float gravel = grip.roadGrip();
            assert(asphalt > gravel && asphalt < lastAsphalt && gravel < lastGravel);
            lastAsphalt = asphalt;
            lastGravel = gravel;
        }
        Car moving;
        moving.pos = {0, 0, 1};
        moving.idx = grip.track.nearest(moving.pos, 0, -1);
        moving.speed = 20;
        grip.settle(moving);
        float gravelHeight = moving.pos.y;
        grip.surface = RoadSurface::Asphalt;
        grip.settle(moving);
        assert(gravelHeight == moving.pos.y);
    }
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
    assert(race.time < 0 && length(race.cars[0].pos - start) == 0 && race.position(0) == int(race.cars.size()));
    // throttle on the start straight: speeds up along the road, stays on the tarmac
    race.cars.resize(1);
    for (int i = 0; i < 20 + 90; ++i) race.step(1 / 30.f, go);
    assert(race.cars[0].speed > 25 && !race.cars[0].offroad && race.cars[0].lapsDone == 0);
    {   // drift: holding a steer key at full speed (drift starts after 0.3 s) slides the car round a hairpin; letting go straightens it
        Race flat;
        Car c; c.speed = 36;
        Input hold; hold.right = true; hold.up = true;
        float travelled = 0, turned = 0, slip = 0;
        while (turned < kPi / 2 && travelled < 100) {
            float before = c.yaw + std::atan2(c.vy, c.speed);
            Vec3 p = c.pos;
            flat.drive(c, hold, 1 / 24.f);
            c.pitch = 0;
            turned += wrapAngle(c.yaw + std::atan2(c.vy, c.speed) - before);
            travelled += length(c.pos - p);
            slip = std::max(slip, std::fabs(std::atan2(c.vy, c.speed)));
        }
        Input go; go.up = true;
        for (int i = 0; i < 24; ++i) { flat.drive(c, go, 1 / 24.f); c.pitch = 0; }
        std::printf("drift: 90 deg in %.0f units, up to %.0f deg sideways; after letting go yaw %.2f\n", travelled, slip * 57.3f, c.yawRate);
        assert(travelled < 60 && std::fabs(c.yawRate) < 0.1f && std::fabs(c.vy) < 0.5f);
    }
    // full race on autopilot: every car finishes 3 laps, mostly on the road, in a plausible time
    Race auto_race;
    for (Weather weather : {Weather::Sunny, Weather::Rain, Weather::Snow})
        for (RoadSurface surface : {RoadSurface::Gravel, RoadSurface::Asphalt}) {
            auto_race.reset();
            auto_race.weather = weather;
            auto_race.surface = surface;
            auto_race.cars[0].ai = true;
            std::vector<int> off(auto_race.cars.size(), 0);
            int steps = 0;
            for (; steps < 30 * 400; ++steps) {
                auto_race.step(1 / 30.f, Input());
                for (size_t i = 0; i < auto_race.cars.size(); ++i) off[i] += auto_race.cars[i].offroad;
                bool all = true;
                for (auto& c : auto_race.cars) all = all && c.finishTime >= 0;
                if (all) break;
            }
            for (size_t i = 0; i < auto_race.cars.size(); ++i) {
                const Car& c = auto_race.cars[i];
                std::printf("car %zu: finish %.1fs best lap %.1fs offroad %.1f%%\n", i, c.finishTime, c.bestLap, 100.f * off[i] / steps);
                assert(c.finishTime > 30 && c.lapsDone == 3 && c.bestLap > 10 && off[i] < steps / 20);
            }
        }
    {   // terrain: the road follows `base` and no hill reaches into the road or onto the 5-unit ground mesh around it
        const Terrain& te = auto_race.terrain;
        float grade = 0, bank = 0, reach = tr.halfWidth + tr.kerb + 5 * 0.7072f * 2;   // road + verge + a mesh cell diagonal
        for (int i = 0; i < tr.n(); ++i) {
            Vec3 a = tr.at(i), b = tr.at(i + 1), rt = tr.right(i);
            grade = std::max(grade, std::fabs(Terrain::base(b.x, b.z) - Terrain::base(a.x, a.z)) / tr.spacing);
            Vec3 l = a - rt * (tr.halfWidth + tr.kerb), r2 = a + rt * (tr.halfWidth + tr.kerb);
            bank = std::max(bank, std::fabs(Terrain::base(l.x, l.z) - Terrain::base(r2.x, r2.z)) / (2 * (tr.halfWidth + tr.kerb)));
            for (float s = -reach; s <= reach; s += 0.5f) {   // beyond the verge only the river may cut lower
                Vec3 p = a + rt * s;
                float dh = te.height(p.x, p.z) - Terrain::base(p.x, p.z);
                assert(std::fabs(s) <= tr.halfWidth + tr.kerb ? std::fabs(dh) < 1e-4f : dh < 1e-4f);
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
    {   // scenery: one bridge over the river, water below the road there, nothing standing on the road or in the water
        const Terrain& te = race.terrain;
        int crossings = 0;
        for (int i = 0; i < tr.n(); ++i) {
            bool in = Terrain::wet(tr.at(i).x, tr.at(i).z) > 0.999f, prev = Terrain::wet(tr.at(i - 1).x, tr.at(i - 1).z) > 0.999f;
            crossings += in && !prev;
        }
        Vec3 b = tr.at(race.bridge);
        assert(crossings == 1 && Terrain::waterLevel(b.x, b.z) < Terrain::base(b.x, b.z) - 0.5f);
        Vec3 bank = b + tr.right(race.bridge) * 10.f;
        assert(te.height(bank.x, bank.z) < Terrain::waterLevel(bank.x, bank.z));     // a river under the bridge
        for (auto& k : race.blocks) {
            int i = tr.nearest(k.pos, 0, -1);
            assert(std::fabs(tr.offset(k.pos, i)) > tr.halfWidth + tr.kerb);
            assert(k.kind == Block::Railing || Terrain::wet(k.pos.x, k.pos.z) == 0);
        }
        for (auto& t : race.trees) assert(Terrain::wet(t.x, t.z) == 0);
        int coins = 0, tanks = 0;
        for (auto& p : race.pickups) { coins += !p.nitro; tanks += p.nitro; }
        assert(coins >= 15 && coins <= 24 && tanks >= 4);
        std::printf("scenery: %zu buildings/railings, bridge at sample %d, %d coins, %d nitrous tanks\n", race.blocks.size(), race.bridge, coins, tanks);
    }
    {   // bridge railings keep a car that slides sideways on the bridge out of the river; houses are solid
        Race br;
        br.cars.resize(1);
        br.time = 0;
        Car& c = br.cars[0];
        c.ai = true;
        int i = br.bridge;
        Vec3 d = tr.dir(i);
        c.pos = tr.at(i); c.idx = i; c.yaw = std::atan2(d.x, d.z) - kPi / 2;    // nose towards the left railing
        c.speed = 25;
        for (int k = 0; k < 30; ++k) { Input none; br.drive(c, none, 1 / 30.f); br.step(0, none); }
        assert(std::fabs(tr.offset(c.pos, c.idx)) < tr.halfWidth + tr.kerb);
        for (const Block& b : br.blocks) {
            if (b.kind != Block::House) continue;
            c.pos = b.pos + Vec3{0.3f, 0, 0.2f};                               // dropped inside a house: pushed out of it
            c.speed = c.vy = 0;
            br.step(0, Input());
            float lx = std::fabs(dot(c.pos - b.pos, Vec3{std::cos(b.yaw), 0, -std::sin(b.yaw)}));
            float lz = std::fabs(dot(c.pos - b.pos, Vec3{std::sin(b.yaw), 0, std::cos(b.yaw)}));
            assert(lx >= b.hx + kCarRadius - 1e-3f || lz >= b.hz + kCarRadius - 1e-3f);
            break;
        }
    }
    {   // pickups: driving through a coin row collects it; a tank refills nitrous; all return next lap
        Race pk;
        Car& c = pk.cars[0];
        const Pickup coin = pk.pickups[0];
        c.pos = coin.pos;
        int lap = c.lapsDone;
        pk.collect(c, lap);
        assert(c.coins == 1 && pk.pickups[0].taken);
        pk.collect(c, lap);
        assert(c.coins == 1);                                   // only once
        for (auto& p : pk.pickups) if (p.nitro) { c.pos = p.pos; break; }
        float before = c.nitro;
        pk.collect(c, lap);
        assert(c.nitro > before);
        c.lapsDone = lap + 1;
        c.pos = {0, 0, 1e4f};
        pk.collect(c, lap);
        for (auto& p : pk.pickups) assert(!p.taken);
    }
    {   // nitrous like a player: cruise at top speed, hold the boost key, let go, still turns as before
        Race flat;
        auto run = [&](Car& c, Input in, float secs) {   // on the spot: flat ground, never reaching the map edge
            for (int i = 0; i < int(secs * 24); ++i) { flat.drive(c, in, 1 / 24.f); c.pitch = 0; c.pos = {0, 0, 0}; }
        };
        Input go; go.up = true;
        Car c; c.nitro = 1;
        run(c, go, 8);
        float cruise = c.speed;
        Input boost = go; boost.nitro = true;
        run(c, boost, 1);
        float after1s = c.speed;
        run(c, boost, 3);
        assert(c.nitro == 0 && !c.boosting);
        float peak = c.speed;
        run(c, go, 3);
        std::printf("nitrous: cruise %.0f, +1 s %.0f, peak %.0f, 3 s after %.0f km/h\n", cruise * 4, after1s * 4, peak * 4, c.speed * 4);
        std::fflush(stdout);
        assert(cruise > 36 && after1s > cruise + 3 && peak > cruise + 7 && c.speed < peak - 4 && c.speed > cruise - 1);
        Car t1, t2; t1.speed = t2.speed = 30; t2.nitro = 1;     // a tap of steer turns the same with or without boost
        Input steer = go; steer.right = true;
        Input steerBoost = steer; steerBoost.nitro = true;
        run(t1, steer, 0.25f); run(t2, steerBoost, 0.25f);
        assert(t1.yaw > 0.1f && t2.yaw > 0.7f * t1.yaw);
        run(t1, go, 0.5f); run(t2, go, 0.5f);
        assert(std::fabs(t1.yawRate) < 0.3f && std::fabs(t2.yawRate) < 0.3f);   // and settles after release
        for (int brakeTap = 0; brakeTap < 2; ++brakeTap) {     // tank runs dry above top speed, then a drift (and a brake tap)
            Car d; d.nitro = 1;
            run(d, go, 8); run(d, boost, 3.2f);
            Input drift = go; drift.right = true;
            Input tap = drift; tap.down = true;
            run(d, drift, 0.6f);
            if (brakeTap) { run(d, tap, 0.2f); run(d, drift, 0.2f); }
            run(d, go, 0.5f);
            assert(std::fabs(d.yawRate) < 0.3f && std::fabs(d.vy) < 1);
        }
    }
    std::printf("track: %d samples, %.0f units\n", tr.n(), tr.n() * tr.spacing);
    std::puts("game ok");
}
