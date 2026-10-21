#!/usr/bin/env python3
"""Build a vector basemap for the BaseUI map frame from OpenStreetMap.

Raster tiles do not fit this device: there is no SD card on the LCDWIKI variant, the
LittleFS partition is 1.09 MB, and the map frame zooms continuously from 100 m to 200 km
rather than in discrete tile steps. Polylines cost roughly a tenth as much, scale to any
zoom, and draw straight into the 1-bit framebuffer.

Output is a single .bin for LittleFS. Two detail levels:

  overview  heavily simplified, small enough to sit in RAM for the whole session and be
            drawn at any zoom without touching flash.
  detail    full resolution, split into a grid of cells so the renderer reads only the
            cells the current view actually overlaps.

Usage:
    python tools/prepare_basemap.py --lat 39.4187 --lon -76.2944 --km 24 \
        --out data/basemap.bin
"""

import argparse
import json
import math
import struct
import sys
import time
import urllib.error
import urllib.request

MAGIC = b"MBM1"
# v2 added street names: one global blob plus a 2-byte id on every way.
# v3 makes each section self-contained - its own name blob - and widens points to i32.
# That lifts both of v2's ceilings: a 65 km box (i16 metres from the file origin) and
# 64 KiB of names in total (u16 ids into one blob). Neither was a property of the board.
FORMAT_VERSION = 3

# A name id is the byte offset of its length-prefixed string inside the name blob, so a
# lookup is one seek and one read. That caps the blob at 64 KiB, which is far more than a
# city-sized box needs (Edgewood, 24 km: ~14 KiB).
NAME_NONE = 0xFFFF
NAME_MAX_BYTES = 47  # bounds the firmware's on-stack buffer; longest real name here is 33

OVERPASS_ENDPOINTS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]

# Layer ids, drawn in this order so roads land on top of water.
LAYER_WATER = 0
LAYER_MAJOR = 1
LAYER_MINOR = 2
LAYER_LOCAL = 3

# highway= values we keep, mapped to a layer. Anything else is dropped: at 1 bit and
# ~1 m/px at full zoom, driveways and footpaths are noise that costs real flash.
HIGHWAY_LAYER = {
    "motorway": LAYER_MAJOR,
    "motorway_link": LAYER_MAJOR,
    "trunk": LAYER_MAJOR,
    "trunk_link": LAYER_MAJOR,
    "primary": LAYER_MAJOR,
    "primary_link": LAYER_MAJOR,
    "secondary": LAYER_MINOR,
    "secondary_link": LAYER_MINOR,
    "tertiary": LAYER_MINOR,
    "tertiary_link": LAYER_MINOR,
    "residential": LAYER_LOCAL,
    "unclassified": LAYER_LOCAL,
}

# Simplification tolerance in metres, per layer, for each detail level. The detail level
# is sized for full zoom (~1 m/px); the overview only ever renders when the view is wide.
TOLERANCE_DETAIL = {
    LAYER_WATER: 8.0,
    LAYER_MAJOR: 4.0,
    LAYER_MINOR: 4.0,
    LAYER_LOCAL: 6.0,
}
# The overview is the one section held in RAM for the whole session, and this board runs
# with ~96 KB of free heap, so it is budgeted hard: water and major roads only, at a
# tolerance that is still sub-pixel when the whole 24 km box is on a 320 px screen
# (~75 m/px). Minor and local roads only ever appear from the cell-indexed detail.
TOLERANCE_OVERVIEW = {
    LAYER_WATER: 250.0,
    LAYER_MAJOR: 200.0,
    LAYER_MINOR: None,
    LAYER_LOCAL: None,
}


def build_query(south, west, north, east, include_local):
    bbox = f"{south:.6f},{west:.6f},{north:.6f},{east:.6f}"
    highway_values = [k for k, v in HIGHWAY_LAYER.items() if include_local or v != LAYER_LOCAL]
    highway_re = "|".join(highway_values)
    return f"""
[out:json][timeout:180];
(
  way["highway"~"^({highway_re})$"]({bbox});
  way["natural"="coastline"]({bbox});
  way["natural"="water"]({bbox});
  way["waterway"~"^(river|stream)$"]({bbox});
  way["landuse"="reservoir"]({bbox});
);
out body geom;
"""


def fetch(query, cache_path=None):
    """Query Overpass, optionally caching the raw response.

    A wide box is tens of megabytes and the public endpoints time out often enough that
    re-querying while tuning the packer is both slow and rude. The cache is keyed by the
    caller, not the query, so change the box and you must change the cache file.
    """
    if cache_path and os.path.exists(cache_path):
        sys.stderr.write(f"using cached response {cache_path}\n")
        with open(cache_path, "rb") as f:
            return json.loads(f.read())

    last_err = None
    for endpoint in OVERPASS_ENDPOINTS:
        for attempt in range(3):
            try:
                sys.stderr.write(f"querying {endpoint} (attempt {attempt + 1})...\n")
                req = urllib.request.Request(
                    endpoint,
                    data=query.encode("utf-8"),
                    headers={
                        "Content-Type": "application/x-www-form-urlencoded",
                        # Overpass asks that bulk clients identify themselves.
                        "User-Agent": "meshtastic-cyd-basemap/1.0 (github.com/bkbroiler88/meshtastic-cyd)",
                    },
                )
                with urllib.request.urlopen(req, timeout=300) as resp:
                    raw = resp.read()
                sys.stderr.write(f"  got {len(raw) / 1024:.0f} KiB\n")
                if cache_path:
                    with open(cache_path, "wb") as f:
                        f.write(raw)
                    sys.stderr.write(f"  cached to {cache_path}\n")
                return json.loads(raw)
            except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError, ValueError) as e:
                last_err = e
                sys.stderr.write(f"  failed: {e}\n")
                time.sleep(5 * (attempt + 1))
    raise SystemExit(f"all Overpass endpoints failed; last error: {last_err}")


def classify(tags):
    if "highway" in tags:
        return HIGHWAY_LAYER.get(tags["highway"])
    if tags.get("natural") in ("coastline", "water"):
        return LAYER_WATER
    if tags.get("waterway") in ("river", "stream"):
        return LAYER_WATER
    if tags.get("landuse") == "reservoir":
        return LAYER_WATER
    return None


def project_all(elements, lat0, lon0, name_min_len=0.0):
    """Equirectangular to metres east/north of the origin - the same projection the
    firmware uses, so points land exactly where the renderer expects them."""
    m_per_deg_lat = 111132.0
    m_per_deg_lon = 111320.0 * math.cos(math.radians(lat0))
    out = []
    for el in elements:
        if el.get("type") != "way" or "geometry" not in el:
            continue
        tags = el.get("tags", {})
        layer = classify(tags)
        if layer is None:
            continue
        # Only roads carry a name - water is never labelled, and leaving those names out
        # keeps the blob inside its 64 KiB addressing.
        name = road_label(tags, layer) if layer != LAYER_WATER else ""
        pts = [
            ((g["lon"] - lon0) * m_per_deg_lon, (g["lat"] - lat0) * m_per_deg_lat)
            for g in el["geometry"]
        ]
        if len(pts) < 2:
            continue
        # A wide box carries tens of thousands of distinct residential street names and the
        # blob is addressed by uint16, so it overruns its 64 KiB id space long before the
        # geometry becomes a problem. Short stubs are what fill it, and they are the names
        # least likely to be wanted: drop the name, keep the road drawn.
        if name_min_len > 0 and layer == LAYER_LOCAL:
            length = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
            if length < name_min_len:
                name = ""
        out.append((layer, name, pts))
    return out


def compact_ref(ref):
    """"I 95" -> "I-95", "US 40;US 1" -> "US-40". First ref only; a shield shows one number."""
    first = ref.split(";")[0].strip()
    return "-".join(first.split())


def road_label(tags, layer):
    """The label a driver would actually recognise, which is not always OSM's `name`.

    Big roads carry the number in `ref` and a ceremonial name in `name`: I-95 through
    Maryland is "John F. Kennedy Memorial Highway". Preferring `name` there is wrong twice
    over - nobody calls it that, and at ~35 characters it is wider than the 240 px screen,
    so the firmware's "road must show 0.8x the label width" rule can never place it and the
    motorway ends up with no label at all at any zoom. Take `ref` for the classes that have
    one, `name` for the streets that do not.

    It also makes stitch() work better: consecutive segments of an interstate share one ref
    but often differ in `name`, and stitch joins on (layer, name).
    """
    ref = (tags.get("ref") or "").strip()
    name = (tags.get("name") or "").strip()
    if layer == LAYER_MAJOR and ref:
        label = compact_ref(ref)
    else:
        label = name or (compact_ref(ref) if ref else "")
    return label[:NAME_MAX_BYTES]


def stitch(ways):
    """Join ways that share an endpoint back into continuous lines.

    OSM splits a single road into many short ways at every attribute change - a speed
    limit, a bridge, a name. Simplifying a 2-point fragment removes nothing, so without
    this the output is dominated by way headers and unremovable points. Chaining first
    lets Douglas-Peucker work over the whole road. Junctions are resolved greedily: one
    arm becomes the long chain, the others stay separate, which is fine for drawing.

    Grouping is keyed on (layer, name), not layer alone. Joining purely on shared endpoints
    would weld two differently-named streets into one chain at every junction, and the
    merged chain would then be labelled with whichever name won.
    """
    from collections import defaultdict

    def key(p):
        return (round(p[0], 2), round(p[1], 2))

    by_layer = defaultdict(list)
    for layer, name, pts in ways:
        by_layer[(layer, name)].append(pts)

    out = []
    for (layer, name), lst in by_layer.items():
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
                    nxt = None
                    for j in ends.get(k, []):
                        if not used[j]:
                            nxt = j
                            break
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


def _perp_dist(p, a, b):
    (px, py), (ax, ay), (bx, by) = p, a, b
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def simplify(pts, tol):
    """Iterative Douglas-Peucker. Recursion would blow the stack on coastlines."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi <= lo + 1:
            continue
        worst, worst_i = 0.0, -1
        for i in range(lo + 1, hi):
            d = _perp_dist(pts[i], pts[lo], pts[hi])
            if d > worst:
                worst, worst_i = d, i
        if worst > tol:
            keep[worst_i] = True
            stack.append((lo, worst_i))
            stack.append((worst_i, hi))
    return [p for p, k in zip(pts, keep) if k]


def clip_to_box(pts, half_m):
    """Split a way wherever it leaves the box, so nothing needs int16 beyond range."""
    runs, cur = [], []
    for x, y in pts:
        if -half_m <= x <= half_m and -half_m <= y <= half_m:
            cur.append((x, y))
        else:
            if len(cur) >= 2:
                runs.append(cur)
            cur = []
    if len(cur) >= 2:
        runs.append(cur)
    return runs


def build_name_blob(ways_lists):
    """Collect distinct names into one blob; return (blob, {name: id}).

    id is the byte offset of the name's length-prefixed record, so the firmware reads a
    name with a single seek. Names are emitted longest-first only to keep the hot ones
    early; ordering is otherwise irrelevant.
    """
    seen = set()
    for lst in ways_lists:
        for w in lst:
            if w[1]:
                seen.add(w[1])
    blob, ids = bytearray(), {}
    for name in sorted(seen):
        enc = name.encode("utf-8")[:NAME_MAX_BYTES]
        ids[name] = len(blob)
        blob += bytes([len(enc)]) + enc
    if len(blob) > NAME_NONE:
        raise SystemExit(f"name blob {len(blob)} bytes exceeds the {NAME_NONE}-byte id space")
    return bytes(blob), ids


def pack_ways(ways, name_ids):
    """v2: ways: list of (layer, name, pts). Returns packed bytes for one section."""
    buf = bytearray()
    buf += struct.pack("<I", len(ways))
    for layer, name, pts in ways:
        nid = name_ids.get(name, NAME_NONE) if name else NAME_NONE
        buf += struct.pack("<BHH", layer, len(pts), nid)
        for x, y in pts:
            buf += struct.pack("<hh", int(round(x)), int(round(y)))
    return bytes(buf)


def pack_section_v3(ways):
    """One self-contained section: its own names, then its ways.

        u32 nameLen
        <nameLen bytes>      length-prefixed strings
        u32 wayCount
        ways: u8 layer, u16 npts, u16 nameId, npts * (i32 x, i32 y)

    nameId is a byte offset inside THIS section's blob, which is why the 64 KiB name
    ceiling disappears: a cell holds a few hundred names, not a whole county's worth.
    Points are i32 metres from the file origin - v2's i16 capped a map at a 65 km box,
    and doubling the coordinate is the cheapest way to lift that. It costs file size on
    an SD card, which is the one resource here that is not scarce.
    """
    seen, blob, ids = set(), bytearray(), {}
    for _, name, _ in ways:
        if name and name not in seen:
            seen.add(name)
            enc = name.encode("utf-8")[:NAME_MAX_BYTES]
            ids[name] = len(blob)
            blob += bytes([len(enc)]) + enc

    buf = bytearray()
    buf += struct.pack("<I", len(blob))
    buf += blob
    buf += struct.pack("<I", len(ways))
    for layer, name, pts in ways:
        nid = ids.get(name, NAME_NONE) if name else NAME_NONE
        buf += struct.pack("<BHH", layer, len(pts), nid)
        for x, y in pts:
            buf += struct.pack("<ii", int(round(x)), int(round(y)))
    return bytes(buf)


# --- v4 map packs ------------------------------------------------------------------------
# One file per area, tiled on a fixed global lat/lon grid so packs for neighbouring areas
# meet seamlessly. Three detail levels, picked by the firmware from the zoom. Layout is
# documented in BaseMap.cpp next to the reader.
V4_SHIFTS = (1, 3, 5)          # tile = 2^-shift degrees: 0.5, 0.125, 0.03125
V4_UNITS = 16384               # tile-relative i16 coordinates, 0..16384 across a tile
V4_TOLERANCE = (               # metres, per level, per layer; None = layer not drawn at that level
    {LAYER_WATER: 120.0, LAYER_MAJOR: 80.0, LAYER_MINOR: None, LAYER_LOCAL: None},
    {LAYER_WATER: 25.0, LAYER_MAJOR: 12.0, LAYER_MINOR: 12.0, LAYER_LOCAL: None},
    {LAYER_WATER: 8.0, LAYER_MAJOR: 4.0, LAYER_MINOR: 4.0, LAYER_LOCAL: 6.0},
)
V4_MIN_LEN = (1500.0, 200.0, 0.0)  # drop shorter runs at each level: stubs are illegible zoomed out


def _clip_seg(x0, y0, x1, y1, xmin, ymin, xmax, ymax):
    """Liang-Barsky. Returns the clipped segment or None."""
    dx, dy = x1 - x0, y1 - y0
    t0, t1 = 0.0, 1.0
    for p_, q_ in ((-dx, x0 - xmin), (dx, xmax - x0), (-dy, y0 - ymin), (dy, ymax - y0)):
        if p_ == 0:
            if q_ < 0:
                return None
            continue
        r = q_ / p_
        if p_ < 0:
            if r > t1:
                return None
            t0 = max(t0, r)
        else:
            if r < t0:
                return None
            t1 = min(t1, r)
    return (x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy)


def split_into_tiles(lonlat, shift):
    """Cut one polyline (lon, lat) into per-tile runs, clipping exactly at tile edges.

    Returns {(ix, iy): [run, ...]} with runs in tile units (0..V4_UNITS)."""
    scale = float(1 << shift)
    out = {}
    last_end = {}  # tile -> last point appended, to join consecutive segments into one run
    for (ax, ay), (bx, by) in zip(lonlat, lonlat[1:]):
        gx0, gy0 = (ax + 180.0) * scale, (ay + 90.0) * scale
        gx1, gy1 = (bx + 180.0) * scale, (by + 90.0) * scale
        for ix in range(int(math.floor(min(gx0, gx1))), int(math.floor(max(gx0, gx1))) + 1):
            for iy in range(int(math.floor(min(gy0, gy1))), int(math.floor(max(gy0, gy1))) + 1):
                c = _clip_seg(gx0, gy0, gx1, gy1, ix, iy, ix + 1, iy + 1)
                if c is None:
                    continue
                p0 = (round((c[0] - ix) * V4_UNITS), round((c[1] - iy) * V4_UNITS))
                p1 = (round((c[2] - ix) * V4_UNITS), round((c[3] - iy) * V4_UNITS))
                if p0 == p1:
                    continue
                runs = out.setdefault((ix, iy), [])
                if last_end.get((ix, iy)) == p0 and runs:
                    runs[-1].append(p1)
                else:
                    runs.append([p0, p1])
                last_end[(ix, iy)] = p1
    return out


def pack_tile_v4(ways):
    """ways: [(layer, name, [(x, y) tile units])]. Same shape as a v3 section, i16 points."""
    blob, ids = bytearray(), {}
    for _, name, _ in ways:
        if name and name not in ids:
            enc = name.encode("utf-8")[:NAME_MAX_BYTES]
            ids[name] = len(blob)
            blob += bytes([len(enc)]) + enc
    if len(blob) > 0xFFFE:
        raise SystemExit("a tile's name blob overflowed; this should not happen at these tile sizes")
    buf = bytearray(struct.pack("<H", len(blob)))
    buf += blob
    # Long ways are split so npts fits u16.
    recs = []
    for layer, name, pts in ways:
        for i in range(0, len(pts) - 1, 65000):
            chunk = pts[i:i + 65001]
            if len(chunk) >= 2:
                recs.append((layer, name, chunk))
    buf += struct.pack("<H", len(recs))
    for layer, name, pts in sorted(recs, key=lambda r: r[0]):  # water first: roads draw over it
        nid = ids.get(name, NAME_NONE) if name else NAME_NONE
        buf += struct.pack("<BHH", layer, len(pts), nid)
        for x, y in pts:
            buf += struct.pack("<hh", x, y)
    buf += struct.pack("<H", 0)  # POI count, reserved
    return bytes(buf)


def build_v4(raw_m, lat0, lon0, bbox, out_path):
    """raw_m: stitched ways in metres about (lat0, lon0). bbox: (south, west, north, east)."""
    m_per_deg_lat = 111132.0
    m_per_deg_lon = 111320.0 * math.cos(math.radians(lat0))
    south, west, north, east = bbox

    def to_lonlat(pts):
        return [(lon0 + x / m_per_deg_lon, lat0 + y / m_per_deg_lat) for x, y in pts]

    def clip_bbox(ll):
        """Keep only what is inside the pack's box, so neighbouring packs do not overlap."""
        runs, cur = [], []
        for (ax, ay), (bx, by) in zip(ll, ll[1:]):
            c = _clip_seg(ax, ay, bx, by, west, south, east, north)
            if c is None:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = []
                continue
            a, b = (c[0], c[1]), (c[2], c[3])
            if cur and cur[-1] == a:
                cur.append(b)
            else:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = [a, b]
        if len(cur) >= 2:
            runs.append(cur)
        return runs

    tiles = {}  # (level, ix, iy) -> [(layer, name, pts)]
    for level, shift in enumerate(V4_SHIFTS):
        tol_for = V4_TOLERANCE[level]
        kept = 0
        for layer, name, pts in raw_m:
            tol = tol_for[layer]
            if tol is None:
                continue
            if V4_MIN_LEN[level] > 0:
                length = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
                if length < V4_MIN_LEN[level]:
                    continue
            simp = simplify(pts, tol)
            if len(simp) < 2:
                continue
            for run in clip_bbox(to_lonlat(simp)):
                for (ix, iy), truns in split_into_tiles(run, shift).items():
                    for tr in truns:
                        tiles.setdefault((level, ix, iy), []).append((layer, name if level > 0 or layer == LAYER_MAJOR else "", tr))
                        kept += 1
        sys.stderr.write(f"level {level} (1/{1 << shift} deg tiles): {kept} runs\n")

    entries = []
    for (level, ix, iy), ways in tiles.items():
        key = (level << 28) | (iy << 14) | ix
        entries.append((key, pack_tile_v4(ways)))
    entries.sort(key=lambda e: e[0])

    header_len, dir_len = 48, 12 * len(entries)
    off = header_len + dir_len
    directory = bytearray()
    for key, blob in entries:
        directory += struct.pack("<III", key, off, len(blob))
        off += len(blob)
    header = bytearray(b"MAP4")
    header += struct.pack("<BB", 4, len(V4_SHIFTS))
    header += bytes(V4_SHIFTS)
    header += b"\0" * 3
    header += struct.pack("<iiii", int(round(south * 1e7)), int(round(west * 1e7)),
                          int(round(north * 1e7)), int(round(east * 1e7)))
    header += struct.pack("<II", len(entries), header_len)
    header += b"\0" * (header_len - len(header))
    assert len(header) == header_len

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(directory)
        for _, blob in entries:
            f.write(blob)

    print(f"wrote {out_path}")
    print(f"  total      {off:,} bytes ({off / 1024:.0f} KiB), {len(entries):,} tiles")
    for level, shift in enumerate(V4_SHIFTS):
        sizes = [len(b) for k, b in entries if (k >> 28) == level]
        if sizes:
            print(f"  level {level}    {len(sizes):,} tiles, largest {max(sizes) / 1024:.1f} KiB, "
                  f"mean {sum(sizes) / len(sizes) / 1024:.1f} KiB")


# --- pack content beyond roads and water (pack version 5) --------------------------------
LAYER_PATH = 4
LAYER_RAIL = 5
# Draw order inside a tile: water, trails, local, rail, minor, major on top.
LINE_ORDER = {LAYER_WATER: 0, LAYER_PATH: 1, LAYER_LOCAL: 2, LAYER_RAIL: 3, LAYER_MINOR: 4, LAYER_MAJOR: 5}
LINE_TOL = (  # metres per level; None = not at that level
    {LAYER_WATER: 120.0, LAYER_MAJOR: 80.0, LAYER_MINOR: None, LAYER_LOCAL: None, LAYER_PATH: None, LAYER_RAIL: None},
    {LAYER_WATER: 25.0, LAYER_MAJOR: 12.0, LAYER_MINOR: 12.0, LAYER_LOCAL: None, LAYER_PATH: None, LAYER_RAIL: 15.0},
    {LAYER_WATER: 8.0, LAYER_MAJOR: 4.0, LAYER_MINOR: 4.0, LAYER_LOCAL: 6.0, LAYER_PATH: 4.0, LAYER_RAIL: 6.0},
)

AREA_WATER, AREA_PARK, AREA_WOOD = 0, 1, 2
AREA_TOL = (                       # metres per level
    {AREA_WATER: 150.0, AREA_PARK: None, AREA_WOOD: None},
    {AREA_WATER: 30.0, AREA_PARK: 30.0, AREA_WOOD: 40.0},
    {AREA_WATER: 6.0, AREA_PARK: 6.0, AREA_WOOD: 10.0},
)
AREA_MIN_M2 = (500000.0, 50000.0, 1500.0)  # smaller areas are left out at each level

POI_KINDS = {  # (key, value) -> firmware POI kind
    ("amenity", "hospital"): 1, ("amenity", "fuel"): 2, ("amenity", "parking"): 3,
    ("amenity", "drinking_water"): 4, ("amenity", "shelter"): 5, ("natural", "peak"): 6,
    ("amenity", "toilets"): 7, ("tourism", "camp_site"): 8, ("tourism", "viewpoint"): 9,
}
PLACE_KINDS = {"city": 0, "town": 1, "village": 2, "suburb": 3, "hamlet": 3, "neighbourhood": 3}
PLACE_WATER = 4
PLACE_LEVELS = {0: (0, 1, 2), 1: (0, 1, 2), 2: (1, 2), 3: (2,), PLACE_WATER: (1, 2)}


def build_query_v5(south, west, north, east, include_local):
    bbox = f"{south:.6f},{west:.6f},{north:.6f},{east:.6f}"
    highway_values = [k for k, v in HIGHWAY_LAYER.items() if include_local or v != LAYER_LOCAL]
    highway_re = "|".join(highway_values)
    paths = ""
    if include_local:
        # Sidewalks and crossings are mapped as footways in towns: thousands of them, and
        # they are what makes a town map unreadable. Trails are the point.
        paths = f"""
  way["highway"~"^(path|track|bridleway|cycleway)$"]({bbox});
  way["highway"="footway"]["footway"!~"."]({bbox});"""
    return f"""
[out:json][timeout:300];
(
  way["highway"~"^({highway_re})$"]({bbox});{paths}
  way["railway"~"^(rail|light_rail|subway|narrow_gauge)$"]["service"!~"."]({bbox});
  way["natural"="coastline"]({bbox});
  way["natural"="water"]({bbox});
  way["waterway"~"^(river|stream)$"]({bbox});
  way["landuse"="reservoir"]({bbox});
  way["leisure"~"^(park|nature_reserve|golf_course|recreation_ground)$"]({bbox});
  way["landuse"~"^(forest|recreation_ground)$"]({bbox});
  way["natural"="wood"]({bbox});
  relation["type"="multipolygon"]["natural"~"^(water|wood)$"]({bbox});
  relation["type"="multipolygon"]["landuse"~"^(forest|reservoir)$"]({bbox});
  relation["type"="multipolygon"]["leisure"~"^(park|nature_reserve)$"]({bbox});
  node["place"~"^(city|town|village|suburb|hamlet|neighbourhood)$"]({bbox});
  node["amenity"~"^(hospital|fuel|parking|drinking_water|shelter|toilets)$"]({bbox});
  way["amenity"~"^(hospital|fuel|parking)$"]({bbox});
  node["natural"="peak"]({bbox});
  node["tourism"~"^(camp_site|viewpoint)$"]({bbox});
);
out body geom;
"""


def area_kind(tags):
    if tags.get("natural") == "water" or tags.get("landuse") == "reservoir":
        return AREA_WATER
    if tags.get("leisure") in ("park", "nature_reserve", "golf_course", "recreation_ground") or \
            tags.get("landuse") == "recreation_ground":
        return AREA_PARK
    if tags.get("natural") == "wood" or tags.get("landuse") == "forest":
        return AREA_WOOD
    return None


def poi_kind(tags):
    for (k, v), kind in POI_KINDS.items():
        if tags.get(k) == v:
            return kind
    return None


def _ring_area(r):
    a = 0.0
    for (x0, y0), (x1, y1) in zip(r, r[1:] + r[:1]):
        a += x0 * y1 - x1 * y0
    return abs(a) / 2.0


def _join_rings(segments):
    """Join open member ways of a multipolygon into closed rings."""
    def key(p):
        return (round(p[0], 2), round(p[1], 2))
    rings, open_ = [], [list(s) for s in segments if len(s) >= 2]
    while open_:
        cur = open_.pop()
        changed = True
        while key(cur[0]) != key(cur[-1]) and changed:
            changed = False
            for i, o in enumerate(open_):
                if key(o[0]) == key(cur[-1]):
                    cur += o[1:]
                elif key(o[-1]) == key(cur[-1]):
                    cur += list(reversed(o))[1:]
                elif key(o[-1]) == key(cur[0]):
                    cur = o[:-1] + cur
                elif key(o[0]) == key(cur[0]):
                    cur = list(reversed(o))[:-1] + cur
                else:
                    continue
                open_.pop(i)
                changed = True
                break
        if len(cur) >= 4 and key(cur[0]) == key(cur[-1]):
            rings.append(cur[:-1])
    return rings


def extract_v5(elements, lat0, lon0, include_local):
    """Split the Overpass answer into lines, areas, POIs and places, all in metres."""
    m_per_deg_lat = 111132.0
    m_per_deg_lon = 111320.0 * math.cos(math.radians(lat0))

    def prj(g):
        return ((g["lon"] - lon0) * m_per_deg_lon, (g["lat"] - lat0) * m_per_deg_lat)

    lines, areas, pois, places = [], [], [], []
    for el in elements:
        tags = el.get("tags", {})
        t = el.get("type")
        if t == "node":
            pt = prj(el)
            pk = poi_kind(tags)
            if pk:
                pois.append((pk, (tags.get("name") or "")[:NAME_MAX_BYTES], pt))
            pl = PLACE_KINDS.get(tags.get("place"))
            if pl is not None and tags.get("name"):
                places.append((pl, tags["name"][:NAME_MAX_BYTES], pt))
            continue
        if t == "way" and "geometry" in el:
            pts = [prj(g) for g in el["geometry"]]
            if len(pts) < 2:
                continue
            hw = tags.get("highway")
            if hw in ("path", "track", "bridleway", "cycleway", "footway"):
                if include_local:
                    lines.append((LAYER_PATH, "", pts))
                continue
            if "railway" in tags:
                lines.append((LAYER_RAIL, "", pts))
                continue
            layer = classify(tags)
            if layer is not None:
                name = road_label(tags, layer) if layer != LAYER_WATER else ""
                lines.append((layer, name, pts))
                if tags.get("waterway") == "river" and tags.get("name"):
                    mid = pts[len(pts) // 2]
                    places.append((PLACE_WATER, tags["name"][:NAME_MAX_BYTES], mid))
            closed = len(pts) >= 4 and math.dist(pts[0], pts[-1]) < 0.5
            ak = area_kind(tags)
            if ak is not None and closed:
                ring = pts[:-1]
                areas.append((ak, [ring]))
                if ak == AREA_WATER and tags.get("name"):
                    cx = sum(p[0] for p in ring) / len(ring)
                    cy = sum(p[1] for p in ring) / len(ring)
                    places.append((PLACE_WATER, tags["name"][:NAME_MAX_BYTES], (cx, cy)))
            pk = poi_kind(tags)
            if pk:
                cx = sum(p[0] for p in pts) / len(pts)
                cy = sum(p[1] for p in pts) / len(pts)
                pois.append((pk, (tags.get("name") or "")[:NAME_MAX_BYTES], (cx, cy)))
            continue
        if t == "relation":
            ak = area_kind(tags)
            if ak is None:
                continue
            segs = [[prj(g) for g in m["geometry"]] for m in el.get("members", [])
                    if m.get("type") == "way" and m.get("geometry")]
            rings = _join_rings(segs)
            if rings:
                areas.append((ak, rings))
                if ak == AREA_WATER and tags.get("name"):
                    big = max(rings, key=_ring_area)
                    places.append((PLACE_WATER, tags["name"][:NAME_MAX_BYTES],
                                   (sum(p[0] for p in big) / len(big), sum(p[1] for p in big) / len(big))))
    return lines, areas, pois, places


def _clip_poly(ring, xmin, ymin, xmax, ymax):
    """Sutherland-Hodgman against an axis-aligned rectangle."""
    def clip(pts, inside, inter):
        out = []
        if not pts:
            return out
        prev = pts[-1]
        for cur in pts:
            if inside(cur):
                if not inside(prev):
                    out.append(inter(prev, cur))
                out.append(cur)
            elif inside(prev):
                out.append(inter(prev, cur))
            prev = cur
        return out

    def ix(a, b, x):
        t_ = (x - a[0]) / (b[0] - a[0])
        return (x, a[1] + t_ * (b[1] - a[1]))

    def iy(a, b, y):
        t_ = (y - a[1]) / (b[1] - a[1])
        return (a[0] + t_ * (b[0] - a[0]), y)

    pts = ring
    pts = clip(pts, lambda p: p[0] >= xmin, lambda a, b: ix(a, b, xmin))
    pts = clip(pts, lambda p: p[0] <= xmax, lambda a, b: ix(a, b, xmax))
    pts = clip(pts, lambda p: p[1] >= ymin, lambda a, b: iy(a, b, ymin))
    pts = clip(pts, lambda p: p[1] <= ymax, lambda a, b: iy(a, b, ymax))
    return pts


def build_pack(lines, areas, pois, places, lat0, lon0, bbox, out_path):
    """Write a version-5 map pack. Everything comes in as metres about (lat0, lon0)."""
    m_per_deg_lat = 111132.0
    m_per_deg_lon = 111320.0 * math.cos(math.radians(lat0))
    south, west, north, east = bbox

    def ll(p):
        return (lon0 + p[0] / m_per_deg_lon, lat0 + p[1] / m_per_deg_lat)

    def in_bbox(lonlat):
        return west <= lonlat[0] <= east and south <= lonlat[1] <= north

    def clip_bbox_line(pts):
        runs, cur = [], []
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            c = _clip_seg(ax, ay, bx, by, west, south, east, north)
            if c is None:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = []
                continue
            a, b = (c[0], c[1]), (c[2], c[3])
            if cur and cur[-1] == a:
                cur.append(b)
            else:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = [a, b]
        if len(cur) >= 2:
            runs.append(cur)
        return runs

    def tile_of(lonlat, shift):
        sc = float(1 << shift)
        gx, gy = (lonlat[0] + 180.0) * sc, (lonlat[1] + 90.0) * sc
        ix_, iy_ = int(math.floor(gx)), int(math.floor(gy))
        return ix_, iy_, (round((gx - ix_) * V4_UNITS), round((gy - iy_) * V4_UNITS))

    # tiles[(level, ix, iy)] = {"lines": [], "areas": [], "pois": [], "places": []}
    tiles = {}

    def tile(level, ix_, iy_):
        return tiles.setdefault((level, ix_, iy_), {"lines": [], "areas": [], "pois": [], "places": []})

    for level, shift in enumerate(V4_SHIFTS):
        sc = float(1 << shift)
        # lines
        for layer, name, pts in lines:
            tol = LINE_TOL[level].get(layer)
            if tol is None:
                continue
            if V4_MIN_LEN[level] > 0 and sum(math.dist(a, b) for a, b in zip(pts, pts[1:])) < V4_MIN_LEN[level]:
                continue
            simp = simplify(pts, tol)
            if len(simp) < 2:
                continue
            keep_name = name if (level > 0 or layer == LAYER_MAJOR) else ""
            for run in clip_bbox_line([ll(p) for p in simp]):
                for (ix_, iy_), truns in split_into_tiles(run, shift).items():
                    for tr in truns:
                        tile(level, ix_, iy_)["lines"].append((layer, keep_name, tr))
        # areas
        for kind, rings in areas:
            tol = AREA_TOL[level].get(kind)
            if tol is None:
                continue
            if sum(_ring_area(r) for r in rings) < AREA_MIN_M2[level]:
                continue
            simp_rings = []
            for r in rings:
                sr = simplify(r + r[:1], tol)[:-1]
                if len(sr) >= 3:
                    simp_rings.append([ll(p) for p in sr])
            if not simp_rings:
                continue
            # bounding tiles of the whole area, then clip every ring to each tile
            xs = [p[0] for r in simp_rings for p in r]
            ys = [p[1] for r in simp_rings for p in r]
            x0 = max(west, min(xs)); x1 = min(east, max(xs))
            y0 = max(south, min(ys)); y1 = min(north, max(ys))
            if x0 >= x1 or y0 >= y1:
                continue
            for ix_ in range(int(math.floor((x0 + 180) * sc)), int(math.floor((x1 + 180) * sc)) + 1):
                for iy_ in range(int(math.floor((y0 + 90) * sc)), int(math.floor((y1 + 90) * sc)) + 1):
                    tw, ts = ix_ / sc - 180.0, iy_ / sc - 90.0
                    rect = (max(tw, west), max(ts, south), min(tw + 1 / sc, east), min(ts + 1 / sc, north))
                    out_rings = []
                    for r in simp_rings:
                        c = _clip_poly(r, *rect)
                        if len(c) >= 3:
                            q = [(round((p[0] - tw) * sc * V4_UNITS), round((p[1] - ts) * sc * V4_UNITS)) for p in c]
                            dedup = [q[0]] + [b for a, b in zip(q, q[1:]) if b != a]
                            if len(dedup) >= 3:
                                out_rings.append(dedup)
                    if out_rings:
                        tile(level, ix_, iy_)["areas"].append((kind, out_rings[:8]))
        # POIs: detail level only
        if level == len(V4_SHIFTS) - 1:
            for kind, name, pt in pois:
                lonlat = ll(pt)
                if in_bbox(lonlat):
                    ix_, iy_, q = tile_of(lonlat, shift)
                    tile(level, ix_, iy_)["pois"].append((kind, name, q))
        # places
        for kind, name, pt in places:
            if level not in PLACE_LEVELS.get(kind, ()):
                continue
            lonlat = ll(pt)
            if in_bbox(lonlat):
                ix_, iy_, q = tile_of(lonlat, shift)
                tile(level, ix_, iy_)["places"].append((kind, name, q))

    def pack_tile(t):
        blob, ids = bytearray(), {}

        def nid(name):
            if not name:
                return NAME_NONE
            if name not in ids:
                enc = name.encode("utf-8")[:NAME_MAX_BYTES]
                ids[name] = len(blob)
                blob.extend(bytes([len(enc)]) + enc)
            return ids[name]

        body = bytearray()
        # areas: water drawn last so lakes sit on top of woods
        recs = []
        for kind, rings in sorted(t["areas"], key=lambda a: {AREA_WOOD: 0, AREA_PARK: 1, AREA_WATER: 2}[a[0]]):
            data = bytearray([len(rings)])
            for r in rings:
                data += struct.pack("<H", len(r))
                for x, y in r:
                    data += struct.pack("<hh", x, y)
            if len(data) <= 1024:  # the firmware fills from one 1 KB window when streaming
                recs.append((kind, bytes(data)))
            else:
                # Too detailed for one record: re-simplify by keeping every n-th point.
                step = math.ceil(len(data) / 1000)
                data = bytearray([len(rings)])
                for r in rings:
                    rr = r[::step] if len(r) > 3 * step else r
                    data += struct.pack("<H", len(rr))
                    for x, y in rr:
                        data += struct.pack("<hh", x, y)
                if len(data) <= 1024:
                    recs.append((kind, bytes(data)))
        body += struct.pack("<H", len(recs))
        for kind, data in recs:
            body += struct.pack("<BH", kind, len(data)) + data
        # lines
        lrecs = []
        for layer, name, pts in t["lines"]:
            for i in range(0, len(pts) - 1, 65000):
                chunk = pts[i:i + 65001]
                if len(chunk) >= 2:
                    lrecs.append((layer, name, chunk))
        lrecs.sort(key=lambda r: LINE_ORDER[r[0]])
        body += struct.pack("<H", len(lrecs))
        for layer, name, pts in lrecs:
            body += struct.pack("<BHH", layer, len(pts), nid(name))
            for x, y in pts:
                body += struct.pack("<hh", x, y)
        body += struct.pack("<H", len(t["pois"]))
        for kind, name, (x, y) in t["pois"]:
            body += struct.pack("<BHhh", kind, nid(name), x, y)
        body += struct.pack("<H", len(t["places"]))
        for kind, name, (x, y) in sorted(t["places"], key=lambda p: p[0]):
            body += struct.pack("<BHhh", kind, nid(name), x, y)
        if len(blob) > 0xFFFE:
            raise SystemExit("a tile's name blob overflowed")
        return struct.pack("<H", len(blob)) + bytes(blob) + bytes(body)

    entries = sorted((((lv << 28) | (iy_ << 14) | ix_), pack_tile(t)) for (lv, ix_, iy_), t in tiles.items())

    header_len = 48
    off = header_len + 12 * len(entries)
    directory = bytearray()
    for key, blob in entries:
        directory += struct.pack("<III", key, off, len(blob))
        off += len(blob)
    header = bytearray(b"MAP4")
    header += struct.pack("<BB", 5, len(V4_SHIFTS))
    header += bytes(V4_SHIFTS)
    header += b"\0" * 3
    header += struct.pack("<iiii", int(round(south * 1e7)), int(round(west * 1e7)),
                          int(round(north * 1e7)), int(round(east * 1e7)))
    header += struct.pack("<II", len(entries), header_len)
    header += b"\0" * (header_len - len(header))
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(directory)
        for _, blob in entries:
            f.write(blob)

    print(f"wrote {out_path}")
    print(f"  total      {off:,} bytes ({off / 1024:.0f} KiB), {len(entries):,} tiles")
    for level in range(len(V4_SHIFTS)):
        sizes = [len(b) for k, b in entries if (k >> 28) == level]
        if sizes:
            print(f"  level {level}    {len(sizes):,} tiles, largest {max(sizes) / 1024:.1f} KiB, "
                  f"mean {sum(sizes) / len(sizes) / 1024:.1f} KiB")
    print(f"  content    {len(lines):,} lines, {len(areas):,} areas, {len(pois):,} POIs, {len(places):,} names")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--km", type=float, default=24.0, help="box width in km")
    ap.add_argument("--grid", type=int, default=8, help="detail cells per side")
    ap.add_argument("--no-local", action="store_true", help="skip residential streets")
    ap.add_argument("--name-min-len", type=float, default=0.0,
                    help="drop names from local roads shorter than this many metres; "
                         "the name blob is uint16-addressed and a wide box overruns it")
    ap.add_argument("--overview-min-len", type=float, default=600.0,
                    help="drop overview lines shorter than this many metres")
    ap.add_argument("--format", type=int, choices=[2, 3, 4], default=4,
                    help="4 (default): a map pack for /maps on the SD card - tiled, any size, packs "
                         "for neighbouring areas join up. 3/2: the single basemap.bin older firmware reads")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", help="reuse/save the raw Overpass JSON here")
    args = ap.parse_args()

    half_km = args.km / 2.0
    half_m = half_km * 1000.0
    dlat = half_km / 111.132
    dlon = half_km / (111.320 * math.cos(math.radians(args.lat)))
    south, north = args.lat - dlat, args.lat + dlat
    west, east = args.lon - dlon, args.lon + dlon
    sys.stderr.write(f"bbox {south:.4f},{west:.4f} .. {north:.4f},{east:.4f}\n")

    if args.format == 4:
        # Points stay in metres about the box centre for stitching and simplification;
        # build_v4 turns them back into degrees for tiling.
        data = None
        if args.cache:
            try:
                with open(args.cache, "r", encoding="utf-8") as f:
                    data = json.load(f)
                sys.stderr.write(f"reusing cached {args.cache}\n")
            except (OSError, ValueError):
                data = None
        if data is None:
            data = fetch(build_query_v5(south, west, north, east, not args.no_local))
            if args.cache:
                with open(args.cache, "w", encoding="utf-8") as f:
                    json.dump(data, f)
        elements = data.get("elements", [])
        sys.stderr.write(f"{len(elements)} elements\n")
        lines, areas, pois, places = extract_v5(elements, args.lat, args.lon, not args.no_local)
        lines = stitch(lines)
        sys.stderr.write(f"{len(lines)} lines after stitching, {len(areas)} areas, {len(pois)} POIs, {len(places)} names\n")
        build_pack(lines, areas, pois, places, args.lat, args.lon, (south, west, north, east), args.out)
        return

    data = None
    if args.cache:
        try:
            with open(args.cache, "r", encoding="utf-8") as f:
                data = json.load(f)
            sys.stderr.write(f"reusing cached {args.cache}\n")
        except (OSError, ValueError):
            data = None
    if data is None:
        data = fetch(build_query(south, west, north, east, not args.no_local))
        if args.cache:
            with open(args.cache, "w", encoding="utf-8") as f:
                json.dump(data, f)

    elements = data.get("elements", [])
    sys.stderr.write(f"{len(elements)} elements\n")
    raw = project_all(elements, args.lat, args.lon, args.name_min_len)
    sys.stderr.write(f"{len(raw)} usable ways\n")
    raw = stitch(raw)
    sys.stderr.write(f"{len(raw)} after stitching\n")

    # --- overview: one flat section, small enough to hold in RAM ---
    overview = []
    for layer, name, pts in raw:
        tol = TOLERANCE_OVERVIEW[layer]
        if tol is None:
            continue
        for run in clip_to_box(pts, half_m):
            # Drop stubs: interchange ramps and short unjoined fragments are illegible at
            # a whole-box view and are what the overview budget actually goes on.
            length = sum(math.dist(run[i], run[i + 1]) for i in range(len(run) - 1))
            if length < args.overview_min_len:
                continue
            s = simplify(run, tol)
            if len(s) >= 2:
                overview.append((layer, name, s))

    # --- detail: bucketed into a grid so the renderer reads only what it needs ---
    cell_m = (2 * half_m) / args.grid
    cells = [[] for _ in range(args.grid * args.grid)]
    for layer, name, pts in raw:
        tol = TOLERANCE_DETAIL[layer]
        for run in clip_to_box(pts, half_m):
            s = simplify(run, tol)
            if len(s) < 2:
                continue
            # Assign a way to every cell it touches, split at cell boundaries so a cell
            # is self-contained. Split by point membership, carrying one point of overlap
            # so segments crossing a boundary still draw.
            cur_cell, cur_pts = None, []
            for x, y in s:
                cx = min(args.grid - 1, max(0, int((x + half_m) // cell_m)))
                cy = min(args.grid - 1, max(0, int((y + half_m) // cell_m)))
                idx = cy * args.grid + cx
                if cur_cell is None:
                    cur_cell = idx
                elif idx != cur_cell:
                    cur_pts.append((x, y))
                    if len(cur_pts) >= 2:
                        cells[cur_cell].append((layer, name, cur_pts))
                    cur_cell, cur_pts = idx, [cur_pts[-2]] if len(cur_pts) >= 2 else []
                cur_pts.append((x, y))
            if cur_cell is not None and len(cur_pts) >= 2:
                cells[cur_cell].append((layer, name, cur_pts))

    if args.format == 3:
        overview_blob = pack_section_v3(overview)
        cell_blobs = [pack_section_v3(c) for c in cells]
        name_blob = b""  # v3 keeps names inside each section; nothing global is left
    else:
        # v2: one global name blob, i16 points. Older firmware reads only this layout.
        name_blob, name_ids = build_name_blob([overview] + cells)
        overview_blob = pack_ways(overview, name_ids)
        cell_blobs = [pack_ways(c, name_ids) for c in cells]

    header = bytearray()
    header += MAGIC
    header += struct.pack("<B", args.format)
    header += struct.pack("<B", args.grid)
    header += struct.pack("<H", 0)  # pad, keeps the offset table 4-byte aligned
    header += struct.pack("<ii", int(round(args.lat * 1e7)), int(round(args.lon * 1e7)))
    header += struct.pack("<I", int(round(half_m)))
    header += struct.pack("<I", int(round(cell_m)))
    # v2 put the global name blob's offset/len here. v3 has no global blob, so these stay
    # zero - the field is kept only so the offset table still starts at byte 32.
    name_off_pos = len(header)
    header += struct.pack("<II", 0, 0)

    # overview offset/len, then one offset/len per cell
    table_entries = 1 + args.grid * args.grid
    table_size = table_entries * 8
    base = len(header) + table_size

    table = bytearray()
    off = base
    table += struct.pack("<II", off, len(overview_blob))
    off += len(overview_blob)
    for blob in cell_blobs:
        table += struct.pack("<II", off, len(blob))
        off += len(blob)

    if args.format == 3:
        struct.pack_into("<II", header, name_off_pos, 0, 0)  # v3 has no global blob
    else:
        struct.pack_into("<II", header, name_off_pos, off, len(name_blob))  # v2 blob after sections
        off += len(name_blob)

    with open(args.out, "wb") as f:
        f.write(header)
        f.write(table)
        f.write(overview_blob)
        for blob in cell_blobs:
            f.write(blob)
        f.write(name_blob)

    total = off
    ov_pts = sum(len(p) for _, _, p in overview)
    det_pts = sum(len(p) for c in cells for _, _, p in c)
    nonempty = sum(1 for b in cell_blobs if len(b) > 8)  # v3 empty = 4B names + 4B count
    named = sum(1 for c in cells for w in c if w[1])
    print(f"wrote {args.out}")
    print(f"  total      {total:,} bytes ({total / 1024:.0f} KiB)")
    print(f"  overview   {len(overview):,} ways / {ov_pts:,} pts / {len(overview_blob) / 1024:.0f} KiB")
    print(f"  detail     {sum(len(c) for c in cells):,} ways / {det_pts:,} pts across {nonempty}/{args.grid ** 2} cells")
    print(f"  largest cell {max(len(b) for b in cell_blobs) / 1024:.1f} KiB")
    # v3 keeps names per section, so "distinct" is counted across the whole file and the
    # per-section figure is what actually has to stay under the u16 id space.
    all_names = {w[1] for sec in [overview] + cells for w in sec if w[1]}
    if args.format == 3:
        name_bytes = sum(struct.unpack_from("<I", b, 0)[0] for b in [overview_blob] + cell_blobs)
        worst = max(struct.unpack_from("<I", b, 0)[0] for b in [overview_blob] + cell_blobs)
        print(f"  names      {len(all_names):,} distinct / {named:,} labelled ways / {name_bytes / 1024:.1f} KiB")
        print(f"  worst section name blob {worst:,} B of {NAME_NONE:,} addressable")
    else:
        print(f"  names      {len(all_names):,} distinct / {named:,} labelled ways / {len(name_blob) / 1024:.1f} KiB")
        if len(name_blob) > NAME_NONE:
            print(f"  WARNING: global name blob {len(name_blob):,} B exceeds the {NAME_NONE:,} B v2 limit; raise --name-min-len")


if __name__ == "__main__":
    main()
