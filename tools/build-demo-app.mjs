import { copyFile, mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const workspaceDir = process.cwd();
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const tscEntrypoint = resolve(repoRoot, "node_modules", "typescript", "lib", "tsc.js");

const result = spawnSync(process.execPath, [tscEntrypoint, "-p", resolve(workspaceDir, "tsconfig.json")], {
  cwd: workspaceDir,
  stdio: "inherit"
});

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}

const srcUiDir = resolve(workspaceDir, "src", "ui");
const distUiDir = resolve(workspaceDir, "dist", "ui");

await mkdir(distUiDir, { recursive: true });
await Promise.all([
  copyFile(resolve(srcUiDir, "index.html"), resolve(distUiDir, "index.html")),
  copyFile(resolve(srcUiDir, "index.css"), resolve(distUiDir, "index.css"))
]);
