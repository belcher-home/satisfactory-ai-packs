import assert from "node:assert/strict";
import test from "node:test";
import { productVersions, repositoryFiles, setProductVersion, versionDrift, type VersionFiles } from "../src/project-versions.js";

/** An in-memory copy of the repository's versioned files, recording writes. */
function memoryFiles(): VersionFiles & { writes: string[] } {
  const real = repositoryFiles(), cache = new Map<string, string>(), writes: string[] = [];
  return {
    writes,
    read: (file) => cache.get(file) ?? (cache.set(file, real.read(file)), cache.get(file)!),
    write: (file, text) => { cache.set(file, text); writes.push(file); },
  };
}

test("every mod version copy equals its source", () => {
  assert.deepEqual(versionDrift(repositoryFiles()), []);
  const versions = productVersions(repositoryFiles());
  assert.match(versions.bridge ?? "", /^\d+\.\d+\.\d+$/);
  assert.match(versions.roadPack ?? "", /^\d+\.\d+\.\d+$/);
});

test("set rewrites a mod's source and every copy, and nothing else", () => {
  const files = memoryFiles();
  const written = setProductVersion(files, "roadPack", "9.9.9");
  assert.deepEqual(written.sort(), ["mods/MKIRoads/MKIRoads/MKIRoads.uplugin", "mods/MKIRoads/README.md", "mods/MKIRoads/modules.json"]);
  assert.deepEqual(versionDrift(files), []);
  assert.equal(productVersions(files).roadPack, "9.9.9");
});

test("drift and bad versions are refused", () => {
  const files = memoryFiles();
  files.write("mods/MKIBridge/MKIBridge/MKIBridge.uplugin", files.read("mods/MKIBridge/MKIBridge/MKIBridge.uplugin").replace(/"VersionName": "[^"]+"/, '"VersionName": "9.9.9"'));
  assert.match(versionDrift(files).join("\n"), /bridge: .*VersionName.* is "9\.9\.9"/);
  assert.throws(() => setProductVersion(memoryFiles(), "bridge", "1.0"), /not MAJOR\.MINOR\.PATCH/);
  assert.throws(() => setProductVersion(memoryFiles(), "workspace" as "bridge", "1.0.0"), /Unknown product/);
});
