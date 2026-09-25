import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { chmod, mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const assets = resolve(root, "packages", "native-addon", "build", "Release");

test("Windows registration repairs MSI read-only assets and skips unchanged DLLs", { skip: process.platform !== "win32" }, async () => {
  const require = createRequire(import.meta.url);
  const addon = require(join(assets, "vr_bridge.node"));
  const localAppData = await mkdtemp(join(tmpdir(), "electron-vr-registration-"));
  const previousLocalAppData = process.env.LOCALAPPDATA;
  process.env.LOCALAPPDATA = localAppData;
  const destination = join(localAppData, "ElectronVR", "OpenXR");

  try {
    assert.equal(addon.installOpenXRApiLayer(assets).enabled, true);
    const dll = join(destination, (await readFile(join(destination, "electron_vr_openxr_layer.json"), "utf8"))
      .match(/electron_vr_openxr_layer_[0-9a-f]+\.dll/)?.[0]);
    const protocol = join(destination, "protocol.json");

    await chmod(dll, 0o444);
    await chmod(protocol, 0o444);
    assert.equal(addon.installOpenXRApiLayer(assets).requiresUpdate, false);
    assert.notEqual((await stat(dll)).mode & 0o200, 0);
    assert.notEqual((await stat(protocol)).mode & 0o200, 0);

    await writeFile(protocol, "outdated protocol");
    await chmod(protocol, 0o444);
    assert.equal(addon.installOpenXRApiLayer(assets).requiresUpdate, false);
    assert.deepEqual(await readFile(protocol), await readFile(join(assets, "protocol.json")));
    assert.notEqual((await stat(protocol)).mode & 0o200, 0);
  } finally {
    try {
      addon.uninstallOpenXRApiLayer(assets);
    } finally {
      if (previousLocalAppData === undefined) delete process.env.LOCALAPPDATA;
      else process.env.LOCALAPPDATA = previousLocalAppData;
      await rm(localAppData, { recursive: true, force: true });
    }
  }
});
