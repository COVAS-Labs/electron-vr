import { constants } from "node:fs";
import { access, mkdir, rename, rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";

async function pathExists(path) {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
}

export async function prepareBuildReleaseDirectory({
  releaseDirectory = resolve(process.cwd(), "build", "Release"),
  relocationDirectory = resolve(process.cwd(), ".tmp", "build-release")
} = {}) {
  if (!(await pathExists(releaseDirectory))) {
    return { releaseDirectory, relocatedPath: null };
  }

  await mkdir(relocationDirectory, { recursive: true });
  const relocatedPath = resolve(relocationDirectory, `Release.stale-${Date.now()}`);

  try {
    await rename(releaseDirectory, relocatedPath);
  } catch (error) {
    throw new Error(
      `Failed to move ${releaseDirectory} out of the way before rebuild. Close any running Electron/demo process and retry. ${error instanceof Error ? error.message : String(error)}`
    );
  }

  try {
    await rm(relocatedPath, { recursive: true, force: true });
  } catch {
    // Best-effort cleanup only. The important part is keeping stale outputs out of build/Release.
  }

  await mkdir(dirname(releaseDirectory), { recursive: true });
  return { releaseDirectory, relocatedPath };
}
