#!/usr/bin/env python3
"""Write docs/pinmap.drawio: Fritzing-style wiring of the Luckfox Pico Mini B, ILI9341 and 3 buttons (pins from README; pad order per user: 11 a side)."""
from xml.sax.saxutils import escape

def esc(s):
    return escape(s, {"\"": "&quot;", "\n": "&lt;br&gt;"})

cells, n = [], [0]

def nid():
    n[0] += 1
    return f"c{n[0]}"

def box(x, y, w, h, style, label=""):
    cells.append(f'<mxCell id="{nid()}" value="{esc(label)}" style="{style}" vertex="1" parent="1">'
                 f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def wire(pts, color, label=""):
    (sx, sy), (tx, ty), mid = pts[0], pts[-1], pts[1:-1]
    p = "".join(f'<mxPoint x="{x}" y="{y}"/>' for x, y in mid)
    for w, c in ((8, "#333333"), (6, color)):  # dark outline + coloured core, like Fritzing wires
        cells.append(f'<mxCell id="{nid()}" value="{esc(label) if w == 6 else ""}" style="endArrow=none;html=1;rounded=1;arcSize=30;'
                     f'strokeWidth={w};strokeColor={c};fontSize=11;fontStyle=1;labelBackgroundColor=#ffffff;" edge="1" parent="1">'
                     f'<mxGeometry relative="1" as="geometry"><mxPoint x="{sx}" y="{sy}" as="sourcePoint"/>'
                     f'<mxPoint x="{tx}" y="{ty}" as="targetPoint"/><Array as="points">{p}</Array></mxGeometry></mxCell>')

PAD = "ellipse;fillColor=#d4af37;strokeColor=#8a6d1d;strokeWidth=2;"
HOLE = "ellipse;fillColor=#222222;strokeColor=none;"
TXT = "text;html=1;fontColor={};fontSize={};fontStyle={};align={};verticalAlign=middle;"

# ---- Luckfox Pico Mini B ----
BX, BY, BW, PITCH, TOP = 560, 120, 220, 40, 160
box(BX, BY, BW, 11 * PITCH + 60, "rounded=1;arcSize=6;fillColor=#1e2a24;strokeColor=#0b0f0d;strokeWidth=3;shadow=1;")
box(BX + 80, BY - 18, 60, 34, "rounded=1;arcSize=30;fillColor=#c0c0c0;strokeColor=#777777;strokeWidth=2;", "USB-C")
box(BX + 40, BY + 40, 140, 30, TXT.format("#ffffff", 15, 1, "center"), "Luckfox Pico Mini B")

USED_L = {2: "GND", 3: "3V3", 7: "SPI0_CLK", 8: "SPI0_MOSI", 9: "SPI0_MISO", 10: "GPIO1_C4", 11: "GPIO1_C5"}
USED_R = {13: "GPIO1_D1", 14: "GPIO1_D2", 15: "GPIO1_D3", 16: "GPIO1_C6", 17: "GPIO1_C7", 21: "GND"}

def pad_xy(p):  # 11 pads a side: 1-11 down the left, 12-22 up the right
    return (BX, TOP + (p - 1) * PITCH) if p <= 11 else (BX + BW, TOP + (22 - p) * PITCH)

for p in range(1, 23):
    x, y = pad_xy(p)
    box(x - 11, y - 11, 22, 22, PAD); box(x - 5, y - 5, 10, 10, HOLE)
    used = USED_L.get(p) or USED_R.get(p)
    col = "#ffffff" if used else "#6f7f77"
    if p <= 11:
        box(x + 16, y - 10, 90, 20, TXT.format(col, 11, 1 if used else 0, "left"), f"{p}  {used or ''}")
    else:
        box(x - 106, y - 10, 90, 20, TXT.format(col, 11, 1 if used else 0, "right"), f"{used or ''}  {p}")

# ---- ILI9341 2.4" module (header on its right edge) ----
DX, DY = 40, 110
box(DX, DY, 330, 420, "rounded=1;arcSize=4;fillColor=#b71c1c;strokeColor=#5d0e0e;strokeWidth=3;shadow=1;")
box(DX + 20, DY + 30, 230, 330, "rounded=0;fillColor=#101418;strokeColor=#000000;strokeWidth=2;")
box(DX + 20, DY + 170, 230, 40, TXT.format("#8fa3b8", 14, 1, "center"), '2.4" ILI9341\n240×320 SPI')
box(DX + 20, DY + 375, 230, 30, TXT.format("#ffffff", 12, 1, "center"), "TFT module")

DPINS = [("VCC", 3, "#e53935"), ("GND", 2, "#212121"), ("CS", 13, "#fb8c00"), ("RESET", 16, "#eeeeee"),
         ("DC/RS", 10, "#43a047"), ("SDI/MOSI", 8, "#1e88e5"), ("SCK", 7, "#fdd835"), ("LED", 11, "#ec407a"),
         ("SDO/MISO", 9, "#8e24aa")]
HX = DX + 330
for k, (name, pad, color) in enumerate(DPINS):
    y = TOP + k * PITCH
    box(HX - 14, y - 9, 18, 18, "rounded=0;fillColor=#222222;strokeColor=#000000;")
    box(HX - 9, y - 4, 8, 8, "rounded=0;fillColor=#d4af37;strokeColor=none;")
    box(HX - 95, y - 10, 78, 20, TXT.format("#ffffff", 11, 1, "right"), name)
    px, py = pad_xy(pad)
    if pad <= 11:  # left side: one vertical lane per wire between module and board
        lane = HX + 30 + k * 16
        wire([(HX, y), (lane, y), (lane, py), (px, py)], color)
    else:  # CS / RESET: under the board and up the right edge
        under = BY + 12 * PITCH + 90 + (k - 2) * 18
        lane = BX + BW + 24 + (k - 2) * 18
        wire([(HX, y), (HX + 16 + (k - 2) * 14, y), (HX + 16 + (k - 2) * 14, under), (lane, under), (lane, py), (px, py)], color)

# ---- 3 tactile buttons to GND, each with a 10k pull-up to 3V3 ----
GX, RAIL, BX3 = 1210, 940, 1060
v3x, v3y = pad_xy(3)
wire([(v3x, v3y), (v3x - 22, v3y), (v3x - 22, 95), (RAIL, 95), (RAIL, 650)], "#e53935", "3V3")
for label, pad, by, color, lane in (("LEFT", 17, 380, "#00acc1", 860), ("NITROUS", 14, 560, "#f4511e", 880),
                                    ("RIGHT", 15, 700, "#7cb342", 900)):
    px, py = pad_xy(pad)
    wire([(px, py), (lane, py), (lane, by), (BX3, by)], color)
    wire([(BX3 + 60, by), (GX, by)], "#212121")
    jx, ry = BX3 - 30, by - 50
    wire([(RAIL, ry), (jx, ry), (jx, by)], "#e53935")
    box(RAIL + 15, ry - 9, 64, 18, "rounded=1;arcSize=50;fillColor=#e3c48f;strokeColor=#8a6d3b;strokeWidth=1;")
    for i, band in enumerate(("#6d3b14", "#111111", "#fb8c00", "#c9a227")):  # brown black orange gold = 10k 5%
        box(RAIL + 25 + i * 11 + (6 if i == 3 else 0), ry - 9, 5, 18, f"rounded=0;fillColor={band};strokeColor=none;")
    box(RAIL + 10, ry - 30, 74, 18, TXT.format("#222222", 11, 1, "center"), "10 kΩ")
    box(jx - 6, by - 6, 12, 12, "ellipse;fillColor=#222222;strokeColor=none;")
    box(BX3, by - 30, 60, 60, "rounded=1;arcSize=10;fillColor=#2b2b2b;strokeColor=#000000;strokeWidth=2;shadow=1;")
    box(BX3 + 14, by - 16, 32, 32, "ellipse;fillColor=#5a5a5a;strokeColor=#111111;strokeWidth=2;")
    box(BX3 - 10, by + 32, 80, 20, TXT.format("#222222", 12, 1, "center"), label)
for x, y in ((RAIL, 330), (RAIL, 510)):  # tee dots on the 3V3 rail
    box(x - 6, y - 6, 12, 12, "ellipse;fillColor=#222222;strokeColor=none;")
gy = pad_xy(21)[1]
wire([(pad_xy(21)[0], gy), (GX, gy), (GX, 700)], "#212121", "GND")

# ---- title + notes ----
box(40, 20, 1100, 40, TXT.format("#222222", 22, 1, "left"), "Luckfox car game: wiring")
box(40, 720, 500, 90, TXT.format("#444444", 12, 0, "left"),
    "Pad numbers are the Pico Mini B side pads, 1–22 (11 a side).\nButtons to GND, 10 kΩ pull-ups to 3V3: pressed = low.\n"
    "Backlight (LED) on pad 11, not straight on 3V3.\nESP32 removed: SPI0 is free for the display.")

xml = ('<mxfile host="pinmap.py"><diagram name="Pin map"><mxGraphModel dx="1200" dy="900" grid="1" gridSize="10" '
       'background="#f4f1ea" pageWidth="1280" pageHeight="840"><root><mxCell id="0"/><mxCell id="1" parent="0"/>'
       + "".join(cells) + "</root></mxGraphModel></diagram></mxfile>\n")
open("docs/pinmap.drawio", "w").write(xml)
print("wrote docs/pinmap.drawio")
