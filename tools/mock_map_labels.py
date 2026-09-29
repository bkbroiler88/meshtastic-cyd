#!/usr/bin/env python3
"""Render what the map frame would look like with street names, at device fidelity.

This is a mockup, not firmware, but it deliberately mirrors what BaseMap.cpp/MapRenderer.cpp
actually do so the result is predictive rather than decorative:

  * 240x320 portrait, the band between the header and the nav bar
  * 1-bit only - every pixel is on or off, exactly like the OLEDDisplay framebuffer
  * the device's own font (ArialMT_Plain_16, 19 px) decoded from OLEDDisplayFonts.cpp,
    so label widths and legibility are the real ones
  * the same layer rules: water dashed, roads solid, residential only below 8 m/px

The label engine here is the one proposed for the firmware: horizontal labels only (the
display library cannot rotate text), anchored to the longest on-screen run of a road,
knocked out of the background, collision-checked, capped, and gated by zoom.

Usage:
    python tools/mock_map_labels.py --lat 39.4187 --lon -76.2944 --out <dir>
"""

import argparse
import importlib.util
import math
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FONT_CPP = os.path.join(
    REPO, ".pio", "libdeps", "lcdwiki_2_8_e22_st7789",
    "ESP8266 and ESP32 OLED driver for SSD1306 displays", "src", "OLEDDisplayFonts.cpp")

# --- device geometry (variant.h + MapRenderer.cpp) -------------------------------------
SCR_W, SCR_H = 240, 320      # LCDWIKI_PORTRAIT
FONT_HEIGHT_SMALL = 19       # HAS_SPI_TFT promotes FONT_SMALL to ArialMT_Plain_16
NAV_H = 16
BAND_TOP = FONT_HEIGHT_SMALL + 1
BAND_BOTTOM = SCR_H - NAV_H
CX = SCR_W // 2
CY = (BAND_TOP + BAND_BOTTOM) // 2
HALF_SPAN_PX = (BAND_BOTTOM - BAND_TOP) // 2

LAYER_WATER, LAYER_MAJOR, LAYER_MINOR, LAYER_LOCAL = 0, 1, 2, 3

# --- label engine knobs (what the firmware would carry as constants) -------------------
LABELS_MAX = 6               # beyond this the 240 px width is unreadable
LABEL_ZOOM_MPERPX = 8.0      # no street names when a pixel is coarser than this
LOCAL_ZOOM_MPERPX = 3.0      # residential names only when really zoomed in
LABEL_PAD = 2                # knockout box padding
MIN_RUN_FACTOR = 0.8         # a road must show at least this much of the label's width
DEDUPE_PX = 110              # do not repeat the same name within this distance


# =======================================================================================
# device font
# =======================================================================================
class DeviceFont:
    """ThingPulse OLEDDisplay bitmap font: header, 4-byte jump table, column-major glyphs."""

    def __init__(self, name="ArialMT_Plain_16"):
        src = open(FONT_CPP, "r", encoding="utf-8", errors="replace").read()
        m = re.search(r"const uint8_t " + name + r"\[\] PROGMEM = \{(.*?)\n\};", src, re.S)
        if not m:
            raise SystemExit(f"{name} not found in {FONT_CPP}")
        body = re.sub(r"//[^\n]*", "", m.group(1))
        data = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        self.width, self.height, self.first, self.count = data[0], data[1], data[2], data[3]
        self.jump = data[4:4 + self.count * 4]
        self.data = data[4 + self.count * 4:]
        self.raster = 1 + ((self.height - 1) >> 3)

    def _entry(self, ch):
        code = ord(ch)
        if code < self.first or code >= self.first + self.count:
            return None
        j = (code - self.first) * 4
        msb, lsb, size, width = self.jump[j], self.jump[j + 1], self.jump[j + 2], self.jump[j + 3]
        if msb == 0xFF and lsb == 0xFF:      # empty glyph (e.g. space)
            return None, width
        return (msb << 8) | lsb, width, size

    def char_width(self, ch):
        e = self._entry(ch)
        if e is None:
            return 0
        return e[1]

    def text_width(self, text):
        return sum(self.char_width(c) for c in text)

    def draw(self, px, x, y, text, on=1):
        """Blit like OLEDDisplay::drawInternal - columns of `raster` bytes, LSB at top."""
        cur = x
        for ch in text:
            e = self._entry(ch)
            if e is None:
                continue
            if len(e) == 2:                   # blank glyph, just advance
                cur += e[1]
                continue
            off, w, size = e
            for i in range(size):
                col = i // self.raster
                byte = self.data[off + i]
                ybase = (i % self.raster) * 8
                for bit in range(8):
                    if byte & (1 << bit):
                        xx, yy = cur + col, y + ybase + bit
                        if 0 <= xx < SCR_W and 0 <= yy < SCR_H:
                            px[xx, yy] = on
            cur += w
        return cur - x


# =======================================================================================
# 1-bit canvas, mirroring the OLEDDisplay primitives the firmware uses
# =======================================================================================
class Canvas:
    def __init__(self):
        self.img = Image.new("1", (SCR_W, SCR_H), 0)
        self.px = self.img.load()

    def pixel(self, x, y, on=1):
        if 0 <= x < SCR_W and BAND_TOP <= y < BAND_BOTTOM:
            self.px[x, y] = on

    def line(self, x0, y0, x1, y1, dashed=False, clip_band=True):
        """Bresenham with the same 2-on/2-off dash BaseMap uses for water."""
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err, phase = dx + dy, 0
        while True:
            if not dashed or (phase & 3) < 2:
                if clip_band:
                    self.pixel(x0, y0)
                elif 0 <= x0 < SCR_W and 0 <= y0 < SCR_H:
                    self.px[x0, y0] = 1
            phase += 1
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def rect(self, x, y, w, h, fill=0):
        for yy in range(max(0, y), min(SCR_H, y + h)):
            for xx in range(max(0, x), min(SCR_W, x + w)):
                self.px[xx, yy] = fill

    def hline(self, x0, x1, y, on=1):
        for x in range(max(0, min(x0, x1)), min(SCR_W, max(x0, x1) + 1)):
            if 0 <= y < SCR_H:
                self.px[x, y] = on

    def vline(self, x, y0, y1, on=1):
        for y in range(max(0, min(y0, y1)), min(SCR_H, max(y0, y1) + 1)):
            if 0 <= x < SCR_W:
                self.px[x, y] = on

    def circle(self, cx, cy, r, filled=False):
        for a in range(0, 360, 4):
            x = int(round(cx + r * math.cos(math.radians(a))))
            y = int(round(cy + r * math.sin(math.radians(a))))
            self.pixel(x, y)
        if filled:
            for yy in range(cy - r, cy + r + 1):
                for xx in range(cx - r, cx + r + 1):
                    if (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                        self.pixel(xx, yy)

    def save(self, path, scale=3):
        out = self.img.convert("L").point(lambda v: 255 if v else 0)
        out = out.resize((SCR_W * scale, SCR_H * scale), Image.NEAREST)
        out.save(path)


# =======================================================================================
# data
# =======================================================================================
def load_prepare_module():
    spec = importlib.util.spec_from_file_location("prep", os.path.join(HERE, "prepare_basemap.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def project_named(elements, lat0, lon0, prep):
    """Like prepare_basemap.project_all but keeps the street name."""
    m_lat = 111132.0
    m_lon = 111320.0 * math.cos(math.radians(lat0))
    out = []
    for el in elements:
        if el.get("type") != "way" or "geometry" not in el:
            continue
        tags = el.get("tags", {})
        layer = prep.classify(tags)
        if layer is None:
            continue
        name = tags.get("name") or tags.get("ref") or ""
        pts = [((g["lon"] - lon0) * m_lon, (g["lat"] - lat0) * m_lat) for g in el["geometry"]]
        if len(pts) >= 2:
            out.append((layer, name, pts))
    return out


def stitch_named(ways):
    """prepare_basemap.stitch, but keyed on (layer, name).

    The shipped stitch() joins on endpoints alone. That is fine for unnamed geometry, but
    for labels it would weld two different streets into one chain and label it wrongly -
    this is the change the real tool needs.
    """
    from collections import defaultdict

    def key(p):
        return (round(p[0], 2), round(p[1], 2))

    groups = defaultdict(list)
    for layer, name, pts in ways:
        groups[(layer, name)].append(pts)

    out = []
    for (layer, name), lst in groups.items():
        ends = defaultdict(list)
        for i, pts in enumerate(lst):
            ends[key(pts[0])].append(i)
            ends[key(pts[-1])].append(i)
        used = [False] * len(lst)
        for i in range(len(lst)):
            if used[i]:
                continue
            used[i] = True
            chain = list(lst[i])
            for forward in (True, False):
                while True:
                    k = key(chain[-1] if forward else chain[0])
                    nxt = next((j for j in ends.get(k, []) if not used[j]), None)
                    if nxt is None:
                        break
                    used[nxt] = True
                    cand = lst[nxt]
                    if forward:
                        chain += (cand[1:] if key(cand[0]) == k else list(reversed(cand))[1:])
                    else:
                        chain = (cand[:-1] if key(cand[-1]) == k else list(reversed(cand))[:-1]) + chain
            out.append((layer, name, chain))
    return out


# =======================================================================================
# frame rendering
# =======================================================================================
def seg_clip(x0, y0, x1, y1):
    """Cohen-Sutherland against the map band, same as BaseMap::clipSeg."""
    def code(x, y):
        c = 0
        if x < 0:
            c |= 1
        elif x > SCR_W - 1:
            c |= 2
        if y < BAND_TOP:
            c |= 4
        elif y > BAND_BOTTOM - 1:
            c |= 8
        return c

    c0, c1 = code(x0, y0), code(x1, y1)
    for _ in range(8):
        if not (c0 | c1):
            return x0, y0, x1, y1
        if c0 & c1:
            return None
        c = c0 or c1
        dx, dy = x1 - x0, y1 - y0
        if c & 4:
            x, y = x0 + dx * (BAND_TOP - y0) / (dy or 1), BAND_TOP
        elif c & 8:
            x, y = x0 + dx * (BAND_BOTTOM - 1 - y0) / (dy or 1), BAND_BOTTOM - 1
        elif c & 2:
            x, y = SCR_W - 1, y0 + dy * (SCR_W - 1 - x0) / (dx or 1)
        else:
            x, y = 0, y0 + dy * (0 - x0) / (dx or 1)
        x, y = int(round(x)), int(round(y))
        if c == c0:
            x0, y0, c0 = x, y, code(x, y)
        else:
            x1, y1, c1 = x, y, code(x, y)
    return None


def render(ways, m_per_px, font, out_path, title, show_labels=True, nodes=(), cap=LABELS_MAX,
           collide=True):
    cv = Canvas()

    def to_screen(p):
        return (int(round(CX + p[0] / m_per_px)), int(round(CY - p[1] / m_per_px)))

    draw_local = m_per_px <= 8.0

    # --- geometry, water first so solid roads win where they cross ---
    candidates = []
    for want_water in (True, False):
        for layer, name, pts in ways:
            if (layer == LAYER_WATER) != want_water:
                continue
            if layer == LAYER_LOCAL and not draw_local:
                continue
            scr = [to_screen(p) for p in pts]
            # Accumulate the whole on-screen run, not the longest single segment: a road is
            # made of many short segments after simplification, so per-segment length badly
            # understates how much of the road the user can actually see.
            visible = []
            for i in range(len(scr) - 1):
                (x0, y0), (x1, y1) = scr[i], scr[i + 1]
                c = seg_clip(x0, y0, x1, y1)
                if c is None:
                    continue
                cx0, cy0, cx1, cy1 = c
                cv.line(cx0, cy0, cx1, cy1, dashed=(layer == LAYER_WATER))
                visible.append((cx0, cy0, cx1, cy1, math.hypot(cx1 - cx0, cy1 - cy0)))
            if name and visible and layer != LAYER_WATER:
                total = sum(v[4] for v in visible)
                # anchor at the halfway point along the visible run
                walked, anchor = 0.0, (visible[0][0], visible[0][1])
                for cx0, cy0, cx1, cy1, ln in visible:
                    if walked + ln >= total / 2 and ln > 0:
                        t = (total / 2 - walked) / ln
                        anchor = (int(cx0 + (cx1 - cx0) * t), int(cy0 + (cy1 - cy0) * t))
                        break
                    walked += ln
                candidates.append((layer, name, total, anchor))

    # --- own position, as MapRenderer draws it ---
    cv.circle(CX, CY, 6)
    cv.circle(CX, CY, 2, filled=True)

    placed = []   # occupied rectangles: labels, node names, scale bar
    # Our own marker must be reserved too, or a knockout box erases the one thing the
    # user is looking for.
    placed.append((CX - 8, CY - 8, 16, 16))

    def collides(r):
        ax, ay, aw, ah = r
        for bx, by, bw, bh in placed:
            if ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah:
                return True
        return False

    # --- example node markers (illustrative positions, not a real mesh) ---
    for nx_m, ny_m, short in nodes:
        x, y = to_screen((nx_m, ny_m))
        if not (0 <= x < SCR_W and BAND_TOP <= y < BAND_BOTTOM):
            continue
        cv.circle(x, y, 3, filled=True)
        w = font.text_width(short)
        r = (x + 6, y - FONT_HEIGHT_SMALL // 2, w + 2, FONT_HEIGHT_SMALL)
        font.draw(cv.px, r[0], r[1], short)
        placed.append(r)

    # --- scale bar (bottom-left), same rules as drawScaleBar ---
    steps = [100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000]
    chosen = steps[0]
    for s in steps:
        px = s / m_per_px
        if 40 <= px <= 90:
            chosen = s
            break
        if px < 40:
            chosen = s
    bar = int(round(chosen / m_per_px))
    by = BAND_BOTTOM - 8
    cv.hline(6, 6 + bar, by)
    cv.vline(6, by - 3, by + 1)
    cv.vline(6 + bar, by - 3, by + 1)
    lbl = f"{int(chosen/1000)}km" if chosen >= 1000 else f"{int(chosen)}m"
    font.draw(cv.px, 6 + bar + 4, by - FONT_HEIGHT_SMALL + 2, lbl)
    placed.append((6, by - FONT_HEIGHT_SMALL, bar + 8 + font.text_width(lbl), FONT_HEIGHT_SMALL + 6))

    # --- street labels ---
    drawn = 0
    if show_labels and m_per_px <= LABEL_ZOOM_MPERPX:
        candidates.sort(key=lambda c: (c[0], -c[2]))
        used_names = []
        for layer, name, run, (mx, my) in candidates:
            if drawn >= cap:
                break
            if layer == LAYER_LOCAL and m_per_px > LOCAL_ZOOM_MPERPX:
                continue
            w = font.text_width(name)
            if run < w * MIN_RUN_FACTOR:
                continue                      # not enough road on screen to own the name
            if any(n == name and math.hypot(mx - px_, my - py_) < DEDUPE_PX
                   for n, px_, py_ in used_names):
                continue
            x = min(max(0, mx - w // 2), SCR_W - w - 1)
            y = min(max(BAND_TOP, my - FONT_HEIGHT_SMALL // 2), BAND_BOTTOM - FONT_HEIGHT_SMALL)
            box = (x - LABEL_PAD, y, w + 2 * LABEL_PAD, FONT_HEIGHT_SMALL)
            if collide and collides(box):
                continue
            cv.rect(*box, fill=0)             # knock out the geometry behind the text
            font.draw(cv.px, x, y, name)
            placed.append(box)
            used_names.append((name, mx, my))
            drawn += 1

    # --- header, approximating drawCommonHeader ---
    cv.rect(0, 0, SCR_W, BAND_TOP, fill=0)
    font.draw(cv.px, 2, 0, title)
    cv.hline(0, SCR_W - 1, BAND_TOP - 1)
    # --- nav bar strip ---
    cv.rect(0, BAND_BOTTOM, SCR_W, NAV_H, fill=0)
    cv.hline(0, SCR_W - 1, BAND_BOTTOM)

    cv.save(out_path)
    return drawn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, default=39.4187)
    ap.add_argument("--lon", type=float, default=-76.2944)
    ap.add_argument("--km", type=float, default=12.0)
    ap.add_argument("--cache", default=None)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    prep = load_prepare_module()
    font = DeviceFont()
    sys.stderr.write(f"font: {font.width}x{font.height}, {font.count} chars\n")

    half_km = args.km / 2.0
    dlat = half_km / 111.132
    dlon = half_km / (111.320 * math.cos(math.radians(args.lat)))
    cache = args.cache or os.path.join(args.out, "osm_named.json")
    import json
    if os.path.exists(cache):
        data = json.load(open(cache, encoding="utf-8"))
        sys.stderr.write(f"reusing {cache}\n")
    else:
        q = prep.build_query(args.lat - dlat, args.lon - dlon, args.lat + dlat, args.lon + dlon, True)
        data = prep.fetch(q)
        json.dump(data, open(cache, "w", encoding="utf-8"))

    raw = project_named(data.get("elements", []), args.lat, args.lon, prep)
    sys.stderr.write(f"{len(raw)} ways\n")
    ways = stitch_named(raw)
    named = sum(1 for _, n, _ in ways if n)
    sys.stderr.write(f"{len(ways)} after name-aware stitching, {named} named\n")

    # simplify at the detail tolerance the packer uses
    out = []
    for layer, name, pts in ways:
        s = prep.simplify(pts, prep.TOLERANCE_DETAIL[layer])
        if len(s) >= 2:
            out.append((layer, name, s))
    ways = out

    # illustrative mesh nodes (metres east/north of us) - NOT a real mesh
    nodes = [(-150.0, 190.0, "HAWK"), (210.0, -120.0, "BR2")]

    frames = [
        # name,           range m, title,         labels, cap,        collision-check
        ("labels_250m", 250.0, "MAP  NO FIX", True, LABELS_MAX, True),
        ("labels_600m", 600.0, "MAP  NO FIX", True, LABELS_MAX, True),
        ("labels_1000m", 1000.0, "MAP  NO FIX", True, LABELS_MAX, True),
        ("plain_600m", 600.0, "MAP  NO FIX", False, LABELS_MAX, True),
        ("labels_4000m", 4000.0, "MAP  NO FIX", True, LABELS_MAX, True),
        # what it looks like WITHOUT collision rejection - the reason that rule exists
        ("nocollide_250m", 250.0, "MAP  NO FIX", True, 14, False),
    ]
    for name, rng, title, labels, cap, collide in frames:
        mpp = rng / HALF_SPAN_PX
        p = os.path.join(args.out, name + ".png")
        n = render(ways, mpp, font, p, title, show_labels=labels, nodes=nodes, cap=cap,
                   collide=collide)
        print(f"{name}: range {rng:>6.0f} m, {mpp:5.2f} m/px, {n} labels -> {os.path.basename(p)}")


if __name__ == "__main__":
    main()
