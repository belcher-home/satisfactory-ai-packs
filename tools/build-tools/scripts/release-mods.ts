/**
 * Write a release of the mods into a clone of the public satisfactory-mki-mods repository.
 *
 *   npm run release:mods -- <satisfactory-mki-mods clone>
 *
 * Everything in the clone except `.git` is replaced by the tracked files of mods/, tools/build-tools and the
 * release/mods-repo template (src/release-mods.ts), from a clean HEAD. Private issue links and links to files the
 * release does not hold become plain text, and the export fails if a private reference survives. It then refreshes the
 * clone's package-lock.json. Review the clone's diff, then commit and tag there, for example `MKIRoads-v0.8.0`.
 */
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve, sep } from "node:path";
import { privateReferences, publicText, RELEASE_TEMPLATE, RELEASE_TEXT, RELEASED_DIRECTORIES, templateTarget } from "../src/release-mods.js";
import { repositoryRoot } from "../src/workspace-paths.js";

const git = (...args: string[]) => execFileSync("git", args, { cwd: repositoryRoot, encoding: "utf8" }).trim();
const target = process.argv[2];
if (!target) { console.error("Usage: npm run release:mods -- <satisfactory-mki-mods clone>"); process.exit(2); }
const output = resolve(process.env.INIT_CWD ?? process.cwd(), target);
if (!existsSync(join(output, ".git"))) throw new Error(`${output} is not a git clone of satisfactory-mki-mods`);
if (output === repositoryRoot || output.startsWith(repositoryRoot + sep)) throw new Error("The release goes into a separate clone, not into this repository");
if (git("status", "--porcelain", "--", ...RELEASED_DIRECTORIES, RELEASE_TEMPLATE)) throw new Error("Commit the released files first: a release is taken from a clean HEAD");

const commit = git("rev-parse", "HEAD");
const sources = git("ls-files", "--", ...RELEASED_DIRECTORIES, RELEASE_TEMPLATE).split("\n").filter(Boolean);
const paths = new Map(sources.map((file) => [file, file.startsWith(RELEASE_TEMPLATE + "/") ? templateTarget(file) : file]));
const released = new Set(paths.values());
for (const entry of readdirSync(output)) if (entry !== ".git") rmSync(join(output, entry), { recursive: true, force: true });
const problems: string[] = [];
for (const [source, path] of paths) {
  const content = execFileSync("git", ["show", `HEAD:${source}`], { cwd: repositoryRoot, maxBuffer: 64 * 1024 * 1024 });
  mkdirSync(dirname(join(output, path)), { recursive: true });
  if (!RELEASE_TEXT.test(path)) { writeFileSync(join(output, path), content); continue; }
  const text = publicText(path, content.toString("utf8"), released);
  problems.push(...privateReferences(path, text));
  writeFileSync(join(output, path), text);
}
if (problems.length > 0) throw new Error(`Private references in the release:\n${problems.join("\n")}`);
execFileSync("npm", ["install", "--package-lock-only", "--ignore-scripts", "--no-audit", "--no-fund"], { cwd: output, stdio: "inherit", shell: true });
console.log(JSON.stringify({ output, from: commit, files: paths.size + 1 }, null, 2));
