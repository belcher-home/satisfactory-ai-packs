/**
 * Single-source mod versions (src/project-versions.ts).
 *
 *   npm run versions                              # print each mod's version; exit 1 on any drift
 *   npm run versions -- check                     # the same, for CI
 *   npm run versions -- set <bridge|roadPack> <MAJOR.MINOR.PATCH>
 *
 * `set` rewrites the product's source and every copy (all or nothing), then re-checks the whole repository.
 */
import { productVersions, repositoryFiles, setProductVersion, versionDrift, type ProductName } from "../src/project-versions.js";

const [command = "check", product, version] = process.argv.slice(2);
const files = repositoryFiles();
if (command === "set") {
  if (!product || !version) { console.error("usage: npm run versions -- set <bridge|roadPack> <MAJOR.MINOR.PATCH>"); process.exit(2); }
  for (const file of setProductVersion(files, product as ProductName, version)) console.log(`updated ${file}`);
} else if (command !== "check") {
  console.error(`Unknown command "${command}"; expected check or set.`);
  process.exit(2);
}
const problems = versionDrift(files);
console.log(JSON.stringify({ versions: productVersions(files), ok: problems.length === 0, problems }, null, 2));
if (problems.length > 0) process.exitCode = 1;
