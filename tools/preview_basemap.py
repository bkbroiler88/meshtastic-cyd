#!/usr/bin/env python3
"""Render a basemap .bin to SVG.

This is the reference decoder: if the firmware's reader and this one disagree, one of
them is wrong about the format. Verifying here costs seconds; verifying on-device costs
a 15-minute build.
"""

import argparse
import struct
import sys

LAYER_STYLE = {
    0: ("#3b6ea5", 1.6),  # water
    1: ("#111111", 1.7),  # major
    2: ("#444444", 1.0),  # minor
    3: ("#999999", 0.5),  # local
}
LAYER_NAME = {0: "water", 1: "major", 2: "minor", 3: "local"}


def read_section(buf, off, length):
    ways = []
    if length < 4:
        return ways
    (n,) = struct.unpack_from("<I", buf, off)
    p = off + 4
    for _ in range(n):
        layer, npts = struct.unpack_from("<BH", buf, p)
        p += 3
        pts = struct.unpack_from(f"<{npts * 2}h", buf, p)
        p += npts * 4
        ways.append((layer, list(zip(pts[0::2], pts[1::2]))))
    return ways


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--overview-only", action="store_true")
    ap.add_argument("--size", type=int, default=1000)
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        buf = f.read()

    magic, ver, grid, _pad = struct.unpack_from("<4sBBH", buf, 0)
    if magic != b"MBM1":
        raise SystemExit(f"bad magic {magic!r}")
    lat0_i, lon0_i = struct.unpack_from("<ii", buf, 8)
    half_m, cell_m = struct.unpack_from("<II", buf, 16)
    print(f"v{ver} grid={grid}x{grid} origin={lat0_i / 1e7:.5f},{lon0_i / 1e7:.5f} "
          f"half={half_m}m cell={cell_m}m")

    table_off = 24
    entries = 1 + grid * grid
    table = [struct.unpack_from("<II", buf, table_off + i * 8) for i in range(entries)]

    ways = read_section(buf, *table[0])
    print(f"overview: {len(ways)} ways")
    if not args.overview_only:
        for i, (off, ln) in enumerate(table[1:]):
            ways += read_section(buf, off, ln)
        print(f"total with detail: {len(ways)} ways")

    counts = {}
    for layer, _ in ways:
        counts[layer] = counts.get(layer, 0) + 1
    print("  " + ", ".join(f"{LAYER_NAME[k]}={v}" for k, v in sorted(counts.items())))

    S = args.size
    scale = S / (2.0 * half_m)

    def sx(x):
        return (x + half_m) * scale

    def sy(y):
        return S - (y + half_m) * scale  # north up

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" '
        f'viewBox="0 0 {S} {S}">',
        f'<rect width="{S}" height="{S}" fill="#f5f3ee"/>',
    ]
    # draw water first, then roads on top - same order as the renderer
    for want in (0, 3, 2, 1):
        colour, w = LAYER_STYLE[want]
        for layer, pts in ways:
            if layer != want:
                continue
            d = " ".join(
                ("M" if i == 0 else "L") + f"{sx(x):.1f},{sy(y):.1f}"
                for i, (x, y) in enumerate(pts)
            )
            out.append(f'<path d="{d}" fill="none" stroke="{colour}" stroke-width="{w}"/>')
    # centre marker - this is where the node sits, and where the map must be centred
    out.append(f'<circle cx="{S/2}" cy="{S/2}" r="6" fill="none" stroke="#c0392b" stroke-width="2.5"/>')
    out.append(f'<circle cx="{S/2}" cy="{S/2}" r="2" fill="#c0392b"/>')
    out.append("</svg>")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
