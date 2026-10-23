import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, renameSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import {
  actionProblems, applyInstall, discoverPlugins, hashTree, inspectInstallation, latestArtifacts, planInstall, removalProblems, restoreInstall, verifyArtifact, RECEIPT_FILE,
} from "../src/road-pack-install.js";
import { installPathFromListing, PLUGIN_INSTALL_PATHS, type InstallablePlugin } from "../src/road-pack.js";
import { bridgePluginDirectory, roadPackPluginDirectory, toolsPluginDirectory } from "../src/workspace-paths.js";

// Everything here runs in temporary fake game trees; no test reads or writes a real game installation.
const sha256 = (data: string | Buffer) => createHash("sha256").update(data).digest("hex");
const bridgeDescriptor = readFileSync(resolve(bridgePluginDirectory, "MKIBridge.uplugin"), "utf8");
const roadPackDescriptor = readFileSync(resolve(roadPackPluginDirectory, "MKIRoads.uplugin"), "utf8");
const toolsDescriptor = readFileSync(resolve(toolsPluginDirectory, "MKITools.uplugin"), "utf8");

function write(path: string, content: string) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, content);
}
function scratch(t: { after: (fn: () => void) => void }): string {
  const directory = mkdtempSync(join(tmpdir(), "mkir-install-"));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
}

/** The archive contents `npm run road-pack -- package` extracts for each plugin, with the real descriptors. */
function pluginFiles(plugin: InstallablePlugin, tag: string, descriptor?: string): Record<string, string> {
  const containers = Object.fromEntries(["pak", "ucas", "utoc"].map((extension) => [`Content/Paks/Windows/${plugin}FactoryGame-Windows.${extension}`, `${plugin} ${extension} ${tag}`]));
  // Both plugins ship one native module (MKI Roads' registers its material variants), with a manifest and DLL per store target.
  const binaries = Object.fromEntries(["Steam", "EGS"].flatMap((store) => [
    [`Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`, JSON.stringify({ BuildId: "SML", Modules: { [plugin]: `FactoryGame${store}-${plugin}-Win64-Shipping.dll` } })],
    [`Binaries/Win64/FactoryGame${store}-${plugin}-Win64-Shipping.dll`, `dll ${store} ${tag}`],
  ]));
  if (plugin === "MKIRoads") return { "MKIRoads.uplugin": descriptor ?? roadPackDescriptor, ...containers, ...binaries };
  if (plugin === "MKITools") return { "MKITools.uplugin": descriptor ?? toolsDescriptor, ...containers, ...binaries };
  return { "MKIBridge.uplugin": descriptor ?? bridgeDescriptor, ...containers, ...binaries };
}

/** A packaged artifact directory as `npm run road-pack -- package` writes it. */
function makeArtifact(root: string, plugin: InstallablePlugin, options: { tag?: string; mount?: string; descriptor?: string; commit?: string; stamp?: string; ok?: boolean; report?: Record<string, unknown> } = {}): string {
  const version = JSON.parse((options.descriptor ?? (plugin === "MKIRoads" ? roadPackDescriptor : bridgeDescriptor))).SemVersion as string;
  const commit = options.commit ?? "5f2e9942a4c83fc5240ce6170a576de7ce15ca77", directory = join(root, `${plugin}-${version}-${commit.slice(0, 12)}-${options.stamp ?? "20261009-120000"}`);
  const files = pluginFiles(plugin, options.tag ?? "new", options.descriptor), zip = `zip ${plugin} ${options.tag ?? "new"}`;
  for (const [path, content] of Object.entries(files)) write(join(directory, "contents", path), content);
  write(join(directory, `${plugin}-Windows.zip`), zip);
  const mount = options.mount ?? `../../../${PLUGIN_INSTALL_PATHS[plugin]}/`;
  write(join(directory, "container-listing.txt"), [
    `LogPakFile: Display: Listing ${plugin}FactoryGame-Windows.pak with mount point "${mount}"`,
    `LogPakFile: Display: "AssetRegistry.bin" offset: 0, size: 1 bytes, sha1: 0, compression: None.`,
    `LogIoStore: Display: Mount point ${mount}Content/`,
    `LogIoStore: Display: "${mount}Content/X.uasset" offset: 0, size: 1 bytes, hash: 0, compression: None.`,
  ].join("\n") + "\n");
  const sums = [[`${plugin}-Windows.zip`, zip] as const, ...Object.entries(files).map(([path, content]) => [`contents/${path}`, content] as const)];
  write(join(directory, "SHA256SUMS.txt"), sums.map(([path, content]) => `${sha256(content)} *${path}`).join("\n") + "\n");
  write(join(directory, "source-commit.txt"), `${commit}\nclean\n`);
  const provenance = { commit, syncSha256: sha256("sync.json"), sdkInputsSha256: sha256("sdk inputs"), ...(plugin === "MKIRoads" ? { contentSha256: sha256("content") } : {}), verified: true };
  write(join(directory, "package-report.json"), JSON.stringify({
    plugin, version, zipSha256: sha256(zip), installPath: PLUGIN_INSTALL_PATHS[plugin], files: Object.keys(files).sort(), provenance, ok: options.ok ?? true, problems: [], ...options.report,
  }, null, 2));
  return directory;
}

/** A fake game root with SML, an unrelated third-party mod, and optionally installed first-party plugins. */
function makeGame(root: string, installed: Partial<Record<string, { plugin: InstallablePlugin; tag: string }>> = {}): string {
  const game = join(root, "game");
  write(join(game, "FactoryGame", "Mods", "SML", "SML.uplugin"), JSON.stringify({ SemVersion: "3.12.0" }));
  write(join(game, "FactoryGame", "Mods", "GameFeatures", "OtherMod", "OtherMod.uplugin"), JSON.stringify({ SemVersion: "1.5.3" }));
  write(join(game, "FactoryGame", "Mods", "GameFeatures", "OtherMod", "Content", "Paks", "Windows", "OtherModFactoryGame-Windows.pak"), "other");
  for (const [path, item] of Object.entries(installed)) for (const [file, content] of Object.entries(pluginFiles(item!.plugin, item!.tag))) write(join(game, path, file), content);
  return game;
}

/** The live layout this tooling fixes: MKI Roads under Mods/GameFeatures, next to the bridge. */
const MISPLACED = {
  "FactoryGame/Mods/GameFeatures/MKIRoads": { plugin: "MKIRoads" as const, tag: "old" },
  "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge" as const, tag: "old" },
};

test("install paths follow the packaged container mount points", () => {
  assert.equal(PLUGIN_INSTALL_PATHS.MKIRoads, "FactoryGame/Mods/MKIRoads");
  assert.equal(PLUGIN_INSTALL_PATHS.MKIBridge, "FactoryGame/Mods/GameFeatures/MKIBridge");
  const listing = (mount: string) => [`Listing MKIRoadsFactoryGame-Windows.pak with mount point "${mount}"`, `LogIoStore: Display: Mount point ${mount}Content/`];
  assert.equal(installPathFromListing("MKIRoads", listing("../../../FactoryGame/Mods/MKIRoads/")), "FactoryGame/Mods/MKIRoads");
  assert.equal(installPathFromListing("MKIRoads", listing("../../../FactoryGame/Mods/GameFeatures/MKIRoads/")), "FactoryGame/Mods/GameFeatures/MKIRoads");
  assert.equal(installPathFromListing("MKIBridge", listing("../../../FactoryGame/Mods/GameFeatures/MKIBridge/")), "FactoryGame/Mods/GameFeatures/MKIBridge");
  assert.equal(installPathFromListing("MKIRoads", listing("../../../FactoryGame/Mods/OtherPlugin/")), undefined);
  assert.equal(installPathFromListing("MKIRoads", [...listing("../../../FactoryGame/Mods/MKIRoads/"), "LogIoStore: Display: Mount point ../../../FactoryGame/Mods/GameFeatures/MKIRoads/Content/"]), undefined);
  assert.equal(installPathFromListing("MKIRoads", []), undefined);
});

test("check detects MKI Roads misplaced under Mods/GameFeatures", (t) => {
  const game = makeGame(scratch(t), MISPLACED);
  assert.deepEqual(discoverPlugins(game).MKIRoads, ["FactoryGame/Mods/GameFeatures/MKIRoads"]);
  const report = inspectInstallation(game);
  assert.equal(report.ok, false);
  assert.deepEqual(report.plugins.MKIRoads.misplaced, ["FactoryGame/Mods/GameFeatures/MKIRoads"]);
  assert.equal(report.plugins.MKIRoads.present, false);
  assert.ok(report.problems.some((problem) => /MKIRoads is installed at FactoryGame\/Mods\/GameFeatures\/MKIRoads, but its containers mount at \.\.\/\.\.\/\.\.\/FactoryGame\/Mods\/MKIRoads\/; move it to FactoryGame\/Mods\/MKIRoads/.test(problem)), report.problems.join("\n"));
  // The misplaced descriptor still satisfies the bridge's dependency, so the bridge loads without MKI Packs > Roads.
  assert.ok(!report.problems.some((problem) => /engine will not load the bridge/.test(problem)));
  assert.equal(report.plugins.MKIBridge.present, true);
  rmSync(join(game, "FactoryGame/Mods/GameFeatures/MKIRoads"), { recursive: true });
  assert.match(inspectInstallation(game).problems.join("\n"), /MKIBridge requires MKIRoads, so the engine will not load the bridge until MKIRoads is installed at FactoryGame\/Mods\/MKIRoads/);
  assert.deepEqual(report.plugins.OtherMod.locations, ["FactoryGame/Mods/GameFeatures/OtherMod"]);
});

test("check accepts the correct layout and flags duplicates, missing files, SML, and incompatible descriptors", (t) => {
  const root = scratch(t);
  const good = makeGame(join(root, "good"), { "FactoryGame/Mods/MKIRoads": { plugin: "MKIRoads", tag: "new" }, "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "new" } });
  const ok = inspectInstallation(good);
  assert.deepEqual(ok.problems, []);
  assert.equal(ok.plugins.MKIRoads.version, JSON.parse(roadPackDescriptor).SemVersion);

  const duplicate = makeGame(join(root, "duplicate"), { ...MISPLACED, "FactoryGame/Mods/MKIRoads": { plugin: "MKIRoads", tag: "new" } });
  assert.match(inspectInstallation(duplicate).problems.join("\n"), /MKIRoads is installed at FactoryGame\/Mods\/GameFeatures\/MKIRoads.*remove that copy/);

  rmSync(join(good, "FactoryGame/Mods/MKIRoads/Content/Paks/Windows/MKIRoadsFactoryGame-Windows.utoc"));
  rmSync(join(good, "FactoryGame/Mods/GameFeatures/MKIBridge/Binaries/Win64/FactoryGameSteam-MKIBridge-Win64-Shipping.dll"));
  const broken = inspectInstallation(good).problems.join("\n");
  assert.match(broken, /FactoryGame\/Mods\/MKIRoads is missing Content\/Paks\/Windows\/MKIRoadsFactoryGame-Windows.utoc/);
  assert.match(broken, /MKIBridge is missing Binaries\/Win64\/FactoryGameSteam-MKIBridge-Win64-Shipping.dll/);

  rmSync(join(good, "FactoryGame/Mods/SML"), { recursive: true });
  assert.match(inspectInstallation(good).problems.join("\n"), /SML is not installed/);

  const incompatible = makeGame(join(root, "incompatible"), { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "new" } });
  write(join(incompatible, "FactoryGame/Mods/MKIRoads/MKIRoads.uplugin"), JSON.stringify({ ...JSON.parse(roadPackDescriptor), SemVersion: "0.9.0" }));
  assert.match(inspectInstallation(incompatible).problems.join("\n"), /installed descriptors: MKIRoads 0.9.0 does not satisfy the MKIBridge requirement/);
  write(join(incompatible, "FactoryGame/Mods/SML/SML.uplugin"), JSON.stringify({ SemVersion: "3.11.0" }));
  assert.match(inspectInstallation(incompatible).problems.join("\n"), /requires SML \^3\.12\.0; installed SML is 3\.11\.0/);
  assert.match(inspectInstallation(join(root, "nowhere")).problems.join("\n"), /has no FactoryGame directory/);
});

test("artifacts are accepted only when hashes, report, cleanliness, and mount point agree", (t) => {
  const root = scratch(t);
  const artifact = verifyArtifact(makeArtifact(join(root, "ok"), "MKIRoads"));
  assert.equal(artifact.installPath, "FactoryGame/Mods/MKIRoads");
  assert.equal(artifact.files.length, 8, "descriptor, three containers, and the native module with its manifest for each store");

  const wrongMount = makeArtifact(join(root, "mount"), "MKIRoads", { mount: "../../../FactoryGame/Mods/GameFeatures/MKIRoads/" });
  assert.throws(() => verifyArtifact(wrongMount), /containers mount at FactoryGame\/Mods\/GameFeatures\/MKIRoads, not FactoryGame\/Mods\/MKIRoads/);
  const tampered = makeArtifact(join(root, "tampered"), "MKIRoads");
  writeFileSync(join(tampered, "contents/Content/Paks/Windows/MKIRoadsFactoryGame-Windows.pak"), "tampered");
  assert.throws(() => verifyArtifact(tampered), /MKIRoadsFactoryGame-Windows.pak does not match SHA256SUMS.txt/);
  const notOk = makeArtifact(join(root, "not-ok"), "MKIRoads", { ok: false });
  assert.throws(() => verifyArtifact(notOk), /package-report.json is not ok/);
  const dirty = makeArtifact(join(root, "dirty"), "MKIRoads");
  writeFileSync(join(dirty, "source-commit.txt"), "abc\ndirty: packages/ has uncommitted changes\n");
  assert.throws(() => verifyArtifact(dirty), /packaged from a dirty tree/);
  assert.throws(() => verifyArtifact(join(root, "missing")), /is not a packaged artifact/);

  const packages = join(root, "packages");
  makeArtifact(packages, "MKIRoads", { stamp: "20261009-100000" });
  const newest = makeArtifact(packages, "MKIRoads", { stamp: "20261009-110000" });
  makeArtifact(packages, "MKIRoads", { stamp: "20261009-120000", ok: false });
  makeArtifact(packages, "MKIRoads", { stamp: "20261009-130000", commit: "1111111111111111111111111111111111111111" });
  assert.deepEqual(latestArtifacts(packages, "5f2e9942a4c83fc5240ce6170a576de7ce15ca77"), { MKIRoads: newest });
});

test("install relocates a misplaced MKI Roads, backs everything up outside Mods, and restore undoes it", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), before = hashTree(join(game, "FactoryGame"));
  const artifacts = [verifyArtifact(makeArtifact(join(root, "packages"), "MKIRoads")), verifyArtifact(makeArtifact(join(root, "packages"), "MKIBridge"))];
  const backup = join(root, "backup");
  const plan = planInstall(game, artifacts, backup);
  assert.deepEqual(plan.problems, []);
  assert.deepEqual(plan.actions.map((action) => [action.kind, action.path]), [
    ["install", "FactoryGame/Mods/MKIRoads"],
    ["remove-misplaced", "FactoryGame/Mods/GameFeatures/MKIRoads"],
    ["replace", "FactoryGame/Mods/GameFeatures/MKIBridge"],
  ]);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before, "planning changes nothing");

  const receipt = applyInstall(plan);
  assert.equal(receipt.state, "installed");
  assert.deepEqual(receipt.after?.problems, []);
  assert.ok(!existsSync(join(game, "FactoryGame/Mods/GameFeatures/MKIRoads")), "the misplaced copy is gone");
  assert.deepEqual(discoverPlugins(game).MKIRoads, ["FactoryGame/Mods/MKIRoads"]);
  assert.equal(readFileSync(join(game, "FactoryGame/Mods/MKIRoads/Content/Paks/Windows/MKIRoadsFactoryGame-Windows.pak"), "utf8"), "MKIRoads pak new");
  assert.equal(readFileSync(join(game, "FactoryGame/Mods/GameFeatures/OtherMod/Content/Paks/Windows/OtherModFactoryGame-Windows.pak"), "utf8"), "other", "unrelated mods are untouched");
  const check = inspectInstallation(game, artifacts);
  assert.equal(check.ok, true, check.problems.join("\n"));
  assert.equal(check.plugins.MKIRoads.matchesArtifact, true);
  assert.equal(readFileSync(join(backup, "FactoryGame/Mods/GameFeatures/MKIRoads/Content/Paks/Windows/MKIRoadsFactoryGame-Windows.pak"), "utf8"), "MKIRoads pak old");
  assert.deepEqual(receipt.backups.map((item) => item.path), ["FactoryGame/Mods/GameFeatures/MKIRoads", "FactoryGame/Mods/GameFeatures/MKIBridge"]);
  assert.ok(existsSync(join(backup, RECEIPT_FILE)));

  const restored = restoreInstall(backup, game);
  assert.equal(restored.state, "restored");
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before, "restore reproduces the original tree");
  assert.throws(() => restoreInstall(backup, game), /is restored; only a receipt that is installed, backed-up, rollback-failed, restore-failed can be restored/);
});

test("install refuses unsafe plans and rolls back a failed copy", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), before = hashTree(join(game, "FactoryGame"));
  const roadPack = verifyArtifact(makeArtifact(join(root, "packages"), "MKIRoads")), bridge = verifyArtifact(makeArtifact(join(root, "packages"), "MKIBridge"));
  assert.match(planInstall(game, [roadPack, bridge], join(game, "FactoryGame/Mods/backup")).problems.join("\n"), /inside FactoryGame\/Mods/);
  write(join(root, "used", "x.txt"), "x");
  assert.match(planInstall(game, [roadPack, bridge], join(root, "used")).problems.join("\n"), /is not empty/);
  assert.match(planInstall(game, [bridge], join(root, "b1")).problems.join("\n"), /MKIRoads 0.1.0 does not satisfy|requires MKIRoads/);
  const incompatible = verifyArtifact(makeArtifact(join(root, "incompatible"), "MKIRoads", { descriptor: JSON.stringify({ ...JSON.parse(roadPackDescriptor), SemVersion: "0.9.0", VersionName: "0.9.0" }) }));
  assert.match(planInstall(game, [incompatible, bridge], join(root, "b2")).problems.join("\n"), /MKIRoads 0.9.0 does not satisfy the MKIBridge requirement/);
  assert.match(planInstall(game, [roadPack, roadPack, bridge], join(root, "b3")).problems.join("\n"), /MKIRoads is given twice/);
  rmSync(join(game, "FactoryGame/Mods/SML"), { recursive: true });
  const noSml = planInstall(game, [roadPack, bridge], join(root, "b4"));
  assert.match(noSml.problems.join("\n"), /SML is not installed/);
  assert.throws(() => applyInstall(noSml), /Install refused/);
  write(join(game, "FactoryGame/Mods/SML/SML.uplugin"), JSON.stringify({ SemVersion: "3.12.0" }));
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before, "refused plans change nothing");

  // A failure after the old directories are removed restores them from the backup.
  const backup = join(root, "b5"), plan = planInstall(game, [roadPack, bridge], backup);
  assert.throws(() => applyInstall(plan, { afterRemove: () => { throw new Error("disk full"); } }), /Install failed and was rolled back .*disk full/);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before, "rollback reproduces the original tree");
  assert.equal(JSON.parse(readFileSync(join(backup, RECEIPT_FILE), "utf8")).state, "rolled-back");
});

// Lifecycle safety (#80 review B1/M5): broad or shared directories are never scheduled, every deletion is backed up,
// restore validates its sources first, and failed rollbacks leave a truthful, retryable receipt.

/** Byte-level hashes of the mods the installer must never touch. */
const unrelated = (game: string) => ({ sml: hashTree(join(game, "FactoryGame/Mods/SML")), other: hashTree(join(game, "FactoryGame/Mods/GameFeatures/OtherMod")) });
const artifactsIn = (root: string) => [verifyArtifact(makeArtifact(join(root, "packages"), "MKIRoads")), verifyArtifact(makeArtifact(join(root, "packages"), "MKIBridge"))];
const receiptAt = (backup: string) => JSON.parse(readFileSync(join(backup, RECEIPT_FILE), "utf8"));

/** Plans an install that must be refused without scheduling any of `forbidden` and without changing the game tree. */
function assertRefused(root: string, game: string, forbidden: string[], pattern: RegExp) {
  const before = hashTree(join(game, "FactoryGame")), plan = planInstall(game, artifactsIn(root), join(root, `backup-${forbidden.length}-${Math.random().toString(36).slice(2)}`));
  assert.match(plan.problems.join("\n"), pattern);
  for (const path of forbidden) assert.ok(!plan.actions.some((action) => action.path === path), `${path} must never be scheduled: ${JSON.stringify(plan.actions)}`);
  assert.throws(() => applyInstall(plan), /Install refused/);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before, "a refused plan changes nothing");
  return plan;
}

test("a descriptor extracted straight into FactoryGame/Mods never schedules Mods for removal", (t) => {
  const root = scratch(t), game = makeGame(root, { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "old" } });
  for (const [file, content] of Object.entries(pluginFiles("MKIRoads", "loose"))) write(join(game, "FactoryGame/Mods", file), content);
  assert.deepEqual(discoverPlugins(game).MKIRoads, ["FactoryGame/Mods"]);
  assert.deepEqual(removalProblems(game, "FactoryGame/Mods", "MKIRoads"), ["FactoryGame/Mods is not a plugin directory inside FactoryGame/Mods; the installer never replaces or removes it"]);
  assert.match(inspectInstallation(game).problems.join("\n"), /MKIRoads is installed at FactoryGame\/Mods, .*by hand: FactoryGame\/Mods is not a plugin directory/);
  const before = unrelated(game);
  const plan = assertRefused(root, game, ["FactoryGame/Mods", "FactoryGame", "FactoryGame/Mods/GameFeatures"], /MKIRoads is misplaced at FactoryGame\/Mods, which the installer will not remove/);
  assert.match(plan.problems.join("\n"), /FactoryGame\/Mods\/MKIRoads.uplugin stops the engine's plugin discovery above FactoryGame\/Mods\/MKIRoads/);
  assert.deepEqual(unrelated(game), before);
});

test("a descriptor extracted straight into Mods/GameFeatures never schedules GameFeatures for removal", (t) => {
  const root = scratch(t), game = makeGame(root, { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "old" } });
  for (const [file, content] of Object.entries(pluginFiles("MKIRoads", "loose"))) write(join(game, "FactoryGame/Mods/GameFeatures", file), content);
  assert.deepEqual(discoverPlugins(game).MKIRoads, ["FactoryGame/Mods/GameFeatures"]);
  assert.match(removalProblems(game, "FactoryGame/Mods/GameFeatures", "MKIRoads").join("\n"), /not a directory dedicated to MKIRoads \(it is named GameFeatures\)/);
  const before = unrelated(game);
  const plan = assertRefused(root, game, ["FactoryGame/Mods/GameFeatures", "FactoryGame/Mods"], /MKIRoads is misplaced at FactoryGame\/Mods\/GameFeatures, which the installer will not remove/);
  assert.match(plan.problems.join("\n"), /FactoryGame\/Mods\/GameFeatures\/MKIRoads.uplugin stops the engine's plugin discovery above FactoryGame\/Mods\/GameFeatures\/MKIBridge/);
  assert.deepEqual(unrelated(game), before);
  // The bridge descriptor in GameFeatures itself is refused the same way.
  const bridgeGame = makeGame(join(root, "bridge"));
  write(join(bridgeGame, "FactoryGame/Mods/GameFeatures/MKIBridge.uplugin"), bridgeDescriptor);
  assertRefused(join(root, "bridge"), bridgeGame, ["FactoryGame/Mods/GameFeatures"], /MKIBridge is misplaced at FactoryGame\/Mods\/GameFeatures, which the installer will not remove/);
});

test("directories that hold SML or another plugin's descriptor fail closed", (t) => {
  const root = scratch(t);
  // A misplaced copy that shares its directory with another plugin.
  const shared = makeGame(join(root, "shared"), MISPLACED);
  write(join(shared, "FactoryGame/Mods/GameFeatures/MKIRoads/ThirdParty.uplugin"), JSON.stringify({ SemVersion: "1.0.0" }));
  assert.deepEqual(discoverPlugins(shared).ThirdParty, ["FactoryGame/Mods/GameFeatures/MKIRoads"]);
  const sharedBefore = unrelated(shared);
  assertRefused(join(root, "shared"), shared, ["FactoryGame/Mods/GameFeatures/MKIRoads"], /FactoryGame\/Mods\/GameFeatures\/MKIRoads also holds FactoryGame\/Mods\/GameFeatures\/MKIRoads\/ThirdParty.uplugin; it is not attributable to MKIRoads alone/);
  assert.deepEqual(unrelated(shared), sharedBefore);

  // A bundle directory named after neither plugin that holds MKI Roads and SML.
  const bundle = makeGame(join(root, "bundle"), { "FactoryGame/Mods/Bundle": { plugin: "MKIRoads", tag: "old" } });
  write(join(bundle, "FactoryGame/Mods/Bundle/SML.uplugin"), JSON.stringify({ SemVersion: "3.12.0" }));
  assertRefused(join(root, "bundle"), bundle, ["FactoryGame/Mods/Bundle"], /FactoryGame\/Mods\/Bundle is not a directory dedicated to MKIRoads \(it is named Bundle\)/);

  // The install target itself holds a nested foreign descriptor, which engine discovery never reaches.
  const nested = makeGame(join(root, "nested"), { "FactoryGame/Mods/MKIRoads": { plugin: "MKIRoads", tag: "old" }, "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "old" } });
  write(join(nested, "FactoryGame/Mods/MKIRoads/Extras/SML/SML.uplugin"), JSON.stringify({ SemVersion: "3.12.0" }));
  assert.ok(!(discoverPlugins(nested).SML ?? []).includes("FactoryGame/Mods/MKIRoads/Extras/SML"), "engine-style discovery never sees the nested descriptor");
  assertRefused(join(root, "nested"), nested, ["FactoryGame/Mods/MKIRoads"], /FactoryGame\/Mods\/MKIRoads also holds FactoryGame\/Mods\/MKIRoads\/Extras\/SML\/SML.uplugin/);

  // Leftovers at the target that include another plugin's descriptor but none of MKI Roads' own.
  const leftovers = makeGame(join(root, "leftovers"), { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "old" } });
  write(join(leftovers, "FactoryGame/Mods/MKIRoads/Other/Other.uplugin"), JSON.stringify({ SemVersion: "1.0.0" }));
  assertRefused(join(root, "leftovers"), leftovers, ["FactoryGame/Mods/MKIRoads"], /FactoryGame\/Mods\/MKIRoads also holds FactoryGame\/Mods\/MKIRoads\/Other\/Other.uplugin/);

  // The bridge target nests a misplaced MKI Roads.
  const bridgeNest = makeGame(join(root, "bridge-nest"), { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "old" }, "FactoryGame/Mods/GameFeatures/MKIBridge/MKIRoads": { plugin: "MKIRoads", tag: "old" } });
  assertRefused(join(root, "bridge-nest"), bridgeNest, ["FactoryGame/Mods/GameFeatures/MKIBridge"], /FactoryGame\/Mods\/GameFeatures\/MKIBridge also holds FactoryGame\/Mods\/GameFeatures\/MKIBridge\/MKIRoads\/MKIRoads.uplugin/);
});

test("leftover target contents without a descriptor are backed up, replaced, rolled back, and restored byte-for-byte", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED);
  write(join(game, "FactoryGame/Mods/MKIRoads/Content/Paks/Windows/Stale-Windows.pak"), "stale pak");
  write(join(game, "FactoryGame/Mods/MKIRoads/notes.txt"), "user notes");
  const before = hashTree(join(game, "FactoryGame")), others = unrelated(game), artifacts = artifactsIn(root);
  assert.equal(inspectInstallation(game).plugins.MKIRoads.present, false, "no top-level descriptor");

  // Rollback after the old directories are removed puts the leftovers back too.
  const failed = join(root, "failed"), failedPlan = planInstall(game, artifacts, failed);
  assert.deepEqual(failedPlan.problems, []);
  assert.deepEqual(failedPlan.actions.map((action) => [action.kind, action.path, action.dropped]), [
    ["replace", "FactoryGame/Mods/MKIRoads", ["Content/Paks/Windows/Stale-Windows.pak", "notes.txt"]],
    ["remove-misplaced", "FactoryGame/Mods/GameFeatures/MKIRoads", []],
    ["replace", "FactoryGame/Mods/GameFeatures/MKIBridge", []],
  ]);
  assert.throws(() => applyInstall(failedPlan, { afterRemove: () => { throw new Error("copy failed"); } }), /rolled back .*copy failed/);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before);
  assert.equal(receiptAt(failed).state, "rolled-back");

  const backup = join(root, "backup"), receipt = applyInstall(planInstall(game, artifacts, backup));
  assert.equal(receipt.state, "installed");
  assert.ok(!existsSync(join(game, "FactoryGame/Mods/MKIRoads/notes.txt")), "the target is replaced, not merged");
  assert.equal(inspectInstallation(game, artifacts).plugins.MKIRoads.matchesArtifact, true);
  assert.equal(readFileSync(join(backup, "FactoryGame/Mods/MKIRoads/notes.txt"), "utf8"), "user notes");
  assert.deepEqual(receipt.backups.map((item) => item.path), ["FactoryGame/Mods/MKIRoads", "FactoryGame/Mods/GameFeatures/MKIRoads", "FactoryGame/Mods/GameFeatures/MKIBridge"]);
  assert.deepEqual(unrelated(game), others);
  restoreInstall(backup, game);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), before);
});

test("restore validates every backup source before deleting anything", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), artifacts = artifactsIn(root), backup = join(root, "backup");
  const original = hashTree(join(game, "FactoryGame"));
  applyInstall(planInstall(game, artifacts, backup));
  const installed = hashTree(join(game, "FactoryGame")), receiptText = readFileSync(join(backup, RECEIPT_FILE), "utf8");
  const misplacedPak = join(backup, "FactoryGame/Mods/GameFeatures/MKIRoads/Content/Paks/Windows/MKIRoadsFactoryGame-Windows.pak");

  // A backup file is missing.
  rmSync(misplacedPak);
  assert.throws(() => restoreInstall(backup, game), /Restore refused before changing anything:\n- backup .*GameFeatures[\\/]MKIRoads no longer matches the receipt's file hashes/);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), installed, "nothing is deleted when a backup is incomplete");
  assert.equal(receiptAt(backup).state, "installed", "a refusal leaves the receipt as it was");
  // A backup file changed.
  write(misplacedPak, "corrupted");
  assert.throws(() => restoreInstall(backup, game), /no longer matches the receipt's file hashes/);
  // A whole backup directory is missing.
  write(misplacedPak, "MKIRoads pak old");
  rmSync(join(backup, "FactoryGame/Mods/GameFeatures/MKIBridge"), { recursive: true });
  assert.throws(() => restoreInstall(backup, game), /backup .*MKIBridge is missing/);
  assert.deepEqual(hashTree(join(game, "FactoryGame")), installed);
  // The receipt omits the backup of a replaced directory.
  const receipt = JSON.parse(receiptText);
  receipt.backups = receipt.backups.filter((item: { path: string }) => item.path !== "FactoryGame/Mods/GameFeatures/MKIBridge");
  writeFileSync(join(backup, RECEIPT_FILE), JSON.stringify(receipt));
  assert.throws(() => restoreInstall(backup, game), /the receipt has no backup of FactoryGame\/Mods\/GameFeatures\/MKIBridge, which the install replaced/);
  // The receipt was edited to delete a broad directory.
  writeFileSync(join(backup, RECEIPT_FILE), JSON.stringify({ ...JSON.parse(receiptText), actions: [...JSON.parse(receiptText).actions, { plugin: "MKIRoads", path: "FactoryGame/Mods", kind: "install" }] }));
  assert.throws(() => restoreInstall(backup, game), /install "FactoryGame\/Mods" for "MKIRoads": FactoryGame\/Mods is protected/);
  // Another plugin was installed into a directory the restore would delete.
  writeFileSync(join(backup, RECEIPT_FILE), receiptText);
  write(join(game, "FactoryGame/Mods/MKIRoads/Later/Later.uplugin"), "{}");
  assert.throws(() => restoreInstall(backup, game), /FactoryGame\/Mods\/MKIRoads also holds FactoryGame\/Mods\/MKIRoads\/Later\/Later.uplugin/);
  rmSync(join(game, "FactoryGame/Mods/MKIRoads/Later"), { recursive: true });
  assert.deepEqual(hashTree(join(game, "FactoryGame")), installed, "every refusal left the installed tree intact");

  // With the backups repaired, the restore succeeds and reproduces the original tree.
  for (const [file, content] of Object.entries(pluginFiles("MKIBridge", "old"))) write(join(backup, "FactoryGame/Mods/GameFeatures/MKIBridge", file), content);
  assert.equal(restoreInstall(backup, game).state, "restored");
  assert.deepEqual(hashTree(join(game, "FactoryGame")), original);
});

test("a failed rollback and a failed restore leave a truthful receipt that a retry completes", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), artifacts = artifactsIn(root), backup = join(root, "backup");
  const original = hashTree(join(game, "FactoryGame")), others = unrelated(game);
  const savedPak = join(backup, "FactoryGame/Mods/GameFeatures/MKIBridge/Content/Paks/Windows/MKIBridgeFactoryGame-Windows.pak");
  assert.throws(() => applyInstall(planInstall(game, artifacts, backup), {
    afterRemove: () => { throw new Error("copy failed"); },
    beforeRollback: () => writeFileSync(savedPak, "damaged"),
  }), /Install failed \(copy failed\) and its rollback also failed \(Restore refused before changing anything:[\s\S]*\); the game tree may be partly changed\. Fix the cause and retry: npm run road-pack:install -- restore/);
  const failed = receiptAt(backup);
  assert.equal(failed.state, "rollback-failed");
  assert.equal(failed.error, "copy failed");
  assert.match(failed.restoreError, /MKIBridge no longer matches the receipt's file hashes/);
  assert.ok(!existsSync(join(game, "FactoryGame/Mods/GameFeatures/MKIBridge")), "the receipt is truthful: the tree is partly changed");
  assert.deepEqual(unrelated(game), others);

  // Retrying with the backup still damaged is refused and keeps the retryable state.
  assert.throws(() => restoreInstall(backup, game), /Restore refused before changing anything/);
  assert.equal(receiptAt(backup).state, "rollback-failed");
  writeFileSync(savedPak, "MKIBridge pak old");

  // A restore that fails after deleting is recorded as restore-failed and can be retried.
  assert.throws(() => restoreInstall(backup, game, { afterDelete: () => { throw new Error("disk full"); } }), /failed \(disk full\); the game tree may be partly changed and the backups are intact, so retry the restore/);
  assert.equal(receiptAt(backup).state, "restore-failed");
  assert.equal(receiptAt(backup).restoreError, "disk full");
  const restored = restoreInstall(backup, game);
  assert.equal(restored.state, "restored");
  assert.equal(restored.restoreError, undefined);
  assert.equal(restored.error, "copy failed", "the original failure stays on record");
  assert.deepEqual(hashTree(join(game, "FactoryGame")), original);
  assert.deepEqual(unrelated(game), others);
  assert.throws(() => restoreInstall(backup, game), /is restored; only a receipt/);
});

test("apply re-checks the tree and refuses when it changed after planning", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), plan = planInstall(game, artifactsIn(root), join(root, "backup"));
  assert.deepEqual(plan.problems, []);
  write(join(game, "FactoryGame/Mods/GameFeatures/MKIRoads/Late.uplugin"), "{}");
  assert.throws(() => applyInstall(plan), /the game tree changed since planning:\n- FactoryGame\/Mods\/GameFeatures\/MKIRoads also holds .*Late.uplugin/);
  rmSync(join(game, "FactoryGame/Mods/GameFeatures/MKIRoads/Late.uplugin"));
  write(join(game, "FactoryGame/Mods/MKIRoads/new.txt"), "appeared");
  assert.throws(() => applyInstall(plan), /FactoryGame\/Mods\/MKIRoads appeared after planning/);
  assert.ok(!existsSync(join(root, "backup", RECEIPT_FILE)), "no backup or receipt is written");
});

// Final hardening (#80 focused re-review): a forged or corrupt receipt can only ever name first-party plugin directories
// the installer could have planned, and an explicit artifact must carry verified package provenance.

test("restore refuses forged or corrupt receipts that name SML, third-party mods, Mods, GameFeatures, or wrong first-party actions", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), backup = join(root, "backup");
  applyInstall(planInstall(game, artifactsIn(root), backup));
  const receiptText = readFileSync(join(backup, RECEIPT_FILE), "utf8"), genuine = JSON.parse(receiptText);
  // A forgery made as convincing as possible: the named directory is copied into the backup with matching hashes.
  const backedUp = (path: string) => {
    cpSync(join(game, path), join(backup, path), { recursive: true });
    return { path, files: hashTree(join(game, path)) };
  };
  const withActions = (actions: unknown[], backups: unknown[] = []) => ({ ...genuine, actions: [...genuine.actions, ...actions], backups: [...genuine.backups, ...backups] });
  const retarget = (index: number, change: Record<string, unknown>) => ({ ...genuine, actions: genuine.actions.map((action: object, at: number) => at === index ? { ...action, ...change } : action) });
  const editBackup = (index: number, edit: (files: { path: string; sha256: string }[]) => object[]) =>
    ({ ...genuine, backups: genuine.backups.map((item: { path: string; files: { path: string; sha256: string }[] }, at: number) => at === index ? { ...item, files: edit(item.files) } : item) });
  const forgeries: [string, unknown, RegExp][] = [
    ["SML as its own plugin", withActions([{ plugin: "SML", path: "FactoryGame/Mods/SML", kind: "replace" }], [backedUp("FactoryGame/Mods/SML")]), /"SML" is not a first-party plugin/],
    ["SML under a first-party name", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/SML", kind: "remove-misplaced" }]), /FactoryGame\/Mods\/SML is protected/],
    ["SML with different case", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/sml", kind: "remove-misplaced" }]), /FactoryGame\/Mods\/sml is protected/],
    ["inside SML", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/SML/MKIRoads", kind: "remove-misplaced" }]), /inside FactoryGame\/Mods\/SML/],
    ["a third-party plugin", withActions([{ plugin: "OtherMod", path: "FactoryGame/Mods/GameFeatures/OtherMod", kind: "remove-misplaced" }], [backedUp("FactoryGame/Mods/GameFeatures/OtherMod")]), /"OtherMod" is not a first-party plugin/],
    ["a third-party directory under a first-party name", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/GameFeatures/OtherMod", kind: "remove-misplaced" }]), /the directory is not named MKIRoads/],
    ["Mods/GameFeatures", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/GameFeatures", kind: "remove-misplaced" }]), /FactoryGame\/Mods\/GameFeatures is protected/],
    ["GameFeatures as a plugin", withActions([{ plugin: "GameFeatures", path: "FactoryGame/Mods/GameFeatures", kind: "remove-misplaced" }]), /"GameFeatures" is not a first-party plugin/],
    ["Mods", withActions([{ plugin: "MKIBridge", path: "FactoryGame/Mods", kind: "replace" }]), /FactoryGame\/Mods is protected/],
    ["the game directory", withActions([{ plugin: "MKIRoads", path: "FactoryGame", kind: "install" }]), /install "FactoryGame" for "MKIRoads": FactoryGame is protected/],
    ["a directory beside Mods", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Content/MKIRoads", kind: "remove-misplaced" }]), /not a normalized directory under FactoryGame\/Mods/],
    ["a path that escapes", withActions([{ plugin: "MKIRoads", path: "FactoryGame/Mods/MKIRoads/../SML", kind: "remove-misplaced" }]), /not a normalized directory/],
    ["an absolute path", withActions([{ plugin: "MKIRoads", path: join(game, "FactoryGame/Mods/SML").replaceAll("\\", "/"), kind: "remove-misplaced" }]), /not a normalized directory/],
    ["install at the wrong path", retarget(0, { path: "FactoryGame/Mods/GameFeatures/MKIBridge" }), /install "FactoryGame\/Mods\/GameFeatures\/MKIBridge" for "MKIRoads": install must target FactoryGame\/Mods\/MKIRoads/],
    ["replace for the other plugin", retarget(2, { plugin: "MKIRoads" }), /replace "FactoryGame\/Mods\/GameFeatures\/MKIBridge" for "MKIRoads": replace must target FactoryGame\/Mods\/MKIRoads/],
    ["a misplaced copy turned into replace", retarget(1, { kind: "replace" }), /replace must target FactoryGame\/Mods\/MKIRoads/],
    ["remove-misplaced at the install path", retarget(0, { kind: "remove-misplaced" }), /a misplaced copy cannot be at the install path FactoryGame\/Mods\/MKIRoads/],
    ["a misplaced copy inside the bridge", retarget(1, { path: "FactoryGame/Mods/GameFeatures/MKIBridge/MKIRoads" }), /inside FactoryGame\/Mods\/GameFeatures\/MKIBridge/],
    ["an unknown kind", retarget(1, { kind: "delete" }), /is malformed: kind/],
    ["a duplicated action", withActions([genuine.actions[1]]), /names FactoryGame\/Mods\/GameFeatures\/MKIRoads more than once/],
    ["a backup holding a foreign descriptor", editBackup(0, (files) => [...files, { path: "Other/Other.uplugin", sha256: sha256("x") }]), /holds Other\/Other.uplugin; it is not attributable to MKIRoads alone/],
    ["a misplaced backup without its descriptor", editBackup(0, (files) => files.filter((file) => file.path !== "MKIRoads.uplugin")), /has no MKIRoads.uplugin/],
    ["a backup file outside its directory", editBackup(0, (files) => [...files, { path: "../../SML/SML.uplugin", sha256: sha256("x") }]), /records a file path outside its directory/],
    ["a corrupt receipt", { ...genuine, actions: "everything" }, /the receipt is malformed: actions/],
    ["a wrong receipt version", { ...genuine, version: 2 }, /the receipt is malformed: version/],
  ];
  // The whole scratch tree (game and backups), except the receipt file each case writes.
  const tree = () => hashTree(root).filter((file) => file.path !== `backup/${RECEIPT_FILE}`);
  const before = tree();
  for (const [name, receipt, pattern] of forgeries) {
    const text = JSON.stringify(receipt, null, 2);
    writeFileSync(join(backup, RECEIPT_FILE), text);
    assert.throws(() => restoreInstall(backup, game), (error: Error) => {
      assert.match(error.message, /^Restore refused before changing anything/, name);
      assert.match(error.message, pattern, name);
      return true;
    });
    assert.deepEqual(tree(), before, `${name}: the whole tree stays byte-identical`);
    assert.equal(readFileSync(join(backup, RECEIPT_FILE), "utf8"), text, `${name}: a refused receipt is not rewritten`);
  }
  // Valid in shape, but the misplaced directory sits inside a third-party plugin: the on-disk check refuses it.
  const nestedPath = "FactoryGame/Mods/GameFeatures/OtherMod/MKIRoads";
  write(join(backup, nestedPath, "MKIRoads.uplugin"), roadPackDescriptor);
  writeFileSync(join(backup, RECEIPT_FILE), JSON.stringify(withActions([{ plugin: "MKIRoads", path: nestedPath, kind: "remove-misplaced" }], [{ path: nestedPath, files: [{ path: "MKIRoads.uplugin", sha256: sha256(roadPackDescriptor) }] }])));
  const nestedBefore = tree();
  assert.throws(() => restoreInstall(backup, game), /Restore refused before changing anything:[\s\S]*FactoryGame\/Mods\/GameFeatures\/OtherMod\/MKIRoads is inside a directory that holds FactoryGame\/Mods\/GameFeatures\/OtherMod\/OtherMod.uplugin/);
  assert.deepEqual(tree(), nestedBefore);
  for (const path of ["FactoryGame/Mods/SML", "FactoryGame/Mods/GameFeatures/OtherMod"]) rmSync(join(backup, path), { recursive: true });

  // The genuine receipt still restores.
  writeFileSync(join(backup, RECEIPT_FILE), receiptText);
  assert.equal(restoreInstall(backup, game).state, "restored");
});

test("apply refuses a forged plan before backing anything up", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), backup = join(root, "backup"), plan = planInstall(game, artifactsIn(root), backup);
  const before = hashTree(root);
  assert.deepEqual(plan.actions.flatMap(actionProblems), [], "every action the planner produces passes");
  for (const action of [
    { plugin: "SML", path: "FactoryGame/Mods/SML", kind: "replace" },
    { plugin: "MKIRoads", path: "FactoryGame/Mods/GameFeatures", kind: "remove-misplaced" },
    { plugin: "MKIRoads", path: "FactoryGame/Mods/GameFeatures/MKIBridge", kind: "replace" },
  ]) {
    assert.throws(() => applyInstall({ ...plan, actions: [...plan.actions, action as never] }), /Install refused; the plan has actions the installer never plans/);
    assert.deepEqual(hashTree(root), before);
  }
  assert.ok(!existsSync(backup));
});

test("explicit artifacts need verified package provenance for the same clean commit", (t) => {
  const root = scratch(t), commit = "5f2e9942a4c83fc5240ce6170a576de7ce15ca77", other = "1111111111111111111111111111111111111111";
  const refused = (name: string, plugin: InstallablePlugin, report: Record<string, unknown>, pattern: RegExp) => assert.throws(() => verifyArtifact(makeArtifact(join(root, name), plugin, { report })), pattern);
  const provenance = (plugin: InstallablePlugin, change: Record<string, unknown>) =>
    ({ commit, syncSha256: sha256("sync.json"), sdkInputsSha256: sha256("sdk inputs"), ...(plugin === "MKIRoads" ? { contentSha256: sha256("content") } : {}), verified: true, ...change });
  assert.equal(verifyArtifact(makeArtifact(join(root, "ok"), "MKIBridge")).commit, commit);

  refused("pre", "MKIRoads", { provenance: undefined }, /no provenance block; it predates provenance-checked packaging/);
  refused("other", "MKIRoads", { provenance: provenance("MKIRoads", { commit: other }) }, /provenance commit 1111111111111111111111111111111111111111 differs from source-commit.txt 5f2e9942/);
  refused("short", "MKIBridge", { provenance: provenance("MKIBridge", { commit: commit.slice(0, 12) }) }, /provenance commit "5f2e9942a4c8" is not a full commit hash/);
  refused("unverified", "MKIBridge", { provenance: provenance("MKIBridge", { verified: false }) }, /provenance was not verified unchanged after packaging/);
  refused("no-sdk", "MKIBridge", { provenance: provenance("MKIBridge", { sdkInputsSha256: undefined }) }, /provenance has no sdkInputsSha256/);
  refused("no-content", "MKIRoads", { provenance: provenance("MKIRoads", { contentSha256: undefined }) }, /provenance has no contentSha256/);
  refused("no-sync", "MKIRoads", { provenance: provenance("MKIRoads", { syncSha256: "x" }) }, /provenance has no syncSha256/);
  refused("problems", "MKIRoads", { problems: ["the SDK inputs changed while packaging"] }, /package-report.json records problems/);
  refused("no-problems", "MKIRoads", { problems: undefined }, /package-report.json records problems: undefined/);
  refused("install-path", "MKIRoads", { installPath: "FactoryGame/Mods/GameFeatures/MKIRoads" }, /installPath "FactoryGame\/Mods\/GameFeatures\/MKIRoads" is not FactoryGame\/Mods\/MKIRoads/);

  // source-commit.txt rewritten to another commit, keeping the report's provenance.
  const rewritten = makeArtifact(join(root, "rewritten"), "MKIRoads");
  writeFileSync(join(rewritten, "source-commit.txt"), `${other}\nclean\n`);
  assert.throws(() => verifyArtifact(rewritten), /provenance commit 5f2e9942a4c83fc5240ce6170a576de7ce15ca77 differs from source-commit.txt 1111111111111111111111111111111111111111[\s\S]*directory name MKIRoads-\S+ is not <plugin>-<version>-<commit>-<stamp>/);
  // A renamed directory no longer names the plugin, version, and commit it was packaged as.
  const renamed = makeArtifact(join(root, "renamed"), "MKIRoads"), moved = join(root, "renamed", `MKIRoads-0.0.1-${commit.slice(0, 12)}-20261009-120000`);
  renameSync(renamed, moved);
  assert.throws(() => verifyArtifact(moved), /directory name MKIRoads-0\.0\.1-5f2e9942a4c8-20261009-120000 is not <plugin>-<version>-<commit>-<stamp>/);
  const plain = join(root, "renamed", "release");
  renameSync(moved, plain);
  assert.throws(() => verifyArtifact(plain), /directory name release is not/);
});

test("Windows path aliases that could resolve inside SML are refused statically and on disk", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), backup = join(root, "backup"), plan = planInstall(game, artifactsIn(root), backup);
  write(join(game, "FactoryGame/Mods/SML/MKIRoads/MKIRoads.uplugin"), roadPackDescriptor);
  const before = hashTree(root);
  // `SML.` and `SML ` open SML on Windows; `SML~1` may be SML's 8.3 short name.
  for (const alias of ["SML.", "SML ", "SML~1", "SML. .", "GameFeatures.", "MKIRoads."]) {
    const path = alias.startsWith("MKIRoads") ? `FactoryGame/Mods/GameFeatures/${alias}` : `FactoryGame/Mods/${alias}/MKIRoads`;
    const action = { plugin: "MKIRoads", path, kind: "remove-misplaced" };
    assert.match(actionProblems(action).join("\n"), /not a normalized directory under FactoryGame\/Mods/, alias);
    assert.match(removalProblems(game, path, "MKIRoads").join("\n"), /segment Windows may resolve to another directory|not a directory dedicated to MKIRoads/, alias);
    assert.throws(() => applyInstall({ ...plan, actions: [...plan.actions, action as never] }), /Install refused; the plan has actions the installer never plans/, alias);
    assert.deepEqual(hashTree(root), before, `${alias}: nothing changes`);
  }
  assert.ok(!existsSync(backup));
});

test("a junction above a plugin directory is never followed into SML", (t) => {
  const root = scratch(t), game = makeGame(root, MISPLACED), backup = join(root, "backup");
  write(join(game, "FactoryGame/Mods/SML/MKIRoads/MKIRoads.uplugin"), roadPackDescriptor);
  symlinkSync(join(game, "FactoryGame/Mods/SML"), join(game, "FactoryGame/Mods/Redirect"), "junction");
  const path = "FactoryGame/Mods/Redirect/MKIRoads", action = { plugin: "MKIRoads" as const, path, kind: "remove-misplaced" as const };
  assert.deepEqual(actionProblems(action), [], "the path is lexically valid; only the on-disk check sees the junction");
  assert.deepEqual(removalProblems(game, path, "MKIRoads"), [`${path} is reached through the link FactoryGame/Mods/Redirect; the installer never replaces or removes it`]);
  const sml = hashTree(join(game, "FactoryGame/Mods/SML")), plan = planInstall(game, artifactsIn(root), backup);
  assert.throws(() => applyInstall({ ...plan, actions: [...plan.actions, action] }), /Install refused; the game tree changed since planning:[\s\S]*reached through the link FactoryGame\/Mods\/Redirect/);
  assert.deepEqual(hashTree(join(game, "FactoryGame/Mods/SML")), sml);
  assert.ok(!existsSync(backup));
  // A forged receipt naming the same path is refused before anything is deleted.
  write(join(backup, path, "MKIRoads.uplugin"), roadPackDescriptor);
  writeFileSync(join(backup, RECEIPT_FILE), JSON.stringify({
    format: "mki-roads.install-receipt", version: 1, createdAt: new Date().toISOString(), gameRoot: game, backupRoot: backup, state: "installed", artifacts: [],
    actions: [action], backups: [{ path, files: [{ path: "MKIRoads.uplugin", sha256: sha256(roadPackDescriptor) }] }],
  }));
  assert.throws(() => restoreInstall(backup, game), /Restore refused before changing anything:[\s\S]*reached through the link FactoryGame\/Mods\/Redirect/);
  assert.deepEqual(hashTree(join(game, "FactoryGame/Mods/SML")), sml);
});

test("a plugin directory that does not resolve to itself under the game root is refused", (t) => {
  const root = scratch(t), game = makeGame(root), path = "FactoryGame/Mods/Elsewhere/MKIRoads";
  write(join(game, path, "MKIRoads.uplugin"), roadPackDescriptor);
  write(join(game, "FactoryGame/Mods/SML/MKIRoads/MKIRoads.uplugin"), roadPackDescriptor);
  assert.deepEqual(removalProblems(game, path, "MKIRoads"), [], "the directory itself is a removable misplaced copy");
  // An alias no lexical or link check sees (for example a reparse point Node does not report as a link) is modelled by
  // making the operating system resolve the directory into SML.
  const native = realpathSync.native, full = resolve(game, path);
  t.mock.method(realpathSync, "native", (target: string) => resolve(target) === full ? native(join(game, "FactoryGame/Mods/SML/MKIRoads")) : native(target));
  assert.deepEqual(removalProblems(game, path, "MKIRoads"), [`${path} resolves to FactoryGame/Mods/SML/MKIRoads under the game root, not to itself; the installer never replaces or removes it`]);
  t.mock.method(realpathSync, "native", (target: string) => { if (resolve(target) === full) throw new Error("EACCES"); return native(target); });
  assert.deepEqual(removalProblems(game, path, "MKIRoads"), [`${path} resolves to (unresolvable) under the game root, not to itself; the installer never replaces or removes it`]);
});

test("MKI Tools is optional: an installation without it is complete, and one installed is checked like the others", (t) => {
  const root = scratch(t);
  const required = { "FactoryGame/Mods/MKIRoads": { plugin: "MKIRoads" as const, tag: "new" }, "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge" as const, tag: "new" } };
  const without = inspectInstallation(makeGame(join(root, "without"), required));
  assert.deepEqual(without.problems, []);
  assert.equal(without.plugins.MKITools.present, false);
  const withTools = inspectInstallation(makeGame(join(root, "with"), { ...required, "FactoryGame/Mods/MKITools": { plugin: "MKITools", tag: "new" } }));
  assert.deepEqual(withTools.problems, []);
  assert.equal(withTools.plugins.MKITools.version, JSON.parse(toolsDescriptor).SemVersion);
  // Negative: MKI Tools under Mods/GameFeatures does not mount there, and MKI Roads stays required.
  const misplaced = inspectInstallation(makeGame(join(root, "misplaced"), { ...required, "FactoryGame/Mods/GameFeatures/MKITools": { plugin: "MKITools", tag: "new" } }));
  assert.match(misplaced.problems.join("\n"), /MKITools is installed at FactoryGame\/Mods\/GameFeatures\/MKITools, but its containers mount at \.\.\/\.\.\/\.\.\/FactoryGame\/Mods\/MKITools\//);
  const noRoads = inspectInstallation(makeGame(join(root, "noroads"), { "FactoryGame/Mods/GameFeatures/MKIBridge": { plugin: "MKIBridge", tag: "new" }, "FactoryGame/Mods/MKITools": { plugin: "MKITools", tag: "new" } }));
  assert.match(noRoads.problems.join("\n"), /MKIRoads is not installed at FactoryGame\/Mods\/MKIRoads/);
});

test("an IoStore container with no asset (a plugin without content) mounts nothing, but one with assets at / still fails", () => {
  // The MKI Tools 0.1.0 listing: its pak holds only config and mounts at the plugin, its IoStore container only a header.
  const listing = [
    'LogPakFile: Display: Listing MKIToolsFactoryGame-Windows.pak with mount point "../../../FactoryGame/Mods/MKITools/"',
    'LogPakFile: Display: "AssetRegistry.bin" offset: 0, size: 104 bytes, sha1: 17FE, compression: None.',
    "LogIoStore: Display: Mount point /",
    'LogIoStore: Display: "<ContainerHeader>" offset: 0, size: 64 bytes, hash: 759e, compression: None.',
    "LogIoStore: Display: 1 files (64 bytes).",
  ];
  assert.equal(installPathFromListing("MKITools", listing), "FactoryGame/Mods/MKITools");
  const withAsset = [...listing.slice(0, 4), 'LogIoStore: Display: "Content/Rock.uasset" offset: 64, size: 10 bytes, hash: 1, compression: None.'];
  assert.equal(installPathFromListing("MKITools", withAsset), undefined);
});
