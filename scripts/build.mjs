import { cp, rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "..");
const tscEntrypoint = resolve(projectRoot, "node_modules", "typescript", "lib", "tsc.js");

const tscResult = spawnSync(process.execPath, [tscEntrypoint, "-p", resolve(projectRoot, "tsconfig.json")], {
  cwd: projectRoot,
  stdio: "inherit"
});

if (tscResult.status !== 0) {
  process.exit(tscResult.status ?? 1);
}

const srcUiDir = resolve(projectRoot, "src", "electron", "ui");
const distUiDir = resolve(projectRoot, "dist", "electron", "ui");

await rm(distUiDir, { force: true, recursive: true });
await cp(srcUiDir, distUiDir, { recursive: true, force: true });
