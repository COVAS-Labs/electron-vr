import { constants } from "node:fs";
import { access, mkdir } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const OPENVR_REPOSITORY_URL = "https://github.com/ValveSoftware/openvr.git";

function getPlatformLibraryDirectory(platform, arch) {
  if (platform === "win32") {
    return arch === "x64" ? "win64" : arch;
  }

  if (platform === "linux") {
    return arch === "x64" ? "linux64" : arch;
  }

  if (platform === "darwin") {
    return "osx32";
  }

  throw new Error(`Unsupported OpenVR SDK platform: ${platform}`);
}

function getRequiredSdkPaths(platform, arch, sdkDir) {
  const libraryDirectory = getPlatformLibraryDirectory(platform, arch);
  const requiredPaths = [resolve(sdkDir, "headers", "openvr.h")];

  if (platform === "win32") {
    requiredPaths.push(resolve(sdkDir, "lib", libraryDirectory, "openvr_api.lib"));
    requiredPaths.push(resolve(sdkDir, "bin", libraryDirectory, "openvr_api.dll"));
    return requiredPaths;
  }

  if (platform === "linux") {
    requiredPaths.push(resolve(sdkDir, "lib", libraryDirectory, "libopenvr_api.so"));
    return requiredPaths;
  }

  if (platform === "darwin") {
    requiredPaths.push(resolve(sdkDir, "bin", libraryDirectory, "libopenvr_api.dylib"));
    return requiredPaths;
  }

  return requiredPaths;
}

async function pathExists(path) {
  try {
    await access(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

async function getMissingRequiredPaths(platform, arch, sdkDir) {
  const requiredPaths = getRequiredSdkPaths(platform, arch, sdkDir);
  const checks = await Promise.all(requiredPaths.map(async (requiredPath) => ({
    requiredPath,
    exists: await pathExists(requiredPath)
  })));

  return checks.filter((entry) => !entry.exists).map((entry) => entry.requiredPath);
}

export function getOpenVrSdkDir() {
  return process.env.OPENVR_SDK_DIR || resolve(repoRoot, ".openvr-sdk");
}

export async function ensureOpenVrSdk({
  platform = process.platform,
  arch = process.arch,
  sdkDir = getOpenVrSdkDir()
} = {}) {
  const missingBeforeClone = await getMissingRequiredPaths(platform, arch, sdkDir);
  if (missingBeforeClone.length === 0) {
    return { sdkDir, fetched: false };
  }

  if (process.env.OPENVR_SDK_DIR) {
    throw new Error(
      `OPENVR_SDK_DIR points to an incomplete OpenVR SDK at ${sdkDir}. Missing: ${missingBeforeClone.join(", ")}`
    );
  }

  if (await pathExists(sdkDir)) {
    throw new Error(
      `Expected a complete OpenVR SDK at ${sdkDir}, but required files are missing. Delete that directory or set OPENVR_SDK_DIR to a valid SDK checkout.`
    );
  }

  await mkdir(dirname(sdkDir), { recursive: true });
  console.log(`OpenVR SDK not found. Cloning ${OPENVR_REPOSITORY_URL} into ${sdkDir}...`);

  const cloneResult = spawnSync("git", ["clone", "--depth=1", OPENVR_REPOSITORY_URL, sdkDir], {
    cwd: repoRoot,
    stdio: "inherit"
  });

  if (cloneResult.status !== 0) {
    throw new Error(`Failed to clone the OpenVR SDK into ${sdkDir}.`);
  }

  const missingAfterClone = await getMissingRequiredPaths(platform, arch, sdkDir);
  if (missingAfterClone.length > 0) {
    throw new Error(`The cloned OpenVR SDK is missing required files: ${missingAfterClone.join(", ")}`);
  }

  return { sdkDir, fetched: true };
}
