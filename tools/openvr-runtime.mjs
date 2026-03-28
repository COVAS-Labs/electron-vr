import { access, copyFile, mkdir } from "node:fs/promises";
import { constants } from "node:fs";
import { dirname, resolve } from "node:path";

function getDefaultSdkDir() {
  return process.env.OPENVR_SDK_DIR || "/home/luca/Dokumente/openvr";
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

export function getOpenVRRuntimeLibraryName(platform = process.platform) {
  return getRuntimeLibraryName(platform);
}

export function getOpenVRRuntimeLibrarySourcePath(platform = process.platform, arch = process.arch, sdkDir = getDefaultSdkDir()) {
  return getRuntimeLibrarySourcePath(platform, arch, sdkDir);
}
