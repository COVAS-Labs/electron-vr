import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { resolve } from "node:path";

import { prepareBuildReleaseDirectory } from "./build-output.mjs";
import { copyOpenVRRuntimeLibrary, prepareOpenVRRuntimeLibraryDestination } from "./openvr-runtime.mjs";
import { ensureOpenVrSdk } from "./openvr-sdk.mjs";

const require = createRequire(import.meta.url);
const nodeGypEntrypoint = require.resolve("node-gyp/bin/node-gyp.js");
const { sdkDir } = await ensureOpenVrSdk();
const releaseDirectory = resolve(process.cwd(), "build", "Release");
const relocationDirectory = resolve(process.cwd(), ".tmp", "openvr-runtime");

await prepareBuildReleaseDirectory({
  releaseDirectory
});

await prepareOpenVRRuntimeLibraryDestination({
  destinationDirectory: releaseDirectory,
  relocationDirectory
});

const result = spawnSync(process.execPath, [nodeGypEntrypoint, "rebuild"], {
  cwd: process.cwd(),
  stdio: "inherit",
  env: {
    ...process.env,
    OPENVR_SDK_DIR: sdkDir
  }
});

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}

await copyOpenVRRuntimeLibrary({
  destinationDirectory: releaseDirectory,
  sdkDir
});
