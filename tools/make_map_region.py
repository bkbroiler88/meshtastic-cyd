#!/usr/bin/env python3
"""Make a map pack for a whole state or region from a Geofabrik OpenStreetMap extract.

make_map.py downloads through the Overpass API, which is right for a town but too slow and
too heavily rate-limited for a state. This reads a Geofabrik .osm.pbf extract instead (one
download, no limits) and builds the same version-5 map pack: the objects are filtered with
the same rules as make_map.py's Overpass query and go through the same extract / stitch /
pack code in prepare_basemap.py.

    python tools/make_map_region.py --region us/maryland
    python tools/make_map_region.py --region europe/ireland-and-northern-ireland --no-local
    python tools/make_map_region.py --pbf delaware-latest.osm.pbf --copy-to E:\\

Regions are Geofabrik paths (see https://download.geofabrik.de); "us/<state>" is short for
"north-america/us/<state>". Put the .map file in the maps folder of the node's microSD card.
A whole state with residential streets is tens of MB and takes a while to build; --no-local
keeps main roads, water, parks, places and points of interest only.

Needs: pip install osmium

Map data (c) OpenStreetMap contributors, ODbL (openstreetmap.org/copyright).
"""

import argparse
import importlib.util
import os
import re
import shutil
import sys
import time
import urllib.request

try:
    import osmium
except ImportError:
    raise SystemExit("This tool needs pyosmium: pip install osmium")

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("prep", os.path.join(HERE, "prepare_basemap.py"))
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)

# --- the Overpass query in prepare_basemap.build_query_v5, as filters -------------------

LOCAL_HW = {k for k, v in prep.HIGHWAY_LAYER.items() if v == prep.LAYER_LOCAL}
PATH_HW = {"path", "track", "bridleway", "cycleway"}
RAIL = {"rail", "light_rail", "subway", "narrow_gauge"}


def want_way(t, include_local):
    hw = t.get("highway")
    if hw in prep.HIGHWAY_LAYER and (include_local or hw not in LOCAL_HW):
        return True
    if include_local and (hw in PATH_HW or (hw == "footway" and "footway" not in t)):
        return True
    if t.get("railway") in RAIL and "service" not in t:
        return True
    if t.get("natural") in ("coastline", "water", "wood"):
        return True
    if t.get("waterway") in ("river", "stream"):
        return True
    if t.get("landuse") in ("reservoir", "forest", "recreation_ground"):
        return True
    if t.get("leisure") in ("park", "nature_reserve", "golf_course", "recreation_ground"):
        return True
    return t.get("amenity") in ("hospital", "fuel", "parking")


def want_node(t):
    return (t.get("place") in ("city", "town", "village", "suburb", "hamlet", "neighbourhood")
            or t.get("amenity") in ("hospital", "fuel", "parking", "drinking_water", "shelter", "toilets")
            or t.get("natural") == "peak"
            or t.get("tourism") in ("camp_site", "viewpoint"))


def want_relation(t):
    return t.get("type") == "multipolygon" and (
        t.get("natural") in ("water", "wood") or t.get("landuse") in ("forest", "reservoir")
        or t.get("leisure") in ("park", "nature_reserve"))


def tags_of(o):
    return {tag.k: tag.v for tag in o.tags}


# --- reading the extract -----------------------------------------------------------------

def elements(pbf, include_local, bbox, stats):
    """Yield Overpass-shaped elements (what prepare_basemap.extract_v5 reads), streaming."""
    south, west, north, east = bbox

    def inside(lat, lon):
        return south <= lat <= north and west <= lon <= east

    # Pass 1: which multipolygons we keep, and the ways they are made of
    rels = []
    member_ways = {}
    t0 = time.time()
    for o in osmium.FileProcessor(pbf, osmium.osm.RELATION):
        t = tags_of(o)
        if want_relation(t):
            mids = [m.ref for m in o.members if m.type == "w"]
            rels.append((t, mids))
            for m in mids:
                member_ways[m] = None
    sys.stderr.write(f"pass 1: {len(rels):,} lakes/parks/woods made of {len(member_ways):,} ways "
                     f"({time.time() - t0:.0f} s)\n")

    # Pass 2: nodes and ways, with node locations resolved
    t0 = time.time()
    n_seen = 0
    fp = osmium.FileProcessor(pbf, osmium.osm.NODE | osmium.osm.WAY).with_locations("flex_mem")
    for o in fp:
        n_seen += 1
        if n_seen % 2_000_000 == 0:
            sys.stderr.write(f"  ...{n_seen:,} objects read ({time.time() - t0:.0f} s)\n")
        if o.is_node():
            if not len(o.tags):
                continue
            t = tags_of(o)
            if want_node(t):
                lat, lon = o.location.lat, o.location.lon
                if inside(lat, lon):
                    stats["nodes"] += 1
                    yield {"type": "node", "lat": lat, "lon": lon, "tags": t}
            continue
        # way
        is_member = o.id in member_ways
        t = tags_of(o) if len(o.tags) else {}
        wanted = bool(t) and want_way(t, include_local)
        if not wanted and not is_member:
            continue
        try:
            geom = [(n.location.lat, n.location.lon) for n in o.nodes]
        except osmium.InvalidLocationError:
            geom = [(n.location.lat, n.location.lon) for n in o.nodes if n.location.valid()]
        if is_member:
            member_ways[o.id] = geom
        if wanted and len(geom) >= 2 and any(inside(la, lo) for la, lo in geom):
            stats["ways"] += 1
            yield {"type": "way", "tags": t, "geometry": [{"lat": la, "lon": lo} for la, lo in geom]}
    sys.stderr.write(f"pass 2: {n_seen:,} objects read ({time.time() - t0:.0f} s)\n")

    for t, mids in rels:
        members = []
        for m in mids:
            g = member_ways.get(m)
            if g:
                members.append({"type": "way", "geometry": [{"lat": la, "lon": lo} for la, lo in g]})
        if members and any(inside(g["lat"], g["lon"]) for m in members for g in m["geometry"]):
            stats["relations"] += 1
            yield {"type": "relation", "tags": t, "members": members}


def extract_bounds(pbf):
    """The extract's own bounding box, from the file header (Geofabrik always writes one)."""
    h = osmium.io.Reader(pbf, osmium.osm.osm_entity_bits.NOTHING)
    box = h.header().box()
    h.close()
    if not box.valid():
        raise SystemExit("this .osm.pbf has no bounding box in its header; pass --bbox south,west,north,east")
    return (box.bottom_left.lat, box.bottom_left.lon, box.top_right.lat, box.top_right.lon)


def download(region, cache_dir):
    if region.startswith("http"):
        url = region
    else:
        r = region.strip("/").removesuffix("-latest.osm.pbf")
        if r.startswith("us/"):
            r = "north-america/" + r
        url = f"https://download.geofabrik.de/{r}-latest.osm.pbf"
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, url.rsplit("/", 1)[-1])
    if os.path.exists(path):
        sys.stderr.write(f"using cached {path}\n")
        return path
    sys.stderr.write(f"downloading {url}\n")
    req = urllib.request.Request(url, headers={"User-Agent": "meshtastic-cyd-make-map-region/1.0"})
    tmp = path + ".part"
    try:
        with urllib.request.urlopen(req, timeout=60) as resp, open(tmp, "wb") as f:
            total = int(resp.headers.get("Content-Length") or 0)
            got, last = 0, 0.0
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if time.time() - last > 2:
                    last = time.time()
                    pct = f" ({100 * got / total:.0f}%)" if total else ""
                    sys.stderr.write(f"  {got / 1e6:.0f} MB{pct}\r")
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{url}: {e}. Check the region path on https://download.geofabrik.de")
    os.replace(tmp, path)
    sys.stderr.write(f"\n  saved {path}\n")
    return path


def main():
    ap = argparse.ArgumentParser(description="Build a map pack for a whole region from a Geofabrik extract.",
                                 epilog="Map data (c) OpenStreetMap contributors, ODbL.")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--region", help='Geofabrik region, e.g. "us/maryland" or "europe/malta"')
    src.add_argument("--pbf", help="an .osm.pbf file you already have")
    ap.add_argument("--bbox", help="south,west,north,east to cut out only part of the extract")
    ap.add_argument("--no-local", action="store_true",
                    help="leave out residential streets and trails (recommended for whole states)")
    ap.add_argument("--out", help="output .map file (default: named after the region)")
    ap.add_argument("--cache-dir", default=os.path.join(os.path.expanduser("~"), ".cache", "meshtastic-maps"),
                    help="where downloaded extracts are kept")
    ap.add_argument("--copy-to", help="also copy into <this>/maps, e.g. the SD card root")
    args = ap.parse_args()

    pbf = args.pbf or download(args.region, args.cache_dir)
    if args.bbox:
        south, west, north, east = (float(v) for v in args.bbox.split(","))
    else:
        south, west, north, east = extract_bounds(pbf)
    lat0, lon0 = (south + north) / 2, (west + east) / 2
    sys.stderr.write(f"area {south:.4f},{west:.4f} .. {north:.4f},{east:.4f}"
                     f"{'' if not args.no_local else ', no residential streets or trails'}\n")

    stats = {"nodes": 0, "ways": 0, "relations": 0}
    t0 = time.time()
    lines, areas, pois, places = prep.extract_v5(elements(pbf, not args.no_local, (south, west, north, east), stats),
                                                 lat0, lon0, not args.no_local)
    sys.stderr.write(f"kept {stats['ways']:,} ways, {stats['relations']:,} multipolygons, {stats['nodes']:,} points\n")
    lines = prep.stitch(lines)
    sys.stderr.write(f"{len(lines):,} lines after stitching, {len(areas):,} areas, {len(pois):,} POIs, "
                     f"{len(places):,} names; packing...\n")

    if not args.out:
        base = args.region or os.path.basename(pbf)
        base = base.rsplit("/", 1)[-1].removesuffix(".osm.pbf").removesuffix("-latest")
        args.out = (re.sub(r"[^a-z0-9]+", "-", base.lower()).strip("-")[:24] or "region") + ".map"
    prep.build_pack(lines, areas, pois, places, lat0, lon0, (south, west, north, east), args.out)
    sys.stderr.write(f"built in {(time.time() - t0) / 60:.1f} min\n")

    if args.copy_to:
        maps = os.path.join(args.copy_to, "maps")
        os.makedirs(maps, exist_ok=True)
        dest = os.path.join(maps, os.path.basename(args.out))
        shutil.copyfile(args.out, dest)
        print(f"copied to {dest}")
    else:
        print("Copy it into the maps folder on the node's microSD card.")
    print("Map data (c) OpenStreetMap contributors, ODbL.")


if __name__ == "__main__":
    main()
