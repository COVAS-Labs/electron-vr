import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readFileSync, renameSync, rmSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
if (process.platform === "linux" && process.arch === "x64") {
  const command = process.argv[2];
  const dataHome = process.env.XDG_DATA_HOME ?? resolve(process.env.HOME ?? "", ".local", "share");
  const installDirectory = resolve(dataHome, "openxr", "1", "api_layers", "implicit.d");
  const manifest = resolve(installDirectory, "electron_vr_openxr_layer.json");
  const disabledManifest = `${manifest}.disabled`;
  const library = resolve(installDirectory, "libelectron_vr_openxr_layer.so");
  const sourceDirectory = resolve(repoRoot, "packages", "native-addon", "build", "Release");
  if (command === "install") {
    mkdirSync(installDirectory, { recursive: true, mode: 0o700 });
    copyFileSync(resolve(sourceDirectory, "libelectron_vr_openxr_layer.so"), library);
    copyFileSync(resolve(sourceDirectory, "electron_vr_openxr_layer_linux.json"), manifest);
    rmSync(disabledManifest, { force: true });
    console.log(`Installed and enabled: ${manifest}`);
  } else if (command === "enable") {
    if (existsSync(disabledManifest)) renameSync(disabledManifest, manifest);
    console.log("Enabled.");
  } else if (command === "disable") {
    if (existsSync(manifest)) renameSync(manifest, disabledManifest);
    console.log("Disabled.");
  } else if (command === "uninstall") {
    rmSync(manifest, { force: true });
    rmSync(disabledManifest, { force: true });
    rmSync(library, { force: true });
    console.log("Uninstalled.");
  } else if (command === "status") {
    const sourceLibrary = resolve(sourceDirectory, "libelectron_vr_openxr_layer.so");
    const sourceManifest = resolve(sourceDirectory, "electron_vr_openxr_layer_linux.json");
    const installed = existsSync(library) && (existsSync(manifest) || existsSync(disabledManifest));
    const activeManifest = existsSync(manifest) ? manifest : disabledManifest;
    const current = installed && readFileSync(sourceLibrary).equals(readFileSync(library)) &&
      readFileSync(sourceManifest).equals(readFileSync(activeManifest));
    console.log(`installed=${installed}`);
    console.log(`enabled=${installed && existsSync(manifest)}`);
    console.log(`requires_update=${installed && !current}`);
    console.log(`manifest=${manifest}`);
  } else {
    throw new Error("Usage: npm run openxr-layer -- <install|enable|disable|status|uninstall>");
  }
  process.exit(0);
}

if (process.platform !== "win32" || process.arch !== "x64") {
  throw new Error("The OpenXR API-layer utility currently supports Linux x64 and Windows x64.");
}
const executable = resolve(
  repoRoot,
  "packages",
  "native-addon",
  "build",
  "Release",
  "electron_vr_openxr_layer_cli.exe"
);
const result = spawnSync(executable, process.argv.slice(2), { stdio: "inherit" });
if (result.error) throw result.error;
process.exitCode = result.status ?? 1;
