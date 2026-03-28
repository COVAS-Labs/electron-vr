import { cp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

function parseArgs(argv) {
  const parsed = {};
  for (const arg of argv) {
    if (!arg.startsWith("--")) {
      continue;
    }

    const [key, ...valueParts] = arg.slice(2).split("=");
    parsed[key] = valueParts.join("=") || "true";
  }
  return parsed;
}

const args = parseArgs(process.argv.slice(2));
const ownerScope = (args["owner-scope"] ?? "covas-labs").toLowerCase();
const packageVersion = args["package-version"];

if (!packageVersion) {
  throw new Error("Expected --package-version.");
}

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sourceDir = resolve(repoRoot, "packages", "electron-vr");
const sourcePackageJson = JSON.parse(await readFile(resolve(sourceDir, "package.json"), "utf8"));
const artifactRoot = resolve(repoRoot, "artifacts", "publish", "public", "package");
const packageName = `@${ownerScope}/electron-vr`;

await rm(artifactRoot, { force: true, recursive: true });
await mkdir(artifactRoot, { recursive: true });
await cp(resolve(sourceDir, "dist"), join(artifactRoot, "dist"), { recursive: true, force: true });
await cp(resolve(sourceDir, "README.md"), join(artifactRoot, "README.md"), { force: true });

const publishManifest = {
  ...sourcePackageJson,
  name: packageName,
  version: packageVersion,
  optionalDependencies: {
    [`@${ownerScope}/electron-vr-prebuilt-linux-x64`]: packageVersion,
    [`@${ownerScope}/electron-vr-prebuilt-win32-x64`]: packageVersion
  },
  publishConfig: {
    registry: "https://npm.pkg.github.com"
  }
};

delete publishManifest.scripts;

await writeFile(join(artifactRoot, "package.json"), `${JSON.stringify(publishManifest, null, 2)}\n`, "utf8");

console.log(`Prepared ${packageName} in ${artifactRoot}`);
