import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "..");
const nodeGypBin = resolve(
  projectRoot,
  "node_modules",
  ".bin",
  process.platform === "win32" ? "node-gyp.cmd" : "node-gyp"
);
const electronPackage = require("electron/package.json");
const electronVersion = String(electronPackage.version).replace(/^[^\d]*/, "");

const result = spawnSync(
  nodeGypBin,
  [
    "rebuild",
    `--target=${electronVersion}`,
    "--dist-url=https://electronjs.org/headers",
    `--arch=${process.arch}`
  ],
  {
    cwd: projectRoot,
    stdio: "inherit"
  }
);

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
