/**
 * Single-source mod versioning. Each mod has exactly one source of truth; every other place that states its version is a
 * copy that must equal it:
 *
 * - **bridge**: MKI Bridge. Source: its descriptor's `SemVersion` (the mod reads its own descriptor at runtime
 *   and SML mod managers read it in the archive); copy: the descriptor's `VersionName`.
 * - **tools**: MKI Tools. Source: its descriptor's `SemVersion`; copy: the descriptor's `VersionName`.
 * - **roadPack**: MKI Roads. Source: `modules.json` `package.version`; copies: its descriptor's `SemVersion` and
 *   `VersionName`, and the version the MKI Roads README states.
 *
 * The bridge requires MKI Roads by a descriptor range (`^0.8.0`). `versionDrift` reports every copy that differs from its
 * source; `setProductVersion` rewrites the source and all copies. JSON files are rewritten through
 * `JSON.stringify(_, null, 2)`, and only when the file already has exactly that format, so a version change never
 * reformats a file.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { repositoryRoot } from "./workspace-paths.js";

/** A JSON value at `path` (object keys) in a repository file. */
export interface JsonVersionLocation { kind: "json"; file: string; path: string[] }
/** A version stated in prose: the first capture group of `pattern`, which must match exactly once. */
export interface TextVersionLocation { kind: "text"; file: string; pattern: RegExp }
export type VersionLocation = JsonVersionLocation | TextVersionLocation;
export interface VersionedProduct { source: JsonVersionLocation; copies: VersionLocation[] }

export const VERSIONED_PRODUCTS = {
  bridge: {
    source: { kind: "json", file: "mods/MKIBridge/MKIBridge/MKIBridge.uplugin", path: ["SemVersion"] },
    copies: [{ kind: "json", file: "mods/MKIBridge/MKIBridge/MKIBridge.uplugin", path: ["VersionName"] }],
  },
  tools: {
    source: { kind: "json", file: "mods/MKITools/MKITools/MKITools.uplugin", path: ["SemVersion"] },
    copies: [{ kind: "json", file: "mods/MKITools/MKITools/MKITools.uplugin", path: ["VersionName"] }],
  },
  roadPack: {
    source: { kind: "json", file: "mods/MKIRoads/modules.json", path: ["package", "version"] },
    copies: [
      { kind: "json", file: "mods/MKIRoads/MKIRoads/MKIRoads.uplugin", path: ["SemVersion"] },
      { kind: "json", file: "mods/MKIRoads/MKIRoads/MKIRoads.uplugin", path: ["VersionName"] },
      { kind: "text", file: "mods/MKIRoads/README.md", pattern: /`MKIRoads\.uplugin` declares `SemVersion` \(currently `([^`]+)`\)/g },
    ],
  },
} as const satisfies Record<string, VersionedProduct>;
export type ProductName = keyof typeof VERSIONED_PRODUCTS;

/** Plain MAJOR.MINOR.PATCH, as SML's SemVersion ranges and npm both accept; no pre-release or build suffix. */
const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;

/** Reads and writes repository files by repository-relative path (injectable for tests). */
export interface VersionFiles { read(file: string): string; write(file: string, text: string): void }
export const repositoryFiles = (root = repositoryRoot): VersionFiles => ({
  read: (file) => readFileSync(resolve(root, file), "utf8"),
  write: (file, text) => writeFileSync(resolve(root, file), text),
});

const describe = (location: VersionLocation) => location.kind === "json" ? `${location.file} ${location.path.map((key) => JSON.stringify(key)).join(".")}` : `${location.file} ${location.pattern.source}`;

function jsonAt(document: unknown, path: readonly string[]): unknown {
  let value = document;
  for (const key of path) value = value !== null && typeof value === "object" && Object.hasOwn(value, key) ? (value as Record<string, unknown>)[key] : undefined;
  return value;
}

/** The version at a location, or a problem describing why it cannot be read. */
export function readVersion(files: VersionFiles, location: VersionLocation): { version: string } | { problem: string } {
  let text: string;
  try { text = files.read(location.file); } catch { return { problem: `${location.file} cannot be read` }; }
  if (location.kind === "text") {
    const matches = [...text.matchAll(location.pattern)];
    return matches.length === 1 ? { version: matches[0][1] } : { problem: `${describe(location)} matches ${matches.length} times (expected once)` };
  }
  let document: unknown;
  try { document = JSON.parse(text); } catch { return { problem: `${location.file} is not valid JSON` }; }
  const value = jsonAt(document, location.path);
  return typeof value === "string" ? { version: value } : { problem: `${describe(location)} is not a string` };
}

/** Every way the repository's versions disagree: an unreadable or non-semver source, or a copy that differs from it. */
export function versionDrift(files: VersionFiles, products: Record<string, VersionedProduct> = VERSIONED_PRODUCTS): string[] {
  const problems: string[] = [];
  for (const [name, product] of Object.entries(products)) {
    const source = readVersion(files, product.source);
    if ("problem" in source) { problems.push(`${name}: source ${source.problem}`); continue; }
    if (!SEMVER.test(source.version)) problems.push(`${name}: source ${describe(product.source)} is "${source.version}", not MAJOR.MINOR.PATCH`);
    for (const copy of product.copies) {
      const found = readVersion(files, copy);
      if ("problem" in found) problems.push(`${name}: ${found.problem}`);
      else if (found.version !== source.version) problems.push(`${name}: ${describe(copy)} is "${found.version}" but ${describe(product.source)} is "${source.version}"`);
    }
  }
  return problems;
}

/** The current version of every product, read from its source. */
export function productVersions(files: VersionFiles): Record<ProductName, string | undefined> {
  return Object.fromEntries(Object.entries(VERSIONED_PRODUCTS).map(([name, product]) => {
    const found = readVersion(files, product.source);
    return [name, "version" in found ? found.version : undefined];
  })) as Record<ProductName, string | undefined>;
}

/**
 * Sets a product's version at its source and every copy, all or nothing: every file is checked (readable, in canonical
 * JSON format, the location present) before any is written. Returns the files written.
 */
export function setProductVersion(files: VersionFiles, product: ProductName, version: string): string[] {
  if (!Object.hasOwn(VERSIONED_PRODUCTS, product)) throw new Error(`Unknown product "${product}"; expected one of ${Object.keys(VERSIONED_PRODUCTS).join(", ")}`);
  if (!SEMVER.test(version)) throw new Error(`Version "${version}" is not MAJOR.MINOR.PATCH`);
  const { source, copies } = VERSIONED_PRODUCTS[product] as VersionedProduct;
  const updated = new Map<string, string>();
  for (const location of [source, ...copies]) {
    const text = updated.get(location.file) ?? files.read(location.file);
    if (location.kind === "text") {
      const matches = [...text.matchAll(location.pattern)];
      if (matches.length !== 1) throw new Error(`${describe(location)} matches ${matches.length} times (expected once)`);
      const match = matches[0], start = match.index! + match[0].indexOf(match[1]);
      updated.set(location.file, text.slice(0, start) + version + text.slice(start + match[1].length));
      continue;
    }
    const document = JSON.parse(text) as Record<string, unknown>;
    if (!updated.has(location.file) && JSON.stringify(document, null, 2) + "\n" !== text) throw new Error(`${location.file} is not in canonical JSON format (2-space indent, LF, final newline); refusing to reformat it`);
    let parent: Record<string, unknown> = document;
    for (const key of location.path.slice(0, -1)) {
      const next = Object.hasOwn(parent, key) ? parent[key] : undefined;
      if (next === null || typeof next !== "object") throw new Error(`${describe(location)} does not exist`);
      parent = next as Record<string, unknown>;
    }
    const last = location.path[location.path.length - 1];
    if (typeof parent[last] !== "string") throw new Error(`${describe(location)} is not a string`);
    parent[last] = version;
    updated.set(location.file, JSON.stringify(document, null, 2) + "\n");
  }
  for (const [file, text] of updated) files.write(file, text);
  return [...updated.keys()];
}
