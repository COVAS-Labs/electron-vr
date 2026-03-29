import { constants } from "node:fs";
import { access, mkdir } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const OPENXR_REPOSITORY_URL = "https://github.com/KhronosGroup/OpenXR-SDK.git";

function getRequiredSdkPaths(sdkDir) {
  return [
    resolve(sdkDir, "include", "openxr", "openxr.h"),
    resolve(sdkDir, "include", "openxr", "openxr_platform.h")
  ];
}

async function pathExists(path) {
  try {
    await access(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

async function getMissingRequiredPaths(sdkDir) {
  const requiredPaths = getRequiredSdkPaths(sdkDir);
  const checks = await Promise.all(requiredPaths.map(async (requiredPath) => ({
    requiredPath,
    exists: await pathExists(requiredPath)
  })));

  return checks.filter((entry) => !entry.exists).map((entry) => entry.requiredPath);
}

export function getOpenXrSdkDir() {
  return process.env.OPENXR_SDK_DIR || resolve(repoRoot, ".openxr-sdk");
}

export async function ensureOpenXrSdk({
  sdkDir = getOpenXrSdkDir()
} = {}) {
  const missingBeforeClone = await getMissingRequiredPaths(sdkDir);
  if (missingBeforeClone.length === 0) {
    return { sdkDir, fetched: false };
  }

  if (process.env.OPENXR_SDK_DIR) {
    throw new Error(
      `OPENXR_SDK_DIR points to an incomplete OpenXR SDK at ${sdkDir}. Missing: ${missingBeforeClone.join(", ")}`
    );
  }

  if (await pathExists(sdkDir)) {
    throw new Error(
      `Expected a complete OpenXR SDK at ${sdkDir}, but required files are missing. Delete that directory or set OPENXR_SDK_DIR to a valid SDK checkout.`
    );
  }

  await mkdir(dirname(sdkDir), { recursive: true });
  console.log(`OpenXR SDK not found. Cloning ${OPENXR_REPOSITORY_URL} into ${sdkDir}...`);

  const cloneResult = spawnSync("git", ["clone", "--depth=1", OPENXR_REPOSITORY_URL, sdkDir], {
    cwd: repoRoot,
    stdio: "inherit"
  });

  if (cloneResult.status !== 0) {
    throw new Error(`Failed to clone the OpenXR SDK into ${sdkDir}.`);
  }

  const missingAfterClone = await getMissingRequiredPaths(sdkDir);
  if (missingAfterClone.length > 0) {
    throw new Error(`The cloned OpenXR SDK is missing required files: ${missingAfterClone.join(", ")}`);
  }

  return { sdkDir, fetched: true };
}
