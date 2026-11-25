// Car game logic: track, arcade car physics, AI drivers, laps and race positions. No rendering here.
#pragma once
#include "engine3d.h"
#include <cmath>
#include <cstdint>
#include <vector>

constexpr float kPi = 3.14159265f;
constexpr float kTopRoad = 38, kTopGrass = 13;          // terminal speeds (units/s) with throttle held
constexpr float kAccel = 16, kBrake = 30, kReverseTop = 8;
constexpr float kCarRadius = 1.3f, kTreeRadius = 0.6f, kWorldHalf = 150, kGravity = 9.8f;

inline float wrapAngle(float a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }

struct Track {
    std::vector<Vec3> c;                  // centreline samples on y=0, closed loop, `spacing` apart
    float halfWidth = 5, kerb = 1.5f, spacing = 3;      // kerb: the dirt verge either side of the road

    int n() const { return int(c.size()); }
    int wrap(int i) const { return ((i % n()) + n()) % n(); }
    const Vec3& at(int i) const { return c[wrap(i)]; }
    Vec3 dir(int i) const { return normalize(at(i + 1) - at(i)); }
    Vec3 right(int i) const { Vec3 d = normalize(at(i + 1) - at(i - 1)); return {d.z, 0, -d.x}; }

    // Nearest sample to p within `range` samples of `hint`; range < 0 searches the whole track.
    int nearest(Vec3 p, int hint, int range) const {
        int lo = range < 0 ? 0 : hint - range, hi = range < 0 ? n() - 1 : hint + range, best = wrap(hint);
        float bd = 1e30f;
        for (int i = lo; i <= hi; ++i) {
            Vec3 d = at(i) - p;
            float dd = d.x * d.x + d.z * d.z;
            if (dd < bd) { bd = dd; best = wrap(i); }
        }
        return best;
    }
    // Signed distance from the centreline near sample i (positive = right of travel).
    float offset(Vec3 p, int i) const {
        Vec3 a = at(i), d = dir(i);
        if (dot(p - a, d) < 0) { a = at(i - 1); d = dir(i - 1); }
        return dot(p - a, Vec3{d.z, 0, -d.x});
    }

    // The circuit: a closed Catmull-Rom curve through hand-placed points, resampled every `spacing` units.
    static Track build() {
        static const float P[][2] = {{-60, -100}, {40, -100}, {95, -80}, {110, -30}, {80, 0}, {40, 10}, {30, 45},
                                     {60, 80}, {40, 115}, {-20, 115}, {-60, 90}, {-70, 40}, {-110, 10}, {-115, -50}, {-95, -90}};
        const int np = sizeof(P) / sizeof(P[0]);
        std::vector<Vec3> dense;
        for (int i = 0; i < np; ++i) {
            const float *p0 = P[(i + np - 1) % np], *p1 = P[i], *p2 = P[(i + 1) % np], *p3 = P[(i + 2) % np];
            for (int s = 0; s < 32; ++s) {
                float t = s / 32.f, t2 = t * t, t3 = t2 * t;
                auto cr = [&](int k) {
                    return 0.5f * (2 * p1[k] + (p2[k] - p0[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
                                   (3 * p1[k] - p0[k] - 3 * p2[k] + p3[k]) * t3);
                };
                dense.push_back({cr(0), 0, cr(1)});
            }
        }
        // resample by arc length into a whole number of equal steps so the loop closes evenly
        std::vector<float> acc(1, 0.f);
        for (size_t i = 0; i < dense.size(); ++i) acc.push_back(acc.back() + length(dense[(i + 1) % dense.size()] - dense[i]));
        Track tr;
        int n = int(acc.back() / tr.spacing + 0.5f);
        tr.spacing = acc.back() / n;
        size_t seg = 0;
        for (int k = 0; k < n; ++k) {
            float s = k * tr.spacing;
            while (acc[seg + 1] < s) ++seg;
            Vec3 a = dense[seg], b = dense[(seg + 1) % dense.size()];
            tr.c.push_back(a + (b - a) * ((s - acc[seg]) / (acc[seg + 1] - acc[seg])));
        }
        // start line (sample 0) halfway down the bottom straight, so the grid behind it is straight too
        std::rotate(tr.c.begin(), tr.c.begin() + tr.nearest({0, 0, -100}, 0, -1), tr.c.end());
        return tr;
    }
};

// Rolling ground. The road lies on `base`, a gentle swell, so it climbs and dips with the land; the steeper
// `hills` only rise away from the road (from 15 units off the centreline), so they can never bury it.
struct Terrain {
    const Track* track;
    static constexpr float kFlat = 15, kBlend = 40;        // distance from the centreline where hills start / reach full height

    static float base(float x, float z) {
        return 6 * std::sin(x * 0.012f + 0.5f) * std::cos(z * 0.015f + 1) + 3 * std::sin((x + z) * 0.02f);
    }
    static float hills(float x, float z) {
        return 6 + 5 * std::sin(x * 0.06f) * std::cos(z * 0.05f + 0.7f) + 3 * std::sin(z * 0.11f + x * 0.03f);
    }
    // Ground height; `hint` is a nearby centreline sample (e.g. a car's idx), or -1 to search the whole track.
    float height(float x, float z, int hint = -1) const {
        Vec3 p{x, 0, z};
        Vec3 d = track->at(track->nearest(p, std::max(hint, 0), hint < 0 ? -1 : 12)) - p;
        float t = std::max(0.f, std::min(1.f, (std::sqrt(d.x * d.x + d.z * d.z) - kFlat) / (kBlend - kFlat)));
        return base(x, z) + hills(x, z) * t * t * (3 - 2 * t);
    }
};

struct Input { bool up = false, down = false, left = false, right = false; };

struct Car {
    Vec3 pos{0, 0, 0};
    float yaw = 0, speed = 0, steer = 0;  // yaw 0 faces +z; steer -1 (left) .. 1 (right)
    float pitch = 0;                      // nose down > 0, from the ground slope under the car
    int idx = 0;                          // nearest centreline sample
    int lapsDone = -1;                    // starts behind the line: the first crossing begins lap 1
    bool halfway = true;                  // must reach the far side of the track before a lap counts
    bool offroad = false;
    float finishTime = -1, lapStart = 0, bestLap = -1;
    bool ai = false;
    float aiTop = 30, aiLane = 0;         // AI cruising speed and lateral line
};

struct Race {
    Track track = Track::build();
    Terrain terrain{&track};
    std::vector<Car> cars;                // cars[0] is the player
    std::vector<Vec3> trees;
    float time = -3;                      // countdown; the race clock starts at 0
    int laps = 3;

    Race() { placeTrees(); reset(); }

    void reset() {
        cars.assign(3, Car());
        // grid slots: AI at the front, player at the back
        const int slot[3] = {2, 0, 1};
        for (int i = 0; i < 3; ++i) {
            Car& c = cars[i];
            int s = track.wrap(-2 - 4 * slot[i]);
            Vec3 d = track.dir(s);
            c.pos = track.at(s) + track.right(s) * (slot[i] % 2 ? -2.2f : 2.2f);
            c.yaw = std::atan2(d.x, d.z);
            c.idx = s;
            settle(c);
        }
        cars[1].ai = cars[2].ai = true;
        cars[1].aiTop = 33; cars[1].aiLane = -1.5f;
        cars[2].aiTop = 31; cars[2].aiLane = 1.5f;
        time = -3;
    }

    void step(float dt, Input in) {
        time += dt;
        if (time < 0) return;             // countdown: everyone waits on the grid
        for (size_t i = 0; i < cars.size(); ++i) {
            Car& c = cars[i];
            bool human = i == 0 && !c.ai && c.finishTime < 0;   // the player's car drives itself after the flag
            drive(c, human ? in : autopilot(c), dt);
            progress(c);
        }
        collide();
    }

    // 1-based race position: finished cars by finish time, then laps and distance.
    int position(int i) const {
        auto key = [&](const Car& c) { return c.finishTime >= 0 ? 1e9 - c.finishTime : double(c.lapsDone) * track.n() + c.idx; };
        int p = 1;
        for (size_t j = 0; j < cars.size(); ++j) if (int(j) != i && key(cars[j]) > key(cars[i])) ++p;
        return p;
    }

    void drive(Car& c, Input in, float dt) const {
        float top = c.offroad ? kTopGrass : kTopRoad;
        float target = float(in.right) - float(in.left);
        c.steer += std::max(-5 * dt, std::min(5 * dt, target - c.steer));
        if (in.up) c.speed += kAccel * dt;
        if (in.down) c.speed -= (c.speed > 0.5f ? kBrake : kAccel * 0.7f) * dt;   // brake, then reverse
        if (!in.up && !in.down) c.speed -= (c.speed > 0 ? 1 : -1) * std::min(std::fabs(c.speed), 4 * dt);
        c.speed -= c.speed * (kAccel / top) * dt;                                  // drag: terminal speed = top
        c.speed = std::max(-kReverseTop, c.speed);
        float v = std::fabs(c.speed);
        float rate = 1.9f * std::min(1.f, v / 6) * (1 - 0.45f * std::min(1.f, v / kTopRoad));
        c.speed += kGravity * std::sin(c.pitch) * dt;                              // slower uphill, faster downhill
        c.yaw = wrapAngle(c.yaw + c.steer * rate * (c.speed >= 0 ? 1 : -1) * dt);
        c.pos = c.pos + Vec3{std::sin(c.yaw), 0, std::cos(c.yaw)} * (c.speed * dt);
        if (std::fabs(c.pos.x) > kWorldHalf || std::fabs(c.pos.z) > kWorldHalf) {
            c.pos.x = std::max(-kWorldHalf, std::min(kWorldHalf, c.pos.x));
            c.pos.z = std::max(-kWorldHalf, std::min(kWorldHalf, c.pos.z));
            c.speed = 0;
        }
        settle(c);
    }

    // Put the car on the ground and tilt it to the slope under its wheels.
    void settle(Car& c) const {
        Vec3 f{std::sin(c.yaw) * 1.4f, 0, std::cos(c.yaw) * 1.4f};
        float front = terrain.height(c.pos.x + f.x, c.pos.z + f.z, c.idx), back = terrain.height(c.pos.x - f.x, c.pos.z - f.z, c.idx);
        c.pos.y = (front + back) / 2;
        c.pitch = -std::atan2(front - back, 2.8f);
    }

    // AI: aim a few samples ahead on its line; slow down for the curvature coming up.
    Input autopilot(const Car& c) const {
        Vec3 target = track.at(c.idx + 4) + track.right(c.idx + 4) * c.aiLane;
        float want = std::atan2(target.x - c.pos.x, target.z - c.pos.z);
        float err = wrapAngle(want - c.yaw);
        Vec3 d0 = track.dir(c.idx + 2), d1 = track.dir(c.idx + 10);
        float bend = std::acos(std::max(-1.f, std::min(1.f, dot(d0, d1))));
        float radius = 8 * track.spacing / std::max(bend, 1e-3f);
        float limit = 0.85f * 1.9f * radius / (1 + radius * 1.9f * 0.45f / kTopRoad);   // speed where turn rate still makes the bend
        float vmax = std::min(c.aiTop, limit);
        Input in;
        in.up = c.speed < vmax;
        in.down = c.speed > vmax + 3;
        in.left = err < -0.04f;
        in.right = err > 0.04f;
        return in;
    }

private:
    void progress(Car& c) {
        int n = track.n(), prev = c.idx;
        c.idx = track.nearest(c.pos, c.idx, 12);
        c.offroad = std::fabs(track.offset(c.pos, c.idx)) > track.halfWidth + track.kerb;
        if (c.idx > n * 2 / 5 && c.idx < n * 3 / 5) c.halfway = true;
        if (prev > n * 4 / 5 && c.idx < n / 5 && c.halfway) {      // crossed the start line going forward
            c.halfway = false;
            if (++c.lapsDone > 0) {
                float lap = time - c.lapStart;
                if (c.bestLap < 0 || lap < c.bestLap) c.bestLap = lap;
            }
            c.lapStart = time;
            if (c.lapsDone == laps && c.finishTime < 0) c.finishTime = time;
        }
    }

    void collide() {
        for (auto& c : cars)
            for (auto& t : trees) {
                Vec3 d = c.pos - t;
                d.y = 0;
                float dist = length(d), min = kCarRadius + kTreeRadius;
                if (dist >= min || dist < 1e-4f) continue;
                c.pos = t + d * (min / dist);
                c.speed *= -0.3f;                                     // bounce off the trunk
            }
        for (size_t i = 0; i < cars.size(); ++i)
            for (size_t j = i + 1; j < cars.size(); ++j) {
                Vec3 d = cars[j].pos - cars[i].pos;
                d.y = 0;
                float dist = length(d), min = 2 * kCarRadius;
                if (dist >= min || dist < 1e-4f) continue;
                Vec3 push = d * ((min - dist) / dist / 2);
                cars[i].pos = cars[i].pos - push;
                cars[j].pos = cars[j].pos + push;
                cars[i].speed *= 0.9f;
                cars[j].speed *= 0.9f;
            }
    }

    void placeTrees() {
        uint32_t s = 12345;
        auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.f; };
        for (int tries = 0; trees.size() < 110 && tries < 5000; ++tries) {
            Vec3 p{(rnd() * 2 - 1) * 140, 0, (rnd() * 2 - 1) * 140};
            if (length(track.at(track.nearest(p, 0, -1)) - p) < track.halfWidth + track.kerb + 6) continue;
            bool clear = true;
            for (auto& t : trees) clear = clear && length(Vec3{t.x - p.x, 0, t.z - p.z}) > 6;
            p.y = terrain.height(p.x, p.z);
            if (clear) trees.push_back(p);
        }
    }
};
