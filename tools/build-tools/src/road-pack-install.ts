import { createHash } from "node:crypto";
import { copyFileSync, existsSync, lstatSync, mkdirSync, readdirSync, readFileSync, realpathSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, isAbsolute, join, relative, resolve } from "node:path";
import * as z from "zod/v4";
import {
  dependencyContractProblems, installPathFromListing, isInstallablePlugin, pluginDependency, readPluginDescriptor, satisfiesRange,
  PLUGIN_INSTALL_PATHS, ROAD_PACK_PLUGIN, type InstallablePlugin, type PluginDescriptor,
} from "./road-pack.js";

/**
 * Install and check tooling for the packaged first-party plugins (issue #63).
 *
 * Everything here operates on an explicit game root (the directory that contains `FactoryGame/`), so it runs the same
 * against a temporary fake game tree as against a real installation. Nothing here starts the game or reads a save.
 *
 * - `verifyArtifact` accepts a `.local/packages/<Plugin>-<version>-<commit>-<stamp>/` directory only when its package
 *   report is `ok` with no problems and records verified package provenance for the same clean commit as
 *   `source-commit.txt` and the directory name, every file matches `SHA256SUMS.txt`, and its container mount points name
 *   the plugin's install directory (`PLUGIN_INSTALL_PATHS`).
 * - `inspectInstallation` reports where every plugin descriptor under `FactoryGame/Mods` is, and flags first-party
 *   plugins that are missing, misplaced (for example MKI Roads under `Mods/GameFeatures/MKIRoads`, where its
 *   `../../../FactoryGame/Mods/MKIRoads/` content mount does not belong to it), duplicated, incomplete, incompatible
 *   with each other or with the installed SML, or different from the given artifacts.
 * - `planInstall`/`applyInstall` replace each target directory and remove misplaced copies, after backing every
 *   replaced or removed directory up outside `FactoryGame/Mods` and hash-checking the backup. Only a directory named
 *   after its plugin that holds no other descriptor (`removalProblems`) is ever replaced or removed; a descriptor
 *   extracted straight into `Mods` or `Mods/GameFeatures`, or sharing a directory with another plugin, fails closed. A
 *   failure restores the backup. `restoreInstall` undoes an install, or retries a failed rollback, from its receipt; it
 *   checks every backup before it deletes anything, and refuses a receipt whose actions the installer could not have
 *   planned (`receiptProblems`: SML, third-party plugins, `Mods`, `Mods/GameFeatures`, or a first-party plugin at the
 *   wrong path or with the wrong kind), so a forged or corrupt receipt changes nothing. Windows path aliases (trailing
 *   dots or spaces, 8.3 short names) and junctions or symbolic links above a directory are refused, so no lexically
 *   valid path can resolve into SML or another plugin.
 */

const sha256 = (path: string) => createHash("sha256").update(readFileSync(path)).digest("hex");
const slash = (path: string) => path.replaceAll("\\", "/");
const stamp = () => new Date().toISOString().replace(/[-:]/g, "").replace(/\..*/, "").replace("T", "-");

export interface FileHash { path: string; sha256: string }

function listFiles(root: string): string[] {
  if (!existsSync(root)) return [];
  if (statSync(root).isFile()) return [root];
  return readdirSync(root, { withFileTypes: true }).flatMap((entry) => entry.isDirectory() ? listFiles(join(root, entry.name)) : [join(root, entry.name)]);
}
/** `/`-separated paths and hashes of every file under `root`, sorted. */
export function hashTree(root: string): FileHash[] {
  return listFiles(root).map((file) => ({ path: slash(relative(root, file)), sha256: sha256(file) })).sort((a, b) => a.path.localeCompare(b.path));
}
function copyTree(from: string, to: string) {
  for (const file of listFiles(from)) {
    const target = join(to, relative(from, file));
    mkdirSync(dirname(target), { recursive: true });
    copyFileSync(file, target);
  }
}
const sameTree = (a: FileHash[], b: FileHash[]) => a.length === b.length && a.every((item, index) => item.path === b[index].path && item.sha256 === b[index].sha256);
function isInside(parent: string, child: string): boolean {
  const path = relative(resolve(parent), resolve(child));
  return path === "" || (!path.startsWith("..") && !isAbsolute(path));
}

// Artifacts -----------------------------------------------------------------------------------------------------------

export interface VerifiedArtifact {
  plugin: InstallablePlugin;
  version: string;
  directory: string;
  /** The extracted, hash-checked archive contents that are installed. */
  contents: string;
  files: FileHash[];
  zipSha256: string;
  commit: string;
  /** Game-root-relative install directory, from the container mount points. */
  installPath: string;
  descriptor: PluginDescriptor;
}

interface PackageReport {
  plugin?: string; version?: string; ok?: boolean; zipSha256?: string; files?: string[]; installPath?: string; problems?: unknown;
  /** What `npm run road-pack -- package` checked: the clean HEAD, sync record, SDK inputs, content, and a matching re-check after packaging. */
  provenance?: { commit?: unknown; syncSha256?: unknown; sdkInputsSha256?: unknown; contentSha256?: unknown; verified?: unknown };
}
const HEX40 = /^[0-9a-f]{40}$/, HEX64 = /^[0-9a-f]{64}$/;

/** Why the package report's provenance does not prove the artifact came from the clean commit in `source-commit.txt`. */
function artifactProvenanceProblems(directory: string, plugin: InstallablePlugin, report: PackageReport, commit: string): string[] {
  const provenance = report.provenance, problems: string[] = [];
  if (!provenance || typeof provenance !== "object") return ["package-report.json has no provenance block; it predates provenance-checked packaging, so repackage it with npm run road-pack -- package"];
  if (typeof provenance.commit !== "string" || !HEX40.test(provenance.commit)) problems.push(`package-report.json provenance commit ${JSON.stringify(provenance.commit)} is not a full commit hash`);
  else if (provenance.commit !== commit) problems.push(`package-report.json provenance commit ${provenance.commit} differs from source-commit.txt ${commit}`);
  for (const key of ["syncSha256", "sdkInputsSha256", ...(plugin === ROAD_PACK_PLUGIN ? ["contentSha256" as const] : [])] as const) {
    const value = provenance[key];
    if (typeof value !== "string" || !HEX64.test(value)) problems.push(`package-report.json provenance has no ${key}`);
  }
  if (provenance.verified !== true) problems.push("package-report.json provenance was not verified unchanged after packaging");
  if (!Array.isArray(report.problems) || report.problems.length > 0) problems.push(`package-report.json records problems: ${JSON.stringify(report.problems)}`);
  if (report.installPath !== PLUGIN_INSTALL_PATHS[plugin]) problems.push(`package-report.json installPath ${JSON.stringify(report.installPath)} is not ${PLUGIN_INSTALL_PATHS[plugin]}`);
  // The directory name records the plugin, version, and commit the package step stamped.
  const name = basename(resolve(directory)), parts = /^(.+)-(\d+\.\d+\.\d+[^-]*)-([0-9a-f]{7,40})-\d{8}-\d{6}$/.exec(name);
  if (!parts || parts[1] !== plugin || parts[2] !== report.version || !commit.startsWith(parts[3])) problems.push(`the directory name ${name} is not <plugin>-<version>-<commit>-<stamp> for ${plugin} ${report.version} at ${commit.slice(0, 12)}`);
  return problems;
}

/** Verifies a packaged artifact directory written by `npm run road-pack -- package`; throws with every problem found. */
export function verifyArtifact(directory: string): VerifiedArtifact {
  const problems: string[] = [], read = (name: string) => existsSync(join(directory, name)) ? readFileSync(join(directory, name), "utf8") : undefined;
  const reportText = read("package-report.json"), sumsText = read("SHA256SUMS.txt"), listingText = read("container-listing.txt"), commitText = read("source-commit.txt");
  if (!reportText || !sumsText || !listingText || !commitText) throw new Error(`${directory} is not a packaged artifact (needs package-report.json, SHA256SUMS.txt, container-listing.txt, and source-commit.txt)`);
  const report = JSON.parse(reportText) as PackageReport, plugin = report.plugin ?? "";
  if (!isInstallablePlugin(plugin)) throw new Error(`${directory} packages ${JSON.stringify(plugin)}, not one of ${Object.keys(PLUGIN_INSTALL_PATHS).join(", ")}`);
  if (report.ok !== true) problems.push("package-report.json is not ok");
  const sums = new Map(sumsText.split(/\r?\n/).filter(Boolean).map((line) => {
    const match = /^([0-9a-f]{64}) \*(.+)$/.exec(line);
    if (!match) problems.push(`SHA256SUMS.txt line ${JSON.stringify(line)} is malformed`);
    return [match?.[2] ?? line, match?.[1] ?? ""] as const;
  }));
  const zip = `${plugin}-Windows.zip`, contents = join(directory, "contents");
  if (!existsSync(join(directory, zip))) problems.push(`${zip} is missing`);
  else {
    const zipSha256 = sha256(join(directory, zip));
    if (zipSha256 !== sums.get(zip) || zipSha256 !== report.zipSha256) problems.push(`${zip} does not match SHA256SUMS.txt and package-report.json`);
  }
  const files = hashTree(contents), expected = [...(report.files ?? [])].sort();
  if (files.length === 0) problems.push("contents/ is empty");
  if (JSON.stringify(files.map((item) => item.path)) !== JSON.stringify(expected)) problems.push(`contents/ files ${JSON.stringify(files.map((item) => item.path))} differ from package-report.json ${JSON.stringify(expected)}`);
  for (const file of files) if (sums.get(`contents/${file.path}`) !== file.sha256) problems.push(`contents/${file.path} does not match SHA256SUMS.txt`);
  const installPath = installPathFromListing(plugin, listingText.split(/\r?\n/));
  if (installPath !== PLUGIN_INSTALL_PATHS[plugin]) problems.push(`the ${plugin} containers mount at ${installPath ?? "(no single plugin directory)"}, not ${PLUGIN_INSTALL_PATHS[plugin]}`);
  const descriptorPath = join(contents, `${plugin}.uplugin`), descriptor = existsSync(descriptorPath) ? readPluginDescriptor(descriptorPath) : {};
  if (descriptor.SemVersion !== report.version) problems.push(`${plugin}.uplugin SemVersion ${descriptor.SemVersion} differs from package-report.json ${report.version}`);
  const [commit, state] = commitText.split(/\r?\n/);
  if (state !== "clean") problems.push(`the artifact was packaged from a dirty tree (${state})`);
  if (!HEX40.test(commit)) problems.push(`source-commit.txt names ${JSON.stringify(commit)}, not a full commit hash`);
  problems.push(...artifactProvenanceProblems(directory, plugin, report, commit));
  if (problems.length > 0) throw new Error(`Artifact ${directory} failed verification:\n- ${problems.join("\n- ")}`);
  return { plugin, version: report.version!, directory: resolve(directory), contents: resolve(contents), files, zipSha256: report.zipSha256!, commit, installPath: installPath!, descriptor };
}

/** The newest artifact directory per plugin under `packagesDirectory` whose `package-report.json` is ok, optionally packaged from `commit`. */
export function latestArtifacts(packagesDirectory: string, commit?: string): Partial<Record<InstallablePlugin, string>> {
  const found: Partial<Record<InstallablePlugin, { directory: string; stamp: string }>> = {};
  for (const entry of existsSync(packagesDirectory) ? readdirSync(packagesDirectory, { withFileTypes: true }) : []) {
    const match = /^(.+)-\d+\.\d+\.\d+[^-]*-([0-9a-f]{7,40})-(\d{8}-\d{6})$/.exec(entry.name), directory = join(packagesDirectory, entry.name);
    if (!entry.isDirectory() || !match || !isInstallablePlugin(match[1])) continue;
    try {
      const report = JSON.parse(readFileSync(join(directory, "package-report.json"), "utf8")) as PackageReport;
      const packagedFrom = readFileSync(join(directory, "source-commit.txt"), "utf8").split(/\r?\n/)[0];
      if (report.ok !== true || (commit && packagedFrom !== commit)) continue;
    } catch { continue; }
    const current = found[match[1]];
    if (!current || match[3] > current.stamp) found[match[1]] = { directory, stamp: match[3] };
  }
  return Object.fromEntries(Object.entries(found).map(([plugin, item]) => [plugin, item!.directory]));
}

// Inspection ----------------------------------------------------------------------------------------------------------

export interface InstalledPlugin {
  /** Game-root-relative directories that hold `<Plugin>.uplugin`. */
  locations: string[];
  expectedPath?: string;
  /** Installed at `expectedPath`. */
  present: boolean;
  version?: string;
  misplaced: string[];
  files?: FileHash[];
  /** Whether the installed files equal the artifact, when one was given. */
  matchesArtifact?: boolean;
}
export interface InstallationReport {
  gameRoot: string;
  ok: boolean;
  problems: string[];
  sml?: { version?: string };
  plugins: Record<string, InstalledPlugin>;
}

/**
 * Every plugin descriptor under `FactoryGame/Mods`, the way the engine discovers them: recursively, without descending
 * into a directory that already holds a `.uplugin`.
 */
export function discoverPlugins(gameRoot: string): Record<string, string[]> {
  const mods = join(gameRoot, "FactoryGame", "Mods"), found: Record<string, string[]> = {};
  const walk = (directory: string) => {
    const entries = readdirSync(directory, { withFileTypes: true });
    const descriptors = entries.filter((entry) => entry.isFile() && entry.name.endsWith(".uplugin"));
    for (const descriptor of descriptors) (found[descriptor.name.slice(0, -".uplugin".length)] ??= []).push(slash(relative(gameRoot, directory)));
    if (descriptors.length === 0) for (const entry of entries) if (entry.isDirectory()) walk(join(directory, entry.name));
  };
  if (existsSync(mods)) walk(mods);
  for (const locations of Object.values(found)) locations.sort();
  return found;
}

/** Game-root-relative paths of every `.uplugin` under `path` (all depths) and of every symbolic link or junction there. */
function descriptorsAndLinks(gameRoot: string, path: string): { descriptors: string[]; links: string[] } {
  const descriptors: string[] = [], links: string[] = [];
  const walk = (directory: string) => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const full = join(directory, entry.name);
      if (entry.isSymbolicLink()) links.push(slash(relative(gameRoot, full)));
      else if (entry.isDirectory()) walk(full);
      else if (entry.name.toLowerCase().endsWith(".uplugin")) descriptors.push(slash(relative(gameRoot, full)));
    }
  };
  walk(join(gameRoot, path));
  return { descriptors: descriptors.sort(), links: links.sort() };
}
function isLink(path: string): boolean {
  try { return lstatSync(path).isSymbolicLink(); } catch { return false; }
}

/**
 * Whether a path segment is not a plain, canonical Windows name: empty, `.` or `..`, a separator, reserved, or control
 * character, a trailing dot or space (Windows strips them, so `SML.` and `SML ` open `SML`), or a `~` (an 8.3 short
 * name such as `SML~1` may open another directory). Such a segment could name a directory the lexical checks never saw.
 */
export const isAliasSegment = (segment: string) => segment === "" || segment === "." || segment === ".." || /[\\/:*?"<>|~\x00-\x1f]/.test(segment) || /[. ]$/.test(segment);

/**
 * Why the game-root-relative directory `path` may not be replaced or removed as `plugin`'s install; empty when it may.
 * It must be a real directory strictly inside `FactoryGame/Mods`, named after the plugin, without links, and must hold
 * no descriptor other than its own top-level `<plugin>.uplugin` (it may hold none: leftover files from an old install).
 * A missing directory has nothing to remove and no problems.
 */
export function removalProblems(gameRoot: string, path: string, plugin: string): string[] {
  const root = resolve(gameRoot), full = resolve(root, path), mods = join(root, "FactoryGame", "Mods"), relativePath = slash(relative(root, full));
  if (!isInside(mods, full) || full === resolve(mods)) return [`${relativePath} is not a plugin directory inside FactoryGame/Mods; the installer never replaces or removes it`];
  if (basename(full) !== plugin) return [`${relativePath} is not a directory dedicated to ${plugin} (it is named ${basename(full)}); the installer never replaces or removes it, so move ${plugin}'s files out by hand`];
  if (relativePath.split("/").some(isAliasSegment)) return [`${relativePath} has a segment Windows may resolve to another directory (a trailing dot or space, a short-name ~, or a reserved character); the installer never replaces or removes it`];
  if (isLink(full)) return [`${relativePath} is a link; the installer never replaces or removes it`];
  // A junction or symbolic link above the directory would redirect it elsewhere (for example into SML).
  const parts = relativePath.split("/");
  for (let index = 1; index < parts.length; index++) {
    const ancestor = parts.slice(0, index).join("/");
    if (isLink(join(root, ancestor))) return [`${relativePath} is reached through the link ${ancestor}; the installer never replaces or removes it`];
  }
  if (!existsSync(full)) return [];
  // The directory must resolve to itself under the game root, so no alias the lexical checks missed points elsewhere.
  let real: string | undefined;
  try { real = slash(relative(realpathSync.native(root), realpathSync.native(full))); } catch { /* reported below */ }
  if (real?.toLowerCase() !== relativePath.toLowerCase()) return [`${relativePath} resolves to ${real ?? "(unresolvable)"} under the game root, not to itself; the installer never replaces or removes it`];
  if (!statSync(full).isDirectory()) return [`${relativePath} is a file, not a plugin directory`];
  const { descriptors, links } = descriptorsAndLinks(root, relativePath), own = `${relativePath}/${plugin}.uplugin`, problems: string[] = [];
  const foreign = descriptors.filter((descriptor) => descriptor !== own);
  if (foreign.length > 0) problems.push(`${relativePath} also holds ${foreign.join(", ")}; it is not attributable to ${plugin} alone, so the installer will not replace or remove it`);
  if (links.length > 0) problems.push(`${relativePath} contains links (${links.join(", ")}); the installer will not replace or remove it`);
  return problems;
}

/** Descriptors in directories between `FactoryGame/Mods` (inclusive) and `path` (exclusive); each stops engine discovery. */
function shadowingDescriptors(gameRoot: string, path: string): string[] {
  const found: string[] = [], parts = path.split("/");
  for (let index = 2; index < parts.length; index++) {
    const directory = join(gameRoot, ...parts.slice(0, index));
    if (!existsSync(directory) || !statSync(directory).isDirectory()) break;
    for (const entry of readdirSync(directory, { withFileTypes: true })) if (entry.isFile() && entry.name.toLowerCase().endsWith(".uplugin")) found.push(`${parts.slice(0, index).join("/")}/${entry.name}`);
  }
  return found;
}

/** Files a working install of `plugin` needs, relative to its directory. */
function requiredFiles(plugin: InstallablePlugin, directory: string): string[] {
  const required = [`${plugin}.uplugin`, ...["pak", "ucas", "utoc"].map((extension) => `Content/Paks/Windows/${plugin}FactoryGame-Windows.${extension}`)];
  for (const manifest of listFiles(join(directory, "Binaries", "Win64")).filter((file) => file.endsWith(".modules"))) {
    try {
      const modules = (JSON.parse(readFileSync(manifest, "utf8").replace(/^﻿/, "")) as { Modules?: Record<string, string> }).Modules ?? {};
      required.push(...Object.values(modules).map((dll) => `Binaries/Win64/${dll}`));
    } catch { required.push(`${slash(relative(directory, manifest))} (valid JSON)`); }
  }
  if (descriptorHasModules(join(directory, `${plugin}.uplugin`)) && !required.some((file) => file.startsWith("Binaries/"))) required.push("Binaries/Win64/*.modules");
  return required;
}
function descriptorHasModules(path: string): boolean {
  try { return (readPluginDescriptor(path).Modules?.length ?? 0) > 0; } catch { return false; }
}

/** Reports the first-party plugin installation under `gameRoot`; `artifacts` adds file-by-file comparison. */
export function inspectInstallation(gameRoot: string, artifacts: readonly VerifiedArtifact[] = []): InstallationReport {
  const root = resolve(gameRoot), problems: string[] = [], plugins: Record<string, InstalledPlugin> = {};
  if (!existsSync(join(root, "FactoryGame"))) return { gameRoot: root, ok: false, problems: [`${root} has no FactoryGame directory; pass the game root that contains FactoryGame/`], plugins };
  const discovered = discoverPlugins(root), descriptors: Partial<Record<InstallablePlugin, PluginDescriptor>> = {};
  for (const [name, locations] of Object.entries(discovered)) {
    if (isInstallablePlugin(name)) continue;
    plugins[name] = { locations, present: true, misplaced: [] };
    if (locations.length > 1) problems.push(`${name} is installed ${locations.length} times (${locations.join(", ")}); the engine loads only one`);
  }
  const smlPath = join(root, "FactoryGame", "Mods", "SML", "SML.uplugin"), sml = existsSync(smlPath) ? readPluginDescriptor(smlPath) : undefined;
  if (!sml) problems.push("SML is not installed at FactoryGame/Mods/SML; both first-party plugins require it");
  for (const plugin of Object.keys(PLUGIN_INSTALL_PATHS) as InstallablePlugin[]) {
    const expectedPath = PLUGIN_INSTALL_PATHS[plugin], locations = discovered[plugin] ?? [], directory = join(root, expectedPath);
    const misplaced = locations.filter((location) => location !== expectedPath), present = locations.includes(expectedPath);
    const status: InstalledPlugin = { locations, expectedPath, present, misplaced };
    plugins[plugin] = status;
    for (const location of misplaced) {
      const fix = present ? "remove that copy" : `move it to ${expectedPath}`, unsafe = removalProblems(root, location, plugin);
      problems.push(`${plugin} is installed at ${location}, but its containers mount at ../../../${expectedPath}/; ${fix} (${unsafe.length === 0 ? "npm run road-pack:install -- install does this with a backup" : `by hand: ${unsafe.join("; ")}`})`);
    }
    if (!present) { if (misplaced.length === 0) problems.push(`${plugin} is not installed at ${expectedPath}`); continue; }
    const descriptor = readPluginDescriptor(join(directory, `${plugin}.uplugin`));
    descriptors[plugin] = descriptor;
    status.version = descriptor.SemVersion;
    status.files = hashTree(directory);
    for (const file of requiredFiles(plugin, directory)) if (!status.files.some((item) => item.path === file)) problems.push(`${expectedPath} is missing ${file}`);
    const smlRange = pluginDependency(descriptor, "SML")?.SemVersion;
    if (sml && smlRange && !(sml.SemVersion && satisfiesRange(sml.SemVersion, smlRange))) problems.push(`${plugin} ${descriptor.SemVersion} requires SML ${smlRange}; installed SML is ${sml.SemVersion ?? "(no SemVersion)"}`);
    const artifact = artifacts.find((item) => item.plugin === plugin);
    if (artifact) {
      status.matchesArtifact = sameTree(status.files, artifact.files);
      if (!status.matchesArtifact) problems.push(`${expectedPath} differs from artifact ${artifact.directory}`);
    }
  }
  if (descriptors.MKIBridge && descriptors.MKIRoads) problems.push(...dependencyContractProblems(descriptors.MKIBridge, descriptors.MKIRoads).map((problem) => `installed descriptors: ${problem}`));
  // A misplaced copy still satisfies the dependency (its descriptor is found), so this applies only when there is none.
  else if (descriptors.MKIBridge && pluginDependency(descriptors.MKIBridge, ROAD_PACK_PLUGIN) && plugins[ROAD_PACK_PLUGIN].locations.length === 0) problems.push(`MKIBridge requires ${ROAD_PACK_PLUGIN}, so the engine will not load the bridge until ${ROAD_PACK_PLUGIN} is installed at ${PLUGIN_INSTALL_PATHS.MKIRoads}`);
  return { gameRoot: root, ok: problems.length === 0, problems, sml: sml ? { version: sml.SemVersion } : undefined, plugins };
}

// Install -------------------------------------------------------------------------------------------------------------

export interface InstallAction {
  plugin: InstallablePlugin;
  /** Game-root-relative directory. */
  path: string;
  /**
   * `replace` backs up and replaces an existing target (an older install, or leftover files without a descriptor);
   * `install` creates a target that does not exist; `remove-misplaced` backs up and removes a misplaced copy.
   */
  kind: "install" | "replace" | "remove-misplaced";
  /** Files in a replaced or removed directory that the artifact does not contain (kept in the backup). */
  dropped?: string[];
}
export interface InstallPlan {
  gameRoot: string;
  backupRoot: string;
  artifacts: VerifiedArtifact[];
  actions: InstallAction[];
  /** Problems that block the install; `applyInstall` refuses a plan with any. */
  problems: string[];
  before: InstallationReport;
}
export interface InstallReceipt {
  format: "mki-roads.install-receipt";
  version: 1;
  createdAt: string;
  gameRoot: string;
  backupRoot: string;
  /**
   * `backed-up` while replacing, then `installed` or `rolled-back`; `restored` after `restoreInstall`. `rollback-failed`
   * and `restore-failed` mean the game tree may be partly changed: the backups are intact and `restoreInstall` retries.
   */
  state: "backed-up" | "installed" | "rolled-back" | "restored" | "rollback-failed" | "restore-failed";
  artifacts: { plugin: string; version: string; directory: string; zipSha256: string; commit: string; installPath: string }[];
  actions: InstallAction[];
  /** Backed-up directories (game-root-relative) and their file hashes. */
  backups: { path: string; files: FileHash[] }[];
  after?: InstallationReport;
  /** Why the install failed. */
  error?: string;
  /** Why the last rollback or restore failed, while it has not been retried successfully. */
  restoreError?: string;
}

export const RECEIPT_FILE = "install-receipt.json";

/** Plans installing verified artifacts into `gameRoot`, with backups under `backupRoot` (outside the game's Mods folder). */
export function planInstall(gameRoot: string, artifacts: readonly VerifiedArtifact[], backupRoot: string): InstallPlan {
  const root = resolve(gameRoot), backup = resolve(backupRoot), before = inspectInstallation(root), problems: string[] = [], actions: InstallAction[] = [];
  if (!existsSync(join(root, "FactoryGame"))) problems.push(`${root} has no FactoryGame directory; pass the game root that contains FactoryGame/`);
  if (!before.sml) problems.push("SML is not installed at FactoryGame/Mods/SML; install SML before the first-party plugins");
  if (isInside(join(root, "FactoryGame", "Mods"), backup)) problems.push(`backup directory ${backup} is inside FactoryGame/Mods, where the engine would load the backed-up plugins`);
  if (existsSync(backup) && readdirSync(backup).length > 0) problems.push(`backup directory ${backup} is not empty`);
  if (artifacts.length === 0) problems.push("no artifacts to install");
  const seen = new Set<string>();
  for (const artifact of artifacts) {
    if (seen.has(artifact.plugin)) problems.push(`${artifact.plugin} is given twice`);
    seen.add(artifact.plugin);
    if (artifact.installPath !== PLUGIN_INSTALL_PATHS[artifact.plugin]) problems.push(`${artifact.plugin} artifact mounts at ${artifact.installPath}, not ${PLUGIN_INSTALL_PATHS[artifact.plugin]}`);
    const status = before.plugins[artifact.plugin], incoming = new Set(artifact.files.map((file) => file.path));
    const dropped = (path: string) => hashTree(join(root, path)).map((file) => file.path).filter((file) => !incoming.has(file));
    // Anything already at the target (an install, or leftovers without a descriptor) is backed up and replaced, never merged.
    const target = join(root, artifact.installPath), targetProblems = removalProblems(root, artifact.installPath, artifact.plugin);
    if (targetProblems.length > 0) problems.push(...targetProblems);
    else actions.push(existsSync(target) ? { plugin: artifact.plugin, path: artifact.installPath, kind: "replace", dropped: dropped(artifact.installPath) } : { plugin: artifact.plugin, path: artifact.installPath, kind: "install" });
    for (const path of shadowingDescriptors(root, artifact.installPath)) problems.push(`${path} stops the engine's plugin discovery above ${artifact.installPath}, so ${artifact.plugin} would not load there; remove that stray descriptor by hand`);
    for (const path of status?.misplaced ?? []) {
      const unsafe = removalProblems(root, path, artifact.plugin);
      if (unsafe.length > 0) problems.push(`${artifact.plugin} is misplaced at ${path}, which the installer will not remove: ${unsafe.join("; ")}`);
      else actions.push({ plugin: artifact.plugin, path, kind: "remove-misplaced", dropped: dropped(path) });
    }
  }
  // The descriptors that will be installed must keep the dependency contract with each other and with the installed SML.
  const finalDescriptor = (plugin: InstallablePlugin): PluginDescriptor | undefined => {
    const artifact = artifacts.find((item) => item.plugin === plugin);
    if (artifact) return artifact.descriptor;
    const path = join(root, PLUGIN_INSTALL_PATHS[plugin], `${plugin}.uplugin`);
    return existsSync(path) ? readPluginDescriptor(path) : undefined;
  };
  const bridge = finalDescriptor("MKIBridge"), roadPack = finalDescriptor("MKIRoads");
  if (bridge && roadPack) problems.push(...dependencyContractProblems(bridge, roadPack));
  else if (bridge && !roadPack) problems.push(`MKIBridge requires ${ROAD_PACK_PLUGIN}; install both artifacts`);
  for (const artifact of artifacts) {
    const range = pluginDependency(artifact.descriptor, "SML")?.SemVersion;
    if (before.sml && range && !(before.sml.version && satisfiesRange(before.sml.version, range))) problems.push(`${artifact.plugin} ${artifact.version} requires SML ${range}; installed SML is ${before.sml.version ?? "(no SemVersion)"}`);
  }
  return { gameRoot: root, backupRoot: backup, artifacts: [...artifacts], actions, problems, before };
}

/** Directories the installer never replaces, removes, or restores into, whatever a plan or receipt says (case-insensitive). */
export const PROTECTED_PATHS = ["FactoryGame", "FactoryGame/Mods", "FactoryGame/Mods/GameFeatures", "FactoryGame/Mods/SML"] as const;
const samePath = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();
const isDescendant = (ancestor: string, path: string) => path.toLowerCase().startsWith(`${ancestor.toLowerCase()}/`);

const actionSchema = z.object({ plugin: z.string(), path: z.string(), kind: z.enum(["install", "replace", "remove-misplaced"]), dropped: z.array(z.string()).optional() });
const fileHashSchema = z.object({ path: z.string().min(1), sha256: z.string().regex(HEX64) });
const receiptSchema = z.object({
  format: z.literal("mki-roads.install-receipt"), version: z.literal(1), gameRoot: z.string().min(1), backupRoot: z.string().min(1),
  state: z.enum(["backed-up", "installed", "rolled-back", "restored", "rollback-failed", "restore-failed"]),
  actions: z.array(z.unknown()), backups: z.array(z.object({ path: z.string(), files: z.array(fileHashSchema) })),
});

/**
 * Why `action` is not one the installer could have planned; empty when it is. Checked statically, so a forged or corrupt
 * plan or receipt can never name SML, a third-party plugin, `Mods`, or `Mods/GameFeatures`:
 * - the plugin is a first-party plugin (`PLUGIN_INSTALL_PATHS`);
 * - `install` and `replace` target exactly that plugin's install path;
 * - `remove-misplaced` targets some other normalized directory under `FactoryGame/Mods` that is named after the plugin and
 *   is neither a protected path nor inside SML or a first-party install path.
 * `removalProblems` (on disk) and the backup checks in `restoreProblems` add the attributability checks.
 */
export function actionProblems(action: unknown): string[] {
  const parsed = actionSchema.safeParse(action);
  if (!parsed.success) return [`action ${JSON.stringify(action)} is malformed: ${parsed.error.issues.map((issue) => `${issue.path.join(".") || "(action)"} ${issue.message}`).join("; ")}`];
  const { plugin, path, kind } = parsed.data, label = `${kind} ${JSON.stringify(path)} for ${JSON.stringify(plugin)}`;
  if (!isInstallablePlugin(plugin)) return [`${label}: ${JSON.stringify(plugin)} is not a first-party plugin (${Object.keys(PLUGIN_INSTALL_PATHS).join(", ")}); the installer never touches it`];
  if (PROTECTED_PATHS.some((protectedPath) => samePath(path, protectedPath))) return [`${label}: ${path} is protected; the installer never replaces or removes it`];
  const segments = path.split("/");
  if (segments.length < 3 || segments[0] !== "FactoryGame" || segments[1] !== "Mods" || segments.some(isAliasSegment)) {
    return [`${label}: the path is not a normalized directory under FactoryGame/Mods (a segment is empty, relative, or a Windows alias: a trailing dot or space, a short-name ~, or a reserved character)`];
  }
  const expected = PLUGIN_INSTALL_PATHS[plugin];
  if (kind !== "remove-misplaced") return path === expected ? [] : [`${label}: ${kind} must target ${expected}`];
  const problems: string[] = [];
  if (samePath(path, expected)) problems.push(`${label}: a misplaced copy cannot be at the install path ${expected}`);
  if (segments.at(-1) !== plugin) problems.push(`${label}: the directory is not named ${plugin}`);
  const inside = ["FactoryGame/Mods/SML", ...Object.values(PLUGIN_INSTALL_PATHS)].filter((ancestor) => isDescendant(ancestor, path));
  if (inside.length > 0) problems.push(`${label}: the directory is inside ${inside.join(", ")}, which the installer never changes`);
  return problems;
}

/** Why a receipt (or a plan's actions and backups) could not have come from this installer; empty when it could. */
export function receiptProblems(receipt: unknown): string[] {
  const parsed = receiptSchema.safeParse(receipt);
  if (!parsed.success) return [`the receipt is malformed: ${parsed.error.issues.map((issue) => `${issue.path.join(".") || "(receipt)"} ${issue.message}`).join("; ")}`];
  const { actions, backups } = parsed.data, problems = actions.flatMap(actionProblems), seen = new Set<string>();
  for (const action of actions as InstallAction[]) {
    if (seen.has(String(action.path).toLowerCase())) problems.push(`the receipt names ${action.path} more than once`);
    seen.add(String(action.path).toLowerCase());
  }
  for (const backup of backups) {
    const action = (actions as InstallAction[]).find((item) => item.path === backup.path && item.kind !== "install");
    if (!action) continue; // restoreProblems reports a backup without a replace or remove action.
    if (backup.files.some((file) => file.path.split("/").some((segment) => segment === "" || /^[. ]+$/.test(segment) || segment.includes("\\") || segment.includes(":")))) problems.push(`backup ${backup.path} records a file path outside its directory`);
    // A backup holds what the installer removed: never another plugin's descriptor, and a misplaced copy always its own.
    const descriptors = backup.files.map((file) => file.path).filter((file) => file.toLowerCase().endsWith(".uplugin")), own = `${action.plugin}.uplugin`;
    const foreign = descriptors.filter((file) => file !== own);
    if (foreign.length > 0) problems.push(`backup ${backup.path} holds ${foreign.join(", ")}; it is not attributable to ${action.plugin} alone`);
    if (action.kind === "remove-misplaced" && !descriptors.includes(own)) problems.push(`backup ${backup.path} has no ${own}; a misplaced copy is only ever a directory with its own descriptor`);
  }
  return problems;
}

function writeReceipt(receipt: InstallReceipt) {
  mkdirSync(receipt.backupRoot, { recursive: true });
  writeFileSync(join(receipt.backupRoot, RECEIPT_FILE), JSON.stringify(receipt, null, 2) + "\n");
}

/** Why `restoreFromBackup` may not start: every backup source and every directory it would delete is checked first. */
function restoreProblems(receipt: InstallReceipt): string[] {
  const invalid = receiptProblems(receipt);
  if (invalid.length > 0) return invalid;
  const problems: string[] = [];
  for (const action of receipt.actions) {
    problems.push(...removalProblems(receipt.gameRoot, action.path, action.plugin));
    // A misplaced copy was found by engine-style discovery, so no directory above it held a descriptor (a third-party plugin).
    if (action.kind === "remove-misplaced") for (const path of shadowingDescriptors(receipt.gameRoot, action.path)) problems.push(`${action.path} is inside a directory that holds ${path}; the installer never restores into another plugin`);
    if (action.kind !== "install" && !receipt.backups.some((backup) => backup.path === action.path)) problems.push(`the receipt has no backup of ${action.path}, which the install ${action.kind === "replace" ? "replaced" : "removed"}`);
  }
  for (const backup of receipt.backups) {
    const saved = join(receipt.backupRoot, backup.path);
    if (!receipt.actions.some((action) => action.path === backup.path && action.kind !== "install")) problems.push(`backup ${backup.path} belongs to no replace or remove action`);
    else if (!isInside(receipt.backupRoot, saved) || isLink(saved)) problems.push(`backup ${saved} is not a plain directory inside ${receipt.backupRoot}`);
    else if (backup.files.length > 0 && !existsSync(saved)) problems.push(`backup ${saved} is missing`);
    else if (!sameTree(hashTree(saved), backup.files)) problems.push(`backup ${saved} no longer matches the receipt's file hashes`);
  }
  return problems;
}

/**
 * Removes the directories the install created, replaced, or removed and puts every backed-up directory back. Nothing is
 * deleted unless every backup source is present and hash-identical to the receipt; the backups are only read, so a
 * restore that fails part-way can be retried.
 */
function restoreFromBackup(receipt: InstallReceipt, afterDelete?: () => void) {
  const problems = restoreProblems(receipt);
  if (problems.length > 0) throw new Error(`Restore refused before changing anything:\n- ${problems.join("\n- ")}`);
  for (const action of receipt.actions) rmSync(join(receipt.gameRoot, action.path), { recursive: true, force: true });
  afterDelete?.();
  for (const backup of receipt.backups) {
    const target = join(receipt.gameRoot, backup.path), saved = join(receipt.backupRoot, backup.path);
    copyTree(saved, target);
    if (!sameTree(hashTree(target), backup.files)) throw new Error(`Restoring ${backup.path} from ${saved} did not reproduce the backed-up files`);
  }
}

export interface ApplyOptions {
  /** Test hook: runs after the backups are taken and the old directories removed, before the artifacts are copied. */
  afterRemove?: () => void;
  /** Test hook: runs after a failure, before the rollback restores the backups. */
  beforeRollback?: () => void;
}

/**
 * Applies a plan: backs up and hash-checks every directory it replaces or removes, removes them, copies each artifact's
 * contents into its install directory, and checks the result with `inspectInstallation`. Any failure after the backup
 * restores it. The receipt (`install-receipt.json` in the backup directory) records every step.
 */
export function applyInstall(plan: InstallPlan, options: ApplyOptions = {}): InstallReceipt {
  if (plan.problems.length > 0) throw new Error(`Install refused:\n- ${plan.problems.join("\n- ")}`);
  const receipt: InstallReceipt = {
    format: "mki-roads.install-receipt", version: 1, createdAt: new Date().toISOString(), gameRoot: plan.gameRoot, backupRoot: plan.backupRoot, state: "backed-up",
    artifacts: plan.artifacts.map((item) => ({ plugin: item.plugin, version: item.version, directory: item.directory, zipSha256: item.zipSha256, commit: item.commit, installPath: item.installPath })),
    actions: plan.actions, backups: [],
  };
  const forged = plan.actions.flatMap(actionProblems);
  if (forged.length > 0) throw new Error(`Install refused; the plan has actions the installer never plans:\n- ${forged.join("\n- ")}`);
  // The tree may have changed since planning, so every directory is checked again before anything is copied or deleted.
  const changed = plan.actions.flatMap((action) => [
    ...removalProblems(plan.gameRoot, action.path, action.plugin),
    ...(action.kind === "install" && existsSync(join(plan.gameRoot, action.path)) ? [`${action.path} appeared after planning`] : []),
    ...(action.kind !== "install" && !existsSync(join(plan.gameRoot, action.path)) ? [`${action.path} disappeared after planning`] : []),
  ]);
  if (changed.length > 0) throw new Error(`Install refused; the game tree changed since planning:\n- ${changed.join("\n- ")}`);
  for (const action of plan.actions) {
    if (action.kind === "install") continue;
    const source = join(plan.gameRoot, action.path), saved = join(plan.backupRoot, action.path), files = hashTree(source);
    copyTree(source, saved);
    if (!sameTree(hashTree(saved), files)) throw new Error(`Backup of ${action.path} to ${saved} does not match the installed files; nothing was changed`);
    receipt.backups.push({ path: action.path, files });
  }
  writeReceipt(receipt);
  try {
    for (const action of plan.actions) rmSync(join(plan.gameRoot, action.path), { recursive: true, force: true });
    options.afterRemove?.();
    for (const artifact of plan.artifacts) {
      const target = join(plan.gameRoot, artifact.installPath);
      copyTree(artifact.contents, target);
      if (!sameTree(hashTree(target), artifact.files)) throw new Error(`${artifact.installPath} does not match artifact ${artifact.directory} after copying`);
    }
    const after = inspectInstallation(plan.gameRoot, plan.artifacts);
    const ours = after.problems.filter((problem) => plan.artifacts.some((artifact) => problem.includes(artifact.plugin)) || problem.startsWith("installed descriptors"));
    if (ours.length > 0) throw new Error(`The installed plugins fail inspection:\n- ${ours.join("\n- ")}`);
    receipt.state = "installed";
    receipt.after = after;
    writeReceipt(receipt);
    return receipt;
  } catch (error) {
    receipt.error = error instanceof Error ? error.message : String(error);
    try {
      options.beforeRollback?.();
      restoreFromBackup(receipt);
    } catch (rollbackError) {
      receipt.state = "rollback-failed";
      receipt.restoreError = rollbackError instanceof Error ? rollbackError.message : String(rollbackError);
      writeReceipt(receipt);
      throw new Error(`Install failed (${receipt.error}) and its rollback also failed (${receipt.restoreError}); the game tree may be partly changed. Fix the cause and retry: npm run road-pack:install -- restore --game "${plan.gameRoot}" --backup "${plan.backupRoot}"`);
    }
    receipt.state = "rolled-back";
    writeReceipt(receipt);
    throw new Error(`Install failed and was rolled back from ${plan.backupRoot}: ${receipt.error}`);
  }
}

/** Receipt states `restoreInstall` accepts: a completed or interrupted install, and failed rollbacks or restores (retries). */
export const RESTORABLE_STATES: readonly InstallReceipt["state"][] = ["installed", "backed-up", "rollback-failed", "restore-failed"];

/**
 * Undoes an install recorded in `<backupRoot>/install-receipt.json`: removes what it installed and restores every
 * backed-up directory. A refusal (a backup missing or changed, or a directory no longer safe to delete) changes nothing;
 * a failure part-way records `restore-failed`, which a later call retries.
 */
export function restoreInstall(backupRoot: string, gameRoot?: string, options: { afterDelete?: () => void } = {}): InstallReceipt {
  const path = join(resolve(backupRoot), RECEIPT_FILE);
  if (!existsSync(path)) throw new Error(`${path} does not exist`);
  let receipt: InstallReceipt;
  try { receipt = JSON.parse(readFileSync(path, "utf8")) as InstallReceipt; } catch (error) { throw new Error(`${path} is not valid JSON: ${error instanceof Error ? error.message : String(error)}`); }
  if (receipt?.format !== "mki-roads.install-receipt") throw new Error(`${path} is not an install receipt`);
  const invalid = receiptProblems(receipt);
  if (invalid.length > 0) throw new Error(`Restore refused before changing anything; ${path} is not a receipt this installer could have written:\n- ${invalid.join("\n- ")}`);
  if (gameRoot && resolve(gameRoot) !== resolve(receipt.gameRoot)) throw new Error(`${path} records game root ${receipt.gameRoot}, not ${resolve(gameRoot)}`);
  if (!RESTORABLE_STATES.includes(receipt.state)) throw new Error(`${path} is ${receipt.state}; only a receipt that is ${RESTORABLE_STATES.join(", ")} can be restored`);
  if (resolve(receipt.backupRoot) !== resolve(backupRoot)) throw new Error(`${path} records backup directory ${receipt.backupRoot}, not ${resolve(backupRoot)}`);
  const problems = restoreProblems(receipt);
  if (problems.length > 0) throw new Error(`Restore refused before changing anything:\n- ${problems.join("\n- ")}`);
  try {
    restoreFromBackup(receipt, options.afterDelete);
  } catch (error) {
    receipt.state = "restore-failed";
    receipt.restoreError = error instanceof Error ? error.message : String(error);
    writeReceipt(receipt);
    throw new Error(`Restore from ${receipt.backupRoot} failed (${receipt.restoreError}); the game tree may be partly changed and the backups are intact, so retry the restore`);
  }
  delete receipt.restoreError;
  receipt.state = "restored";
  writeReceipt(receipt);
  return receipt;
}

/** A fresh, timestamped backup directory name. */
export const installBackupName = () => `install-${stamp()}`;
