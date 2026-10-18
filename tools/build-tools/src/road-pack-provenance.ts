import { createHash } from "node:crypto";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { join, relative } from "node:path";

/**
 * Package provenance for the SDK workflow (issue #63): `npm run road-pack -- package` stamps an artifact with the
 * repository HEAD only when every input it packages provably came from that clean HEAD.
 *
 * - `sync` records the commit, whether the repository was clean, and the hash of every mirrored source file.
 * - `import` and `verify` record an `EditorProvenance` next to their reports: the commit, cleanliness, the hash of the
 *   `sync.json` they ran against, the hash of their report, and the hash of the MKI Roads content tree they produced
 *   or checked.
 * - `packageProvenanceProblems` checks all of that against the current HEAD, working tree, source files, SDK mirror,
 *   and SDK content. Every file Alpakit could package from the SDK plugin directory (everything except build outputs and
 *   the editor-checked MKI Roads content) must be a mirrored source file or an approved SDK-only file with its pinned
 *   hash, so stale or unreviewed SDK files (extra Config, Resources, or bridge Content) block packaging.
 */

export interface FileHash { path: string; sha256: string }

export interface SyncRecord {
  command: "sync";
  commit: string;
  dirty: boolean;
  plugins: { plugin: string; target: string; files: FileHash[] }[];
}

export interface EditorProvenance {
  format: "mki-roads.editor-provenance";
  mode: "import" | "verify";
  commit: string;
  /** `git status --porcelain` was empty when the editor ran. */
  clean: boolean;
  /** SHA-256 of the `sync.json` the SDK mirror was written from. */
  syncSha256: string;
  /** SHA-256 of the editor's report file. */
  reportSha256: string;
  ok: boolean;
  /** `treeDigest` of the MKI Roads SDK content after the editor ran. */
  contentSha256: string;
}

/** One SHA-256 over a sorted file list's paths and hashes. */
export function treeDigest(files: readonly FileHash[]): string {
  const sorted = [...files].sort((a, b) => a.path.localeCompare(b.path));
  return createHash("sha256").update(sorted.map((file) => `${file.sha256} ${file.path}\n`).join("")).digest("hex");
}

export interface PackageProvenanceInput {
  plugin: string;
  /** Full commit hash of HEAD. */
  head: string;
  /** `git status --porcelain` for the whole repository (untracked files included); empty when clean. */
  status: string;
  /** The text of `.local/road-pack/sync.json`, when it exists. */
  syncText?: string;
  /** Plugin-relative mirrored source files in the repository now. */
  sourceFiles: FileHash[];
  /** Plugin-relative mirrored files git tracks at HEAD. */
  trackedFiles: string[];
  /**
   * Plugin-relative files in the SDK plugin directory now: everything Alpakit may package, so only build outputs
   * (`SDK_BUILD_OUTPUTS`) and, for MKI Roads, the editor-checked `Content` are left out.
   */
  sdkFiles: FileHash[];
  /** Plugin-relative symbolic links and junctions in the SDK plugin directory (`sdkInputFiles`); any blocks packaging. */
  sdkLinks?: string[];
  /** Approved files that exist only in the SDK copy (for example a local `Config/Alpakit.ini`), by path, with their pinned SHA-256. */
  sdkOnlyFiles?: Record<string, string>;
  /** Required for plugins with imported content (MKI Roads). */
  editor?: {
    import?: { provenance?: EditorProvenance; reportSha256?: string };
    verify?: { provenance?: EditorProvenance; reportSha256?: string };
    /** `treeDigest` of the SDK content now. */
    contentSha256: string;
  };
}

/** Top-level SDK plugin directories that hold build outputs, never packaging inputs. */
export const SDK_BUILD_OUTPUTS = ["Binaries", "Intermediate", "Saved"] as const;

/**
 * Every file Alpakit may package from the SDK plugin directory `target`: everything except the top-level build outputs
 * (`SDK_BUILD_OUTPUTS`, exact names) and, for MKI Roads, the editor-checked `Content`. Symbolic links and junctions
 * anywhere in the directory, at any depth inside an excluded directory too, are returned in `links` rather than
 * followed, so a redirected input can never be packaged unchecked. Excluded directories are only listed for links;
 * their files are never hashed.
 */
export function sdkInputFiles(target: string, plugin: string): { files: FileHash[]; links: string[] } {
  const skipped = new Set<string>([...SDK_BUILD_OUTPUTS, ...(plugin === "MKIRoads" ? ["Content"] : [])]), files: FileHash[] = [], links: string[] = [];
  const walk = (directory: string, excluded: boolean) => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const full = join(directory, entry.name), path = relative(target, full).replaceAll("\\", "/");
      const skip = excluded || (directory === target && skipped.has(entry.name));
      if (entry.isSymbolicLink()) links.push(path);
      else if (entry.isDirectory()) walk(full, skip);
      else if (!skip) files.push({ path, sha256: createHash("sha256").update(readFileSync(full)).digest("hex") });
    }
  };
  if (existsSync(target)) walk(target, false);
  return { files: files.sort((a, b) => a.path.localeCompare(b.path)), links: links.sort() };
}

const sameFiles = (a: readonly FileHash[], b: readonly FileHash[]) => treeDigest(a) === treeDigest(b);
const syncSha256 = (text: string) => createHash("sha256").update(text).digest("hex");

/** Every reason the SDK inputs and outputs for `plugin` do not provably correspond to the clean HEAD; empty when they do. */
export function packageProvenanceProblems(input: PackageProvenanceInput): string[] {
  const problems: string[] = [], head = input.head.slice(0, 12);
  if (input.status.trim() !== "") problems.push(`the repository has uncommitted or untracked changes; commit them, then rerun sync, import, and package:\n  ${input.status.trim().split(/\r?\n/).join("\n  ")}`);
  const tracked = new Set(input.trackedFiles), untracked = input.sourceFiles.filter((file) => !tracked.has(file.path)).map((file) => file.path);
  const missing = input.trackedFiles.filter((path) => !input.sourceFiles.some((file) => file.path === path));
  if (untracked.length > 0) problems.push(`${input.plugin} source files are not tracked at HEAD ${head} (ignored or untracked), but sync mirrors them: ${untracked.join(", ")}`);
  if (missing.length > 0) problems.push(`${input.plugin} tracked source files are missing from the working tree: ${missing.join(", ")}`);

  let record: SyncRecord | undefined;
  try { record = input.syncText ? JSON.parse(input.syncText) as SyncRecord : undefined; } catch { problems.push("sync.json is not valid JSON; rerun npm run road-pack -- sync"); }
  const synced = record?.plugins?.find((item) => item.plugin === input.plugin);
  if (!input.syncText) problems.push("no sync.json; run npm run road-pack -- sync");
  else if (record) {
    if (record.commit !== input.head) problems.push(`the SDK was synced from ${String(record.commit).slice(0, 12)}, not HEAD ${head}; rerun sync, import, and package`);
    if (record.dirty !== false) problems.push("the SDK was synced from a dirty working tree; commit, then rerun sync, import, and package");
    if (!synced) problems.push(`sync.json has no ${input.plugin} record; rerun sync for ${input.plugin}`);
    else if (!sameFiles(synced.files, input.sourceFiles)) problems.push(`${input.plugin} source files differ from what sync.json recorded; rerun sync`);
  }
  // The SDK plugin directory must hold exactly what was synced, plus only approved SDK-only files with their pinned hashes.
  const pinned = input.sdkOnlyFiles ?? {}, sdk = new Map(input.sdkFiles.map((file) => [file.path, file.sha256]));
  const changed = input.sourceFiles.filter((file) => sdk.get(file.path) !== file.sha256).map((file) => file.path);
  const extra = input.sdkFiles.filter((file) => !input.sourceFiles.some((item) => item.path === file.path));
  const unapproved = extra.filter((file) => !Object.hasOwn(pinned, file.path)).map((file) => file.path);
  const altered = extra.filter((file) => Object.hasOwn(pinned, file.path) && pinned[file.path] !== file.sha256).map((file) => file.path);
  const absent = Object.keys(pinned).filter((path) => !sdk.has(path));
  if (changed.length > 0) problems.push(`the SDK copy of ${input.plugin} differs from the repository at ${changed.join(", ")}; rerun sync`);
  if (unapproved.length > 0) problems.push(`the SDK copy of ${input.plugin} has files that are neither in the repository nor approved SDK-only files, so they would be packaged unchecked: ${unapproved.join(", ")}; remove them or rerun sync`);
  if (altered.length > 0) problems.push(`approved SDK-only files of ${input.plugin} differ from their pinned hashes: ${altered.join(", ")}`);
  if (absent.length > 0) problems.push(`approved SDK-only files of ${input.plugin} are missing from the SDK: ${absent.join(", ")}`);
  if ((input.sdkLinks ?? []).length > 0) problems.push(`the SDK copy of ${input.plugin} contains links or junctions, whose targets Alpakit could package unchecked: ${input.sdkLinks!.join(", ")}; replace them with real files or remove them`);

  if (input.editor) {
    const currentSync = input.syncText ? syncSha256(input.syncText) : undefined;
    for (const mode of ["import", "verify"] as const) {
      const step = input.editor[mode], provenance = step?.provenance;
      if (!provenance) { problems.push(`no ${mode} provenance; run npm run road-pack -- import after sync`); continue; }
      if (provenance.format !== "mki-roads.editor-provenance" || provenance.mode !== mode) problems.push(`${mode}-provenance.json is not an editor provenance record for ${mode}`);
      if (provenance.commit !== input.head) problems.push(`${mode} ran at ${String(provenance.commit).slice(0, 12)}, not HEAD ${head}; rerun import`);
      if (provenance.clean !== true) problems.push(`${mode} ran on a dirty working tree; rerun import from a clean tree`);
      if (provenance.ok !== true) problems.push(`${mode} did not pass`);
      if (!currentSync || provenance.syncSha256 !== currentSync) problems.push(`${mode} ran against a different sync than the current sync.json; rerun import`);
      if (!step?.reportSha256 || provenance.reportSha256 !== step.reportSha256) problems.push(`the ${mode} report is missing or changed since ${mode} ran; rerun import`);
      if (provenance.contentSha256 !== input.editor.contentSha256) problems.push(`the ${input.plugin} SDK content differs from what ${mode} ${mode === "import" ? "produced" : "checked"}; rerun import`);
    }
  }
  return problems;
}
