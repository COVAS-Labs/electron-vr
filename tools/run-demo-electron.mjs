import { createRequire } from "node:module";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";

const require = createRequire(import.meta.url);
const electronBinary = require("electron");
const demoDir = process.cwd();
const result = spawnSync(electronBinary, [resolve(demoDir), ...process.argv.slice(2)], {
  cwd: demoDir,
  stdio: "inherit"
});

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
