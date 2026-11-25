# luckfox_car_game

A textured 3D rally game for the Luckfox Pico Mini B and an ILI9341 display, built on [luckfox_tft_ili9341](https://github.com/pahasara2003/luckfox_tft_ili9341) (git submodule in `lib/ili9341`).

Clone with `git clone --recurse-submodules`.

Race two AI cars over 3 laps of a gravel road through rolling hills, with pine trees, a mountain backdrop and a start gantry. The road climbs and dips (up to a 14% grade): cars tilt with the slope, slow down uphill and speed up downhill. The HUD shows lap, race time, best lap, position, speed and a minimap. Leaving the road slows you down; trees and other cars get in your way. You drive with your PC's keyboard over the USB `adb` link.

## Build and play

Wire the display as in the [library README](lib/ili9341/README.md#wiring-luckfox-pico-mini-b-example), connect the board over USB, then:

```sh
make deploy                # cross-compile with the Luckfox SDK toolchain, push to /root/cargame and the car models to /root/assets/cars
python3 tools/drive.py     # prepare the board, start the game, open the controller window
```

Keep the controller window focused: tap **Up/W** once and the car keeps driving, **Down/S** brakes (and stops the auto-throttle), **Left/Right** steer, **R** restarts, **Esc** quits. Add `flip` (`python3 tools/drive.py flip`) if the picture is upside down.

`make deploy` expects the SDK at `~/luckfox-pico`; pass `CROSS=/path/to/arm-rockchip830-linux-uclibcgnueabihf-g++` otherwise.

**Board with the ESP32 Wi-Fi image:** `drive.py` hands SPI0 from the Wi-Fi driver to `spidev` and holds the ESP32 in reset, because the ESP32 shares the display's D/C and backlight pins (see the library README). Wi-Fi is off until the next reboot.

## Other modes

```sh
make cargame                         # host build of the same game
./cargame ppm 14 a > frame.ppm       # simulate 14 s on autopilot and save that frame (keys: u d l r, a = autopilot)
./cargame bench 200                  # render-only timing
adb shell /root/cargame demo         # on the board: attract mode, no controller needed
make test                            # engine and game self-checks on the host
```

## Layout

| File | What it is |
| --- | --- |
| `src/engine3d.h` | Software rasterizer: camera, near-plane clipping, perspective-correct textures, 1/z depth buffer, flat lighting, fog, spot and rim lights |
| `src/objload.h` | OBJ/MTL loader with colours and PPM textures (`assimp export` and `magick` convert other formats) |
| `src/game.h` | Track, terrain heights, car physics, AI, laps and positions; no rendering |
| `src/cargame.cpp` | Textures, meshes, HUD, display thread and input for the game |
| `src/demo3d.cpp` | Model viewer demo with spot lights (`demo3d FRAMES display MODEL.obj`) |
| `tools/drive.py` | PC keyboard controller |
| `assets/cars/` | Car models from Kenney's [Car Kit](https://kenney.nl/assets/car-kit) (CC0), palette texture converted to PPM |

## Performance

On the Pico Mini B (single Cortex-A7, 1.1 GHz) the game runs at about 24 fps. A frame takes about 22 ms to render on its own (`cargame bench`), and about 28 ms while the display thread shares the CPU core. Sending 320 × 240 pixels over SPI takes longer than rendering, so the display sets the pace. Frames are sent on a second thread while the next one renders.

The 24 fps needs the `pushImage` change in `lib/ili9341` that packs pixels into 4 KB SPI writes. With the original one-write-per-row code the game runs at about 17 fps.

## Credits

Car models: [Kenney](https://kenney.nl) Car Kit, CC0 (`assets/cars/LICENSE-Kenney-Car-Kit.txt`).
