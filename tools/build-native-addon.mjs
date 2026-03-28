import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";

const require = createRequire(import.meta.url);
const nodeGypEntrypoint = require.resolve("node-gyp/bin/node-gyp.js");

const result = spawnSync(process.execPath, [nodeGypEntrypoint, "rebuild"], {
  cwd: process.cwd(),
  stdio: "inherit"
});

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
