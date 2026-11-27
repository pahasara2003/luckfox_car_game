# AGENTS.md

How to work on this repo as an AI agent. Read `README.md` for what the game is and how to play it.

## Layout

- `src/engine3d.h`: software rasterizer.
  - Left-handed coordinates; yaw 0 faces +z.
  - Pitch is applied before yaw; nose down is positive pitch.
- `src/game.h`: track, terrain, physics, AI and laps. It has no rendering code, so it can be tested on its own.
- `src/cargame.cpp`: textures, meshes, sky, HUD, display thread and input.
- `src/objload.h`: OBJ/MTL loader. Textures must be PPM files.
- `src/test3d.cpp`: self-checks, run with `make test`.
- `tools/drive.py`: PC controller.
  - Runs a board setup script that frees SPI0 from the ESP32.
  - Sends each key change as one byte: `'@'` + bits (1 up, 2 down, 4 left, 8 right).
- `assets/cars/`: Kenney Car Kit models (CC0).
  - On the board they are loaded from `<exe dir>/assets/cars/`.
- `lib/ili9341`: display library (git submodule). Don't change it unless the task is about the display.

## Develop on the host first

```sh
make test                          # must pass before any deploy
make cargame
./cargame ppm 14 a > frame.ppm     # render frame at 14 s on autopilot; inspect the image
./cargame bench 200                # render time per frame
python3 tools/drive.py sim         # live playable simulator in a window (needs a desktop)
```

For a headless check of the frames sent to the display:
`(printf A; sleep 3; printf q) | ./cargame sim > frames.bin`.
- Each frame is 153600 bytes of big-endian RGB565, exactly the bytes `pushImage` sends over SPI.
- Status lines go to stderr.
- The sim caps at 24 fps, like the board.
- It doesn't model the board's slower CPU (use `bench` on the board for that) or panel wiring problems.

Check visual changes on a `ppm` frame, and look at it, before you put them on the board. To convert the frame: `magick frame.ppm frame.png`.

## Board

- **Deploy and run:** `make deploy`, then `python3 tools/drive.py`.
  - `drive.py` opens a GUI window and kills any `cargame` that is already running.
- **Toolchain:** the SDK must be at `~/luckfox-pico`.
- **Performance:**
  - The game runs at about 24 fps.
  - The SPI transfer to the display sets the pace; rendering takes about 22 ms per frame.
  - Keep `cargame bench` on the board under 30 ms, or the frame rate drops.
- **The CPU is a single Cortex-A7.**
  - Prefer flat colours and fog culling to extra triangles.
  - Measure before and after any change to the renderer.
- **Busy GPIO:** if you see "GPIO busy", or the timings jump, another `cargame` is still running. Ask the user before you kill it.

## Rules

- You cannot see the panel. Never claim a change "looks right" on the hardware. Say what you checked (tests, ppm frame, bench) and ask the user to confirm.
- **Terrain:**
  - Hills must stay outside `Terrain::kFlat` from the road.
  - `make test` checks the grade and bank limits (under 15%).
  - When you move the track, update those tests too.
- **New assets:**
  - Must have a CC0 or similarly permissive licence.
  - Put the licence file in `assets/`.
  - Add a credit to the README.
  - Original designs only: don't copy existing games.
- **Keep the README in step with your changes:** controls, performance numbers and file layout.
- **Ask before** any destructive step: deleting files, killing processes on the board, rewriting git history.
- **Commit only when asked**, on a feature branch.
