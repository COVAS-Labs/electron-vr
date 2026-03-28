import { rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");

await Promise.all([
  rm(resolve(repoRoot, "artifacts"), { force: true, recursive: true }),
  rm(resolve(repoRoot, "packages", "electron-vr", "dist"), { force: true, recursive: true }),
  rm(resolve(repoRoot, "packages", "native-addon", "build"), { force: true, recursive: true }),
  rm(resolve(repoRoot, "apps", "demo-electron", "dist"), { force: true, recursive: true })
]);
