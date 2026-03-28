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
