/**
 * Install, check, and restore the packaged MKI Roads and MKI Bridge (issue #63).
 *
 *   npm run road-pack:install -- check   --game <root> [--artifacts <dir>[,<dir>]]
 *   npm run road-pack:install -- install --game <root> [--artifacts <dir>[,<dir>]] [--plugins MKIRoads,MKIBridge] [--backup-dir <dir>] [--dry-run]
 *   npm run road-pack:install -- restore --game <root> --backup <dir>
 *   npm run road-pack:install -- fake-game <dir> [--mirror <root>]
 *
 * `--game` is always explicit: the directory that contains FactoryGame/ (a real installation, or a fake tree made by
 * `fake-game`). Nothing defaults to the live game. `--artifacts` defaults to the newest ok package of each plugin built
 * from the current HEAD under .local/packages. MKI Roads installs to FactoryGame/Mods/MKIRoads and the bridge to
 * FactoryGame/Mods/GameFeatures/MKIBridge, as their container mount points require. `install` refuses while
 * Satisfactory is running (`--skip-process-check` for fake trees only), backs up every directory it replaces or removes
 * to .local/road-pack/<install-stamp>/ (never inside FactoryGame/Mods), and rolls back on failure. It replaces or removes
 * only directories attributable to one plugin (never Mods, Mods/GameFeatures, or a directory with another descriptor).
 * `restore` checks every backup before deleting anything and retries a rollback-failed or restore-failed receipt. `fake-game` creates
 * a minimal tree with SML, or with `--mirror` copies a game's FactoryGame/Mods (read-only on the source) so an install
 * can be rehearsed against the exact current layout. No command starts the game or touches a save.
 */
import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { applyInstall, inspectInstallation, installBackupName, latestArtifacts, planInstall, restoreInstall, verifyArtifact, type VerifiedArtifact } from "../src/road-pack-install.js";
import { isInstallablePlugin, PLUGIN_INSTALL_PATHS } from "../src/road-pack.js";
import { localPackagesDirectory, repositoryRoot } from "../src/workspace-paths.js";

const args = process.argv.slice(2), command = args[0];
const option = (name: string) => { const index = args.indexOf(name); return index >= 0 ? args[index + 1] : undefined; };
const flag = (name: string) => args.includes(name);
const WORK = resolve(repositoryRoot, ".local", "road-pack");
const print = (value: unknown) => console.log(JSON.stringify(value, null, 2));

function gameRoot(): string {
  const root = option("--game");
  if (!root) throw new Error("Pass --game <root>: the directory that contains FactoryGame/ (use fake-game to make a test tree)");
  return resolve(root);
}

function selectedPlugins() {
  return (option("--plugins") ?? Object.keys(PLUGIN_INSTALL_PATHS).join(",")).split(",").map((name) => {
    if (!isInstallablePlugin(name.trim())) throw new Error(`Unknown plugin ${name}; expected one of ${Object.keys(PLUGIN_INSTALL_PATHS).join(", ")}`);
    return name.trim() as keyof typeof PLUGIN_INSTALL_PATHS;
  });
}

function artifacts(required: boolean): VerifiedArtifact[] {
  const given = option("--artifacts");
  if (given) return given.split(",").map((directory) => verifyArtifact(resolve(directory.trim())));
  const head = execFileSync("git", ["-C", repositoryRoot, "rev-parse", "HEAD"], { encoding: "utf8" }).trim(), latest = latestArtifacts(localPackagesDirectory, head);
  const plugins = selectedPlugins(), missing = plugins.filter((plugin) => !latest[plugin]);
  if (missing.length > 0) {
    if (!required) return [];
    throw new Error(`No ok package of ${missing.join(", ")} built from HEAD ${head.slice(0, 12)} under ${localPackagesDirectory}; run npm run road-pack -- package or pass --artifacts`);
  }
  return plugins.map((plugin) => verifyArtifact(latest[plugin]!));
}

/** Satisfactory's shipping executables (Steam and Epic). */
function runningGame(): string[] {
  if (process.platform !== "win32") return [];
  const output = execFileSync("tasklist.exe", ["/FO", "CSV", "/NH"], { encoding: "utf8" });
  return [...new Set(output.split(/\r?\n/).map((line) => /^"([^"]+)"/.exec(line)?.[1] ?? "").filter((name) => /^FactoryGame.*\.exe$/i.test(name)))];
}

function summary(report: ReturnType<typeof inspectInstallation>) {
  return { gameRoot: report.gameRoot, ok: report.ok, problems: report.problems, sml: report.sml,
    plugins: Object.fromEntries(Object.entries(report.plugins).filter(([name]) => isInstallablePlugin(name)).map(([name, item]) => [name, { ...item, files: item.files?.length }])),
    otherPlugins: Object.fromEntries(Object.entries(report.plugins).filter(([name]) => !isInstallablePlugin(name)).map(([name, item]) => [name, item.locations])) };
}

function check() {
  const report = inspectInstallation(gameRoot(), artifacts(false));
  print(summary(report));
  if (!report.ok) process.exitCode = 1;
}

function install() {
  const root = gameRoot(), backupRoot = resolve(option("--backup-dir") ?? join(WORK, installBackupName()));
  const plan = planInstall(root, artifacts(true), backupRoot);
  const printable = { gameRoot: plan.gameRoot, backupRoot: plan.backupRoot, dryRun: flag("--dry-run"), problems: plan.problems, actions: plan.actions,
    artifacts: plan.artifacts.map((item) => ({ plugin: item.plugin, version: item.version, commit: item.commit, zipSha256: item.zipSha256, installPath: item.installPath, directory: item.directory })),
    before: summary(plan.before) };
  if (flag("--dry-run")) { print(printable); if (plan.problems.length > 0) process.exitCode = 1; return; }
  if (!flag("--skip-process-check")) {
    const running = runningGame();
    if (running.length > 0) throw new Error(`Satisfactory is running (${running.join(", ")}); quit the game before installing`);
  }
  const receipt = applyInstall(plan);
  print({ ...printable, state: receipt.state, receipt: join(receipt.backupRoot, "install-receipt.json"), after: summary(receipt.after!), restore: `npm run road-pack:install -- restore --game "${root}" --backup "${receipt.backupRoot}"` });
}

function restore() {
  const backup = option("--backup");
  if (!backup) throw new Error("Pass --backup <dir> (the backup directory an install printed)");
  if (!flag("--skip-process-check") && runningGame().length > 0) throw new Error("Satisfactory is running; quit the game before restoring");
  const receipt = restoreInstall(resolve(backup), gameRoot());
  print({ state: receipt.state, gameRoot: receipt.gameRoot, restored: receipt.backups.map((item) => item.path), after: summary(inspectInstallation(receipt.gameRoot)) });
}

function fakeGame() {
  const target = args[1] ? resolve(args[1]) : undefined, mirror = option("--mirror");
  if (!target) throw new Error("Usage: npm run road-pack:install -- fake-game <dir> [--mirror <root>]");
  if (existsSync(target) && readdirSync(target).length > 0) throw new Error(`${target} is not empty`);
  if (mirror) {
    const mods = join(resolve(mirror), "FactoryGame", "Mods");
    if (!existsSync(mods)) throw new Error(`${mods} does not exist`);
    cpSync(mods, join(target, "FactoryGame", "Mods"), { recursive: true });
  } else {
    mkdirSync(join(target, "FactoryGame", "Mods", "SML"), { recursive: true });
    writeFileSync(join(target, "FactoryGame", "Mods", "SML", "SML.uplugin"), JSON.stringify({ FileVersion: 3, SemVersion: "3.12.0", VersionName: "3.12.0", FriendlyName: "Satisfactory Mod Loader (fake game tree)" }, null, 2) + "\n");
  }
  writeFileSync(join(target, "FAKE_GAME_TREE.txt"), `Fake Satisfactory game tree for road-pack:install rehearsals${mirror ? `; FactoryGame/Mods mirrored from ${resolve(mirror)}` : ""}.\n`);
  print({ fakeGame: target, mirroredFrom: mirror ? resolve(mirror) : null, report: summary(inspectInstallation(target)) });
}

try {
  if (command === "check") check();
  else if (command === "install") install();
  else if (command === "restore") restore();
  else if (command === "fake-game") fakeGame();
  else { console.error("Usage: npm run road-pack:install -- check|install|restore|fake-game (see scripts/road-pack-install.ts)"); process.exitCode = 2; }
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
