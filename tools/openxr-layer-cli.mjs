import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

if (process.platform !== "win32" || process.arch !== "x64") {
  throw new Error("The OpenXR API-layer utility currently supports Windows x64 only.");
}

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
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
