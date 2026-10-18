/**
 * Glyph outlines for the depot bay legend (#318): the maintainer chose Roboto Black (900) for LOADING / UNLOADING. The letters are
 * painted into MKI Roads' road-marking mesh, so their outlines are read from the font file once, flattened and
 * triangulated here, and written to a generated C++ header (scripts/road-pack-glyphs.ts). Roboto is under the Apache
 * License 2.0; the engine ships the font file (Engine/Content/Slate/Fonts/Roboto-Black.ttf).
 *
 * Only what those letters need of TrueType: the table directory, head, maxp, hhea, hmtx, cmap format 4, loca and simple
 * glyf outlines (quadratic curves). Coordinates are returned in units of the font's cap height, y up.
 */

export interface GlyphMesh { letter: string; advance: number; triangles: number[] }

function table(font: Buffer, tag: string): number {
  const count = font.readUInt16BE(4);
  for (let index = 0; index < count; index++) {
    const record = 12 + index * 16;
    if (font.toString("latin1", record, record + 4) === tag) return font.readUInt32BE(record + 8);
  }
  throw new Error(`font has no ${tag} table`);
}

function glyphIndex(font: Buffer, code: number): number {
  const cmap = table(font, "cmap"), tables = font.readUInt16BE(cmap + 2);
  for (let index = 0; index < tables; index++) {
    const platform = font.readUInt16BE(cmap + 4 + index * 8), encoding = font.readUInt16BE(cmap + 6 + index * 8);
    const sub = cmap + font.readUInt32BE(cmap + 8 + index * 8);
    if (platform !== 3 || encoding !== 1 || font.readUInt16BE(sub) !== 4) continue;
    const segments = font.readUInt16BE(sub + 6) / 2, ends = sub + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, offsets = deltas + segments * 2;
    for (let segment = 0; segment < segments; segment++) {
      const end = font.readUInt16BE(ends + segment * 2), start = font.readUInt16BE(starts + segment * 2);
      if (code < start || code > end) continue;
      const delta = font.readInt16BE(deltas + segment * 2), rangeOffset = font.readUInt16BE(offsets + segment * 2);
      if (rangeOffset === 0) return (code + delta) & 0xffff;
      const glyph = font.readUInt16BE(offsets + segment * 2 + rangeOffset + (code - start) * 2);
      return glyph === 0 ? 0 : (glyph + delta) & 0xffff;
    }
  }
  throw new Error(`font maps no glyph for ${String.fromCharCode(code)}`);
}

/** The glyph's contours as closed polylines (quadratic curves flattened into `steps` pieces), in font units. */
function contours(font: Buffer, glyph: number, steps: number): number[][] {
  const head = table(font, "head"), loca = table(font, "loca"), glyf = table(font, "glyf");
  const long = font.readInt16BE(head + 50) === 1;
  const offset = long ? font.readUInt32BE(loca + glyph * 4) : font.readUInt16BE(loca + glyph * 2) * 2;
  const next = long ? font.readUInt32BE(loca + glyph * 4 + 4) : font.readUInt16BE(loca + glyph * 2 + 2) * 2;
  if (next === offset) return [];
  let at = glyf + offset;
  const count = font.readInt16BE(at);
  if (count < 0) throw new Error("composite glyphs are not supported");
  at += 10;
  const ends: number[] = [];
  for (let index = 0; index < count; index++) { ends.push(font.readUInt16BE(at)); at += 2; }
  const points = ends[ends.length - 1] + 1;
  at += 2 + font.readUInt16BE(at);
  const flags: number[] = [];
  while (flags.length < points) {
    const flag = font.readUInt8(at++);
    flags.push(flag);
    if (flag & 8) { const repeat = font.readUInt8(at++); for (let index = 0; index < repeat; index++) flags.push(flag); }
  }
  const xs: number[] = [], ys: number[] = [];
  let value = 0;
  for (const flag of flags) {
    if (flag & 2) { const d = font.readUInt8(at++); value += flag & 16 ? d : -d; } else if (!(flag & 16)) { value += font.readInt16BE(at); at += 2; }
    xs.push(value);
  }
  value = 0;
  for (const flag of flags) {
    if (flag & 4) { const d = font.readUInt8(at++); value += flag & 32 ? d : -d; } else if (!(flag & 32)) { value += font.readInt16BE(at); at += 2; }
    ys.push(value);
  }
  const out: number[][] = [];
  let first = 0;
  for (const last of ends) {
    // On-curve points, with the implied ones between two off-curve points.
    const raw: { x: number; y: number; on: boolean }[] = [];
    for (let index = first; index <= last; index++) raw.push({ x: xs[index], y: ys[index], on: (flags[index] & 1) === 1 });
    const full: { x: number; y: number; on: boolean }[] = [];
    for (let index = 0; index < raw.length; index++) {
      const a = raw[index], b = raw[(index + 1) % raw.length];
      full.push(a);
      if (!a.on && !b.on) full.push({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2, on: true });
    }
    const start = full.findIndex((point) => point.on);
    const ring = [...full.slice(start), ...full.slice(0, start)];
    const flat: number[] = [];
    for (let index = 0; index < ring.length; index++) {
      const p = ring[index];
      if (!p.on) continue;
      flat.push(p.x, p.y);
      const q = ring[(index + 1) % ring.length];
      if (q.on) continue;
      const r = ring[(index + 2) % ring.length];
      for (let step = 1; step < steps; step++) {
        const t = step / steps, u = 1 - t;
        flat.push(u * u * p.x + 2 * u * t * q.x + t * t * r.x, u * u * p.y + 2 * u * t * q.y + t * t * r.y);
      }
    }
    out.push(flat);
    first = last + 1;
  }
  return out;
}

const area = (ring: number[]) => { let twice = 0; for (let i = 0, n = ring.length / 2; i < n; i++) { const j = (i + 1) % n; twice += ring[i * 2] * ring[j * 2 + 1] - ring[j * 2] * ring[i * 2 + 1]; } return twice / 2; };
const reverse = (ring: number[]) => { const out: number[] = []; for (let i = ring.length / 2 - 1; i >= 0; i--) out.push(ring[i * 2], ring[i * 2 + 1]); return out; };
function inside(ring: number[], x: number, y: number): boolean {
  let hit = false;
  for (let i = 0, n = ring.length / 2, j = n - 1; i < n; j = i++) {
    const xi = ring[i * 2], yi = ring[i * 2 + 1], xj = ring[j * 2], yj = ring[j * 2 + 1];
    if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) hit = !hit;
  }
  return hit;
}

/** Joins a hole (clockwise) into its outer ring (counter-clockwise) by a bridge from the hole's rightmost point. */
function bridge(outer: number[], hole: number[]): number[] {
  let m = 0;
  for (let i = 1; i < hole.length / 2; i++) if (hole[i * 2] > hole[m * 2]) m = i;
  const mx = hole[m * 2], my = hole[m * 2 + 1];
  // The nearest outer vertex to the right of M that the segment from M reaches without crossing the outer ring.
  let best = -1, bestDistance = Infinity;
  const n = outer.length / 2;
  for (let i = 0; i < n; i++) {
    const px = outer[i * 2], py = outer[i * 2 + 1];
    if (px < mx) continue;
    const distance = (px - mx) ** 2 + (py - my) ** 2;
    if (distance >= bestDistance) continue;
    let crosses = false;
    for (let a = 0; a < n && !crosses; a++) {
      const b = (a + 1) % n;
      if (a === i || b === i) continue;
      crosses = segmentsCross(mx, my, px, py, outer[a * 2], outer[a * 2 + 1], outer[b * 2], outer[b * 2 + 1]);
    }
    if (!crosses) { best = i; bestDistance = distance; }
  }
  if (best < 0) throw new Error("no bridge from a hole to its outline");
  const out: number[] = [];
  for (let i = 0; i <= best; i++) out.push(outer[i * 2], outer[i * 2 + 1]);
  for (let k = 0; k <= hole.length / 2; k++) { const i = (m + k) % (hole.length / 2); out.push(hole[i * 2], hole[i * 2 + 1]); }
  out.push(outer[best * 2], outer[best * 2 + 1]);
  for (let i = best + 1; i < n; i++) out.push(outer[i * 2], outer[i * 2 + 1]);
  return out;
}
function segmentsCross(ax: number, ay: number, bx: number, by: number, cx: number, cy: number, dx: number, dy: number): boolean {
  const d1 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx), d2 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
  const d3 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax), d4 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

/** Ear clipping of a simple counter-clockwise ring (bridged holes allowed): triangles as x,y triples. */
function earClip(ring: number[]): number[] {
  const points: { x: number; y: number }[] = [];
  for (let i = 0; i < ring.length / 2; i++) points.push({ x: ring[i * 2], y: ring[i * 2 + 1] });
  const index = points.map((_, i) => i), out: number[] = [];
  const cross = (a: { x: number; y: number }, b: { x: number; y: number }, c: { x: number; y: number }) => (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  let guard = 0;
  while (index.length > 3 && guard++ < 100000) {
    let clipped = false;
    for (let k = 0; k < index.length; k++) {
      const a = points[index[(k + index.length - 1) % index.length]], b = points[index[k]], c = points[index[(k + 1) % index.length]];
      if (cross(a, b, c) <= 1e-9) continue;
      let empty = true;
      for (const other of index) {
        const p = points[other];
        if (p === a || p === b || p === c || (p.x === a.x && p.y === a.y) || (p.x === b.x && p.y === b.y) || (p.x === c.x && p.y === c.y)) continue;
        if (cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0) { empty = false; break; }
      }
      if (!empty) continue;
      out.push(a.x, a.y, b.x, b.y, c.x, c.y);
      index.splice(k, 1);
      clipped = true;
      break;
    }
    if (!clipped) throw new Error("ear clipping found no ear");
  }
  if (index.length === 3) { const [a, b, c] = index.map((i) => points[i]); out.push(a.x, a.y, b.x, b.y, c.x, c.y); }
  return out;
}

/** The letters of `text` as triangle meshes, in cap heights, from the TrueType font in `font`. */
export function legendGlyphs(font: Buffer, text: string, steps = 8): { capHeight: number; glyphs: GlyphMesh[] } {
  const head = table(font, "head"), hhea = table(font, "hhea"), hmtx = table(font, "hmtx");
  const metrics = font.readUInt16BE(hhea + 34);
  // The cap height: the top of an H.
  const capHeight = Math.max(...contours(font, glyphIndex(font, 72), 2).flat().filter((_, i) => i % 2 === 1));
  void head;
  const glyphs: GlyphMesh[] = [];
  for (const letter of [...new Set(text)].sort()) {
    const glyph = glyphIndex(font, letter.charCodeAt(0));
    const advance = font.readUInt16BE(hmtx + Math.min(glyph, metrics - 1) * 4) / capHeight;
    // TrueType outlines run clockwise and holes counter-clockwise; ear clipping wants the outline counter-clockwise.
    const rings = contours(font, glyph, steps).map((ring) => ring.map((value) => value / capHeight));
    const outers = rings.filter((ring) => area(ring) < 0).map(reverse), holes = rings.filter((ring) => area(ring) > 0).map(reverse);
    const triangles: number[] = [];
    for (let outer of outers) {
      for (const hole of holes.filter((ring) => inside(outer, ring[0], ring[1])).sort((a, b) => Math.max(...b.filter((_, i) => i % 2 === 0)) - Math.max(...a.filter((_, i) => i % 2 === 0)))) outer = bridge(outer, hole);
      triangles.push(...earClip(outer));
    }
    glyphs.push({ letter, advance, triangles: triangles.map((value) => Math.round(value * 10000) / 10000) });
  }
  return { capHeight, glyphs };
}

/** A float literal C++ accepts: always with a decimal point (0f is not one). */
export function cppFloat(value: number): string {
  const text = String(value);
  return `${/[.e]/.test(text) ? text : `${text}.0`}f`;
}

/** The generated C++ header for the glyphs. */
export function legendGlyphsHeader(glyphs: GlyphMesh[], source: string): string {
  const lines = [
    `// Generated by tools/build-tools/scripts/road-pack-glyphs.ts from ${source} (Roboto, Apache License 2.0). Do not edit.`,
    "// The depot bay legend's letters (#318) as triangles, x,y per vertex, in cap heights with the baseline at y 0.",
    "#pragma once",
    "",
    "struct FMKIRoadLegendGlyph { TCHAR Letter; float Advance; int32 First; int32 Vertices; };",
    "",
    "static const float GMKIRoadLegendGlyphPoints[] = {",
  ];
  let first = 0;
  const entries: string[] = [];
  for (const glyph of glyphs) {
    for (let i = 0; i < glyph.triangles.length; i += 12) lines.push("\t" + glyph.triangles.slice(i, i + 12).map(cppFloat).join(", ") + ",");
    entries.push(`\t{ TEXT('${glyph.letter}'), ${glyph.advance.toFixed(4)}f, ${first}, ${glyph.triangles.length / 2} },`);
    first += glyph.triangles.length / 2;
  }
  lines.push("};", "", "static const FMKIRoadLegendGlyph GMKIRoadLegendGlyphs[] = {", ...entries, "};", "");
  return lines.join("\n");
}
