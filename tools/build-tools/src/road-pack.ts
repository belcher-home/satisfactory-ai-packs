import { readFileSync } from "node:fs";
import * as z from "zod/v4";

/**
 * MKI Roads companion contract (issue #63).
 *
 * MKI Roads is a first-party content mod released from this repository. It is usable on its
 * own and must not depend on the bridge. MKI Bridge declares it as a required
 * (non-optional), versioned plugin dependency, so the engine and mod managers only load the bridge
 * with a compatible MKI Roads installed. The bridge still reports the contract in
 * `/v1/capabilities` (`companions[]` with `required: true`, and `features.road_network`), and
 * planners check it fail-closed before emitting MKI Roads placements.
 */

export const ROAD_PACK_PLUGIN = "MKIRoads";
export const ROAD_NETWORK_FEATURE = "road_network";

export interface SemVer { major: number; minor: number; patch: number }

export function parseSemVer(text: string): SemVer | undefined {
  const match = /^v?(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$/.exec(text.trim());
  return match ? { major: Number(match[1]), minor: Number(match[2]), patch: Number(match[3]) } : undefined;
}
export function compareSemVer(a: SemVer, b: SemVer): number {
  return a.major - b.major || a.minor - b.minor || a.patch - b.patch;
}
function comparatorMatches(version: SemVer, comparator: string): boolean {
  const match = /^(\^|~|>=|<=|>|<|=)?\s*(.+)$/.exec(comparator.trim());
  const bound = match ? parseSemVer(match[2]) : undefined;
  if (!match || !bound) throw new Error(`Unsupported version comparator ${JSON.stringify(comparator)}`);
  const order = compareSemVer(version, bound);
  switch (match[1] ?? "=") {
    case "=": return order === 0;
    case ">=": return order >= 0;
    case "<=": return order <= 0;
    case ">": return order > 0;
    case "<": return order < 0;
    case "~": return order >= 0 && version.major === bound.major && version.minor === bound.minor;
    default: {
      // Caret: the left-most non-zero component is fixed (node-semver and SML semantics).
      if (order < 0) return false;
      if (bound.major > 0) return version.major === bound.major;
      if (bound.minor > 0) return version.major === 0 && version.minor === bound.minor;
      return version.major === 0 && version.minor === 0 && version.patch === bound.patch;
    }
  }
}
/** `range` supports `||` alternatives of space-separated comparators (`^`, `~`, `=`, `>=`, `<=`, `>`, `<`). */
export function satisfiesRange(version: string, range: string): boolean {
  const parsed = parseSemVer(version);
  if (!parsed) return false;
  return range.split("||").some((alternative) => alternative.trim().split(/\s+/).filter(Boolean).every((comparator) => comparatorMatches(parsed, comparator)));
}

export interface PluginDescriptor {
  SemVersion?: string; VersionName?: string; CanContainContent?: boolean; Modules?: unknown[];
  Plugins?: { Name: string; Enabled?: boolean; Optional?: boolean; SemVersion?: string; BasePlugin?: boolean }[];
}
export function readPluginDescriptor(path: string): PluginDescriptor {
  return JSON.parse(readFileSync(path, "utf8").replace(/^﻿/, "")) as PluginDescriptor;
}
/** The dependency entry a plugin descriptor declares for `name`, if any. */
export function pluginDependency(descriptor: PluginDescriptor, name: string) {
  return descriptor.Plugins?.find((item) => item.Name === name);
}

const companionSchema = z.object({
  name: z.string(),
  declared: z.boolean(),
  required: z.boolean(),
  requiredVersion: z.string(),
  present: z.boolean(),
  version: z.string().optional(),
  compatible: z.boolean(),
  mountPoint: z.string(),
  features: z.array(z.string()),
  reason: z.string().optional(),
});
const capabilitiesSchema = z.object({
  companions: z.array(companionSchema).optional(),
  features: z.record(z.string(), z.object({ available: z.boolean(), requires: z.array(z.string()), reason: z.string().optional() })).optional(),
});
export type BridgeCompanion = z.infer<typeof companionSchema>;

export type RoadNetworkAvailability =
  | { available: true; roadPackVersion: string; requiredVersion: string; mountPoint: string }
  | { available: false; reason: string };

/**
 * Decide from a `/v1/capabilities` response whether MKI Roads placements may be emitted for a
 * manifest built against `manifestVersion`. Fails closed: a bridge that does not report companions,
 * a missing or incompatible MKI Roads, or an installed release older than the manifest all refuse.
 */
export function roadNetworkAvailability(capabilities: unknown, manifestVersion: string): RoadNetworkAvailability {
  const parsed = capabilitiesSchema.safeParse(capabilities);
  if (!parsed.success || !parsed.data.companions || !parsed.data.features) return { available: false, reason: "the bridge does not report companion mods; update MKI Bridge" };
  const companion = parsed.data.companions.find((item) => item.name === ROAD_PACK_PLUGIN);
  if (!companion?.declared) return { available: false, reason: `the bridge does not declare ${ROAD_PACK_PLUGIN}` };
  if (!companion.required) return { available: false, reason: `the bridge declares ${ROAD_PACK_PLUGIN} optional, not as its required dependency; update MKI Bridge` };
  if (!companion.present || !companion.version) return { available: false, reason: `${ROAD_PACK_PLUGIN} is not installed (the bridge requires ${companion.requiredVersion})` };
  if (!companion.compatible || !satisfiesRange(companion.version, companion.requiredVersion)) return { available: false, reason: `${ROAD_PACK_PLUGIN} ${companion.version} does not satisfy the bridge requirement ${companion.requiredVersion}` };
  if (!satisfiesRange(companion.version, `^${manifestVersion}`)) return { available: false, reason: `installed ${ROAD_PACK_PLUGIN} ${companion.version} does not provide the assets of manifest version ${manifestVersion}` };
  const feature = parsed.data.features[ROAD_NETWORK_FEATURE];
  if (!feature?.available) return { available: false, reason: `the bridge reports ${ROAD_NETWORK_FEATURE} unavailable${feature?.reason ? `: ${feature.reason}` : ""}` };
  return { available: true, roadPackVersion: companion.version, requiredVersion: companion.requiredVersion, mountPoint: companion.mountPoint };
}

/**
 * The dependency contract between the two plugin descriptors: the bridge requires (non-optional, enabled) a
 * SemVer range that the MKI Roads release satisfies, and MKI Roads does not depend on the bridge.
 */
export function dependencyContractProblems(bridge: PluginDescriptor, roadPack: PluginDescriptor, bridgeName = "MKIBridge"): string[] {
  const problems: string[] = [], dependency = pluginDependency(bridge, ROAD_PACK_PLUGIN);
  if (!dependency) problems.push(`${bridgeName} does not declare ${ROAD_PACK_PLUGIN}`);
  else {
    if (dependency.Optional) problems.push(`${bridgeName} declares ${ROAD_PACK_PLUGIN} optional; it must be required`);
    if (dependency.Enabled === false) problems.push(`${bridgeName} disables ${ROAD_PACK_PLUGIN}`);
    if (!dependency.SemVersion) problems.push(`${bridgeName} declares no SemVersion range for ${ROAD_PACK_PLUGIN}`);
    else if (!roadPack.SemVersion || !satisfiesRange(roadPack.SemVersion, dependency.SemVersion)) problems.push(`${ROAD_PACK_PLUGIN} ${roadPack.SemVersion ?? "(no SemVersion)"} does not satisfy the ${bridgeName} requirement ${dependency.SemVersion}`);
  }
  if (pluginDependency(roadPack, bridgeName)) problems.push(`${ROAD_PACK_PLUGIN} must not depend on ${bridgeName}`);
  return problems;
}

/**
 * Where each first-party plugin is installed, relative to the game root. Alpakit bakes the plugin's SDK location into
 * its pak and IoStore mount points (`../../../<path>/`), so the installed directory must be exactly that path: MKI
 * Roads is an ordinary mod under `Mods/`, the bridge a game-feature mod under `Mods/GameFeatures/`. A plugin
 * installed anywhere else still has its descriptor found, but its content mounts somewhere the plugin does not own.
 */
export const PLUGIN_INSTALL_PATHS = {
  MKIRoads: "FactoryGame/Mods/MKIRoads",
  MKIBridge: "FactoryGame/Mods/GameFeatures/MKIBridge",
} as const;
export type InstallablePlugin = keyof typeof PLUGIN_INSTALL_PATHS;
export const isInstallablePlugin = (name: string): name is InstallablePlugin => Object.hasOwn(PLUGIN_INSTALL_PATHS, name);
/** The container mount root a packaged plugin must use: `../../../FactoryGame/Mods/MKIRoads/`. */
export const pluginMountRoot = (plugin: InstallablePlugin) => `../../../${PLUGIN_INSTALL_PATHS[plugin]}/`;

/** The pak and IoStore mount points named in `UnrealPak -List` output, `/`-terminated. */
export function containerMountPoints(listing: readonly string[]): string[] {
  const mounts = new Set<string>();
  for (const line of listing) {
    const mounted = /mount point "?([^"\s]+)"?/i.exec(line)?.[1];
    if (mounted) mounts.add(mounted.endsWith("/") ? mounted : `${mounted}/`);
  }
  return [...mounts];
}

/**
 * The game-root-relative install directory that a container listing's mount points require
 * (`../../../FactoryGame/Mods/MKIRoads/` and `.../MKIRoads/Content/` give `FactoryGame/Mods/MKIRoads`).
 * Undefined unless every mount point is under one plugin directory named `plugin`.
 */
export function installPathFromListing(plugin: string, listing: readonly string[]): string | undefined {
  const paths = new Set(containerMountPoints(listing).map((mount) => {
    const match = /^\.\.\/\.\.\/\.\.\/(FactoryGame\/Mods\/(?:[^/]+\/)*?([^/]+))\/(?:Content\/)?$/.exec(mount);
    return match && match[2] === plugin ? match[1] : undefined;
  }));
  return paths.size === 1 ? [...paths][0] : undefined;
}

/** SML-distributed module manifests carry this BuildId; a raw UBT BuildId makes the shipped game refuse the DLL. */
export const DISTRIBUTABLE_BUILD_ID = "SML";

export interface PackagedArtifact {
  plugin: string;
  version: string;
  /** Paths inside the Alpakit archive, `/`-separated. */
  files: string[];
  /** `UnrealPak -List` output for every .pak/.utoc container in the archive. */
  containerListing: string[];
  /** `.modules` manifest text by archive path. */
  modulesManifests: Record<string, string>;
  /** Native plugins ship DLLs; content-only plugins must not. */
  expectsBinaries: boolean;
  /** The native module names the plugin descriptor declares (`Modules[].Name`); required when `expectsBinaries`. */
  nativeModules?: string[];
  /** Asset names (without extension) that the containers must hold. */
  expectedAssets?: string[];
  /**
   * When this package run started and each archived binary's modification time (ms since the epoch), by archive path
   * (#134). A native plugin's store DLLs must have been linked during the run: UBT once treated a stale Steam DLL as up
   * to date and only relinked the EGS target, so the archive shipped an older bridge that still passed every other check.
   */
  freshness?: { packageStartedMs: number; binaryModifiedMs: Record<string, number> };
}

/** Slack for the archive's 2 s zip timestamps and clock rounding when comparing a binary's time with the package start (#134). */
export const BINARY_FRESHNESS_SLACK_MS = 5000;

/**
 * The plugin's shipping build outputs in its SDK Binaries directory (file names), which packaging moves aside before UAT
 * runs so both store targets relink from the synced source (#134). Editor binaries (UnrealEditor-*) are left alone.
 */
export function shippingBuildOutputs(names: readonly string[]): string[] {
  return names.filter((name) => /^FactoryGame(?:Steam|EGS)(?:-.+)?\.(?:dll|pdb|modules)$/i.test(name)).sort();
}

/**
 * The game targets Alpakit's Windows (`Targets=Windows`) Shipping package builds a plugin's modules for, one per store:
 * `FactoryGameSteam` and `FactoryGameEGS`. Each writes one manifest and one DLL per module into `Binaries/Win64`.
 */
export const STORE_TARGETS = ["Steam", "EGS"] as const;
export type StoreTarget = (typeof STORE_TARGETS)[number];
export const storeModulesManifest = (store: StoreTarget) => `Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`;
export const storeModuleBinary = (store: StoreTarget, module: string) => `FactoryGame${store}-${module}-Win64-Shipping.dll`;
/** The native module names a plugin descriptor declares. */
export const descriptorModules = (descriptor: PluginDescriptor) => (descriptor.Modules ?? []).flatMap((item) => typeof (item as { Name?: unknown })?.Name === "string" ? [(item as { Name: string }).Name] : []);

/**
 * File paths from `UnrealPak <container> -List` output. Pak entries are relative to the pak's mount point
 * (`Listing X.pak with mount point "../../../FactoryGame/Mods/P/"`); IoStore entries are already mounted.
 */
export function containerEntries(listing: readonly string[]): string[] {
  const entries: string[] = [];
  let mount = "";
  for (const line of listing) {
    const mounted = /mount point "?([^"\s]+)"?/i.exec(line);
    if (mounted) { mount = mounted[1].endsWith("/") ? mounted[1] : `${mounted[1]}/`; continue; }
    const entry = /"([^"]+)" offset:/.exec(line)?.[1];
    if (!entry || entry.startsWith("<")) continue;
    entries.push(entry.startsWith("../") ? entry : `${mount}${entry}`);
  }
  return entries;
}

/** Release checks for an Alpakit archive: descriptor, containers, plugin-only content, and distributable binaries. */
export function packagedArtifactProblems(artifact: PackagedArtifact): string[] {
  const problems: string[] = [], files = new Set(artifact.files);
  if (!files.has(`${artifact.plugin}.uplugin`)) problems.push(`${artifact.plugin}.uplugin is missing from the archive root`);
  for (const extension of ["pak", "ucas", "utoc"]) {
    const container = `Content/Paks/Windows/${artifact.plugin}FactoryGame-Windows.${extension}`;
    if (!files.has(container)) problems.push(`${container} is missing`);
  }
  // Every container must mount at the plugin's install directory (PLUGIN_INSTALL_PATHS); other plugins may use either
  // the ordinary (Mods/<Plugin>/) or the game-feature (Mods/GameFeatures/<Plugin>/) layout, but only one of them.
  const entries = containerEntries(artifact.containerListing);
  const roots = isInstallablePlugin(artifact.plugin) ? [pluginMountRoot(artifact.plugin)] : [`../../../FactoryGame/Mods/${artifact.plugin}/`, `../../../FactoryGame/Mods/GameFeatures/${artifact.plugin}/`];
  if (entries.length === 0) problems.push("the container listing is empty");
  for (const mount of containerMountPoints(artifact.containerListing)) if (!roots.some((root) => mount.startsWith(root))) problems.push(`container mount point ${mount} is not the ${artifact.plugin} install directory ${roots.join(" or ")}`);
  if (entries.length > 0 && !installPathFromListing(artifact.plugin, artifact.containerListing)) problems.push(`the ${artifact.plugin} containers do not share one install directory`);
  for (const entry of entries) {
    if (/(^|\/)FactoryGame\/Content\//.test(entry)) problems.push(`base-game content is packaged: ${entry}`);
    else if (!roots.some((root) => entry.startsWith(root))) problems.push(`entry outside the ${artifact.plugin} mount: ${entry}`);
  }
  for (const name of artifact.expectedAssets ?? []) if (!entries.some((entry) => entry.endsWith(`/${name}.uasset`))) problems.push(`${name}.uasset is not packaged`);
  const binaries = artifact.files.filter((file) => file.startsWith("Binaries/"));
  if (!artifact.expectsBinaries && binaries.length > 0) problems.push(`a content-only plugin ships binaries: ${binaries.join(", ")}`);
  if (artifact.expectsBinaries) {
    // Alpakit archives whatever is in the SDK Binaries directory, so a stale manifest, a DLL from an earlier build, or a
    // renamed module would ship unnoticed. Every store-target manifest must be packaged and only those count; each must
    // name exactly the descriptor's modules with their store-target DLLs, and every binary must be one of those
    // manifests, DLLs, or .pdbs.
    const modules = artifact.nativeModules ?? [], expectedKeys = JSON.stringify([...modules].sort());
    if (modules.length === 0) problems.push(`${artifact.plugin}.uplugin declares no native modules for a plugin built with binaries`);
    const manifests = Object.entries(artifact.modulesManifests), expectedManifests = STORE_TARGETS.map(storeModulesManifest);
    if (manifests.length === 0) problems.push("no .modules manifest is packaged");
    else for (const path of expectedManifests) if (!Object.hasOwn(artifact.modulesManifests, path)) problems.push(`${path} is missing; a native plugin ships one manifest per store target`);
    const accepted = new Set<string>();
    for (const [path, text] of manifests) {
      const store = STORE_TARGETS.find((item) => storeModulesManifest(item) === path);
      if (!store) { problems.push(`${path} is not a store-target manifest (${expectedManifests.join(", ")}), so it cannot name packaged binaries (stale build output?)`); continue; }
      let manifest: { BuildId?: unknown; Modules?: Record<string, unknown> };
      try { manifest = JSON.parse(text.replace(/^﻿/, "")); } catch { problems.push(`${path} is not JSON`); continue; }
      if (manifest.BuildId !== DISTRIBUTABLE_BUILD_ID) problems.push(`${path} BuildId is ${JSON.stringify(manifest.BuildId)}, not "${DISTRIBUTABLE_BUILD_ID}"`);
      const entries = Object.entries(manifest.Modules ?? {}), keys = entries.map(([name]) => name);
      if (JSON.stringify([...keys].sort()) !== expectedKeys) problems.push(`${path} names modules ${keys.join(", ") || "(none)"}, not the ${artifact.plugin}.uplugin modules ${modules.join(", ") || "(none)"}`);
      accepted.add(path);
      for (const [name, dll] of entries) {
        if (!modules.includes(name)) continue;
        const expected = storeModuleBinary(store, name);
        if (dll !== expected) { problems.push(`${path} maps ${name} to ${JSON.stringify(dll)}, not ${expected}`); continue; }
        if (!files.has(`Binaries/Win64/${expected}`)) problems.push(`${path} names missing ${expected}`);
        accepted.add(`Binaries/Win64/${expected}`).add(`Binaries/Win64/${expected.replace(/\.dll$/, ".pdb")}`);
      }
    }
    const unreferenced = binaries.filter((file) => !accepted.has(file));
    if (unreferenced.length > 0) problems.push(`binaries no packaged .modules manifest names (stale build outputs?): ${unreferenced.join(", ")}`);
    // #134: every store target's module DLL was linked during this package run, not left over from an earlier build.
    if (artifact.freshness) for (const store of STORE_TARGETS) for (const name of modules) {
      const path = `Binaries/Win64/${storeModuleBinary(store, name)}`, modified = artifact.freshness.binaryModifiedMs[path];
      if (!files.has(path)) continue;
      if (modified === undefined) problems.push(`${path} has no recorded modification time, so its freshness cannot be checked`);
      else if (modified < artifact.freshness.packageStartedMs - BINARY_FRESHNESS_SLACK_MS) {
        problems.push(`${path} was linked ${new Date(modified).toISOString()}, before this package run started (${new Date(artifact.freshness.packageStartedMs).toISOString()}): a stale build output`);
      }
    }
  }
  return problems;
}

/** Native classes the cooked MKI Roads registration chain is built from: root module -> schematic -> recipes -> descriptors -> buildables (Blueprints of the native spline road), filed under a build category and subcategory. */
export const REGISTRATION_CHAIN_CLASSES = [
  "/Script/SML.GameWorldModule", "/Script/FactoryGame.FGSchematic", "/Script/FactoryGame.FGRecipe", "/Script/FactoryGame.FGBuildingDescriptor",
  "/Script/MKIRoads.MKIRoadSplineRoad", "/Script/FactoryGame.FGBuildCategory", "/Script/FactoryGame.FGBuildSubCategory",
] as const;

/**
 * Checks the cooked `AssetRegistry.bin` that the MKI Roads pak ships. SML finds a mod's root module only through this
 * registry (Blueprint assets of a module type tagged `bRootModule=True`), so the cooked registry must name every chain
 * asset, the chain's native classes, and the `bRootModule` tag. This is a string-level check of the registry's name
 * and path tables; the editor `verify` step proves the chain's links and values.
 */
export function cookedRegistryProblems(registry: Uint8Array, expectedPackages: readonly string[]): string[] {
  const text = Buffer.from(registry).toString("latin1"), problems: string[] = [];
  if (text.length === 0) problems.push("the cooked AssetRegistry.bin is empty");
  for (const item of [...expectedPackages, ...REGISTRATION_CHAIN_CLASSES.map((path) => path.slice(path.lastIndexOf(".") + 1)), "bRootModule"]) {
    if (!text.includes(item)) problems.push(`the cooked AssetRegistry.bin does not name ${item}`);
  }
  return problems;
}

export function assertRoadNetworkAvailable(availability: RoadNetworkAvailability | undefined): asserts availability is Extract<RoadNetworkAvailability, { available: true }> {
  if (!availability) throw new Error(`${ROAD_PACK_PLUGIN} road placement needs a road_network availability check from the bridge capabilities; build with vanilla foundations instead`);
  if (!availability.available) throw new Error(`${ROAD_PACK_PLUGIN} road placement is unavailable: ${availability.reason}; build with vanilla foundations instead`);
}
