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

const artifactName = `vr_bridge-${runtime}-v${runtimeVersion}-${platform}-${arch}-all-backends`;
const artifactDir = join(artifactRoot, artifactName);

await rm(artifactDir, { force: true, recursive: true });
await mkdir(artifactDir, { recursive: true });

await copyFile(addonSourcePath, join(artifactDir, "vr_bridge.node"));

const metadata = {
  moduleName: "vr_bridge",
  packageName: packageJson.name,
  packageVersion: packageJson.version,
  runtime,
  runtimeVersion,
  platform,
  arch,
  builtAt: new Date().toISOString(),
  backends: ["openxr", "openvr", "mock"],
  artifactName
};

await writeFile(join(artifactDir, "metadata.json"), `${JSON.stringify(metadata, null, 2)}\n`, "utf8");

console.log(`Prepared prebuilt artifact in ${artifactDir}`);
