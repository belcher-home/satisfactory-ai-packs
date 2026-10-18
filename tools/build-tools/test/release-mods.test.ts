import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";
import { privateReferences, publicText, RELEASE_TEMPLATE, RELEASE_TEXT, RELEASED_DIRECTORIES, templateTarget } from "../src/release-mods.js";
import { repositoryRoot } from "../src/workspace-paths.js";

// The template and the git history exist only in the development repository; a released copy skips these checks.
const inDevelopment = { skip: !existsSync(resolve(repositoryRoot, RELEASE_TEMPLATE)) };
const PRIVATE = ["belcher-home", "satisfactory-mki"].join("/");
const held = new Set(["mods/MKIRoads/README.md", "mods/MKIBridge/README.md", "tools/build-tools/scripts/road-pack.ts"]);

test("private issue links become their plain label", () => {
  const text = `Fixed in [#180](https://github.com/${PRIVATE}/issues/180) and [PR 7](https://github.com/${PRIVATE}-bridge/pull/7#x).`;
  assert.equal(publicText("mods/MKIRoads/README.md", text, held), "Fixed in #180 and PR 7.");
});

test("relative links survive only when the release holds their target", () => {
  const text = "[bridge](../MKIBridge/README.md#current-contract), [tool](../../tools/build-tools/scripts/road-pack.ts), [docs](../../docs/NATIVE_BUILD.md), [dir](../MKIRoads/)";
  assert.equal(publicText("mods/MKIRoads/README.md", text, held), "[bridge](../MKIBridge/README.md#current-contract), [tool](../../tools/build-tools/scripts/road-pack.ts), docs, [dir](../MKIRoads/)");
});

test("links in source files and links to the public repository are left alone", () => {
  const source = "// see [x](../../docs/NATIVE_BUILD.md)";
  assert.equal(publicText("mods/MKIRoads/MKIRoads/Source/MKIRoads/Private/A.cpp", source, held), source);
  const external = "[SML](https://docs.ficsit.app/) and [mods](https://github.com/belcher-home/satisfactory-mki-mods/issues/1)";
  assert.equal(publicText("mods/MKIRoads/README.md", external, held), external);
});

test("private references are reported; the public repository and placeholders are not", () => {
  assert.deepEqual(privateReferences("a.md", "see " + ["belcher-home", "satisfactory-mki"].join("/")), ["a.md: private repository"]);
  assert.deepEqual(privateReferences("a.md", "see " + ["belcher-home", "satisfactory-mki-bridge", "issues"].join("/")), ["a.md: private repository"]);
  assert.deepEqual(privateReferences("a.md", "asked by " + ["Dar", "rick"].join("")), ["a.md: maintainer's name"]);
  assert.deepEqual(privateReferences("a.md", "mail someone" + "@" + "gmail.com"), ["a.md: email address"]);
  assert.deepEqual(privateReferences("a.md", ["C:", "Users", "someone"].join("\\")), ["a.md: home directory"]);
  assert.deepEqual(privateReferences("a.md", "the maintainer; D:/SatisfactoryModding/SatisfactoryModLoader; belcher-home/satisfactory-mki-mods"), []);
});

test("template files land at the release root", () => {
  assert.equal(templateTarget(`${RELEASE_TEMPLATE}/README.md`), "README.md");
  assert.equal(templateTarget(`${RELEASE_TEMPLATE}/.github/workflows/ci.yml`), ".github/workflows/ci.yml");
});

test("every released file and template is free of private references after rewriting", inDevelopment, () => {
  const files = execFileSync("git", ["ls-files", "--", ...RELEASED_DIRECTORIES, RELEASE_TEMPLATE], { cwd: repositoryRoot, encoding: "utf8" }).split("\n").filter(Boolean);
  const released = new Set(files.map((file) => file.startsWith(RELEASE_TEMPLATE + "/") ? templateTarget(file) : file));
  const problems = files.filter((file) => RELEASE_TEXT.test(file)).flatMap((file) => {
    const path = file.startsWith(RELEASE_TEMPLATE + "/") ? templateTarget(file) : file;
    return privateReferences(path, publicText(path, readFileSync(resolve(repositoryRoot, file), "utf8"), released));
  });
  assert.deepEqual(problems, []);
});

test("the template's LICENSE is the unmodified GPL-3.0 text", inDevelopment, () => {
  const license = readFileSync(resolve(repositoryRoot, RELEASE_TEMPLATE, "LICENSE"), "utf8");
  assert.match(license, /^ {20}GNU GENERAL PUBLIC LICENSE\n {23}Version 3, 29 June 2007\n/);
  assert.equal(license.split("\n").length, 675);
});
