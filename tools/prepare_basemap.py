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
            except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
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

    overview_blob = pack_section_v3(overview)
    cell_blobs = [pack_section_v3(c) for c in cells]
    name_blob = b""  # v3 keeps names inside each section; nothing global is left

    header = bytearray()
    header += MAGIC
    header += struct.pack("<B", FORMAT_VERSION)
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

    struct.pack_into("<II", header, name_off_pos, 0, 0)

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
    name_bytes = sum(struct.unpack_from("<I", b, 0)[0] for b in [overview_blob] + cell_blobs)
    worst = max(struct.unpack_from("<I", b, 0)[0] for b in [overview_blob] + cell_blobs)
    print(f"  names      {len(all_names):,} distinct / {named:,} labelled ways / {name_bytes / 1024:.1f} KiB")
    print(f"  worst section name blob {worst:,} B of {NAME_NONE:,} addressable")


if __name__ == "__main__":
    main()
