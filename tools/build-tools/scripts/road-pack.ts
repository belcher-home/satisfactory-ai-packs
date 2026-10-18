/**
 * Reproducible SDK workflow for the MKI Roads content mod and the MKI Bridge (issue #63).
 *
 *   npm run road-pack -- sync     [--plugins MKIRoads,MKIBridge]
 *   npm run road-pack -- icons    [--update-golden]  # render the build-menu icons and compare them with the golden
 *   npm run road-pack -- editor   # compile the synced native modules for the SDK editor (FactoryEditor Development)
 *   npm run road-pack -- import   # icons + editor, build MKI Roads content in the SDK editor, then verify it in a fresh editor
 *   npm run road-pack -- verify
 *   npm run road-pack -- package  [--plugins MKIRoads,MKIBridge]
 *   npm run road-pack -- all      # sync, icons, editor, import + verify, package both plugins
 *
 * Environment: SML_ROOT (default D:/SatisfactoryModding/SatisfactoryModLoader), UE_ROOT (default
 * D:/DevTools/UnrealEngine-CSS). The SML checkout is changed only by `sync` (mirrors the repository plugin
 * sources into <SML>/Mods, backing up anything it overwrites) and by `import` (writes the generated MKI Roads
 * content into <SML>/Mods/MKIRoads/Content). Both paths are ignored by the SML repository. Nothing here
 * touches a game installation or a save. Artifacts, reports, and backups go to .local/. `package` refuses unless the
 * repository is clean and sync.json and the import/verify provenance records match HEAD and the SDK tree
 * (src/road-pack-provenance.ts), so an artifact never stamps stale SDK content as the current commit. Every SDK file
 * Alpakit could package must be a synced source file, editor-checked MKI Roads content, a build output, or an approved
 * SDK-only file with its pinned hash (PluginSpec.sdkOnly). package-report.json records the checked provenance, which
 * road-pack:install requires.
 */
import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, cpSync, existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { loadRoadKit, roadPackAssetPackages, roadPackEditorPlan } from "../src/road-kit.js";
import { encodePng, ICON_RENDER_ATTEMPTS, isRenderCrash, loadRoadKitIcons, renderRoadKitIcons, roadKitIconsDocument } from "../src/road-pack-icons.js";
import {
  cookedRegistryProblems, dependencyContractProblems, descriptorModules, installPathFromListing, packagedArtifactProblems, readPluginDescriptor, shippingBuildOutputs, PLUGIN_INSTALL_PATHS, type InstallablePlugin,
} from "../src/road-pack.js";
import { hashTree } from "../src/road-pack-install.js";
import { packageProvenanceProblems, sdkInputFiles, treeDigest, type EditorProvenance } from "../src/road-pack-provenance.js";
import {
  bridgePluginDirectory, localPackagesDirectory, repositoryRoot, roadKitIconsPath, roadKitModulesPath, roadPackPluginDirectory, roadPackUnrealScriptsDirectory,
} from "../src/workspace-paths.js";

const args = process.argv.slice(2), command = args[0];
const option = (name: string) => { const index = args.indexOf(name); return index >= 0 ? args[index + 1] : undefined; };
const SML = resolve(process.env.SML_ROOT ?? "D:/SatisfactoryModding/SatisfactoryModLoader");
const UE = resolve(process.env.UE_ROOT ?? "D:/DevTools/UnrealEngine-CSS");
const PROJECT = join(SML, "FactoryGame.uproject");
const WORK = resolve(repositoryRoot, ".local", "road-pack");
const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..*/, "").replace("T", "-");
const git = (...gitArgs: string[]) => execFileSync("git", ["-C", repositoryRoot, ...gitArgs], { encoding: "utf8" }).trim();
const sha256 = (path: string) => createHash("sha256").update(readFileSync(path)).digest("hex");

interface PluginSpec {
  name: InstallablePlugin;
  source: string;
  /** Destination inside the SML checkout. */
  target: string;
  /** Repository-relative paths mirrored into the target; directories are replaced wholesale. */
  mirrored: string[];
  /** Compile native modules while packaging. */
  build: boolean;
  /**
   * Files that exist only in the SDK copy and are packaged, by plugin-relative path, with their reviewed SHA-256. Any
   * other SDK file outside the mirrored sources, build outputs, and editor-checked MKI Roads content blocks packaging.
   */
  sdkOnly: Record<string, string>;
}
// SDK locations mirror the game install layout (PLUGIN_INSTALL_PATHS): Alpakit derives the container mount points from them.
const PLUGINS: Record<InstallablePlugin, PluginSpec> = {
  // MKI Roads ships one runtime module: the spline road classes and the root game-world module's native parent.
  MKIRoads: { name: "MKIRoads", source: roadPackPluginDirectory, target: join(SML, "Mods", "MKIRoads"), mirrored: ["MKIRoads.uplugin", "Config", "Source"], build: true, sdkOnly: {} },
  MKIBridge: {
    name: "MKIBridge", source: bridgePluginDirectory, target: join(SML, "Mods", "GameFeatures", "MKIBridge"),
    mirrored: ["MKIBridge.uplugin", "Config", "Source"], build: true,
    // The game-feature data and input config assets created with the plugin. They exist only in the SDK copy because the
    // repository commits no .uasset files; a fresh SML checkout needs them created before packaging (docs/NATIVE_BUILD.md,
    // #3). The Alpakit target list (Config/Alpakit.ini) is tracked and mirrored since #3.
    sdkOnly: {
      "Content/NewPlayerMappableInputConfig.uasset": "f6b64faff3530acb9fcd72e64caba039080d28a943dc6d1aedd5af315563e3f4",
      "Content/MKIBridge.uasset": "8097a234c99b8b16f52f96c792e837f60e64aa7f3b041a9c42ce88a9102bffb1",
    },
  },
};
const selectedPlugins = () => (option("--plugins") ?? "MKIRoads,MKIBridge").split(",").map((name) => {
  const spec = PLUGINS[name.trim() as InstallablePlugin];
  if (!spec) throw new Error(`Unknown plugin ${name}; expected one of ${Object.keys(PLUGINS).join(", ")}`);
  return spec;
});

function listFiles(root: string): string[] {
  if (!existsSync(root)) return [];
  if (statSync(root).isFile()) return [root];
  return readdirSync(root, { withFileTypes: true }).flatMap((entry) => entry.isDirectory() ? listFiles(join(root, entry.name)) : [join(root, entry.name)]);
}

function requireSdk() {
  for (const path of [PROJECT, join(UE, "Engine", "Build", "BatchFiles", "RunUAT.bat"), join(UE, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe")]) {
    if (!existsSync(path)) throw new Error(`Missing ${path}; set SML_ROOT and UE_ROOT`);
  }
}

function sync() {
  requireSdk();
  const backup = join(WORK, `sdk-backup-${stamp}`), records = [];
  for (const spec of selectedPlugins()) {
    const files: { path: string; sha256: string }[] = [];
    for (const item of spec.mirrored) {
      const from = join(spec.source, item), to = join(spec.target, item);
      for (const existing of listFiles(to)) {
        const saved = join(backup, spec.name, relative(spec.target, existing));
        mkdirSync(dirname(saved), { recursive: true });
        copyFileSync(existing, saved);
      }
      if (statSync(from).isDirectory()) {
        // Source is mirrored exactly; Config keeps SDK-only files (for example a local Alpakit.ini).
        if (item !== "Config") rmSync(to, { recursive: true, force: true });
        cpSync(from, to, { recursive: true });
      } else {
        mkdirSync(dirname(to), { recursive: true });
        copyFileSync(from, to);
      }
      for (const file of listFiles(from)) files.push({ path: relative(spec.source, file).replaceAll("\\", "/"), sha256: sha256(file) });
    }
    mkdirSync(join(spec.target, "Content"), { recursive: true });
    records.push({ plugin: spec.name, target: spec.target, files });
  }
  const record = { command: "sync", commit: git("rev-parse", "HEAD"), dirty: git("status", "--porcelain") !== "", sml: SML, backup: existsSync(backup) ? backup : null, plugins: records };
  mkdirSync(WORK, { recursive: true });
  writeFileSync(join(WORK, "sync.json"), JSON.stringify(record, null, 2) + "\n");
  console.log(JSON.stringify({ ...record, plugins: records.map((item) => ({ plugin: item.plugin, target: item.target, files: item.files.length })) }, null, 2));
}

/** Render every spline road and road pattern icon to .local/road-pack/icons; the pixels must equal the committed golden. */
function icons(updateGolden = args.includes("--update-golden")): string {
  const directory = join(WORK, "icons"), rendered = renderRoadKitIcons(loadRoadKit()), document = roadKitIconsDocument(rendered);
  rmSync(directory, { recursive: true, force: true });
  mkdirSync(directory, { recursive: true });
  for (const icon of rendered) writeFileSync(join(directory, `${icon.iconPath.slice(icon.iconPath.lastIndexOf(".") + 1)}.png`), encodePng(icon.rgba, icon.size, icon.size));
  if (updateGolden) writeFileSync(roadKitIconsPath, JSON.stringify(document, null, 2) + "\n");
  const golden = existsSync(roadKitIconsPath) ? loadRoadKitIcons() : undefined;
  const mismatched = Object.keys(document.icons).filter((path) => golden?.icons[path]?.rgbaSha256 !== document.icons[path].rgbaSha256);
  const stale = golden ? Object.keys(golden.icons).filter((path) => !document.icons[path]) : [];
  console.log(JSON.stringify({ command: "icons", directory, icons: rendered.length, goldenUpdated: updateGolden, mismatched, stale }, null, 2));
  if (!golden || mismatched.length > 0 || stale.length > 0) throw new Error(`Icons differ from ${roadKitIconsPath}; review them in ${directory} and rerun with --update-golden`);
  return directory;
}

/**
 * The icons step in its own Node process, retried when that process crashes (#314: Node 20 segfaults intermittently in
 * the rasterizer on this machine, and a crash took the whole road-pack run with it). A golden mismatch is retried once:
 * on this machine one icon once rendered different pixels for unchanged code (#316), but a real change mismatches again
 * and fails.
 */
function iconsIsolated(): string {
  let mismatches = 0;
  for (let attempt = 1; attempt <= ICON_RENDER_ATTEMPTS; attempt++) {
    const result = spawnSync(process.execPath, [...process.execArgv, process.argv[1]!, "icons"], { stdio: "inherit" });
    if (result.status === 0) return join(WORK, "icons");
    if (!isRenderCrash(result.status, result.signal) && ++mismatches >= 2) throw new Error(`The icons step failed twice (exit ${result.status}); see its output above`);
    console.error(`The icons step ${isRenderCrash(result.status, result.signal) ? "crashed" : "failed"} (exit ${result.status ?? result.signal}), attempt ${attempt} of ${ICON_RENDER_ATTEMPTS}`);
  }
  throw new Error(`The icons step did not pass in ${ICON_RENDER_ATTEMPTS} attempts`);
}

/** Compile the synced native plugin modules (MKI Roads and the bridge) for the SDK editor, which import/verify load. */
function buildEditor() {
  requireSdk();
  mkdirSync(WORK, { recursive: true });
  const log = join(WORK, "editor-build.log");
  const batchFiles = join(UE, "Engine", "Build", "BatchFiles");
  const result = spawnSync("cmd.exe", ["/d", "/s", "/c", `""${join(batchFiles, "Build.bat")}" FactoryEditor Win64 Development "-Project=${PROJECT}" -WaitMutex -NoHotReloadFromIDE"`], { cwd: batchFiles, encoding: "utf8", maxBuffer: 1 << 30, windowsVerbatimArguments: true });
  writeFileSync(log, `${result.stdout ?? ""}${result.stderr ?? ""}`);
  const binaries = Object.values(PLUGINS).map((spec) => join(spec.target, "Binaries", "Win64", `UnrealEditor-${spec.name}.dll`));
  console.log(JSON.stringify({ command: "editor", exit: result.status, log, binaries: binaries.map((file) => ({ file, present: existsSync(file) })) }, null, 2));
  if (result.status !== 0 || binaries.some((file) => !existsSync(file))) throw new Error(`Editor build failed (exit ${result.status}); see ${log}`);
}

function runEditor(mode: "import" | "verify"): Record<string, unknown> {
  requireSdk();
  mkdirSync(WORK, { recursive: true });
  const report = join(WORK, `${mode}-report.json`), config = join(WORK, `${mode}-config.json`), log = join(WORK, `${mode}-editor.log`);
  rmSync(report, { force: true });
  // Import rebuilds the content, so neither earlier provenance record still describes it.
  for (const step of mode === "import" ? ["import", "verify"] : [mode]) rmSync(editorProvenancePath(step), { force: true });
  if (mode === "import") {
    // MKI Roads content is a build output: rebuild it from the manifest and the icons every time.
    const content = join(PLUGINS.MKIRoads.target, "Content");
    if (!existsSync(join(PLUGINS.MKIRoads.target, "MKIRoads.uplugin"))) throw new Error("MKIRoads is not synced into the SDK; run npm run road-pack -- sync first");
    rmSync(content, { recursive: true, force: true });
    mkdirSync(content, { recursive: true });
  }
  // The plan carries every value derived in road-kit.ts (spline road entries, patterns, asset list), so the editor
  // builds and verifies exactly what the TypeScript contract and its tests describe.
  const plan = join(WORK, "editor-plan.json");
  writeFileSync(plan, JSON.stringify(roadPackEditorPlan(loadRoadKit()), null, 2) + "\n");
  writeFileSync(config, JSON.stringify({ mode, manifest: roadKitModulesPath, plan, iconDir: mode === "import" ? iconsIsolated() : join(WORK, "icons"), report }, null, 2));
  const result = spawnSync(join(UE, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe"), [
    PROJECT, "-run=pythonscript", `-script=${join(roadPackUnrealScriptsDirectory, "road_pack_editor.py")}`, "-unattended", "-nop4", "-nosplash", "-NullRHI", "-stdout", "-FullStdOutLogOutput",
  ], { cwd: SML, env: { ...process.env, MKIR_EDITOR_CONFIG: config }, encoding: "utf8", maxBuffer: 1 << 30 });
  writeFileSync(log, `${result.stdout ?? ""}${result.stderr ?? ""}`);
  if (!existsSync(report)) throw new Error(`Editor ${mode} wrote no report (exit ${result.status}); see ${log}`);
  const parsed = JSON.parse(readFileSync(report, "utf8")) as { ok: boolean; errors: string[]; checks: { ok: boolean }[] };
  const provenance: EditorProvenance = {
    format: "mki-roads.editor-provenance", mode, commit: git("rev-parse", "HEAD"), clean: git("status", "--porcelain") === "",
    syncSha256: existsSync(SYNC_RECORD) ? sha256(SYNC_RECORD) : "", reportSha256: sha256(report), ok: parsed.ok === true, contentSha256: roadPackContentDigest(),
  };
  writeFileSync(editorProvenancePath(mode), JSON.stringify(provenance, null, 2) + "\n");
  console.log(JSON.stringify({ mode, ok: parsed.ok, exit: result.status, checks: parsed.checks.length, failed: parsed.errors, report, log }, null, 2));
  if (!parsed.ok) throw new Error(`Editor ${mode} failed; see ${report}`);
  return parsed;
}

function unrealPakList(container: string): string[] {
  const result = spawnSync(join(UE, "Engine", "Binaries", "Win64", "UnrealPak.exe"), [container, "-List"], { encoding: "utf8", maxBuffer: 1 << 28 });
  return `${result.stdout ?? ""}`.split(/\r?\n/);
}

const SYNC_RECORD = join(WORK, "sync.json");
const editorProvenancePath = (mode: string) => join(WORK, `${mode}-provenance.json`);
const roadPackContentDigest = () => treeDigest(hashTree(join(PLUGINS.MKIRoads.target, "Content")));
const readJson = <T>(path: string): T | undefined => { try { return JSON.parse(readFileSync(path, "utf8")) as T; } catch { return undefined; } };

/** Throws unless the synced SDK sources and (for MKI Roads) the imported and verified content come from the clean HEAD. */
function requireProvenance(spec: PluginSpec): { commit: string; syncSha256: string; sdkInputsSha256: string; contentSha256?: string } {
  const head = git("rev-parse", "HEAD"), sourceRelative = relative(repositoryRoot, spec.source).replaceAll("\\", "/");
  const filesOf = (root: string) => spec.mirrored.flatMap((item) => listFiles(join(root, item))).map((file) => ({ path: relative(root, file).replaceAll("\\", "/"), sha256: sha256(file) }));
  const tracked = git("ls-files", "--", ...spec.mirrored.map((item) => `${sourceRelative}/${item}`)).split(/\r?\n/).filter(Boolean).map((path) => path.slice(sourceRelative.length + 1));
  const report = (mode: string) => existsSync(join(WORK, `${mode}-report.json`)) ? sha256(join(WORK, `${mode}-report.json`)) : undefined;
  const contentSha256 = spec.name === "MKIRoads" ? roadPackContentDigest() : undefined, sdk = sdkInputFiles(spec.target, spec.name), sdkFiles = sdk.files;
  const problems = packageProvenanceProblems({
    plugin: spec.name, head, status: git("status", "--porcelain"), syncText: existsSync(SYNC_RECORD) ? readFileSync(SYNC_RECORD, "utf8") : undefined,
    sourceFiles: filesOf(spec.source), trackedFiles: tracked, sdkFiles, sdkLinks: sdk.links, sdkOnlyFiles: spec.sdkOnly,
    editor: contentSha256 === undefined ? undefined : {
      import: { provenance: readJson<EditorProvenance>(editorProvenancePath("import")), reportSha256: report("import") },
      verify: { provenance: readJson<EditorProvenance>(editorProvenancePath("verify")), reportSha256: report("verify") },
      contentSha256,
    },
  });
  if (problems.length > 0) throw new Error(`Refusing to package ${spec.name}: its SDK inputs do not provably match the clean HEAD ${head.slice(0, 12)}:\n- ${problems.join("\n- ")}`);
  return { commit: head, syncSha256: sha256(SYNC_RECORD), sdkInputsSha256: treeDigest(sdkFiles), contentSha256 };
}

function packagePlugin(spec: PluginSpec) {
  requireSdk();
  const provenance = requireProvenance(spec);
  const archive = join(SML, "Saved", "ArchivedPlugins", spec.name, `${spec.name}-Windows.zip`);
  rmSync(archive, { force: true });
  const version = (JSON.parse(readFileSync(join(spec.source, `${spec.name}.uplugin`), "utf8")) as { SemVersion: string }).SemVersion;
  const commit = git("rev-parse", "--short=12", "HEAD"), out = join(localPackagesDirectory, `${spec.name}-${version}-${commit}-${stamp}`);
  mkdirSync(out, { recursive: true });
  // #134: UBT once judged a stale Steam DLL up to date and relinked only the EGS target, so the archive shipped an older
  // build. Move the plugin's shipping outputs aside first: both store targets must relink from the synced source, and
  // the archived DLLs must then be newer than this run (packagedArtifactProblems, freshness).
  const packageStartedMs = Date.now(), replacedBuildOutputs: string[] = [];
  const sdkBinaries = join(spec.target, "Binaries", "Win64");
  if (spec.build && existsSync(sdkBinaries)) {
    const aside = join(out, "replaced-build-outputs");
    for (const name of shippingBuildOutputs(readdirSync(sdkBinaries))) {
      mkdirSync(aside, { recursive: true });
      renameSync(join(sdkBinaries, name), join(aside, name));
      replacedBuildOutputs.push(name);
    }
  }
  // The same UAT command line Alpakit issues for a Windows-only Shipping package.
  const uatArgs = [
    `-ScriptsForProject=${PROJECT}`, "PackagePlugin", `-project=${PROJECT}`, "-clientconfig=Shipping", "-serverconfig=Shipping", "-utf8output",
    `-DLCName=${spec.name}`, ...(spec.build ? ["-build"] : []), "-platform=Win64", "-nocompileeditor", "-installed",
  ];
  const batchFiles = join(UE, "Engine", "Build", "BatchFiles");
  const result = spawnSync("cmd.exe", ["/d", "/s", "/c", `""${join(batchFiles, "RunUAT.bat")}" ${uatArgs.map((item) => `"${item}"`).join(" ")}"`], { cwd: batchFiles, encoding: "utf8", maxBuffer: 1 << 30, windowsVerbatimArguments: true });
  writeFileSync(join(out, "uat.log"), `${result.stdout ?? ""}${result.stderr ?? ""}`);
  if (result.status !== 0 || !existsSync(archive)) throw new Error(`PackagePlugin ${spec.name} failed (exit ${result.status}); see ${join(out, "uat.log")}`);
  const zip = join(out, `${spec.name}-Windows.zip`), staged = join(out, "contents");
  copyFileSync(archive, zip);
  mkdirSync(staged, { recursive: true });
  // Windows bsdtar reads zip archives; a GNU tar earlier on PATH (Git Bash) would treat "D:" as a remote host.
  execFileSync(process.platform === "win32" ? join(process.env.SystemRoot ?? "C:/Windows", "System32", "tar.exe") : "tar", ["-xf", zip, "-C", staged]);
  const files = listFiles(staged).map((file) => relative(staged, file).replaceAll("\\", "/")).sort();
  const containers = files.filter((file) => /\.(pak|utoc)$/.test(file));
  const listing = containers.flatMap((file) => unrealPakList(join(staged, file)));
  const manifests = Object.fromEntries(files.filter((file) => file.endsWith(".modules")).map((file) => [file, readFileSync(join(staged, file), "utf8")]));
  // The module manifests must name exactly the native modules the descriptor the game loads declares.
  const nativeModules = descriptorModules(readPluginDescriptor(join(files.includes(`${spec.name}.uplugin`) ? staged : spec.source, `${spec.name}.uplugin`)));
  const binaryModifiedMs = Object.fromEntries(files.filter((file) => file.startsWith("Binaries/")).map((file) => [file, statSync(join(staged, file)).mtimeMs]));
  const problems = packagedArtifactProblems({ plugin: spec.name, version, files, containerListing: listing, modulesManifests: manifests, expectsBinaries: spec.build, nativeModules,
    freshness: spec.build ? { packageStartedMs, binaryModifiedMs } : undefined,
    expectedAssets: spec.name === "MKIRoads" ? roadPackAssetPackages(loadRoadKit()).map((path) => path.slice(path.lastIndexOf("/") + 1)) : undefined,
  });
  // The cooked asset registry is how SML finds the root module that registers the schematic, recipes, and build menu.
  let cookedRegistry: { path: string; sha256: string; problems: string[] } | undefined;
  if (spec.name === "MKIRoads") {
    const pak = join(staged, "Content", "Paks", "Windows", `${spec.name}FactoryGame-Windows.pak`), extracted = join(out, "pak-extract");
    spawnSync(join(UE, "Engine", "Binaries", "Win64", "UnrealPak.exe"), [pak, "-Extract", extracted], { encoding: "utf8", maxBuffer: 1 << 28 });
    const registry = join(extracted, "AssetRegistry.bin");
    const registryProblems = existsSync(registry) ? cookedRegistryProblems(readFileSync(registry), roadPackAssetPackages(loadRoadKit())) : [`${registry} was not extracted from ${pak}`];
    cookedRegistry = { path: relative(out, registry).replaceAll("\\", "/"), sha256: existsSync(registry) ? sha256(registry) : "", problems: registryProblems };
    problems.push(...registryProblems);
  }
  // The game must install the plugin where its containers mount.
  const installPath = installPathFromListing(spec.name, listing);
  if (installPath !== PLUGIN_INSTALL_PATHS[spec.name]) problems.push(`containers mount at ${installPath ?? "(no single plugin directory)"}, not the install directory ${PLUGIN_INSTALL_PATHS[spec.name]}`);
  // The packaged descriptor must keep the dependency contract: the bridge requires a compatible MKI Roads, never the reverse.
  if (files.includes(`${spec.name}.uplugin`)) {
    const packaged = readPluginDescriptor(join(staged, `${spec.name}.uplugin`));
    const bridge = spec.name === "MKIBridge" ? packaged : readPluginDescriptor(join(PLUGINS.MKIBridge.source, "MKIBridge.uplugin"));
    const roadPack = spec.name === "MKIRoads" ? packaged : readPluginDescriptor(join(PLUGINS.MKIRoads.source, "MKIRoads.uplugin"));
    problems.push(...dependencyContractProblems(bridge, roadPack));
  }
  const hashes = [zip, ...files.map((file) => join(staged, file))].map((file) => ({ file: relative(out, file).replaceAll("\\", "/"), sha256: sha256(file) }));
  writeFileSync(join(out, "SHA256SUMS.txt"), hashes.map((item) => `${item.sha256} *${item.file}`).join("\n") + "\n");
  writeFileSync(join(out, "container-listing.txt"), listing.join("\n") + "\n");
  // Packaging takes minutes: the inputs must still be the ones checked before it started.
  let verified = false;
  try {
    const after = requireProvenance(spec);
    verified = JSON.stringify(after) === JSON.stringify(provenance);
    if (!verified) problems.push(`the SDK inputs changed while packaging (${JSON.stringify(provenance)} -> ${JSON.stringify(after)})`);
  } catch (error) { problems.push(error instanceof Error ? error.message : String(error)); }
  writeFileSync(join(out, "source-commit.txt"), `${provenance.commit}\n${problems.length === 0 ? "clean" : "dirty: the SDK inputs changed or stopped matching HEAD while packaging"}\n`);
  const summary = { plugin: spec.name, version, artifact: out, zip, zipSha256: sha256(zip), installPath: PLUGIN_INSTALL_PATHS[spec.name], files, cookedRegistry, provenance: { ...provenance, verified },
    freshness: spec.build ? { packageStartedAt: new Date(packageStartedMs).toISOString(), replacedBuildOutputs, binaryModifiedAt: Object.fromEntries(Object.entries(binaryModifiedMs).map(([file, ms]) => [file, new Date(ms).toISOString()])) } : undefined,
    ok: problems.length === 0, problems };
  writeFileSync(join(out, "package-report.json"), JSON.stringify(summary, null, 2) + "\n");
  console.log(JSON.stringify(summary, null, 2));
  if (problems.length > 0) throw new Error(`Packaged ${spec.name} failed verification`);
}

loadRoadKit();
try {
  if (command === "sync") sync();
  else if (command === "icons") icons();
  else if (command === "editor") buildEditor();
  else if (command === "import") { buildEditor(); runEditor("import"); runEditor("verify"); }
  else if (command === "verify") runEditor("verify");
  else if (command === "package") for (const spec of selectedPlugins()) packagePlugin(spec);
  else if (command === "all") { sync(); buildEditor(); runEditor("import"); runEditor("verify"); for (const spec of selectedPlugins()) packagePlugin(spec); }
  else { console.error("Usage: npm run road-pack -- sync|icons|editor|import|verify|package|all [--plugins MKIRoads,MKIBridge] [--update-golden]"); process.exitCode = 2; }
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
