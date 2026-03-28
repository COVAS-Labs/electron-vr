import { access, copyFile, mkdir, readdir, rename, unlink } from "node:fs/promises";
import { constants } from "node:fs";
import { dirname, resolve } from "node:path";

import { getOpenVrSdkDir } from "./openvr-sdk.mjs";

function getDefaultSdkDir() {
  return getOpenVrSdkDir();
}

function getRuntimeLibraryName(platform) {
  if (platform === "win32") {
    return "openvr_api.dll";
  }
  if (platform === "linux") {
    return "libopenvr_api.so";
  }
  if (platform === "darwin") {
    return "libopenvr_api.dylib";
  }
  throw new Error(`Unsupported OpenVR runtime platform: ${platform}`);
}

function getRuntimeLibrarySourcePath(platform, arch, sdkDir = getDefaultSdkDir()) {
  if (platform === "win32") {
    const winArch = arch === "x64" ? "win64" : arch;
    return resolve(sdkDir, "bin", winArch, getRuntimeLibraryName(platform));
  }

  if (platform === "linux") {
    const linuxArch = arch === "x64" ? "linux64" : arch;
    return resolve(sdkDir, "lib", linuxArch, getRuntimeLibraryName(platform));
  }

  if (platform === "darwin") {
    return resolve(sdkDir, "bin", "osx32", getRuntimeLibraryName(platform));
  }

  throw new Error(`Unsupported OpenVR runtime platform: ${platform}`);
}

export async function copyOpenVRRuntimeLibrary({
  destinationDirectory,
  platform = process.platform,
  arch = process.arch,
  sdkDir = getDefaultSdkDir()
}) {
  const sourcePath = getRuntimeLibrarySourcePath(platform, arch, sdkDir);
  const fileName = getRuntimeLibraryName(platform);
  const destinationPath = resolve(destinationDirectory, fileName);

  await access(sourcePath, constants.R_OK);
  await mkdir(dirname(destinationPath), { recursive: true });
  await copyFile(sourcePath, destinationPath);

  return {
    sourcePath,
    destinationPath,
    fileName
  };
}

export async function prepareOpenVRRuntimeLibraryDestination({
  destinationDirectory,
  platform = process.platform,
  relocationDirectory = resolve(destinationDirectory, "..", "..", ".tmp", "openvr-runtime")
}) {
  const fileName = getRuntimeLibraryName(platform);
  const baseName = fileName.replace(/\.[^.]+$/, "");

  let entries = [];
  try {
    entries = await readdir(destinationDirectory, { withFileTypes: true });
  } catch {
    return { destinationPath: resolve(destinationDirectory, fileName), relocatedPath: null, fileName };
  }

  const candidates = entries
    .filter((entry) => entry.isFile())
    .map((entry) => entry.name)
    .filter((name) =>
      name === fileName ||
      name.startsWith(`${fileName}.`) ||
      name.startsWith(`${baseName}.`)
    );

  if (candidates.length === 0) {
    return { destinationPath: resolve(destinationDirectory, fileName), relocatedPath: null, fileName };
  }

  await mkdir(relocationDirectory, { recursive: true });

  let lastRelocatedPath = null;
  for (const candidate of candidates) {
    const sourcePath = resolve(destinationDirectory, candidate);
    const relocatedPath = resolve(relocationDirectory, `${candidate}.stale-${Date.now()}`);

    try {
      await rename(sourcePath, relocatedPath);
    } catch (error) {
      throw new Error(
        `Failed to move ${candidate} out of ${destinationDirectory}. Close any running Electron/demo process and retry. ${error instanceof Error ? error.message : String(error)}`
      );
    }

    try {
      await unlink(relocatedPath);
    } catch {
      // Ignore best-effort cleanup. Keeping stale files outside the build tree is enough for node-gyp clean.
    }

    lastRelocatedPath = relocatedPath;
  }

  return {
    destinationPath: resolve(destinationDirectory, fileName),
    relocatedPath: lastRelocatedPath,
    fileName
  };
}

export function getOpenVRRuntimeLibraryName(platform = process.platform) {
  return getRuntimeLibraryName(platform);
}

export function getOpenVRRuntimeLibrarySourcePath(platform = process.platform, arch = process.arch, sdkDir = getDefaultSdkDir()) {
  return getRuntimeLibrarySourcePath(platform, arch, sdkDir);
}
