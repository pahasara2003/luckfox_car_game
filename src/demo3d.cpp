// usage: demo3d [frames] [display|nodisplay|ppm|keys] [model.obj]
// keys: run as `adb shell -t /tmp/demo3d 0 keys /tmp/teapot.obj`; arrows turn car / model, +/- zoom, q quits.
// Cinematic still: a model resting on a box car, warm key spotlight, cool backlight + rim, letterboxed.
// "ppm" renders one frame to stdout (for previewing on a PC).
#include "board_config.h"
#include "ILI9341.h"
#include "objload.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <exception>
#include <memory>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

// Flat floor made of 1x1 tiles at height y: tiles under/behind the camera get dropped (no near clipping), the rest stay.
static Mesh tiles(int x0, int x1, int z0, int z1, float y, uint16_t color) {
    Mesh m;
    m.rim = false;
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x) {
            int i = int(m.v.size());
            m.v.push_back({float(x), y, float(z)}); m.v.push_back({float(x), y, float(z + 1)});
            m.v.push_back({float(x + 1), y, float(z + 1)}); m.v.push_back({float(x + 1), y, float(z)});
            m.t.push_back({i, i + 1, i + 2, color, nullptr, {}}); m.t.push_back({i, i + 2, i + 3, color, nullptr, {}});
        }
    return m;
}

int main(int argc, char** argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    const char* mode = argc > 2 ? argv[2] : "display";
    bool keys = !std::strcmp(mode, "keys");
    bool ppm = !std::strcmp(mode, "ppm"), headless = ppm || !std::strcmp(mode, "nodisplay");
    try {
        const int W = 320, H = 240, PH = 160, BAR = (H - PH) / 2;   // 2:1 picture between black bars
        std::unique_ptr<ILI9341> tft;
        if (!headless) {
            tft.reset(new ILI9341(board::spi, board::gpiochip, board::dc, board::reset, board::hz, board::cs));
            tft->init();
            tft->setRotation((board::rotation + 1) % 4);             // landscape
            tft->fillScreen(0x0000);
        }
        Renderer r(W, PH, 40);                                       // longer lens
        r.cam = {0, -0.45f, -1.4f};
        r.ambient = 0.06f; r.sun = 0.08f;
        Vec3 subject{0, -0.3f, 2};
        SpotLight key;                                               // warm, front-left, above
        key.pos = {-2.2f, 2.2f, 0.3f}; key.dir = normalize(subject - key.pos);
        key.cosInner = 0.985f; key.cosOuter = 0.94f; key.intensity = 1.6f; key.color = {1, 0.82f, 0.55f};
        SpotLight back;                                              // cool, behind-right, above
        back.pos = {3, 2.2f, 4}; back.dir = normalize(subject - back.pos);
        back.cosInner = 0.99f; back.cosOuter = 0.965f; back.intensity = 0.8f; back.color = {0.35f, 0.55f, 1};
        r.spots = {key, back};
        r.rim.pos = back.pos; r.rim.color = back.color; r.rim.strength = 2.2f;

        Mesh floor = tiles(-10, 10, -8, 10, -1.4f, 0x8410), body = box(1.2f, 0.4f, 2.2f, 0xF800), cab = box(1.0f, 0.4f, 1.0f, 0x07FF);
        Mesh model = argc > 3 ? loadObj(argv[3], 1.3f, 0xFD20) : box(0.6f, 0.6f, 0.6f, 0xFD20);
        float minY = 0; for (auto& v : model.v) minY = std::min(minY, v.y);
        Transform tf;
        Transform tb; tb.pos = {0, -1.2f, 2}; tb.yaw = 0.6f;         // car, three-quarter view
        Transform tc = tb; tc.pos.y = -0.8f;                         // roof box, top at y=-0.6
        Transform tm = tb; tm.pos.y = -0.6f - minY; tm.yaw = -0.5f;  // model resting on the roof

        float carYaw = tb.yaw, modelYaw = tm.yaw;
        auto render = [&] {
            tb.yaw = tc.yaw = carYaw; tm.yaw = carYaw + modelYaw;   // model rides on the car, plus its own spin
            r.clear(0x0000);
            r.draw(floor, tf); r.draw(body, tb); r.draw(cab, tc); r.draw(model, tm);
        };

        if (keys) {
            termios old{};
            bool tty = tcgetattr(0, &old) == 0;
            if (tty) { termios raw = old; raw.c_lflag &= ~(ICANON | ECHO); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &raw); }
            else std::fprintf(stderr, "stdin is not a terminal: run with adb shell -t\n");
            std::printf("arrows: left/right turn car, up/down spin model, +/- zoom, q quits\r\n"); std::fflush(stdout);
            const float step = 0.12f;
            render(); if (tft) tft->pushImage(0, BAR, W, PH, r.color.data());
            std::string seq;
            char ch;
            while (read(0, &ch, 1) == 1) {
                if (ch == 'q' || ch == 3) break;                      // q or Ctrl-C (raw mode keeps ^C as a byte only if ISIG is off)
                if (ch == '+' || ch == '=' || ch == '-' || ch == '_') {
                    // camera z kept in [-5, 0]: the car (up to 1.25 from z=2) stays in front of the near plane
                    r.cam.z = std::max(-5.f, std::min(0.f, r.cam.z + (ch == '-' || ch == '_' ? -0.3f : 0.3f)));
                    seq.clear();
                } else {
                    seq += ch;
                    if (seq[0] != 27) { seq.clear(); continue; }
                    if (seq.size() < 3) continue;
                    if (seq[1] == '[' || seq[1] == 'O') {
                        if (seq[2] == 'C') carYaw -= step;
                        else if (seq[2] == 'D') carYaw += step;
                        else if (seq[2] == 'A') modelYaw += step;
                        else if (seq[2] == 'B') modelYaw -= step;
                    }
                    seq.clear();
                }
                // skip already-queued repeats so holding a key doesn't lag behind
                pollfd pf{0, POLLIN, 0};
                if (poll(&pf, 1, 0) > 0) continue;
                render(); if (tft) tft->pushImage(0, BAR, W, PH, r.color.data());
            }
            if (tty) tcsetattr(0, TCSANOW, &old);
            return 0;
        }

        auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < frames; ++f) {
            render();
            if (ppm) {
                std::printf("P3 %d %d 31\n", W, H);
                for (int i = 0; i < W * H; ++i) {
                    int y = i / W;
                    uint16_t c = y < BAR || y >= BAR + PH ? 0 : r.color[i - BAR * W];
                    std::printf("%d %d %d\n", c >> 11, (c >> 5 & 63) / 2, c & 31);
                }
                return 0;
            }
            if (tft) tft->pushImage(0, BAR, W, PH, r.color.data());
            if (f % 60 == 59) {
                double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("frame %d: %.1f fps\n", f + 1, (f + 1) / s);
                std::fflush(stdout);
            }
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
