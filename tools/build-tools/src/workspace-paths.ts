import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

function findPackageRoot(start: string): string {
  let current = start;
  for (;;) {
    const manifest = resolve(current, "package.json");
    if (existsSync(manifest)) {
      const parsed = JSON.parse(readFileSync(manifest, "utf8")) as { name?: string };
      if (parsed.name === "@satisfactory-mki/build-tools") return current;
    }
    const parent = dirname(current);
    if (parent === current) throw new Error("Unable to locate the build-tools package root");
    current = parent;
  }
}

function findRepositoryRoot(start: string): string {
  let current = start;
  for (;;) {
    const manifest = resolve(current, "package.json");
    if (existsSync(manifest)) {
      const parsed = JSON.parse(readFileSync(manifest, "utf8")) as { workspaces?: unknown };
      if (Array.isArray(parsed.workspaces)) return current;
    }
    const parent = dirname(current);
    if (parent === current) throw new Error("Unable to locate the repository workspace root");
    current = parent;
  }
}

const toolsPackageRoot = findPackageRoot(dirname(fileURLToPath(import.meta.url)));
export const repositoryRoot = findRepositoryRoot(toolsPackageRoot);
export const roadPackDirectory = resolve(repositoryRoot, "mods", "MKIRoads");
export const roadPackPluginDirectory = resolve(roadPackDirectory, "MKIRoads");
export const roadPackUnrealScriptsDirectory = resolve(roadPackDirectory, "unreal");
export const roadKitModulesPath = resolve(roadPackDirectory, "modules.json");
export const roadKitIconsPath = resolve(roadPackDirectory, "generated", "road-kit.icons.json");
export const bridgePluginDirectory = resolve(repositoryRoot, "mods", "MKIBridge", "MKIBridge");
export const toolsPluginDirectory = resolve(repositoryRoot, "mods", "MKITools", "MKITools");
/** The bridge module's private sources: one translation unit per operation family since #44, plus the shared internals. */
export const bridgePrivateSourceDirectory = resolve(bridgePluginDirectory, "Source", "MKIBridge", "Private");
/**
 * Every private source file of the native bridge as one LF text, for the structural checks the Node CI can run on code it
 * cannot compile (#41): the shared helper definitions (BridgeInternal.cpp), the module and routing unit
 * (MKIBridge.cpp), the other family units alphabetically, and last the internal header, so a search for a
 * definition finds the definition before the header's declaration.
 */
export function nativeBridgeSource(): string {
  const files = readdirSync(bridgePrivateSourceDirectory).filter((name) => /\.cpp$/.test(name));
  const order = ["BridgeInternal.cpp", "MKIBridge.cpp", ...files.filter((name) => name !== "BridgeInternal.cpp" && name !== "MKIBridge.cpp").sort(), "BridgeInternal.h"];
  return order.map((name) => readFileSync(resolve(bridgePrivateSourceDirectory, name), "utf8").replace(/\r\n/g, "\n")).join("\n");
}
export const localPackagesDirectory = resolve(repositoryRoot, ".local", "packages");
