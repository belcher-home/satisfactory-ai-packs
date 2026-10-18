/**
 * Regenerates MKI Roads' depot bay legend letters (#318) from Roboto Black.
 *
 *   npm run road-pack:glyphs [-- --font <path to Roboto-Black.ttf>]
 *
 * The font defaults to the engine's copy (UE_ROOT, default D:/DevTools/UnrealEngine-CSS, Engine/Content/Slate/Fonts). The
 * header is committed, so builds do not need the font.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { legendGlyphs, legendGlyphsHeader } from "../src/road-pack-glyphs.js";
import { roadPackPluginDirectory } from "../src/workspace-paths.js";

const args = process.argv.slice(2);
const option = (name: string) => { const index = args.indexOf(name); return index >= 0 ? args[index + 1] : undefined; };
const font = resolve(option("--font") ?? join(process.env.UE_ROOT ?? "D:/DevTools/UnrealEngine-CSS", "Engine", "Content", "Slate", "Fonts", "Roboto-Black.ttf"));
const { glyphs } = legendGlyphs(readFileSync(font), "LOADINGUNSTOP");
const out = join(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadLegendGlyphs.generated.h");
writeFileSync(out, legendGlyphsHeader(glyphs, "Roboto-Black.ttf"));
console.log(JSON.stringify({ font, out, glyphs: glyphs.map((glyph) => ({ letter: glyph.letter, advance: glyph.advance, triangles: glyph.triangles.length / 6 })) }, null, 2));
