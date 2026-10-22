/*
 * Map pack builder (pack version 5) for the Meshtastic CYD map frame, in the browser.
 *
 * A line-for-line port of tools/prepare_basemap.py (extract_v5 / stitch / build_pack): the
 * same Overpass query, the same simplification, tiling and byte layout, so a pack built here
 * matches one built by the Python tool from the same download. Python's round() is
 * half-to-even and math.radians() is x * (pi/180); both are mirrored below because they
 * decide individual bytes.
 *
 * Map data (c) OpenStreetMap contributors, ODbL.
 */
(function (root) {
  'use strict';

  const NAME_NONE = 0xffff;
  const NAME_MAX_BYTES = 47;
  const LAYER_WATER = 0, LAYER_MAJOR = 1, LAYER_MINOR = 2, LAYER_LOCAL = 3, LAYER_PATH = 4, LAYER_RAIL = 5;
  const HIGHWAY_LAYER = {
    motorway: LAYER_MAJOR, motorway_link: LAYER_MAJOR, trunk: LAYER_MAJOR, trunk_link: LAYER_MAJOR,
    primary: LAYER_MAJOR, primary_link: LAYER_MAJOR, secondary: LAYER_MINOR, secondary_link: LAYER_MINOR,
    tertiary: LAYER_MINOR, tertiary_link: LAYER_MINOR, residential: LAYER_LOCAL, unclassified: LAYER_LOCAL,
  };
  const SHIFTS = [1, 3, 5];
  const UNITS = 16384;
  const MIN_LEN = [1500.0, 200.0, 0.0];
  const LINE_ORDER = { 0: 0, 4: 1, 3: 2, 5: 3, 2: 4, 1: 5 };
  const LINE_TOL = [
    { 0: 120.0, 1: 80.0 },
    { 0: 25.0, 1: 12.0, 2: 12.0, 5: 15.0 },
    { 0: 8.0, 1: 4.0, 2: 4.0, 3: 6.0, 4: 4.0, 5: 6.0 },
  ];
  const AREA_WATER = 0, AREA_PARK = 1, AREA_WOOD = 2;
  const AREA_TOL = [{ 0: 150.0 }, { 0: 30.0, 1: 30.0, 2: 40.0 }, { 0: 6.0, 1: 6.0, 2: 10.0 }];
  const AREA_MIN_M2 = [500000.0, 50000.0, 1500.0];
  const POI_KINDS = [
    ['amenity', 'hospital', 1], ['amenity', 'fuel', 2], ['amenity', 'parking', 3],
    ['amenity', 'drinking_water', 4], ['amenity', 'shelter', 5], ['natural', 'peak', 6],
    ['amenity', 'toilets', 7], ['tourism', 'camp_site', 8], ['tourism', 'viewpoint', 9],
  ];
  const PLACE_KINDS = { city: 0, town: 1, village: 2, suburb: 3, hamlet: 3, neighbourhood: 3 };
  const PLACE_WATER = 4;
  const PLACE_LEVELS = { 0: [0, 1, 2], 1: [0, 1, 2], 2: [1, 2], 3: [2], 4: [1, 2] };
  const DEG = Math.PI / 180; // math.radians(x) == x * (pi / 180)

  /** Python round(): half to even. */
  function pyRound(x) {
    const f = Math.floor(x), d = x - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return f % 2 === 0 ? f : f + 1;
  }
  /** Key for Python's (round(x, 2), round(y, 2)) equality; -0 and 0 compare equal there too. */
  function key2(p) {
    return String(Number(p[0].toFixed(2))) + '|' + String(Number(p[1].toFixed(2)));
  }
  function dist(a, b) { return Math.hypot(a[0] - b[0], a[1] - b[1]); }
  function pathLen(pts) { let s = 0; for (let i = 0; i + 1 < pts.length; i++) s += dist(pts[i], pts[i + 1]); return s; }
  function truncChars(s, n) { return Array.from(s).slice(0, n).join(''); }

  function bboxFor(lat, lon, km) {
    const halfKm = km / 2.0;
    const dlat = halfKm / 111.132;
    const dlon = halfKm / (111.320 * Math.cos(lat * DEG));
    return { south: lat - dlat, west: lon - dlon, north: lat + dlat, east: lon + dlon };
  }

  function buildQuery(b, includeLocal) {
    const bb = [b.south, b.west, b.north, b.east].map((v) => v.toFixed(6)).join(',');
    const hw = Object.keys(HIGHWAY_LAYER).filter((k) => includeLocal || HIGHWAY_LAYER[k] !== LAYER_LOCAL).join('|');
    const paths = includeLocal
      ? `\n  way["highway"~"^(path|track|bridleway|cycleway)$"](${bb});\n  way["highway"="footway"]["footway"!~"."](${bb});`
      : '';
    return `
[out:json][timeout:300];
(
  way["highway"~"^(${hw})$"](${bb});${paths}
  way["railway"~"^(rail|light_rail|subway|narrow_gauge)$"]["service"!~"."](${bb});
  way["natural"="coastline"](${bb});
  way["natural"="water"](${bb});
  way["waterway"~"^(river|stream)$"](${bb});
  way["landuse"="reservoir"](${bb});
  way["leisure"~"^(park|nature_reserve|golf_course|recreation_ground)$"](${bb});
  way["landuse"~"^(forest|recreation_ground)$"](${bb});
  way["natural"="wood"](${bb});
  relation["type"="multipolygon"]["natural"~"^(water|wood)$"](${bb});
  relation["type"="multipolygon"]["landuse"~"^(forest|reservoir)$"](${bb});
  relation["type"="multipolygon"]["leisure"~"^(park|nature_reserve)$"](${bb});
  node["place"~"^(city|town|village|suburb|hamlet|neighbourhood)$"](${bb});
  node["amenity"~"^(hospital|fuel|parking|drinking_water|shelter|toilets)$"](${bb});
  way["amenity"~"^(hospital|fuel|parking)$"](${bb});
  node["natural"="peak"](${bb});
  node["tourism"~"^(camp_site|viewpoint)$"](${bb});
);
out body geom;
`;
  }

  // --- classification (prepare_basemap.py classify / road_label / area_kind / poi_kind) ----

  function classify(tags) {
    if ('highway' in tags) return HIGHWAY_LAYER.hasOwnProperty(tags.highway) ? HIGHWAY_LAYER[tags.highway] : null;
    if (tags.natural === 'coastline' || tags.natural === 'water') return LAYER_WATER;
    if (tags.waterway === 'river' || tags.waterway === 'stream') return LAYER_WATER;
    if (tags.landuse === 'reservoir') return LAYER_WATER;
    return null;
  }
  function compactRef(ref) { return ref.split(';')[0].trim().split(/\s+/).filter(Boolean).join('-'); }
  function roadLabel(tags, layer) {
    const ref = (tags.ref || '').trim();
    const name = (tags.name || '').trim();
    let label;
    if (layer === LAYER_MAJOR && ref) label = compactRef(ref);
    else label = name || (ref ? compactRef(ref) : '');
    return truncChars(label, NAME_MAX_BYTES);
  }
  function areaKind(tags) {
    if (tags.natural === 'water' || tags.landuse === 'reservoir') return AREA_WATER;
    if (['park', 'nature_reserve', 'golf_course', 'recreation_ground'].includes(tags.leisure) ||
        tags.landuse === 'recreation_ground') return AREA_PARK;
    if (tags.natural === 'wood' || tags.landuse === 'forest') return AREA_WOOD;
    return null;
  }
  function poiKind(tags) {
    for (const [k, v, kind] of POI_KINDS) if (tags[k] === v) return kind;
    return null;
  }
  function ringArea(r) {
    let a = 0;
    for (let i = 0; i < r.length; i++) {
      const p = r[i], q = r[(i + 1) % r.length];
      a += p[0] * q[1] - q[0] * p[1];
    }
    return Math.abs(a) / 2.0;
  }

  function joinRings(segments) {
    const rings = [];
    const open = segments.filter((s) => s.length >= 2).map((s) => s.slice());
    while (open.length) {
      let cur = open.pop();
      let changed = true;
      while (key2(cur[0]) !== key2(cur[cur.length - 1]) && changed) {
        changed = false;
        for (let i = 0; i < open.length; i++) {
          const o = open[i];
          const oF = key2(o[0]), oL = key2(o[o.length - 1]), cF = key2(cur[0]), cL = key2(cur[cur.length - 1]);
          if (oF === cL) cur = cur.concat(o.slice(1));
          else if (oL === cL) cur = cur.concat(o.slice().reverse().slice(1));
          else if (oL === cF) cur = o.slice(0, -1).concat(cur);
          else if (oF === cF) cur = o.slice().reverse().slice(0, -1).concat(cur);
          else continue;
          open.splice(i, 1);
          changed = true;
          break;
        }
      }
      if (cur.length >= 4 && key2(cur[0]) === key2(cur[cur.length - 1])) rings.push(cur.slice(0, -1));
    }
    return rings;
  }

  function extract(elements, lat0, lon0, includeLocal) {
    const mLat = 111132.0, mLon = 111320.0 * Math.cos(lat0 * DEG);
    const prj = (g) => [(g.lon - lon0) * mLon, (g.lat - lat0) * mLat];
    const lines = [], areas = [], pois = [], places = [];
    for (const el of elements) {
      const tags = el.tags || {};
      const t = el.type;
      if (t === 'node') {
        const pt = prj(el);
        const pk = poiKind(tags);
        if (pk) pois.push([pk, truncChars(tags.name || '', NAME_MAX_BYTES), pt]);
        const pl = PLACE_KINDS.hasOwnProperty(tags.place) ? PLACE_KINDS[tags.place] : null;
        if (pl !== null && tags.name) places.push([pl, truncChars(tags.name, NAME_MAX_BYTES), pt]);
        continue;
      }
      if (t === 'way' && el.geometry) {
        const pts = el.geometry.map(prj);
        if (pts.length < 2) continue;
        const hw = tags.highway;
        if (['path', 'track', 'bridleway', 'cycleway', 'footway'].includes(hw)) {
          if (includeLocal) lines.push([LAYER_PATH, '', pts]);
          continue;
        }
        if ('railway' in tags) {
          lines.push([LAYER_RAIL, '', pts]);
          continue;
        }
        const layer = classify(tags);
        if (layer !== null) {
          const name = layer !== LAYER_WATER ? roadLabel(tags, layer) : '';
          lines.push([layer, name, pts]);
          if (tags.waterway === 'river' && tags.name)
            places.push([PLACE_WATER, truncChars(tags.name, NAME_MAX_BYTES), pts[Math.floor(pts.length / 2)]]);
        }
        const closed = pts.length >= 4 && dist(pts[0], pts[pts.length - 1]) < 0.5;
        const ak = areaKind(tags);
        if (ak !== null && closed) {
          const ring = pts.slice(0, -1);
          areas.push([ak, [ring]]);
          if (ak === AREA_WATER && tags.name) {
            let cx = 0, cy = 0;
            for (const p of ring) { cx += p[0]; cy += p[1]; }
            places.push([PLACE_WATER, truncChars(tags.name, NAME_MAX_BYTES), [cx / ring.length, cy / ring.length]]);
          }
        }
        const pk = poiKind(tags);
        if (pk) {
          let cx = 0, cy = 0;
          for (const p of pts) { cx += p[0]; cy += p[1]; }
          pois.push([pk, truncChars(tags.name || '', NAME_MAX_BYTES), [cx / pts.length, cy / pts.length]]);
        }
        continue;
      }
      if (t === 'relation') {
        const ak = areaKind(tags);
        if (ak === null) continue;
        const segs = (el.members || []).filter((m) => m.type === 'way' && m.geometry && m.geometry.length)
          .map((m) => m.geometry.map(prj));
        const rings = joinRings(segs);
        if (rings.length) {
          areas.push([ak, rings]);
          if (ak === AREA_WATER && tags.name) {
            let big = rings[0];
            for (const r of rings) if (ringArea(r) > ringArea(big)) big = r;
            let cx = 0, cy = 0;
            for (const p of big) { cx += p[0]; cy += p[1]; }
            places.push([PLACE_WATER, truncChars(tags.name, NAME_MAX_BYTES), [cx / big.length, cy / big.length]]);
          }
        }
      }
    }
    return { lines, areas, pois, places };
  }

  /** Join ways that share an endpoint, grouped by (layer, name). */
  function stitch(ways) {
    const groups = new Map();
    for (const [layer, name, pts] of ways) {
      const k = layer + '\u0000' + name;
      if (!groups.has(k)) groups.set(k, { layer, name, list: [] });
      groups.get(k).list.push(pts);
    }
    const out = [];
    for (const { layer, name, list } of groups.values()) {
      const ends = new Map();
      const addEnd = (k, i) => { if (!ends.has(k)) ends.set(k, []); ends.get(k).push(i); };
      list.forEach((pts, i) => { addEnd(key2(pts[0]), i); addEnd(key2(pts[pts.length - 1]), i); });
      const used = new Array(list.length).fill(false);
      for (let i = 0; i < list.length; i++) {
        if (used[i]) continue;
        used[i] = true;
        let chain = list[i].slice();
        for (const forward of [true, false]) {
          for (;;) {
            const k = key2(forward ? chain[chain.length - 1] : chain[0]);
            let nxt = -1;
            for (const j of ends.get(k) || []) if (!used[j]) { nxt = j; break; }
            if (nxt < 0) break;
            used[nxt] = true;
            const cand = list[nxt];
            if (forward) chain = chain.concat(key2(cand[0]) === k ? cand.slice(1) : cand.slice().reverse().slice(1));
            else chain = (key2(cand[cand.length - 1]) === k ? cand.slice(0, -1) : cand.slice().reverse().slice(0, -1)).concat(chain);
          }
        }
        out.push([layer, name, chain]);
      }
    }
    return out;
  }

  function perpDist(p, a, b) {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    if (dx === 0 && dy === 0) return Math.hypot(p[0] - a[0], p[1] - a[1]);
    const t = Math.max(0.0, Math.min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy)));
    return Math.hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
  }
  /** Iterative Douglas-Peucker, same traversal order as the Python. */
  function simplify(pts, tol) {
    if (pts.length < 3) return pts;
    const keep = new Array(pts.length).fill(false);
    keep[0] = keep[pts.length - 1] = true;
    const stack = [[0, pts.length - 1]];
    while (stack.length) {
      const [lo, hi] = stack.pop();
      if (hi <= lo + 1) continue;
      let worst = 0.0, wi = -1;
      for (let i = lo + 1; i < hi; i++) {
        const d = perpDist(pts[i], pts[lo], pts[hi]);
        if (d > worst) { worst = d; wi = i; }
      }
      if (worst > tol) { keep[wi] = true; stack.push([lo, wi]); stack.push([wi, hi]); }
    }
    return pts.filter((_, i) => keep[i]);
  }

  /** Liang-Barsky. */
  function clipSeg(x0, y0, x1, y1, xmin, ymin, xmax, ymax) {
    const dx = x1 - x0, dy = y1 - y0;
    let t0 = 0.0, t1 = 1.0;
    for (const [p, q] of [[-dx, x0 - xmin], [dx, xmax - x0], [-dy, y0 - ymin], [dy, ymax - y0]]) {
      if (p === 0) { if (q < 0) return null; continue; }
      const r = q / p;
      if (p < 0) { if (r > t1) return null; t0 = Math.max(t0, r); }
      else { if (r < t0) return null; t1 = Math.min(t1, r); }
    }
    return [x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy];
  }

  function splitIntoTiles(lonlat, shift) {
    const scale = 2 ** shift;
    const out = new Map(), lastEnd = new Map();
    for (let s = 0; s + 1 < lonlat.length; s++) {
      const [ax, ay] = lonlat[s], [bx, by] = lonlat[s + 1];
      const gx0 = (ax + 180.0) * scale, gy0 = (ay + 90.0) * scale, gx1 = (bx + 180.0) * scale, gy1 = (by + 90.0) * scale;
      for (let ix = Math.floor(Math.min(gx0, gx1)); ix <= Math.floor(Math.max(gx0, gx1)); ix++) {
        for (let iy = Math.floor(Math.min(gy0, gy1)); iy <= Math.floor(Math.max(gy0, gy1)); iy++) {
          const c = clipSeg(gx0, gy0, gx1, gy1, ix, iy, ix + 1, iy + 1);
          if (!c) continue;
          const p0 = [pyRound((c[0] - ix) * UNITS), pyRound((c[1] - iy) * UNITS)];
          const p1 = [pyRound((c[2] - ix) * UNITS), pyRound((c[3] - iy) * UNITS)];
          if (p0[0] === p1[0] && p0[1] === p1[1]) continue;
          const tk = ix + ',' + iy;
          if (!out.has(tk)) out.set(tk, { ix, iy, runs: [] });
          const runs = out.get(tk).runs;
          const le = lastEnd.get(tk);
          if (le && le[0] === p0[0] && le[1] === p0[1] && runs.length) runs[runs.length - 1].push(p1);
          else runs.push([p0, p1]);
          lastEnd.set(tk, p1);
        }
      }
    }
    return out;
  }

  /** Sutherland-Hodgman against an axis-aligned rectangle. */
  function clipPoly(ring, xmin, ymin, xmax, ymax) {
    function clip(pts, inside, inter) {
      const out = [];
      if (!pts.length) return out;
      let prev = pts[pts.length - 1];
      for (const cur of pts) {
        if (inside(cur)) { if (!inside(prev)) out.push(inter(prev, cur)); out.push(cur); }
        else if (inside(prev)) out.push(inter(prev, cur));
        prev = cur;
      }
      return out;
    }
    const ixf = (a, b, x) => { const t = (x - a[0]) / (b[0] - a[0]); return [x, a[1] + t * (b[1] - a[1])]; };
    const iyf = (a, b, y) => { const t = (y - a[1]) / (b[1] - a[1]); return [a[0] + t * (b[0] - a[0]), y]; };
    let pts = ring;
    pts = clip(pts, (p) => p[0] >= xmin, (a, b) => ixf(a, b, xmin));
    pts = clip(pts, (p) => p[0] <= xmax, (a, b) => ixf(a, b, xmax));
    pts = clip(pts, (p) => p[1] >= ymin, (a, b) => iyf(a, b, ymin));
    pts = clip(pts, (p) => p[1] <= ymax, (a, b) => iyf(a, b, ymax));
    return pts;
  }

  class Bytes {
    constructor() { this.a = []; }
    u8(v) { this.a.push(v & 0xff); }
    u16(v) { this.a.push(v & 0xff, (v >> 8) & 0xff); }
    i16(v) { this.u16(v & 0xffff); }
    u32(v) { this.a.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff); }
    i32(v) { this.u32(v >>> 0); }
    bytes(arr) { for (const b of arr) this.a.push(b); }
  }

  function buildPack(c, lat0, lon0, b, progress) {
    const mLat = 111132.0, mLon = 111320.0 * Math.cos(lat0 * DEG);
    const ll = (p) => [lon0 + p[0] / mLon, lat0 + p[1] / mLat];
    const inBbox = (q) => b.west <= q[0] && q[0] <= b.east && b.south <= q[1] && q[1] <= b.north;
    function clipBboxLine(pts) {
      const runs = [];
      let cur = [];
      for (let i = 0; i + 1 < pts.length; i++) {
        const cl = clipSeg(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], b.west, b.south, b.east, b.north);
        if (!cl) { if (cur.length >= 2) runs.push(cur); cur = []; continue; }
        const A = [cl[0], cl[1]], B = [cl[2], cl[3]];
        if (cur.length && cur[cur.length - 1][0] === A[0] && cur[cur.length - 1][1] === A[1]) cur.push(B);
        else { if (cur.length >= 2) runs.push(cur); cur = [A, B]; }
      }
      if (cur.length >= 2) runs.push(cur);
      return runs;
    }
    function tileOf(q, shift) {
      const sc = 2 ** shift;
      const gx = (q[0] + 180.0) * sc, gy = (q[1] + 90.0) * sc;
      const ix = Math.floor(gx), iy = Math.floor(gy);
      return [ix, iy, [pyRound((gx - ix) * UNITS), pyRound((gy - iy) * UNITS)]];
    }
    const tiles = new Map();
    const tile = (lv, ix, iy) => {
      const k = lv + ',' + ix + ',' + iy;
      if (!tiles.has(k)) tiles.set(k, { lv, ix, iy, lines: [], areas: [], pois: [], places: [] });
      return tiles.get(k);
    };

    SHIFTS.forEach((shift, level) => {
      if (progress) progress(`Tiling level ${level + 1} of 3`);
      const sc = 2 ** shift;
      for (const [layer, name, pts] of c.lines) {
        const tol = LINE_TOL[level][layer];
        if (tol === undefined) continue;
        if (MIN_LEN[level] > 0 && pathLen(pts) < MIN_LEN[level]) continue;
        const simp = simplify(pts, tol);
        if (simp.length < 2) continue;
        const keepName = (level > 0 || layer === LAYER_MAJOR) ? name : '';
        for (const run of clipBboxLine(simp.map(ll)))
          for (const { ix, iy, runs } of splitIntoTiles(run, shift).values())
            for (const tr of runs) tile(level, ix, iy).lines.push([layer, keepName, tr]);
      }
      for (const [kind, rings] of c.areas) {
        const tol = AREA_TOL[level][kind];
        if (tol === undefined) continue;
        let tot = 0;
        for (const r of rings) tot += ringArea(r);
        if (tot < AREA_MIN_M2[level]) continue;
        const simpRings = [];
        for (const r of rings) {
          const sr = simplify(r.concat([r[0]]), tol).slice(0, -1);
          if (sr.length >= 3) simpRings.push(sr.map(ll));
        }
        if (!simpRings.length) continue;
        let mnx = Infinity, mxx = -Infinity, mny = Infinity, mxy = -Infinity;
        for (const r of simpRings) for (const p of r) {
          mnx = Math.min(mnx, p[0]); mxx = Math.max(mxx, p[0]); mny = Math.min(mny, p[1]); mxy = Math.max(mxy, p[1]);
        }
        const x0 = Math.max(b.west, mnx), x1 = Math.min(b.east, mxx), y0 = Math.max(b.south, mny), y1 = Math.min(b.north, mxy);
        if (x0 >= x1 || y0 >= y1) continue;
        for (let ix = Math.floor((x0 + 180) * sc); ix <= Math.floor((x1 + 180) * sc); ix++) {
          for (let iy = Math.floor((y0 + 90) * sc); iy <= Math.floor((y1 + 90) * sc); iy++) {
            const tw = ix / sc - 180.0, ts = iy / sc - 90.0;
            const rect = [Math.max(tw, b.west), Math.max(ts, b.south), Math.min(tw + 1 / sc, b.east), Math.min(ts + 1 / sc, b.north)];
            const outRings = [];
            for (const r of simpRings) {
              const cp = clipPoly(r, rect[0], rect[1], rect[2], rect[3]);
              if (cp.length >= 3) {
                const q = cp.map((p) => [pyRound((p[0] - tw) * sc * UNITS), pyRound((p[1] - ts) * sc * UNITS)]);
                const dedup = [q[0]];
                for (let i = 1; i < q.length; i++) if (q[i][0] !== q[i - 1][0] || q[i][1] !== q[i - 1][1]) dedup.push(q[i]);
                if (dedup.length >= 3) outRings.push(dedup);
              }
            }
            if (outRings.length) tile(level, ix, iy).areas.push([kind, outRings.slice(0, 8)]);
          }
        }
      }
      if (level === SHIFTS.length - 1) {
        for (const [kind, name, pt] of c.pois) {
          const q = ll(pt);
          if (inBbox(q)) { const [ix, iy, u] = tileOf(q, shift); tile(level, ix, iy).pois.push([kind, name, u]); }
        }
      }
      for (const [kind, name, pt] of c.places) {
        if (!(PLACE_LEVELS[kind] || []).includes(level)) continue;
        const q = ll(pt);
        if (inBbox(q)) { const [ix, iy, u] = tileOf(q, shift); tile(level, ix, iy).places.push([kind, name, u]); }
      }
    });

    const enc = new TextEncoder();
    function packTile(t) {
      const blob = [], ids = new Map();
      const nid = (name) => {
        if (!name) return NAME_NONE;
        if (!ids.has(name)) {
          const e = Array.from(enc.encode(name)).slice(0, NAME_MAX_BYTES);
          ids.set(name, blob.length);
          blob.push(e.length, ...e);
        }
        return ids.get(name);
      };
      const body = new Bytes();
      const areaOrder = { 2: 0, 1: 1, 0: 2 };
      const recs = [];
      const sortedAreas = t.areas.map((a, i) => [a, i]).sort((x, y) => (areaOrder[x[0][0]] - areaOrder[y[0][0]]) || (x[1] - y[1])).map((x) => x[0]);
      for (const [kind, rings] of sortedAreas) {
        const ringBytes = (rs) => {
          const d = new Bytes();
          d.u8(rs.length);
          for (const r of rs) { d.u16(r.length); for (const [x, y] of r) { d.i16(x); d.i16(y); } }
          return d.a;
        };
        let data = ringBytes(rings);
        if (data.length <= 1024) recs.push([kind, data]);
        else {
          const step = Math.ceil(data.length / 1000);
          data = ringBytes(rings.map((r) => (r.length > 3 * step ? r.filter((_, i) => i % step === 0) : r)));
          if (data.length <= 1024) recs.push([kind, data]);
        }
      }
      body.u16(recs.length);
      for (const [kind, data] of recs) { body.u8(kind); body.u16(data.length); body.bytes(data); }
      const lrecs = [];
      for (const [layer, name, pts] of t.lines)
        for (let i = 0; i < pts.length - 1; i += 65000) {
          const chunk = pts.slice(i, i + 65001);
          if (chunk.length >= 2) lrecs.push([layer, name, chunk]);
        }
      const lsorted = lrecs.map((r, i) => [r, i]).sort((x, y) => (LINE_ORDER[x[0][0]] - LINE_ORDER[y[0][0]]) || (x[1] - y[1])).map((x) => x[0]);
      body.u16(lsorted.length);
      for (const [layer, name, pts] of lsorted) {
        body.u8(layer); body.u16(pts.length); body.u16(nid(name));
        for (const [x, y] of pts) { body.i16(x); body.i16(y); }
      }
      body.u16(t.pois.length);
      for (const [kind, name, [x, y]] of t.pois) { body.u8(kind); body.u16(nid(name)); body.i16(x); body.i16(y); }
      body.u16(t.places.length);
      const psorted = t.places.map((p, i) => [p, i]).sort((x, y) => (x[0][0] - y[0][0]) || (x[1] - y[1])).map((x) => x[0]);
      for (const [kind, name, [x, y]] of psorted) { body.u8(kind); body.u16(nid(name)); body.i16(x); body.i16(y); }
      if (blob.length > 0xfffe) throw new Error("a tile's name blob overflowed");
      const out = new Bytes();
      out.u16(blob.length); out.bytes(blob); out.bytes(body.a);
      return out.a;
    }

    if (progress) progress('Packing tiles');
    const entries = [];
    for (const t of tiles.values()) entries.push([((t.lv << 28) | (t.iy << 14) | t.ix) >>> 0, packTile(t)]);
    entries.sort((x, y) => x[0] - y[0]);

    const headerLen = 48;
    let off = headerLen + 12 * entries.length;
    const total = off + entries.reduce((s, e) => s + e[1].length, 0);
    const out = new Uint8Array(total);
    const dv = new DataView(out.buffer);
    out.set([0x4d, 0x41, 0x50, 0x34], 0); // "MAP4"
    out[4] = 5;
    out[5] = SHIFTS.length;
    out.set(SHIFTS, 6);
    dv.setInt32(12, pyRound(b.south * 1e7), true);
    dv.setInt32(16, pyRound(b.west * 1e7), true);
    dv.setInt32(20, pyRound(b.north * 1e7), true);
    dv.setInt32(24, pyRound(b.east * 1e7), true);
    dv.setUint32(28, entries.length, true);
    dv.setUint32(32, headerLen, true);
    let dir = headerLen;
    for (const [k, blob] of entries) {
      dv.setUint32(dir, k, true);
      dv.setUint32(dir + 4, off, true);
      dv.setUint32(dir + 8, blob.length, true);
      out.set(blob, off);
      dir += 12;
      off += blob.length;
    }
    const levels = [0, 1, 2].map((lv) => entries.filter((e) => e[0] >>> 28 === lv).map((e) => e[1].length));
    return {
      bytes: out,
      stats: {
        tiles: entries.length,
        levels: levels.map((s) => ({ tiles: s.length, largest: s.length ? Math.max(...s) : 0 })),
        lines: c.lines.length, areas: c.areas.length, pois: c.pois.length, places: c.places.length,
      },
    };
  }

  /** Overpass JSON -> pack. lat/lon is the box centre, km its width. */
  function build(overpassJson, lat, lon, km, includeLocal, progress) {
    const b = bboxFor(lat, lon, km);
    if (progress) progress('Sorting roads, water, parks and places');
    const c = extract(overpassJson.elements || [], lat, lon, includeLocal);
    if (progress) progress('Joining road segments');
    c.lines = stitch(c.lines);
    return buildPack(c, lat, lon, b, progress);
  }

  /** Read a pack back, for the preview: calls visit(level, lon0, lat0, span, tileBytes, offset). */
  function decode(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) !== 'MAP4') throw new Error('not a map pack');
    const shifts = [bytes[6], bytes[7], bytes[8]];
    const box = { south: dv.getInt32(12, true) * 1e-7, west: dv.getInt32(16, true) * 1e-7, north: dv.getInt32(20, true) * 1e-7, east: dv.getInt32(24, true) * 1e-7 };
    const count = dv.getUint32(28, true), dirOff = dv.getUint32(32, true);
    const tiles = [];
    for (let i = 0; i < count; i++) {
      const k = dv.getUint32(dirOff + i * 12, true);
      const lv = k >>> 28, iy = (k >>> 14) & 0x3fff, ix = k & 0x3fff;
      const span = 1 / 2 ** shifts[lv];
      tiles.push({ lv, lon0: ix * span - 180, lat0: iy * span - 90, span, off: dv.getUint32(dirOff + i * 12 + 4, true), len: dv.getUint32(dirOff + i * 12 + 8, true) });
    }
    return { box, tiles, version: bytes[4], dv };
  }

  const api = { build, buildQuery, bboxFor, decode, pyRound };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MapPack = api;
})(typeof window !== 'undefined' ? window : this);
