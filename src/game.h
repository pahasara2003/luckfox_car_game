// Car game logic: track, arcade car physics, AI drivers, laps and race positions. No rendering here.
#pragma once
#include "engine3d.h"
#include <cmath>
#include <cstdint>
#include <vector>

constexpr float kPi = 3.14159265f;
constexpr float kTopRoad = 38, kTopGrass = 13;          // terminal speeds (units/s) with throttle held
constexpr float kAccel = 16, kBrake = 30, kReverseTop = 8;
constexpr float kGripRoad = 2.6f, kGripGrass = 1.3f;   // tyre friction; above real (~1.2) because the bends are tight for the speeds
constexpr float kCarRadius = 1.3f, kTreeRadius = 0.6f, kWorldHalf = 150, kGravity = 9.8f;
constexpr float kTopNitro = 52, kNitroPull = 1.8f, kNitroSeconds = 3;   // a full tank boosts for 3 s
constexpr float kNitroTank = 0.35f;                     // share of the tank one pickup refills

enum class Weather { Sunny, Rain, Snow };
enum class RoadSurface { Gravel, Asphalt };

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

inline float smooth01(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3 - 2 * t); }

// Rolling ground. The road lies on `base`, a gentle swell, so it climbs and dips with the land; the steeper
// `hills` only rise away from the road (from 15 units off the centreline), so they can never bury it.
// A lake in the infield drains through a river that passes under the west straight (on a bridge) to the edge.
struct Terrain {
    const Track* track;
    static constexpr float kFlat = 15, kBlend = 40;        // distance from the centreline where hills start / reach full height
    static constexpr float kLakeX = -25, kLakeZ = -60, kLakeR = 24, kRiverHalf = 4, kBank = 5;
    static constexpr int kRiverPts = 6;
    static const float (&riverPts())[kRiverPts][2] {
        static const float r[kRiverPts][2] = {{-25, -60}, {-55, -46}, {-85, -26}, {-116, -22}, {-138, -34}, {-170, -36}};
        return r;
    }
    // Water surface: just under the swell, so the river runs downhill with the land.
    static float waterLevel(float x, float z) { return base(x, z) - 0.6f; }
    // 1 on the lake or river bed, easing to 0 across the bank.
    static float wet(float x, float z) {
        float lake = std::hypot(x - kLakeX, z - kLakeZ) - kLakeR, river = 1e9f;
        const auto& R = riverPts();
        for (int i = 0; i + 1 < kRiverPts; ++i) {
            float ax = R[i][0], az = R[i][1], dx = R[i + 1][0] - ax, dz = R[i + 1][1] - az;
            float t = std::max(0.f, std::min(1.f, ((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz)));
            river = std::min(river, std::hypot(x - ax - dx * t, z - az - dz * t) - kRiverHalf);
        }
        return 1 - smooth01(std::min(lake, river) / kBank);
    }

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
        float dist = std::sqrt(d.x * d.x + d.z * d.z);
        float shore = std::hypot(x - kLakeX, z - kLakeZ) - kLakeR;                  // meadows round the lake: seen from the road
        float h = base(x, z) + hills(x, z) * smooth01((dist - kFlat) / (kBlend - kFlat)) * smooth01(shore / 35);
        float carve = wet(x, z) * smooth01((dist - track->halfWidth - track->kerb) / 3);   // the road crosses on a bridge
        return h + (base(x, z) - 3 - h) * carve;
    }
};

struct Input { bool up = false, down = false, left = false, right = false, nitro = false; float wheel = 0; };   // wheel: AI's analog steer, used when neither key is held

struct Car {
    Vec3 pos{0, 0, 0};
    float yaw = 0, speed = 0, steer = 0;  // yaw 0 faces +z; speed is along the nose; steer -1 (left) .. 1 (right)
    float vy = 0, yawRate = 0;            // sideways speed (+ = to the right of the nose) and turn rate: a drift is vy != 0
    float ax = 0;                         // smoothed forward acceleration, moves weight between the axles
    float drift = 0;                      // 0..1: how far the player has thrown the car into a slide
    float held = 0;                       // seconds the current steer key has been held
    float pitch = 0;                      // nose down > 0, from the ground slope under the car
    int idx = 0;                          // nearest centreline sample
    int lapsDone = -1;                    // starts behind the line: the first crossing begins lap 1
    bool halfway = true;                  // must reach the far side of the track before a lap counts
    bool offroad = false;
    float nitro = 0.5f;                   // 0..1 of a full nitrous tank
    bool boosting = false;
    int coins = 0;
    float finishTime = -1, lapStart = 0, bestLap = -1;
    bool ai = false;
    float aiTop = 30, aiLane = 0;         // AI cruising speed and lateral line
    float aiCorner = 1;                   // AI corner speed: share of the grip it dares to use
};

// Solid box on the ground (house, tower or bridge railing): yaw 0 has its depth `hz` along +z.
struct Block { enum Kind { House, Tower, Railing }; Vec3 pos; float yaw, hx, hz; Kind kind; int style; };   // style: unused, 0
struct Pickup { Vec3 pos; bool nitro, taken; };

struct Race {
    Track track = Track::build();
    Terrain terrain{&track};
    std::vector<Car> cars;                // cars[0] is the player
    std::vector<Vec3> trees;
    std::vector<Block> blocks;
    std::vector<Pickup> pickups;          // coins and nitrous for the player; they come back every lap
    int bridge = 0;                       // centreline sample where the road crosses the river
    float bridgeLen = 24;
    int tunnelLo = 19, tunnelHi = 31;     // centreline samples where the road runs through a hill (the walls are railings)
    float time = -3;                      // countdown; the race clock starts at 0
    int laps = 3;
    Weather weather = Weather::Sunny;
    RoadSurface surface = RoadSurface::Gravel;

    float weatherGrip() const {
        return weather == Weather::Rain ? 0.78f : weather == Weather::Snow ? 0.48f : 1.f;
    }

    float roadGrip() const {
        return (surface == RoadSurface::Asphalt ? 3.0f : kGripRoad) * weatherGrip();
    }

    Race() { placeScenery(); placeTrees(); reset(); }

    void reset() {
        cars.assign(6, Car());
        // grid slots: AI at the front, player at the back
        const int slot[6] = {5, 0, 1, 2, 3, 4};
        for (int i = 0; i < 6; ++i) {
            Car& c = cars[i];
            int s = track.wrap(-2 - 2 * slot[i]);
            Vec3 d = track.dir(s);
            c.pos = track.at(s) + track.right(s) * (slot[i] % 2 ? -2.2f : 2.2f);
            c.yaw = std::atan2(d.x, d.z);
            c.idx = s;
            settle(c);
        }
        const float top[5] = {38, 37.5f, 37, 36.5f, 36};
        const float corner[5] = {1.15f, 1.1f, 1.05f, 1.0f, 0.95f};
        const float lane[5] = {-1.5f, 1.5f, -0.5f, 0.5f, 0};
        for (int i = 1; i < 6; ++i) {
            cars[i].ai = true;
            cars[i].aiTop = top[i - 1];
            cars[i].aiCorner = corner[i - 1];
            cars[i].aiLane = lane[i - 1];
        }
        for (auto& p : pickups) p.taken = false;
        time = -3;
    }

    void step(float dt, Input in) {
        time += dt;
        if (time < 0) return;             // countdown: everyone waits on the grid
        for (size_t i = 0; i < cars.size(); ++i) {
            Car& c = cars[i];
            bool human = i == 0 && !c.ai && c.finishTime < 0;   // the player's car drives itself after the flag
            int lapsBefore = c.lapsDone;
            drive(c, human ? in : autopilot(c), dt);
            progress(c);
            if (i == 0) collect(c, lapsBefore);
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
        float target = in.left || in.right ? float(in.right) - float(in.left) : in.wheel;
        c.steer += std::max(-6 * dt, std::min(6 * dt, target - c.steer));
        c.held = in.left || in.right ? c.held + dt : 0;
        c.boosting = in.nitro && !in.down && c.nitro > 0;
        if (c.boosting) c.nitro = std::max(0.f, c.nitro - dt / kNitroSeconds);
        bool hard = c.held > 0.3f && std::fabs(c.speed) > 12;                      // only a steer key held at speed
        c.drift += ((hard ? 1.f : 0.f) - c.drift) * std::min(1.f, (hard ? 2.5f : 8) * dt); // throws the car into a drift
        int n = int(std::ceil(dt / 0.0025f));                                      // tyres are stiff: small steps
        for (int i = 0; i < n; ++i) tyres(c, in, dt / n);
        if (std::fabs(c.pos.x) > kWorldHalf || std::fabs(c.pos.z) > kWorldHalf) {
            c.pos.x = std::max(-kWorldHalf, std::min(kWorldHalf, c.pos.x));
            c.pos.z = std::max(-kWorldHalf, std::min(kWorldHalf, c.pos.z));
            c.speed = c.vy = c.yawRate = 0;
        }
        settle(c);
    }

    // Two-axle ("bicycle") car with Pacejka tyres, all forces per unit mass. Each axle's grip is shared between
    // driving/braking and cornering (friction circle); past the peak slip angle the tyre slides and grip falls,
    // so a rear axle that runs out of grip swings out: lift off or brake into a bend and the weight moves forward.
    void tyres(Car& c, Input in, float dt) const {
        const float a = 1.25f, b = 1.35f, L = a + b, cgH = 0.28f, gyr2 = 1.5f;   // CG to axles, CG height (halved with the doubled grip), yaw inertia / mass
        float mu = c.offroad ? kGripGrass * weatherGrip() : roadGrip();
        float vx = c.speed, vy = c.vy, r = c.yawRate, v = std::hypot(vx, vy);
        float slide = v > 1 ? std::atan2(vy, std::fabs(vx)) : 0;                   // body slip angle
        float lock = 0.6f / (1 + v / 10);                                          // less lock at speed, like a driver
        if (c.steer * slide > 0) lock = std::min(0.6f, std::max(lock, std::fabs(slide) + 0.1f));   // countersteer
        // caster: with the wheel let go, the front wheels swing to follow the slide, which catches it
        float delta = c.steer * lock + slide * (1 - std::fabs(c.steer));
        float cd = std::cos(delta), sd = std::sin(delta);
        float drive = 0, brake = 0;
        if (c.boosting) drive = kNitroPull * kAccel * std::max(-0.5f, 1 - vx / kTopNitro);
        else if (in.up) drive = kAccel * std::max(-0.5f, 1 - vx / kTopRoad);     // engine pull fades toward top speed, holds it back above
        if (in.down) { if (vx > 0.5f) brake = kBrake; else drive = -0.7f * kAccel * std::max(0.f, 1 + vx / kReverseTop); }
        if (!in.up && !in.down) brake = 1.5f;                                      // rolling resistance, engine braking (~0.15 g)
        brake = std::min(brake, std::fabs(vx) / dt) * (vx >= 0 ? 1 : -1);          // brakes stop the car, never reverse it
        float fzF = std::max(0.f, (kGravity * b - c.ax * cgH) / L), fzR = std::max(0.f, (kGravity * a + c.ax * cgH) / L);
        float dF = mu * fzF, dR = 1.25f * mu * fzR;                               // grip per axle; wider rear tyres
        // 4WD split 40/60 to the rear; brakes share by axle load, capped by ABS; wheelspin caps at grip
        float bF = std::copysign(std::min(0.75f * dF, std::fabs(brake) * fzF / kGravity), brake);
        float bR = std::copysign(std::min(0.7f * dR, std::fabs(brake) * fzR / kGravity), brake);   // ABS keeps the rear tracking
        float pF = 0.4f * drive, limF = 0.7f * dF;                                 // centre diff: front keeps 30% grip
        if (std::fabs(pF) > limF) pF = std::copysign(limF, pF);                    // to steer, the rest goes rearward
        float xF = std::max(-dF, std::min(dF, pF - bF));
        float xR = std::max(-dR, std::min(dR, drive - pF - bR));
        auto lateral = [](float slip, float d, float x, float stiff) {             // magic formula, grip left after x
            return -std::sqrt(std::max(0.f, d * d - x * x)) * std::sin(1.15f * std::atan(stiff * slip));
        };
        float vfy = vy + a * r, latF = vfy * cd - vx * sd, lonF = vx * cd + vfy * sd;
        float yF = lateral(std::atan2(latF, std::max(std::fabs(lonF), 2.f)), dF, xF, 14);
        float yR = lateral(std::atan2(vy - b * r, std::max(std::fabs(vx), 2.f)), dR, xR, 24);  // rear stiffer: stable below the limit, slides under braking
        float fx = xF * cd - yF * sd + xR, fy = xF * sd + yF * cd + yR;
        float grass = c.offroad ? kAccel * (1 / kTopGrass - 1 / kTopRoad) : 0;      // grass drag: top speed kTopGrass
        c.speed += (fx + r * vy + kGravity * std::sin(c.pitch) - grass * vx) * dt;  // slower uphill, faster downhill
        c.vy += (fy - r * vx - grass * vy) * dt;
        c.yawRate += (a * (xF * sd + yF * cd) - b * yR) / gyr2 * dt;
        c.ax += (fx - c.ax) * std::min(1.f, 10 * dt);                               // suspension takes a moment
        if (c.drift > 0 && c.speed > 5) {      // drift: like a rally driver balancing throttle and countersteer, the path
            float side = c.steer >= 0 ? 1.f : -1.f, sp = std::hypot(c.speed, c.vy);   // bends tight while the nose
            float turn = side * std::min(1.5f, sp / 24);                               // points ~25 deg into the bend
            float dir = std::atan2(c.vy, c.speed) + turn * c.drift * dt;               // travel direction, from the nose
            float spin = turn + 1.5f * (std::atan2(c.vy, c.speed) + side * 0.35f);    // hold the slide angle
            c.yawRate += (spin - c.yawRate) * std::min(1.f, 4 * dt) * c.drift;      // eases in: gentle rotation
            sp *= 1 - 0.35f * std::fabs(std::sin(dir)) * c.drift * dt;                  // sliding scrubs speed
            c.speed = sp * std::cos(dir);
            c.vy = sp * std::sin(dir);
        }
        c.speed = std::max(-kReverseTop, c.speed);
        c.yaw = wrapAngle(c.yaw + c.yawRate * dt);
        Vec3 fwd{std::sin(c.yaw), 0, std::cos(c.yaw)}, right{fwd.z, 0, -fwd.x};
        c.pos = c.pos + (fwd * c.speed + right * c.vy) * dt;
    }

    // Put the car on the ground and tilt it to the slope under its wheels.
    void settle(Car& c) const {
        Vec3 f{std::sin(c.yaw) * 1.4f, 0, std::cos(c.yaw) * 1.4f};
        float front = terrain.height(c.pos.x + f.x, c.pos.z + f.z, c.idx), back = terrain.height(c.pos.x - f.x, c.pos.z - f.z, c.idx);
        c.pos.y = (front + back) / 2;
        c.pitch = -std::atan2(front - back, 2.8f);
    }

    // AI drives like a player on the keys (the drift assist is how anyone gets round fast), aiming a few samples ahead
    // on its line, braking for the corners it sees coming, and pulling out to pass a car in its way.
    Input autopilot(const Car& c) const {
        int n = track.n();
        float lane = c.aiLane, room = track.halfWidth - 1.5f;
        for (const Car& o : cars) {
            if (&o == &c) continue;
            int ahead = ((o.idx - c.idx) % n + n) % n;
            float side = track.offset(o.pos, o.idx);
            if (ahead >= 1 && ahead <= 5 && std::fabs(side - lane) < 2.4f)
                lane = side + (side > 0 ? -2.8f : 2.8f);                                // go round it on the wider side
        }
        lane = std::max(-room, std::min(room, lane));
        Vec3 target = track.at(c.idx + 5) + track.right(c.idx + 5) * lane;
        float travel = c.yaw + std::atan2(c.vy, std::max(c.speed, 1.f));
        float err = wrapAngle(std::atan2(target.x - c.pos.x, target.z - c.pos.z) - travel);
        // keep the pack together: a little more pace behind the player, a little less far ahead
        const Car& me = cars[0];
        float gap = float((me.lapsDone - c.lapsDone) * n + me.idx - c.idx);
        float top = &c == &me ? c.aiTop : std::min(kTopRoad + 1.5f, c.aiTop + std::max(-2.f, std::min(2.f, gap * 0.04f)));
        float vmaxSq = top * top;
        float cornerAccel = c.aiCorner * roadGrip() * kGravity;
        for (int k = 1; k < 30; ++k) {                                          // corners within braking distance
            float bend = std::acos(std::max(-1.f, std::min(1.f, dot(track.dir(c.idx + k), track.dir(c.idx + k + 3)))));
            float radius = 3 * track.spacing / std::max(bend, 1e-3f);
            vmaxSq = std::min(vmaxSq, cornerAccel * radius + 2 * 9.f * k * track.spacing);
        }
        float vmax = std::sqrt(vmaxSq);
        Input in;
        in.up = c.speed < vmax;
        in.down = c.speed > vmax + 2;
        in.right = err > 0.12f;
        in.left = err < -0.12f;
        return in;
    }

    // Coins and nitrous under the car; all of them return when it starts a new lap.
    void collect(Car& c, int lapsBefore) {
        if (c.lapsDone != lapsBefore) for (auto& p : pickups) p.taken = false;
        for (auto& p : pickups) {
            if (p.taken || std::hypot(p.pos.x - c.pos.x, p.pos.z - c.pos.z) > kCarRadius + 0.7f) continue;
            p.taken = true;
            if (p.nitro) c.nitro = std::min(1.f, c.nitro + kNitroTank);
            else ++c.coins;
        }
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

    static Vec3 velocity(const Car& c) {
        Vec3 f{std::sin(c.yaw), 0, std::cos(c.yaw)};
        return f * c.speed + Vec3{f.z, 0, -f.x} * c.vy;
    }
    static void addVelocity(Car& c, Vec3 dv) {
        Vec3 f{std::sin(c.yaw), 0, std::cos(c.yaw)};
        c.speed += dot(dv, f);
        c.vy += dot(dv, Vec3{f.z, 0, -f.x});
    }

    void collide() {
        for (auto& c : cars)
            for (auto& b : blocks) {                                  // oriented box: nearest point, then push out
                float cy = std::cos(b.yaw), sy = std::sin(b.yaw);
                Vec3 fwd{sy, 0, cy}, rt{cy, 0, -sy}, d = c.pos - b.pos;
                float lx = dot(d, rt), lz = dot(d, fwd);
                float ex = lx - std::max(-b.hx, std::min(b.hx, lx)), ez = lz - std::max(-b.hz, std::min(b.hz, lz));
                float dist = std::hypot(ex, ez);
                if (dist >= kCarRadius) continue;
                if (dist < 1e-4f) {                                   // centre inside: leave by the nearest face
                    if (b.hx - std::fabs(lx) < b.hz - std::fabs(lz)) { ex = lx >= 0 ? 1.f : -1.f; ez = 0; }
                    else { ex = 0; ez = lz >= 0 ? 1.f : -1.f; }
                    dist = 0;
                    c.pos = b.pos + rt * (ex ? ex * b.hx : lx) + fwd * (ez ? ez * b.hz : lz);
                } else { ex /= dist; ez /= dist; }
                Vec3 n = rt * ex + fwd * ez;
                c.pos = c.pos + n * (kCarRadius - dist);
                float into = dot(velocity(c), n);
                if (into < 0) addVelocity(c, n * (-1.3f * into));     // bounce off with restitution 0.3
                c.yawRate *= 0.7f;
            }
        for (auto& c : cars)
            for (auto& t : trees) {
                Vec3 d = c.pos - t;
                d.y = 0;
                float dist = length(d), min = kCarRadius + kTreeRadius;
                if (dist >= min || dist < 1e-4f) continue;
                c.pos = t + d * (min / dist);
                c.speed *= -0.3f;                                     // bounce off the trunk
                c.vy *= 0.3f;
                c.yawRate *= 0.5f;
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
                Vec3 n = d * (1 / dist);                              // equal masses: impulse along the contact normal
                float closing = dot(velocity(cars[i]) - velocity(cars[j]), n);
                if (closing > 0) {
                    Vec3 j2 = n * (0.65f * closing);                  // (1 + restitution 0.3) / 2
                    addVelocity(cars[i], j2 * -1);
                    addVelocity(cars[j], j2);
                }
            }
    }

    // A village on the north straight, shops by the start, towers on the east straight, a bridge over the river,
    // and rows of coins with a nitrous tank between every other row.
    void placeScenery() {
        auto roadside = [&](int i, float side, float setback, float hx, float hz, Block::Kind kind, int style) {
            Vec3 out = track.right(i) * side, p = track.at(i) + out * setback;
            p.y = Terrain::base(p.x, p.z);
            blocks.push_back({p, std::atan2(-out.x, -out.z), hx, hz, kind, style});
        };
        uint32_t s = 777;                                        // each house its own size; the front stays 9.5 off the centreline
        auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.f; };
        auto house = [&](int i, float side) { float hz = 2.6f + rnd(); roadside(i, side, 9.5f + hz, 2.9f + 1.4f * rnd(), hz, Block::House, 0); };
        for (int i = 124; i <= 148; i += 5)
            for (float side : {-1.f, 1.f}) house(i + (side > 0 ? 2 : 0), side);
        for (int i = 238; i <= 252; i += 5) house(i, 1);
        for (int i = 34; i <= 50; i += 6) { float hz = 3.5f + rnd(); roadside(i, -1, 10 + hz, 3.5f + rnd(), hz, Block::Tower, 0); }
        int lo = -1, hi = -1;                                    // the bridge: middle of the samples over the river
        for (int i = 0; i < track.n(); ++i)
            if (Terrain::wet(track.at(i).x, track.at(i).z) > 0.999f) { if (lo < 0) lo = i; hi = i; }
        bridge = (lo + hi) / 2;                                  // one crossing, checked by make test
        Vec3 d = track.dir(bridge);
        for (float side : {-1.f, 1.f}) {
            Vec3 p = track.at(bridge) + track.right(bridge) * (side * (track.halfWidth + track.kerb + 0.3f));
            p.y = Terrain::base(p.x, p.z);
            blocks.push_back({p, std::atan2(d.x, d.z), 0.3f, bridgeLen / 2, Block::Railing, 0});
        }
        for (int i = tunnelLo; i < tunnelHi; i += 2)             // tunnel walls, one box per two samples so they follow the bend
            for (float side : {-1.f, 1.f}) {
                int m = i + 1;
                Vec3 d = track.dir(m), p = track.at(m) + track.right(m) * (side * (track.halfWidth + track.kerb + 0.3f));
                p.y = Terrain::base(p.x, p.z);
                blocks.push_back({p, std::atan2(d.x, d.z), 0.3f, track.spacing + 0.15f, Block::Railing, 0});
            }
        for (int i = 12, g = 0; i + 24 < track.n(); i += 44, ++g) {   // a short row every ~130 units, room to see the road
            float lane = (g % 3 - 1) * 2.5f;
            for (int k = 0; k < 9; k += 3) {
                Vec3 p = track.at(i + k) + track.right(i + k) * lane;
                pickups.push_back({{p.x, Terrain::base(p.x, p.z) + 0.9f, p.z}, false, false});
            }
            if (g < 5) {                                         // a tank halfway to the next row (none on the grid)
                Vec3 p = track.at(i + 22) + track.right(i + 22) * (lane ? -lane : 2.5f);
                pickups.push_back({{p.x, Terrain::base(p.x, p.z) + 0.9f, p.z}, true, false});
            }
        }
    }

    void placeTrees() {
        uint32_t s = 12345;
        auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.f; };
        for (int tries = 0; trees.size() < 110 && tries < 5000; ++tries) {
            Vec3 p{(rnd() * 2 - 1) * 140, 0, (rnd() * 2 - 1) * 140};
            if (length(track.at(track.nearest(p, 0, -1)) - p) < track.halfWidth + track.kerb + 6) continue;
            if (Terrain::wet(p.x, p.z) > 0) continue;
            int near = track.nearest(p, 0, -1);
            if (near >= tunnelLo - 4 && near <= tunnelHi + 4 && length(track.at(near) - p) < 20) continue;   // the hill over the tunnel
            bool clear = true;
            for (auto& b : blocks) clear = clear && length(Vec3{b.pos.x - p.x, 0, b.pos.z - p.z}) > std::max(b.hx, b.hz) + 3;
            for (auto& t : trees) clear = clear && length(Vec3{t.x - p.x, 0, t.z - p.z}) > 6;
            p.y = terrain.height(p.x, p.z);
            if (clear) trees.push_back(p);
        }
    }
};
