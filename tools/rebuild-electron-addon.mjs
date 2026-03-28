import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";

const require = createRequire(import.meta.url);
const nodeGypEntrypoint = require.resolve("node-gyp/bin/node-gyp.js");
const electronPackage = require("electron/package.json");
const electronVersion = String(electronPackage.version).replace(/^[^\d]*/, "");

const result = spawnSync(
  process.execPath,
  [
    nodeGypEntrypoint,
    "rebuild",
    `--target=${electronVersion}`,
    "--dist-url=https://electronjs.org/headers",
    `--arch=${process.arch}`
  ],
  {
    cwd: process.cwd(),
    stdio: "inherit"
  }
);

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
