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
const runtime = args.runtime;

if (runtime !== "node" && runtime !== "electron") {
  throw new Error("Expected --runtime=node or --runtime=electron.");
}

const ownerScope = (args["owner-scope"] ?? "covas-labs").toLowerCase();
const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "..");
const packageJson = JSON.parse(await readFile(resolve(projectRoot, "package.json"), "utf8"));
const addonSourcePath = resolve(projectRoot, "build", "Release", "vr_bridge.node");
const artifactRoot = resolve(projectRoot, "artifacts", "prebuilds");
const platform = process.platform;
const arch = process.arch;

let runtimeVersion;
if (runtime === "node") {
  runtimeVersion = process.versions.node;
} else {
  const electronPackageJson = JSON.parse(
    await readFile(resolve(projectRoot, "node_modules", "electron", "package.json"), "utf8")
  );
  runtimeVersion = electronPackageJson.version;
}

const publishVersion = args["package-version"] ?? packageJson.version;
const artifactName = `vr_bridge-${runtime}-v${runtimeVersion}-${platform}-${arch}-all-backends`;
const artifactDir = join(artifactRoot, artifactName);
const packageDir = join(artifactDir, "package");
const packageName = `@${ownerScope}/vr-bridge-prebuilt-${runtime}-${platform}-${arch}`;
const packageReadme = [
  `# ${packageName}`,
  "",
  `Prebuilt ${runtime} addon package for ${platform}-${arch}.`,
  "",
  "## Included backends",
  "",
  "- openxr",
  "- openvr",
  "- mock"
].join("\n");

await rm(artifactDir, { force: true, recursive: true });
await mkdir(packageDir, { recursive: true });

await copyFile(addonSourcePath, join(packageDir, "vr_bridge.node"));

const metadata = {
  moduleName: "vr_bridge",
  packageName,
  sourcePackageName: packageJson.name,
  sourcePackageVersion: packageJson.version,
  packageVersion: publishVersion,
  runtime,
  runtimeVersion,
  platform,
  arch,
  builtAt: new Date().toISOString(),
  backends: ["openxr", "openvr", "mock"],
  artifactName
};

const packageManifest = {
  name: packageName,
  version: publishVersion,
  description: `Prebuilt ${runtime} VR bridge addon for ${platform}-${arch}.`,
  main: "index.js",
  os: [platform],
  cpu: [arch],
  files: ["index.js", "metadata.json", "README.md", "vr_bridge.node"],
  publishConfig: {
    registry: "https://npm.pkg.github.com"
  },
  keywords: ["electron", "node-gyp", "napi", "openxr", "openvr", runtime, platform, arch]
};

await writeFile(join(packageDir, "index.js"), "module.exports = require('./vr_bridge.node');\n", "utf8");
await writeFile(join(packageDir, "metadata.json"), `${JSON.stringify(metadata, null, 2)}\n`, "utf8");
await writeFile(join(packageDir, "package.json"), `${JSON.stringify(packageManifest, null, 2)}\n`, "utf8");
await writeFile(join(packageDir, "README.md"), `${packageReadme}\n`, "utf8");

console.log(`Prepared prebuilt artifact in ${artifactDir}`);
console.log(`PACKAGE_DIR=${packageDir}`);
