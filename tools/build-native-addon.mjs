import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { resolve } from "node:path";

import { prepareBuildReleaseDirectory } from "./build-output.mjs";
import { ensureOpenXrSdk } from "./openxr-sdk.mjs";
import { copyOpenVRRuntimeLibrary, prepareOpenVRRuntimeLibraryDestination } from "./openvr-runtime.mjs";
import { ensureOpenVrSdk } from "./openvr-sdk.mjs";

const require = createRequire(import.meta.url);
const nodeGypEntrypoint = require.resolve("node-gyp/bin/node-gyp.js");
const { sdkDir } = await ensureOpenVrSdk();
const openxrSdk = process.platform === "win32" ? await ensureOpenXrSdk() : null;
const releaseDirectory = resolve(process.cwd(), "build", "Release");
const relocationDirectory = resolve(process.cwd(), ".tmp", "openvr-runtime");
const builtAddonPath = resolve(releaseDirectory, "vr_bridge.node");

await prepareBuildReleaseDirectory({
  releaseDirectory
});

await prepareOpenVRRuntimeLibraryDestination({
  destinationDirectory: releaseDirectory,
  relocationDirectory
});

const result = spawnSync(process.execPath, [nodeGypEntrypoint, "rebuild"], {
  cwd: process.cwd(),
  encoding: "utf8",
    env: {
      ...process.env,
      OPENVR_SDK_DIR: sdkDir,
      ...(openxrSdk ? { OPENXR_SDK_DIR: openxrSdk.sdkDir } : {})
    }
  });

if (result.stdout) {
  process.stdout.write(result.stdout);
}
if (result.stderr) {
  process.stderr.write(result.stderr);
}

if (result.status !== 0) {
  const combinedOutput = `${result.stdout ?? ""}${result.stderr ?? ""}`;
  const hitNodeGypCleanupBug = combinedOutput.includes("node_gyp_bins") && combinedOutput.includes("ENOENT");
  if (!hitNodeGypCleanupBug || !existsSync(builtAddonPath)) {
    process.exit(result.status ?? 1);
  }
}

await copyOpenVRRuntimeLibrary({
  destinationDirectory: releaseDirectory,
  sdkDir
});
