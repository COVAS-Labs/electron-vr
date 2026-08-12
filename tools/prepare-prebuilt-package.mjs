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
const registry = args.registry ?? "https://npm.pkg.github.com";
const access = args.access;

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
const windowsLayerAssets = [
  "electron_vr_openxr_layer.dll",
  "electron_vr_openxr_layer_cli.exe",
  "electron_vr_openxr_layer.json",
  "protocol.json"
];

await rm(packageDir, { force: true, recursive: true });
await mkdir(packageDir, { recursive: true });
await copyFile(addonSourcePath, join(packageDir, "vr_bridge.node"));
if (platform === "win32" && arch === "x64") {
  for (const asset of windowsLayerAssets) {
    await copyFile(resolve(repoRoot, "packages", "native-addon", "build", "Release", asset), join(packageDir, asset));
  }
}
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
  bundledRuntimeLibraries,
  ...(platform === "win32" && arch === "x64" ? {
    openxrApiLayer: {
      manifest: "electron_vr_openxr_layer.json",
      library: "electron_vr_openxr_layer.dll",
      cli: "electron_vr_openxr_layer_cli.exe",
      protocolVersion: 1
    }
  } : {})
};

if (platform === "win32" && arch === "x64") {
  await writeFile(
    join(packageDir, "openxr-layer-cli.js"),
    `#!/usr/bin/env node
const { spawnSync } = require("node:child_process");
const { join } = require("node:path");
const result = spawnSync(join(__dirname, "electron_vr_openxr_layer_cli.exe"), process.argv.slice(2), { stdio: "inherit" });
if (result.error) throw result.error;
process.exitCode = result.status == null ? 1 : result.status;
`,
    "utf8"
  );
}

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
    repository: {
      type: "git",
      url: "git+https://github.com/COVAS-Labs/electron-vr.git"
    },
    main: "index.js",
    os: [platform],
    cpu: [arch],
    files: ["index.js", "metadata.json", "README.md", "vr_bridge.node", ...bundledRuntimeLibraries,
      ...(platform === "win32" && arch === "x64" ? [...windowsLayerAssets, "openxr-layer-cli.js"] : [])],
    ...(platform === "win32" && arch === "x64" ? { bin: { "electron-vr-openxr-layer": "openxr-layer-cli.js" } } : {}),
     publishConfig: {
       registry,
       ...(access ? { access } : {})
     }
   }, null, 2)}\n`,
  "utf8"
);

console.log(`Prepared ${packageName} in ${packageDir} with ${bundledRuntimeLibraries.join(", ")}`);
