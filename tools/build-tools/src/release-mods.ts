/**
 * The public satisfactory-mki-mods repository holds only released mods: one commit per release, written by
 * `npm run release:mods` (scripts/release-mods.ts) from this repository. These are the rules for what goes into it and
 * the rewriting that keeps private development out of it.
 */
import { posix } from "node:path";

/** Tracked paths copied as they are: the mods and the tools that build, package, install and version them. */
export const RELEASED_DIRECTORIES = ["mods", "tools/build-tools"];
/** The repository's own files (README, license, notice, changelog, media, CI, package.json), copied to its root. */
export const RELEASE_TEMPLATE = "release/mods-repo";
export const RELEASE_TEXT = /\.(md|ts|json|cpp|h|cs|py|ini|sh|uplugin|yml|gitattributes|gitignore)$|(^|\/)(LICENSE|NOTICE)$/;
const PRIVATE_ISSUE_LINK = /\[([^\]]+)\]\(https:\/\/github\.com\/belcher-home\/satisfactory-(?:mki|ai)(?:-bridge)?\/(?:issues|pull)\/\d+[^)]*\)/g;

/** Text that must not reach the public repository. */
export const PRIVATE_REFERENCES: Array<[string, RegExp]> = [
  ["private repository", /belcher-home\/satisfactory-(?:mki|ai)(?:-bridge)?(?![-\w])/],
  // Built from parts so this file does not match itself.
  ["maintainer's name", new RegExp(["Dar", "rick"].join(""))],
  ["email address", /[A-Za-z0-9._%+-]+@(gmail|outlook|hotmail|yahoo)\.com/],
  ["home directory", /[A-Za-z]:[\\/]+Users[\\/]/],
];

/** Plain text for links to private issues, and for relative markdown links to files the release does not hold. */
export function publicText(path: string, text: string, released: ReadonlySet<string>): string {
  let out = text.replace(PRIVATE_ISSUE_LINK, "$1");
  if (path.endsWith(".md")) {
    out = out.replace(/\[([^\]]+)\]\(((?:\.\.?\/)[^)#\s]*)(#[^)]*)?\)/g, (link, label: string, target: string) => {
      const resolved = posix.normalize(posix.join(posix.dirname(path), target)).replace(/\/$/, "");
      const held = released.has(resolved) || [...released].some((file) => file.startsWith(resolved + "/"));
      return held ? link : label;
    });
  }
  return out;
}

export function privateReferences(path: string, text: string): string[] {
  return PRIVATE_REFERENCES.filter(([, pattern]) => pattern.test(text)).map(([name]) => `${path}: ${name}`);
}

/** The released path of a template file: release/mods-repo/README.md becomes README.md. */
export function templateTarget(path: string): string {
  return path.slice(RELEASE_TEMPLATE.length + 1);
}
