import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { roadJunctionEntries, splineRoadPatterns, splineRoadVariants, type RoadKitDocument } from "./road-kit.js";
import { roadKitIconsPath } from "./workspace-paths.js";

/**
 * Build-menu icons for MKI Roads (issue #86): one per spline road entry, drawn top-down from
 * the deck's lanes, its lane markings (#159: the double yellow center, dashed white lane lines, and white edge lines,
 * widened to read at icon size), and its deck thickness, filled with the asphalt icon color. Nothing is traced or copied
 * from another asset. Rendering is pure arithmetic, so the pixels (and their hash in generated/road-kit.icons.json) are
 * identical on every machine. The modular pieces' icons went with those pieces (#180). Road marking patterns are drawn
 * as the vanilla line patterns' icons are (renderRoadPatternIcon).
 */

export const ROAD_KIT_ICONS_SCHEMA = "mki-roads.icons";
export const ROAD_KIT_ICONS_SCHEMA_VERSION = 2;
const SUPERSAMPLE = 4;

/** An icon: `key` names the entry it shows (`spline_road_<n>m`, `road_pattern_<id>`). */
export interface RoadKitIcon { iconPath: string; key: string; size: number; rgba: Uint8Array }
export interface RoadKitIconSummary { key: string; size: number; rgbaSha256: string; coverage: number; bounds: [number, number, number, number] }
export interface RoadKitIconsDocument { schema: string; schemaVersion: number; generator: string; icons: Record<string, RoadKitIconSummary> }

type Rgb = [number, number, number];
const hex = (value: string): Rgb => [1, 3, 5].map((index) => parseInt(value.slice(index, index + 2), 16)) as Rgb;

/** One spline road build-menu or road pattern icon: a straight deck of `lanes` lanes and its deck thickness. */
export interface SplineRoadIconSpec { iconPath: string; key: string; lanes: number; thicknessCm: number; dashed: boolean }

/**
 * The spline road entries' icons (Road 1 m, 2 m, 4 m: two lanes) and each road pattern's (four lanes, so its dashed lane
 * lines show). The maintainer: each shows the solid white line on the outside and the double solid yellow line in the middle.
 */
export function splineRoadIconSpecs(kit: RoadKitDocument): SplineRoadIconSpec[] {
  const roads = splineRoadVariants(kit).map((variant) => ({ iconPath: variant.iconPath, key: `spline_road_${variant.thicknessCm / 100}m`, lanes: 2, thicknessCm: variant.thicknessCm, dashed: false }));
  const patterns = splineRoadPatterns(kit).entries.map((entry) => ({ iconPath: entry.iconPath, key: `road_pattern_${entry.id}`, lanes: 4, thicknessCm: 400, dashed: true }));
  return [...roads, ...patterns];
}

/**
 * A spline road icon, top-down: an asphalt deck running up the icon with a solid white line along each outside edge, the
 * double solid yellow center line, and (patterns) dashed white lane lines, above a concrete side band whose height
 * shows the deck thickness (1, 2, or 4 m), as the vanilla 8x1, 8x2, and 8x4 foundation icons differ. Lines are widened to
 * read at icon size.
 *
 * Hot path: the loop evaluates millions of samples per icon, so it reads scalars only, with indexed loops and no array
 * reads, property lookups, destructuring, or closures per sample. On Node 20 (V8 11.3) array destructuring and per-sample
 * closures in such loops intermittently crash the process during OSR/deoptimization ("Check failed: fixed_size_above_fp
 * ...", or an Array Iterator called on a number; #91).
 */
export function renderSplineRoadIcon(kit: RoadKitDocument, spec: SplineRoadIconSpec, size = kit.package.buildMenu.icons.sizePx): RoadKitIcon {
  const fill = hex(kit.package.asphalt.iconColor), side: Rgb = [156, 156, 150];
  const white = hex(kit.package.markings.paints.white.color), yellow = hex(kit.package.markings.paints.yellow.color);
  const deckWidth = size * (spec.lanes <= 2 ? 0.44 : 0.64), left = (size - deckWidth) / 2, right = left + deckWidth;
  const band = size * 0.11 * spec.thicknessCm / 400, top = size * 0.07, bottom = size * 0.93 - band;
  const line = size / 80, edgeCenter = line * 1.9, pairOffset = line * 0.95, dash = size * 0.07;
  const laneWidth = deckWidth / spec.lanes, centerX = size / 2, lanes = spec.lanes, dashed = spec.dashed, half = line / 2, period = dash * 2;
  const fillR = fill[0], fillG = fill[1], fillB = fill[2], sideR = side[0], sideG = side[1], sideB = side[2];
  const whiteR = white[0], whiteG = white[1], whiteB = white[2], yellowR = yellow[0], yellowG = yellow[1], yellowB = yellow[2];
  const samples = SUPERSAMPLE * SUPERSAMPLE, rgba = new Uint8Array(size * size * 4);
  for (let row = 0; row < size; row++) {
    for (let column = 0; column < size; column++) {
      let r = 0, g = 0, b = 0, covered = 0;
      for (let sy = 0; sy < SUPERSAMPLE; sy++) {
        const y = row + (sy + 0.5) / SUPERSAMPLE;
        if (y < top || y > bottom + band) continue;
        for (let sx = 0; sx < SUPERSAMPLE; sx++) {
          const x = column + (sx + 0.5) / SUPERSAMPLE;
          if (x < left || x > right) continue;
          let paint = 0;
          if (y > bottom) paint = 1;
          // The edge trims (#316): white from the deck's edge in to the old line's inner side.
          else if (x - left <= edgeCenter + half || right - x <= edgeCenter + half) paint = 2;
          else if (Math.abs(Math.abs(x - centerX) - pairOffset) <= half) paint = 3;
          else if (dashed && (y - top) % period < dash) {
            for (let boundary = 1; boundary < lanes; boundary++) if (boundary * 2 !== lanes && Math.abs(x - left - boundary * laneWidth) <= half) paint = 2;
          }
          if (paint === 0) { r += fillR; g += fillG; b += fillB; }
          else if (paint === 1) { r += sideR; g += sideG; b += sideB; }
          else if (paint === 2) { r += whiteR; g += whiteG; b += whiteB; }
          else { r += yellowR; g += yellowG; b += yellowB; }
          covered++;
        }
      }
      if (covered === 0) continue;
      const index = (row * size + column) * 4;
      rgba[index] = Math.round(r / covered); rgba[index + 1] = Math.round(g / covered); rgba[index + 2] = Math.round(b / covered);
      rgba[index + 3] = Math.round(255 * covered / samples);
    }
  }
  return { iconPath: spec.iconPath, key: spec.key, size, rgba };
}

/** The vanilla pattern icons' line color (IconDesc_*Line_512, read from the game 1.2.4.0 IoStore container). */
export const PATTERN_ICON_LINE: Rgb = [203, 203, 203];

/**
 * A road marking pattern's icon, drawn as the vanilla line patterns' icons are (IconDesc_DoubleLine_512 and friends): light
 * grey line art on a transparent tile, lines 12/512 of the tile wide, dashes three to a tile. The maintainer: the deck-and-color
 * icon read as a color swatch in the customizer. For the North American layout on four lanes: a solid line along each
 * edge, the solid double center line, and a dashed line between the lanes of each direction.
 *
 * Hot path: scalars only per sample (#91), as renderSplineRoadIcon.
 */
export function renderRoadPatternIcon(spec: SplineRoadIconSpec, size: number): RoadKitIcon {
  const line = size * 12 / 512, half = line / 2, edge = line * 1.5, pair = line * 1.25, center = size / 2;
  const lanes = spec.lanes, laneWidth = (size - 2 * edge) / lanes;
  // Vanilla dashes: three per tile, about 116/512 long with 70/512 gaps, starting 12/512 in.
  const dashStart = size * 12 / 512, dash = size * 116 / 512, period = size * 186 / 512;
  const lineR = PATTERN_ICON_LINE[0], lineG = PATTERN_ICON_LINE[1], lineB = PATTERN_ICON_LINE[2];
  const samples = SUPERSAMPLE * SUPERSAMPLE, rgba = new Uint8Array(size * size * 4);
  for (let row = 0; row < size; row++) {
    for (let column = 0; column < size; column++) {
      let covered = 0;
      for (let sy = 0; sy < SUPERSAMPLE; sy++) {
        const y = row + (sy + 0.5) / SUPERSAMPLE;
        const inDash = y >= dashStart && (y - dashStart) % period < dash;
        for (let sx = 0; sx < SUPERSAMPLE; sx++) {
          const x = column + (sx + 0.5) / SUPERSAMPLE;
          let on = Math.abs(x - edge) <= half || Math.abs(size - edge - x) <= half || Math.abs(Math.abs(x - center) - pair) <= half;
          if (!on && inDash) {
            for (let boundary = 1; boundary < lanes; boundary++) if (boundary * 2 !== lanes && Math.abs(x - edge - boundary * laneWidth) <= half) on = true;
          }
          if (on) covered++;
        }
      }
      if (covered === 0) continue;
      const index = (row * size + column) * 4;
      rgba[index] = lineR; rgba[index + 1] = lineG; rgba[index + 2] = lineB;
      rgba[index + 3] = Math.round(255 * covered / samples);
    }
  }
  return { iconPath: spec.iconPath, key: spec.key, size, rgba };
}

/** One intersection entry's icon (#227): its arms (3 or 4), drawn on a 2-lane junction. */
export interface RoadJunctionIconSpec { iconPath: string; key: string; arms: 1 | 2 | 3 | 4 | 7 | 9 }

export function roadJunctionIconSpecs(kit: RoadKitDocument): RoadJunctionIconSpec[] {
  return roadJunctionEntries(kit).map((entry) => ({ iconPath: entry.iconPath, key: entry.arms === 1 ? "road_turnaround" : entry.arms >= 6 ? `road_truck_depot_${entry.arms}` : `road_junction_${entry.arms}way`, arms: entry.arms }));
}

/**
 * A turnaround icon's painted island ring, and the round end it had before #314 (radius, and the tangents from the arm
 * end's corners (2, +-1)), in half road widths: outside that curve the square deck is white (#316).
 */
const TURN_ISLAND = 0.7, TURN_RADIUS = 1.9, TURN_SLOPE = 0.615, TURN_TANGENT_X = TURN_SLOPE * TURN_RADIUS / Math.sqrt(TURN_SLOPE * TURN_SLOPE + 1);

/** Inside a turnaround's old round end (#236), drawn in by trim: the drivable part of its square deck (#316). */
function insideTurnaroundBulb(x: number, y: number, trim: number): boolean {
  const radius = TURN_RADIUS - trim, ay = Math.abs(y);
  return x <= 2 && (x * x + y * y <= radius * radius || (x >= TURN_TANGENT_X && ay <= 1 - trim + (2 - x) * TURN_SLOPE));
}

/**
 * Inside a junction's deck, in units of half the road's width (the crossing square is |x|, |y| <= 1, each arm reaches to 2,
 * the corner tiles fill between the aprons of neighbouring arms): +x, -x and +y (the stem, down the icon) are arms, and -y on a
 * 4-way. Scalars only, as the hot paths below (#91).
 */
function insideJunction(x: number, y: number, arms: number): boolean {
  const ax = Math.abs(x), ay = Math.abs(y);
  // A turnaround (#236): a square deck of whole tiles (#314), its arm end at x = 2.
  // A truck depot (#237, #240, #251): the road across the icon with a bay half a road wide below it (and above it, both
  // sides), its ends cut back to the bends. The station's pad beyond the bay is drawn apart (onDepotPad).
  if (arms === 7) return ax <= 2 && y >= -1 && y <= 1.5 && !(y > 1 && ax > 1.6);
  if (arms === 9) return ax <= 2 && ay <= 1.5 && !(ay > 1 && ax > 1.6);
  if (arms === 1) return ax <= 2 && ay <= 2;
  if (ax > 2 || ay > 2) return false;
  if (ax <= 1 && ay <= 1) return true;
  // A 2-way (#228) is a corner: the +x arm and the stem only.
  if (ay <= 1) return x > 0 || arms >= 3;
  if (ax <= 1) return y > 0 || arms >= 4;
  // A corner tile between two arms, paved whole (#314); its curb return is painted (cornerCurb).
  return (y > 0 || arms >= 4) && (x > 0 || arms >= 3);
}

/** On an intersection's corner tile (#314): the curb return painted across it is drawn there, not an edge line. */
function onCornerTile(x: number, y: number, arms: number): boolean {
  return arms >= 2 && arms <= 4 && Math.abs(x) > 1 && Math.abs(y) > 1 && insideJunction(x, y, arms);
}

/** A truck depot's station pad (#252), beyond its bay: on +y, and on -y too for both sides. In half road widths. */
function onDepotPad(x: number, y: number, arms: number): boolean {
  if (arms !== 7 && arms !== 9) return false;
  // #257: the pad is four foundations along the bay, one deep.
  if (y > 1.5 && y <= 2.0 && Math.abs(x) <= 1.33) return true;
  return arms === 9 && y < -1.5 && y >= -2.0 && Math.abs(x) <= 1.33;
}

/**
 * The road lines drawn on a road piece's deck (#252; the maintainer: icons as illustrative as the Road's, with its lines): 1 for
 * yellow, 2 for white, 0 for none. The double yellow center line runs along each arm (from its end to the crossing square
 * on an intersection, into the painted island ring on a turnaround, the whole length of a depot), and a depot's bays are
 * set off by a dashed white line. Units of half the road's width; scalars only (#91).
 */
function roadPieceLine(x: number, y: number, arms: number, half: number, pair: number, dash: number): number {
  const ax = Math.abs(x), ay = Math.abs(y);
  const onPairY = Math.abs(ay - pair) <= half, onPairX = Math.abs(ax - pair) <= half;
  if (arms === 1) {
    // The turnaround's island ring and the center line from the arm end in to it.
    const ring = TURN_ISLAND, r = Math.sqrt(x * x + y * y);
    if (Math.abs(r - ring) <= half) return 1;
    return onPairY && x >= ring && x <= 2 ? 1 : 0;
  }
  if (arms === 7 || arms === 9) {
    if (onPairY && ax <= 2) return 1;
    const period = dash * 2;
    if ((Math.abs(y - 1) <= half || (arms === 9 && Math.abs(y + 1) <= half)) && ax <= 1.6 && (x + 2) % period < dash) return 2;
    return 0;
  }
  if (onPairY && ax >= 1 && ax <= 2 && (x > 0 || arms >= 3)) return 1;
  if (onPairX && ay >= 1 && ay <= 2 && (y > 0 || arms >= 4)) return 1;
  return 0;
}

/**
 * A road piece's icon, top-down: the asphalt deck of a 2-lane intersection (2-, 3-, or 4-way), turnaround, or truck depot,
 * with a white line round its edges but the arm ends, where roads join, the road's double yellow center line along its
 * arms, and (#252) a depot's dashed bay line and its station pad in factory-zone orange stripes. Hot path: scalars only per
 * sample (#91).
 */
export function renderRoadJunctionIcon(kit: RoadKitDocument, spec: RoadJunctionIconSpec, size = kit.package.buildMenu.icons.sizePx): RoadKitIcon {
  const fill = hex(kit.package.asphalt.iconColor), white = hex(kit.package.markings.paints.white.color);
  const yellow = hex(kit.package.markings.paints.yellow.color), orange = hex(kit.package.markings.paints.orange.color);
  const fillR = fill[0], fillG = fill[1], fillB = fill[2], whiteR = white[0], whiteG = white[1], whiteB = white[2];
  const yellowR = yellow[0], yellowG = yellow[1], yellowB = yellow[2], orangeR = orange[0], orangeG = orange[1], orangeB = orange[2];
  // Two units of reach fill 0.86 of the icon; the edge line is size / 80 wide, inset by about the same.
  const unit = size * 0.43 / 2, center = size / 2, line = (size / 80) / unit, inset = (size / 60) / unit, arms = spec.arms;
  const samples = SUPERSAMPLE * SUPERSAMPLE, rgba = new Uint8Array(size * size * 4);
  const near = inset + line, far = inset, half = line / 2, pair = line * 0.95, dash = 0.28, stripe = 0.16;
  for (let row = 0; row < size; row++) {
    for (let column = 0; column < size; column++) {
      let r = 0, g = 0, b = 0, covered = 0;
      for (let sy = 0; sy < SUPERSAMPLE; sy++) {
        const y = (row + (sy + 0.5) / SUPERSAMPLE - center) / unit;
        for (let sx = 0; sx < SUPERSAMPLE; sx++) {
          const x = (column + (sx + 0.5) / SUPERSAMPLE - center) / unit;
          if (!insideJunction(x, y, arms)) {
            if (!onDepotPad(x, y, arms)) continue;
            // The pad: factory-zone stripes at 45 degrees (#246, #252), and a yellow trim round its outer edges (#316).
            const ay = Math.abs(y);
            if (ay >= 2 - near || Math.abs(x) >= 1.33 - near) { r += yellowR; g += yellowG; b += yellowB; covered++; continue; }
            const band = (x - y) / stripe;
            if (band - Math.floor(band) < 0.5) { r += orangeR; g += orangeG; b += orangeB; }
            else { r += fillR; g += fillG; b += fillB; }
            covered++;
            continue;
          }
          // The edge line: inside the band between `far` and `near` from the outline (deck kept in every direction to `far`,
          // but not to `near`), and not along an arm's end.
          const keepsFar = insideJunction(x + far, y, arms) && insideJunction(x - far, y, arms) && insideJunction(x, y + far, arms) && insideJunction(x, y - far, arms)
            && insideJunction(x + far * 0.7071, y + far * 0.7071, arms) && insideJunction(x - far * 0.7071, y + far * 0.7071, arms) && insideJunction(x + far * 0.7071, y - far * 0.7071, arms) && insideJunction(x - far * 0.7071, y - far * 0.7071, arms);
          const keepsNear = insideJunction(x + near, y, arms) && insideJunction(x - near, y, arms) && insideJunction(x, y + near, arms) && insideJunction(x, y - near, arms)
            && insideJunction(x + near * 0.7071, y + near * 0.7071, arms) && insideJunction(x - near * 0.7071, y + near * 0.7071, arms) && insideJunction(x + near * 0.7071, y - near * 0.7071, arms) && insideJunction(x - near * 0.7071, y - near * 0.7071, arms);
          const atArmEnd = arms === 1 ? x > 2 - near && Math.abs(y) <= 1 : arms >= 6 ? Math.abs(x) > 2 - near && Math.abs(y) <= 1 : (Math.abs(x) > 2 - near && Math.abs(y) <= 1) || (Math.abs(y) > 2 - near && Math.abs(x) <= 1);
          // The non-drivable margin is white to the edge (#316): a trim from every edge but the arm ends in to the old line's
          // inner side; on a corner tile (#314) everything beyond the curb return (a quarter circle of one unit, the 8 m
          // curb, about the outer corner, widened by the trim); on a turnaround everything outside its old round end.
          const corner = onCornerTile(x, y, arms);
          let curb = false;
          if (corner) { const cx = 2 - Math.abs(x), cy = 2 - Math.abs(y); curb = cx * cx + cy * cy <= (1 + near) * (1 + near); }
          const surround = arms === 1 && !insideTurnaroundBulb(x, y, near);
          // A depot's road ends (#316): the corner square where a joined road's edge trim meets the bay end's trim.
          const stub = (arms === 7 || arms === 9) && Math.abs(x) >= 2 - near && ((y <= 1 && y >= 1 - near) || (arms === 9 && y >= -1 && y <= -1 + near));
          // In front of each bay, the pad's edge in yellow on the deck side, in line with the white either side (#316).
          const padFront = (arms === 7 || arms === 9) && Math.abs(x) <= 1.33 && ((y <= 1.5 && y >= 1.5 - near) || (arms === 9 && y >= -1.5 && y <= -1.5 + near));
          if (padFront) { r += yellowR; g += yellowG; b += yellowB; covered++; continue; }
          const marking = curb || surround || stub || (!keepsNear && !atArmEnd && !corner) ? 2 : roadPieceLine(x, y, arms, half, pair, dash);
          if (marking === 2) { r += whiteR; g += whiteG; b += whiteB; }
          else if (marking === 1) { r += yellowR; g += yellowG; b += yellowB; }
          else { r += fillR; g += fillG; b += fillB; }
          covered++;
        }
      }
      if (covered === 0) continue;
      const index = (row * size + column) * 4;
      rgba[index] = Math.round(r / covered); rgba[index + 1] = Math.round(g / covered); rgba[index + 2] = Math.round(b / covered);
      rgba[index + 3] = Math.round(255 * covered / samples);
    }
  }
  return { iconPath: spec.iconPath, key: spec.key, size, rgba };
}

/** Every icon the import step creates: each spline road entry's (the deck), each road pattern's (vanilla line art), and each intersection's. */
export function renderRoadKitIcons(kit: RoadKitDocument, size?: number): RoadKitIcon[] {
  return [
    ...splineRoadIconSpecs(kit).map((spec) => spec.key.startsWith("road_pattern_")
      ? renderRoadPatternIcon(spec, size ?? kit.package.buildMenu.icons.sizePx)
      : renderSplineRoadIcon(kit, spec, size)),
    ...roadJunctionIconSpecs(kit).map((spec) => renderRoadJunctionIcon(kit, spec, size)),
  ];
}

/**
 * Whether a child render exited by crashing rather than by reporting (#314): a signal, an access violation or stack
 * overflow on Windows (0xC0000005, 0xC00000FD), or an abort or segfault elsewhere (134, 139). On the maintainer's Windows
 * machine Node 20 segfaults intermittently in the icon rasterizer's hot loop, on main's code too (#91); the pixels are
 * checked against the golden, so a crashed render is retried, and a mismatch only once (it fails when it repeats).
 */
export function isRenderCrash(status: number | null, signal: string | null): boolean {
  if (signal) return true;
  return status === 3221225477 || status === 3221225725 || status === 134 || status === 139;
}

/** Render attempts before a crashing icon step gives up (#314). */
export const ICON_RENDER_ATTEMPTS = 4;

export function summarizeIcon(icon: RoadKitIcon): RoadKitIconSummary {
  let covered = 0, minX = icon.size, minY = icon.size, maxX = -1, maxY = -1;
  for (let row = 0; row < icon.size; row++) for (let column = 0; column < icon.size; column++) {
    if (icon.rgba[(row * icon.size + column) * 4 + 3] === 0) continue;
    covered++; minX = Math.min(minX, column); maxX = Math.max(maxX, column); minY = Math.min(minY, row); maxY = Math.max(maxY, row);
  }
  return { key: icon.key, size: icon.size, rgbaSha256: createHash("sha256").update(icon.rgba).digest("hex"), coverage: Math.round(covered / (icon.size * icon.size) * 1e4) / 1e4, bounds: [minX, minY, maxX, maxY] };
}

export function roadKitIconsDocument(icons: readonly RoadKitIcon[]): RoadKitIconsDocument {
  return { schema: ROAD_KIT_ICONS_SCHEMA, schemaVersion: ROAD_KIT_ICONS_SCHEMA_VERSION, generator: "tools/build-tools/src/road-pack-icons.ts", icons: Object.fromEntries(icons.map((icon) => [icon.iconPath, summarizeIcon(icon)])) };
}
export function loadRoadKitIcons(path = roadKitIconsPath): RoadKitIconsDocument {
  const document = JSON.parse(readFileSync(path, "utf8")) as RoadKitIconsDocument;
  if (document.schema !== ROAD_KIT_ICONS_SCHEMA || document.schemaVersion !== ROAD_KIT_ICONS_SCHEMA_VERSION) throw new Error(`Unsupported road kit icons document ${document.schema} v${document.schemaVersion}`);
  return document;
}

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
function crc32(buffer: Buffer): number { let crc = 0xffffffff; for (const byte of buffer) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8); return (crc ^ 0xffffffff) >>> 0; }
function chunk(type: string, data: Buffer): Buffer {
  const length = Buffer.alloc(4), crc = Buffer.alloc(4), body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  length.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, crc]);
}
/** 8-bit RGBA PNG. */
export function encodePng(rgba: Uint8Array, width: number, height: number): Buffer {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let row = 0; row < height; row++) Buffer.from(rgba.buffer, rgba.byteOffset + row * width * 4, width * 4).copy(raw, row * (width * 4 + 1) + 1);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", header), chunk("IDAT", deflateSync(raw, { level: 9 })), chunk("IEND", Buffer.alloc(0))]);
}
