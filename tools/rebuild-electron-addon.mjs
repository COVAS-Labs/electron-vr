import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { resolve } from "node:path";

import { prepareBuildReleaseDirectory } from "./build-output.mjs";
import { ensureOpenXrSdk } from "./openxr-sdk.mjs";
import { copyOpenVRRuntimeLibrary, prepareOpenVRRuntimeLibraryDestination } from "./openvr-runtime.mjs";
import { ensureOpenVrSdk } from "./openvr-sdk.mjs";

const require = createRequire(import.meta.url);
const nodeGypEntrypoint = require.resolve("node-gyp/bin/node-gyp.js");
const electronPackage = require("electron/package.json");
const electronVersion = String(electronPackage.version).replace(/^[^\d]*/, "");
const { sdkDir } = await ensureOpenVrSdk();
const openxrSdk = process.platform === "win32" ? await ensureOpenXrSdk() : null;
const releaseDirectory = resolve(process.cwd(), "build", "Release");
const relocationDirectory = resolve(process.cwd(), ".tmp", "openvr-runtime");

await prepareBuildReleaseDirectory({
  releaseDirectory
});

await prepareOpenVRRuntimeLibraryDestination({
  destinationDirectory: releaseDirectory,
  relocationDirectory
});

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
    stdio: "inherit",
    env: {
      ...process.env,
      OPENVR_SDK_DIR: sdkDir,
      ...(openxrSdk ? { OPENXR_SDK_DIR: openxrSdk.sdkDir } : {})
    }
  }
);

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}

await copyOpenVRRuntimeLibrary({
  destinationDirectory: releaseDirectory,
  sdkDir
});
