import { access, copyFile, mkdir } from "node:fs/promises";
import { constants } from "node:fs";
import { dirname, resolve } from "node:path";

const LINUX_LIBRARY_CANDIDATES = {
  "libopenxr_loader.so.1": [
    "/lib/x86_64-linux-gnu/libopenxr_loader.so.1",
    "/usr/lib/x86_64-linux-gnu/libopenxr_loader.so.1"
  ],
  "libjsoncpp.so.25": [
    "/lib/x86_64-linux-gnu/libjsoncpp.so.25",
    "/usr/lib/x86_64-linux-gnu/libjsoncpp.so.25"
  ]
};

async function resolveReadablePath(candidates) {
  for (const candidate of candidates) {
    try {
      await access(candidate, constants.R_OK);
      return candidate;
    } catch {
      // Try next candidate.
    }
  }

  throw new Error(`Could not find a readable library in: ${candidates.join(", ")}`);
}

export async function copyBundledOpenXRRuntimeLibraries({
  destinationDirectory,
  platform = process.platform
}) {
  if (platform !== "linux") {
    return [];
  }

  const copiedLibraries = [];
  for (const [fileName, candidates] of Object.entries(LINUX_LIBRARY_CANDIDATES)) {
    const sourcePath = await resolveReadablePath(candidates);
    const destinationPath = resolve(destinationDirectory, fileName);

    await mkdir(dirname(destinationPath), { recursive: true });
    await copyFile(sourcePath, destinationPath);

    copiedLibraries.push({
      fileName,
      sourcePath,
      destinationPath
    });
  }

  return copiedLibraries;
}
