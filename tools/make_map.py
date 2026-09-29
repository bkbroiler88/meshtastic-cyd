#!/usr/bin/env python3
"""Make a basemap for the map frame from a place name.

The firmware ships without map data: you supply it. This wraps prepare_basemap.py with
geocoding so you do not have to look up coordinates.

    python tools/make_map.py "Edgewood, MD"
    python tools/make_map.py "Boulder, Colorado" --km 30
    python tools/make_map.py --lat 39.4187 --lon -76.2944 --km 24
    python tools/make_map.py "Edgewood, MD" --copy-to E:\\        # straight onto the SD card

Put the resulting basemap.bin in the root of the node's microSD card. The card is only
mounted while Bluetooth is off, so turn Bluetooth off (Menu > Bluetooth > Disabled) to use
maps; with Bluetooth on the map frame says so.

Map data (c) OpenStreetMap contributors, ODbL (openstreetmap.org/copyright).
"""

import argparse
import importlib.util
import json
import math
import os
import shutil
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

# int16 metres relative to the box centre: the half-width has to stay inside 32767.
MAX_KM = 64.0
# the name blob is addressed by a uint16 offset, so ~64 KiB of street names is the ceiling.
NAME_BLOB_LIMIT = 65535


def geocode(place):
    """Nominatim. One request, identified per their usage policy."""
    url = "https://nominatim.openstreetmap.org/search?" + urllib.parse.urlencode(
        {"q": place, "format": "json", "limit": 1})
    req = urllib.request.Request(url, headers={
        "User-Agent": "meshtastic-cyd-make-map/1.0 (github.com/bkbroiler88/meshtastic-cyd)"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        hits = json.load(resp)
    if not hits:
        raise SystemExit(f'Could not find "{place}". Try adding a state or country, '
                         f'or pass --lat/--lon directly.')
    hit = hits[0]
    # flush: the packer writes progress to stderr, which is unbuffered, so without this
    # our messages arrive after its output and the run reads out of order.
    print(f'Found: {hit["display_name"]}', flush=True)
    return float(hit["lat"]), float(hit["lon"])


def main():
    ap = argparse.ArgumentParser(
        description="Build a basemap for the Meshtastic map frame.",
        epilog="Map data (c) OpenStreetMap contributors, ODbL.")
    ap.add_argument("place", nargs="?", help='e.g. "Edgewood, MD"')
    ap.add_argument("--lat", type=float, help="instead of a place name")
    ap.add_argument("--lon", type=float)
    ap.add_argument("--km", type=float, default=24.0, help="box width in km (default 24)")
    ap.add_argument("--out", default="basemap.bin")
    ap.add_argument("--copy-to", help="also copy the result here, e.g. the SD card root")
    ap.add_argument("--no-local", action="store_true",
                    help="skip residential streets: smaller file, major roads only")
    ap.add_argument("--grid", type=int, default=16)
    ap.add_argument("--cache", help="keep the raw OpenStreetMap download here to reuse")
    args = ap.parse_args()

    if args.lat is None or args.lon is None:
        if not args.place:
            ap.error('give a place name, e.g. "Edgewood, MD", or --lat and --lon')
        lat, lon = geocode(args.place)
    else:
        lat, lon = args.lat, args.lon

    if args.km > MAX_KM:
        raise SystemExit(f"--km {args.km} is too wide; the format stores positions as int16 "
                         f"metres from the centre, so the limit is {MAX_KM:.0f} km.")
    if args.km > 40:
        print(f"note: {args.km:.0f} km is a large area. If it fails on the name blob limit, "
              f"rerun with --no-local.")

    print(f"Centre {lat:.5f}, {lon:.5f}  |  {args.km:.0f} km box  |  downloading from "
          f"OpenStreetMap, this takes a minute...", flush=True)

    spec = importlib.util.spec_from_file_location("prep", os.path.join(HERE, "prepare_basemap.py"))
    prep = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prep)

    argv = ["prepare_basemap.py", "--lat", str(lat), "--lon", str(lon), "--km", str(args.km),
            "--grid", str(args.grid), "--out", args.out]
    if args.no_local:
        argv.append("--no-local")
    if args.cache:
        argv += ["--cache", args.cache]

    saved, sys.argv = sys.argv, argv
    try:
        prep.main()
    except SystemExit as e:
        if e.code:
            if "name blob" in str(e):
                raise SystemExit(f"{e}\n\nToo many street names for this area. Rerun with "
                                 f"--no-local, or a smaller --km.")
            raise
    finally:
        sys.argv = saved

    size = os.path.getsize(args.out)
    print(f"\n{args.out}  ({size / 1024:.0f} KiB)")

    if args.copy_to:
        dest = args.copy_to
        if os.path.isdir(dest):
            dest = os.path.join(dest, "basemap.bin")
        shutil.copyfile(args.out, dest)
        print(f"copied to {dest}")
        print("Eject the card, put it in the node, and turn Bluetooth off to use maps.")
    else:
        print("Copy it to the root of the node's microSD card as basemap.bin.")
        print("The card is only mounted while Bluetooth is off (Menu > Bluetooth > Disabled).")
    print("\nMap data (c) OpenStreetMap contributors, ODbL.")


if __name__ == "__main__":
    main()
