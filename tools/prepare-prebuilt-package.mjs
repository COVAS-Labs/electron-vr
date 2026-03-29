import { copyFile, mkdir, rm, writeFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { copyBundledOpenXRRuntimeLibraries } from "./openxr-runtime.mjs";
import { copyOpenVRRuntimeLibrary, getOpenVRRuntimeLibraryName } from "./openvr-runtime.mjs";

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
const runtimeLibrary = await copyOpenVRRuntimeLibrary({
  destinationDirectory: packageDir,
  platform,
  arch
});
const bundledOpenxrLibraries = await copyBundledOpenXRRuntimeLibraries({
  destinationDirectory: packageDir,
  platform
});

const runtimeLibraryName = getOpenVRRuntimeLibraryName(platform);
const bundledRuntimeLibraries = [runtimeLibraryName, ...bundledOpenxrLibraries.map((library) => library.fileName)];

const metadata = {
  packageName,
  packageVersion,
  runtime: "electron",
  platform,
  arch,
  backends: ["openxr", "openvr", "mock"],
  bundledRuntimeLibraries
};

await writeFile(
  join(packageDir, "index.js"),
  `const path = require("node:path");
const packageDir = __dirname;
if (process.platform === "win32") {
  process.env.PATH = process.env.PATH ? packageDir + ";" + process.env.PATH : packageDir;
} else if (process.platform === "linux") {
  const currentLdLibraryPath = process.env.LD_LIBRARY_PATH || "";
  process.env.LD_LIBRARY_PATH = currentLdLibraryPath ? packageDir + ":" + currentLdLibraryPath : packageDir;
}
module.exports = require(path.join(packageDir, "vr_bridge.node"));
`,
  "utf8"
);
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
    files: ["index.js", "metadata.json", "README.md", "vr_bridge.node", ...bundledRuntimeLibraries],
    publishConfig: {
      registry: "https://npm.pkg.github.com"
    }
  }, null, 2)}\n`,
  "utf8"
);

console.log(`Prepared ${packageName} in ${packageDir} with ${bundledRuntimeLibraries.join(", ")}`);
