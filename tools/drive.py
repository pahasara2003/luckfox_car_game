#!/usr/bin/env python3
"""Keyboard controller for the Luckfox car game.

Prepares the board, starts the game over adb and opens a small window. While the window has focus:
arrows or WASD drive, Space (or Shift) fires the nitrous, R restarts, Esc or Q quits.
T cycles night, morning, day and golden hour. C cycles sunny, rain and snow.
G switches between gravel and asphalt. Extra arguments go to the game (e.g. `flip`).

`drive.py sim` needs no board: it runs the host build (`make cargame`) and shows the frames it would send
to the panel, decoded from the same big-endian RGB565 bytes, at 2x size in the window.

A terminal can only report key presses, not releases, so the keys are read here (Tk) and each change
is sent to the game's stdin as one byte: '@' + bits (1 up, 2 down, 4 left, 8 right, 16 nitrous).
"""
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk

GAME = "/root/cargame"
W, H = 320, 240
HOST_GAME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cargame")

# RGB565 pixel -> 3 PPM bytes. Indexed by the pixel's two bytes read little-endian, because the game sends
# them big-endian (high byte first, as on the SPI wire) and memoryview.cast("H") reads native little-endian.
LUT = [bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
       for v in ((i >> 8 | i << 8) & 0xFFFF for i in range(65536))]

# The board's ESP32 Wi-Fi image binds SPI0 to its own driver and shares the display's D/C and backlight
# pins with the ESP32; with the ESP32 running, the panel stays blank. So: stop the Wi-Fi driver, hand
# SPI0 to spidev, hold the ESP32 in reset, turn on the backlight and pulse the display reset.
# Nothing here is permanent: rebooting the board brings Wi-Fi back.
SETUP = r"""
killall cargame demo3d 2>/dev/null
cd /sys/bus/spi
[ -e drivers/esp_spi/spi0.0 ] && echo spi0.0 > drivers/esp_spi/unbind
rmmod esp32_spi 2>/dev/null; rmmod cfg80211 2>/dev/null
if [ ! -e /dev/spidev0.0 ]; then
  echo spidev > devices/spi0.0/driver_override
  echo spi0.0 > drivers/spidev/bind
fi
for g in 53 54 56; do [ -d /sys/class/gpio/gpio$g ] || echo $g > /sys/class/gpio/export; done
echo low > /sys/class/gpio/gpio56/direction
echo high > /sys/class/gpio/gpio53/direction
echo low > /sys/class/gpio/gpio54/direction
sleep 0.1
echo 1 > /sys/class/gpio/gpio54/value
sleep 0.2
[ -e /dev/spidev0.0 ] && [ -x /root/cargame ] && [ -f /root/assets/cars/sedan-sports.obj ]
"""

BITS = {"Up": 1, "w": 1, "Down": 2, "s": 2, "Left": 4, "a": 4, "Right": 8, "d": 8,
        "space": 16, "Shift_L": 16, "Shift_R": 16}
CONDITION_KEYS = ("t", "c", "g")


def keyname(event):
    k = event.keysym
    return k if len(k) > 1 else k.lower()          # arrows/Escape as-is, letters ignore caps lock


def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
        return
    sim = sys.argv[1:2] == ["sim"]
    if sim:
        if not os.access(HOST_GAME, os.X_OK):
            sys.exit("no host build: run `make cargame` first")
        game = subprocess.Popen([HOST_GAME, *sys.argv[1:]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0)
        text_out = game.stderr                      # stdout carries the frames
    else:
        if subprocess.run(["adb", "shell", SETUP]).returncode != 0:
            sys.exit("board setup failed: is the board connected, and was `make deploy` run?")
        game = subprocess.Popen(["adb", "shell", GAME] + sys.argv[1:], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        text_out = game.stdout

    root = tk.Tk()
    root.title("Luckfox car game")
    tk.Label(root, text="Up: go   Down: brake   Left/Right: steer   R: restart\nT: time   C: climate   G: road   Esc: quit", padx=24, pady=12).pack()
    status = tk.StringVar(value="starting...")
    tk.Label(root, textvariable=status, font="TkFixedFont", padx=24, pady=12).pack()
    screen = tk.Label(root) if sim else None
    if screen:
        screen.pack()
    frame = [None]                                  # newest frame from the game, as PPM bytes

    held, pending_release, last_sent = set(), {}, [None]

    def send(data):
        try:
            game.stdin.write(data)
            game.stdin.flush()
        except OSError:
            pass                                    # game already gone; the status line says so

    def sync():
        bits = 0
        for k in held:
            bits |= BITS.get(k, 0)
        if bits != last_sent[0]:
            last_sent[0] = bits
            send(bytes([0x40 | bits]))

    def release_now(k):
        pending_release.pop(k, None)
        held.discard(k)
        if k in BITS:
            sync()

    def on_press(event):
        k = keyname(event)
        if k in pending_release:                    # key auto-repeat sends release+press pairs: ignore them
            root.after_cancel(pending_release.pop(k))
        if k in BITS:
            held.add(k)
            sync()
        elif k == "r":
            send(b"r")
        elif k in CONDITION_KEYS and k not in held:
            held.add(k)
            send(k.encode())
        elif k in ("Escape", "q"):
            quit_game()

    def on_release(event):
        k = keyname(event)
        if (k in BITS or k in CONDITION_KEYS) and k not in pending_release:
            pending_release[k] = root.after(40, release_now, k)

    def on_focus_out(_):
        for job in pending_release.values():
            root.after_cancel(job)
        pending_release.clear()
        held.clear()                                # no stuck throttle when the window loses focus
        sync()

    lines = queue.Queue()

    def reader():                                   # game's status lines, read off the Tk thread
        for raw in iter(text_out.readline, b""):
            lines.put(raw.decode(errors="replace").strip())
        lines.put("game exited")

    def frame_reader():                             # sim: whole frames off stdout, keep only the newest
        size = W * H * 2
        while True:
            data = b""
            while len(data) < size:
                chunk = game.stdout.read(size - len(data))
                if not chunk:
                    return
                data += chunk
            frame[0] = b"P6\n%d %d\n255\n" % (W, H) + b"".join(map(LUT.__getitem__, memoryview(data).cast("H")))

    def show_frame():
        if frame[0] is not None:
            img = tk.PhotoImage(data=frame[0], format="PPM").zoom(2)
            screen.configure(image=img)
            screen.image = img                      # keep a reference or Tk drops the image
            frame[0] = None
        root.after(15, show_frame)

    def show_status():
        while not lines.empty():
            text = lines.get()
            if text and text != "READY":
                status.set(text)
        root.after(200, show_status)

    def quit_game():
        send(b"q")
        try:
            game.wait(timeout=3)
        except subprocess.TimeoutExpired:
            game.kill()
        root.destroy()

    threading.Thread(target=reader, daemon=True).start()
    if sim:
        threading.Thread(target=frame_reader, daemon=True).start()
        show_frame()
    root.bind("<KeyPress>", on_press)
    root.bind("<KeyRelease>", on_release)
    root.bind("<FocusOut>", on_focus_out)
    root.protocol("WM_DELETE_WINDOW", quit_game)
    show_status()
    root.focus_force()
    root.mainloop()


if __name__ == "__main__":
    main()
