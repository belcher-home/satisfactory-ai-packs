import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";
import { packageProvenanceProblems, sdkInputFiles, treeDigest, type EditorProvenance, type FileHash, type PackageProvenanceInput } from "../src/road-pack-provenance.js";

// Package provenance (#80 review M6): packaging fails unless the synced SDK inputs and the imported and verified
// content provably come from the current clean HEAD.
const HEAD = "8a1d2810000000000000000000000000000000ab", OLD = "1a3e9bb2420bdaf80410db7c70884ede2ec73cf6";
const sha256 = (text: string) => createHash("sha256").update(text).digest("hex");
const SOURCE: FileHash[] = [
  { path: "MKIRoads.uplugin", sha256: sha256("descriptor 0.4.0") },
  { path: "Config/DefaultGame.ini", sha256: sha256("config") },
  { path: "Source/MKIRoads/MKIRoads.Build.cs", sha256: sha256("build") },
];
const CONTENT = treeDigest([{ path: "Build_MKIR_Straight.uasset", sha256: sha256("asset") }]);

function syncText(overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({ command: "sync", commit: HEAD, dirty: false, plugins: [{ plugin: "MKIRoads", target: "SML/Mods/MKIRoads", files: SOURCE }], ...overrides });
}
function editor(mode: "import" | "verify", sync: string, overrides: Partial<EditorProvenance> = {}): { provenance: EditorProvenance; reportSha256: string } {
  const reportSha256 = sha256(`${mode} report`);
  return { provenance: { format: "mki-roads.editor-provenance", mode, commit: HEAD, clean: true, syncSha256: sha256(sync), reportSha256, ok: true, contentSha256: CONTENT, ...overrides }, reportSha256 };
}
function current(overrides: Partial<PackageProvenanceInput> = {}): PackageProvenanceInput {
  const sync = overrides.syncText ?? syncText();
  return {
    plugin: "MKIRoads", head: HEAD, status: "", syncText: sync, sourceFiles: SOURCE, trackedFiles: SOURCE.map((file) => file.path),
    sdkFiles: [...SOURCE, { path: "Config/Alpakit.ini", sha256: sha256("sdk only") }], sdkOnlyFiles: { "Config/Alpakit.ini": sha256("sdk only") },
    editor: { import: editor("import", sync), verify: editor("verify", sync), contentSha256: CONTENT },
    ...overrides,
  };
}
const problems = (overrides: Partial<PackageProvenanceInput> = {}) => packageProvenanceProblems(current(overrides)).join("\n");

test("inputs synced, imported, and verified at the clean HEAD are accepted", () => {
  assert.deepEqual(packageProvenanceProblems(current()), []);
  // Plugins without imported content need only the sync checks.
  assert.deepEqual(packageProvenanceProblems({ ...current(), plugin: "MKIRoads", editor: undefined }), []);
});

test("a dirty or untracked working tree blocks packaging", () => {
  assert.match(problems({ status: " M mods/MKIRoads/MKIRoads/MKIRoads.uplugin" }), /uncommitted or untracked changes[\s\S]*MKIRoads.uplugin/);
  assert.match(problems({ status: "?? tools/build-tools/src/new.ts" }), /uncommitted or untracked changes/);
  // A gitignored file inside a mirrored directory is invisible to git status but would still be synced.
  const ignored = [...SOURCE, { path: "Source/MKIRoads/Private/Local.cpp", sha256: sha256("local") }];
  assert.match(problems({ sourceFiles: ignored, syncText: syncText({ plugins: [{ plugin: "MKIRoads", target: "t", files: ignored }] }) }), /not tracked at HEAD 8a1d28100000 \(ignored or untracked\), but sync mirrors them: Source\/MKIRoads\/Private\/Local.cpp/);
  assert.match(problems({ trackedFiles: [...SOURCE.map((file) => file.path), "Source/MKIRoads/Gone.cpp"] }), /tracked source files are missing from the working tree: Source\/MKIRoads\/Gone.cpp/);
});

test("a stale, dirty, missing, or mismatched sync blocks packaging", () => {
  assert.match(problems({ syncText: syncText({ commit: OLD }) }), /the SDK was synced from 1a3e9bb2420b, not HEAD 8a1d28100000/);
  assert.match(problems({ syncText: syncText({ dirty: true }) }), /synced from a dirty working tree/);
  assert.match(problems({ syncText: undefined }), /no sync.json/);
  assert.match(problems({ syncText: "{" }), /sync.json is not valid JSON/);
  assert.match(problems({ syncText: syncText({ plugins: [] }) }), /sync.json has no MKIRoads record/);
  // The source changed after sync (then committed, so HEAD and status alone would not notice).
  const edited = SOURCE.map((file) => file.path === "MKIRoads.uplugin" ? { ...file, sha256: sha256("descriptor 0.3.4") } : file);
  assert.match(problems({ sourceFiles: edited }), /MKIRoads source files differ from what sync.json recorded; rerun sync/);
});

test("an SDK mirror that no longer matches the synced sources blocks packaging", () => {
  const tampered = current().sdkFiles.map((file) => file.path === "Source/MKIRoads/MKIRoads.Build.cs" ? { ...file, sha256: sha256("edited in SDK") } : file);
  assert.match(problems({ sdkFiles: tampered }), /the SDK copy of MKIRoads differs from the repository at Source\/MKIRoads\/MKIRoads.Build.cs/);
  assert.match(problems({ sdkFiles: SOURCE.slice(1) }), /differs from the repository at MKIRoads.uplugin/);
  assert.match(problems({ sdkFiles: [...current().sdkFiles, { path: "Source/MKIRoads/Stale.cpp", sha256: sha256("stale") }] }), /neither in the repository nor approved SDK-only files, so they would be packaged unchecked: Source\/MKIRoads\/Stale.cpp/);
});

test("every packaged SDK input outside the synced sources must be an approved SDK-only file with its pinned hash", () => {
  const sdk = current().sdkFiles, unchecked = (path: string) => problems({ sdkFiles: [...sdk, { path, sha256: sha256("stale") }] });
  // Extra Config, Resources, or plugin Content in the SDK copy is packaged by Alpakit, so it must not slip through.
  assert.match(unchecked("Config/DefaultEngine.ini"), /would be packaged unchecked: Config\/DefaultEngine.ini/);
  assert.match(unchecked("Resources/Icon128.png"), /would be packaged unchecked: Resources\/Icon128.png/);
  assert.match(unchecked("Content/MKIBridge.uasset"), /would be packaged unchecked: Content\/MKIBridge.uasset/);
  assert.match(unchecked("Stray.txt"), /would be packaged unchecked: Stray.txt/);
  // An approved SDK-only file whose content changed, or that disappeared, also blocks packaging.
  const edited = sdk.map((file) => file.path === "Config/Alpakit.ini" ? { ...file, sha256: sha256("Targets=Windows,LinuxServer") } : file);
  assert.match(problems({ sdkFiles: edited }), /approved SDK-only files of MKIRoads differ from their pinned hashes: Config\/Alpakit.ini/);
  assert.match(problems({ sdkFiles: SOURCE }), /approved SDK-only files of MKIRoads are missing from the SDK: Config\/Alpakit.ini/);
  // Nothing is SDK-only unless it is approved.
  assert.match(problems({ sdkOnlyFiles: undefined }), /would be packaged unchecked: Config\/Alpakit.ini/);
});

test("stale or missing import and verify outputs block packaging", () => {
  const sync = syncText();
  const withStep = (mode: "import" | "verify", step: { provenance?: EditorProvenance; reportSha256?: string } | undefined) =>
    problems({ editor: { ...current().editor!, [mode]: step } });
  assert.match(withStep("import", undefined), /no import provenance/);
  assert.match(withStep("verify", { reportSha256: sha256("verify report") }), /no verify provenance/);
  assert.match(withStep("import", editor("import", sync, { commit: OLD })), /import ran at 1a3e9bb2420b, not HEAD 8a1d28100000/);
  assert.match(withStep("verify", editor("verify", sync, { clean: false })), /verify ran on a dirty working tree/);
  assert.match(withStep("verify", editor("verify", sync, { ok: false })), /verify did not pass/);
  assert.match(withStep("import", editor("import", syncText({ commit: OLD }))), /import ran against a different sync than the current sync.json/);
  assert.match(withStep("verify", { ...editor("verify", sync), reportSha256: sha256("another report") }), /the verify report is missing or changed since verify ran/);
  assert.match(withStep("verify", { provenance: editor("verify", sync).provenance }), /the verify report is missing/);
  assert.match(withStep("import", editor("verify", sync)), /import-provenance.json is not an editor provenance record for import/);
  // Content rebuilt or edited after verify, or verify run against content another import produced.
  assert.match(problems({ editor: { ...current().editor!, contentSha256: treeDigest([]) } }), /the MKIRoads SDK content differs from what import produced[\s\S]*differs from what verify checked/);
  assert.match(withStep("import", editor("import", sync, { contentSha256: treeDigest([]) })), /differs from what import produced/);
});

test("treeDigest is order-independent and content-sensitive", () => {
  const files = [{ path: "b", sha256: sha256("b") }, { path: "a", sha256: sha256("a") }];
  assert.equal(treeDigest(files), treeDigest([...files].reverse()));
  assert.notEqual(treeDigest(files), treeDigest([{ path: "a", sha256: sha256("a") }]));
  assert.notEqual(treeDigest(files), treeDigest([{ path: "b", sha256: sha256("b") }, { path: "a", sha256: sha256("A") }]));
});

/** A fake SDK plugin directory with `files` (plugin-relative path -> content). */
function sdkTree(t: { after: (fn: () => void) => void }, files: Record<string, string>): string {
  const root = mkdtempSync(join(tmpdir(), "mkir-sdk-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  for (const [path, content] of Object.entries(files)) {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), content);
  }
  return root;
}

test("SDK input enumeration skips only top-level build outputs and MKI Roads' editor-checked Content", (t) => {
  const target = sdkTree(t, {
    "MKIRoads.uplugin": "descriptor", "Config/Alpakit.ini": "sdk only", "Resources/Icon128.png": "icon", "Stray.txt": "stray",
    "Binaries/Win64/FactoryGameSteam-MKIRoadsOld-Win64-Shipping.dll": "stale", "Intermediate/Build/x.obj": "obj", "Saved/Logs/x.log": "log",
    "Content/Buildables/Build_MKIR.uasset": "asset",
    // Only the top-level directories are build outputs; a nested one is an input.
    "Source/MKIRoads/Binaries/Embedded.bin": "embedded",
  });
  const paths = (plugin: string) => sdkInputFiles(target, plugin).files.map((file) => file.path);
  assert.deepEqual(paths("MKIRoads"), ["Config/Alpakit.ini", "MKIRoads.uplugin", "Resources/Icon128.png", "Source/MKIRoads/Binaries/Embedded.bin", "Stray.txt"]);
  // The bridge's Content is not editor-checked, so it is an input.
  assert.ok(paths("MKIBridge").includes("Content/Buildables/Build_MKIR.uasset"));
  assert.equal(sdkInputFiles(target, "MKIRoads").files.find((file) => file.path === "Stray.txt")?.sha256, sha256("stray"));
  assert.deepEqual(sdkInputFiles(join(target, "missing"), "MKIRoads"), { files: [], links: [] });
});

test("unchecked SDK inputs found by enumeration fail closed", (t) => {
  // A lower-case build-output name is not excluded (exact names only), so its files are inputs and must be approved.
  const target = sdkTree(t, { "MKIRoads.uplugin": "descriptor", "binaries/Win64/Stale.dll": "stale", "Saved/ok.log": "log" });
  const { files, links } = sdkInputFiles(target, "MKIRoads");
  const descriptor = files.find((file) => file.path === "MKIRoads.uplugin")!, sourceFiles = [descriptor];
  const input = { ...current(), sourceFiles, trackedFiles: ["MKIRoads.uplugin"], sdkFiles: files, sdkLinks: links, sdkOnlyFiles: {}, syncText: undefined, editor: undefined };
  assert.match(packageProvenanceProblems(input).join("\n"), /would be packaged unchecked: binaries\/Win64\/Stale.dll/);
  // A junction is never followed, wherever it is, even as an excluded build-output directory.
  const outside = sdkTree(t, { "Payload.uasset": "payload" });
  symlinkSync(outside, join(target, "Config"), "junction");
  rmSync(join(target, "Saved"), { recursive: true });
  symlinkSync(outside, join(target, "Saved"), "junction");
  const linked = sdkInputFiles(target, "MKIRoads");
  assert.deepEqual(linked.links, ["Config", "Saved"]);
  assert.ok(!linked.files.some((file) => file.path.endsWith("Payload.uasset")));
  assert.match(packageProvenanceProblems({ ...input, sdkFiles: linked.files, sdkLinks: linked.links }).join("\n"), /contains links or junctions, whose targets Alpakit could package unchecked: Config, Saved/);
});

test("links nested inside excluded build outputs and Content are found without hashing their generated files", (t) => {
  const target = sdkTree(t, {
    "MKIRoads.uplugin": "descriptor", "Source/MKIRoads/Private/Module.cpp": "source",
    "Content/Buildables/Build_MKIR.uasset": "asset", "Binaries/Win64/FactoryGameSteam-MKIRoads-Win64-Shipping.dll": "dll",
    "Intermediate/Build/Win64/x.obj": "obj", "Saved/Logs/x.log": "log",
  });
  const outside = sdkTree(t, { "Payload.uasset": "payload" });
  for (const path of ["Content/Buildables/Linked", "Binaries/Win64/Linked", "Intermediate/Build/Win64/Linked", "Saved/Logs/Linked", "Source/MKIRoads/Private/Linked"]) symlinkSync(outside, join(target, path), "junction");
  const { files, links } = sdkInputFiles(target, "MKIRoads");
  assert.deepEqual(links, ["Binaries/Win64/Linked", "Content/Buildables/Linked", "Intermediate/Build/Win64/Linked", "Saved/Logs/Linked", "Source/MKIRoads/Private/Linked"]);
  assert.deepEqual(files.map((file) => file.path), ["MKIRoads.uplugin", "Source/MKIRoads/Private/Module.cpp"], "excluded files are not hashed and links are never followed");
  // The bridge's Content is an input, so its files are hashed; its nested links are found either way.
  const bridge = sdkInputFiles(target, "MKIBridge");
  assert.ok(bridge.files.some((file) => file.path === "Content/Buildables/Build_MKIR.uasset"));
  assert.deepEqual(bridge.links, links);
  const input = { ...current(), sourceFiles: files, trackedFiles: files.map((file) => file.path), sdkFiles: files, sdkLinks: links, sdkOnlyFiles: {}, syncText: undefined, editor: undefined };
  assert.match(packageProvenanceProblems(input).join("\n"), /contains links or junctions, whose targets Alpakit could package unchecked: Binaries\/Win64\/Linked, Content\/Buildables\/Linked, Intermediate\/Build\/Win64\/Linked, Saved\/Logs\/Linked, Source\/MKIRoads\/Private\/Linked/);
});
