import { copyFile, mkdir, readFile, rm, writeFile } from "node:fs/promises";
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
const addonSourcePath = resolve(repoRoot, "packages", "native-addon", "build", "Release", "vr_bridge.node");
const artifactRoot = resolve(repoRoot, "artifacts", "publish");
const platform = process.platform;
const arch = process.arch;
const packageName = `@${ownerScope}/electron-vr-prebuilt-${platform}-${arch}`;
const packageDir = join(artifactRoot, `prebuilt-${platform}-${arch}`, "package");

await rm(packageDir, { force: true, recursive: true });
await mkdir(packageDir, { recursive: true });
await copyFile(addonSourcePath, join(packageDir, "vr_bridge.node"));

const metadata = {
  packageName,
  packageVersion,
  runtime: "electron",
  platform,
  arch,
  backends: ["openxr", "openvr", "mock"]
};

await writeFile(join(packageDir, "index.js"), "module.exports = require('./vr_bridge.node');\n", "utf8");
await writeFile(join(packageDir, "metadata.json"), `${JSON.stringify(metadata, null, 2)}\n`, "utf8");
await writeFile(
  join(packageDir, "README.md"),
  `# ${packageName}\n\nInternal Electron prebuilt addon package for ${platform}-${arch}.\n`,
  "utf8"
);
await writeFile(
  join(packageDir, "package.json"),
  `${JSON.stringify({
    name: packageName,
    version: packageVersion,
    private: false,
    description: `Internal Electron prebuilt addon for ${platform}-${arch}.`,
    main: "index.js",
    os: [platform],
    cpu: [arch],
    files: ["index.js", "metadata.json", "README.md", "vr_bridge.node"],
    publishConfig: {
      registry: "https://npm.pkg.github.com"
    }
  }, null, 2)}\n`,
  "utf8"
);

console.log(`Prepared ${packageName} in ${packageDir}`);
