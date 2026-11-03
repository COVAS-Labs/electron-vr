import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import vm from "node:vm";
import ts from "typescript";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const require = createRequire(import.meta.url);
const source = await readFile(path.join(root, "packages/electron-vr/src/bridge.ts"), "utf8");
const compiled = ts.transpileModule(source.replaceAll("import.meta.url", JSON.stringify(import.meta.url)), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
}).outputText;

function fixture(platform, info = {}, submission = () => true) {
  const calls = { shared: [], software: [], readback: 0, bitmap: 0, errors: [], released: 0 };
  const runtime = { platform, selectedBackend: "openvr", openxrMode: "none", ...info };
  const addon = {
    getRuntimeInfo: () => runtime,
    initializeVR: () => true,
    isInitialized: () => true,
    shutdownVR: () => {},
    getLastError: () => "injected shared-import failure",
    submitSharedTexture(value) { calls.shared.push(value); return submission(runtime); },
    submitSoftwareFrame(value) { calls.software.push(value); return true; }
  };
  const addonRequire = () => addon;
  addonRequire.resolve = () => path.join(root, "fixture-prebuilt/index.js");
  const exports = {};
  vm.runInNewContext(compiled, {
    exports, module: { exports }, Buffer,
    process: { platform, arch: "x64", env: {}, cwd: () => root },
    console: { log() {}, warn() {}, error: (...args) => calls.errors.push(args) },
    setInterval: () => 1, clearInterval() {},
    require(name) {
      if (name === "node:module") return { createRequire: () => addonRequire };
      if (name === "node:fs") return { existsSync: () => false };
      return require(name);
    }
  });
  const contents = new EventEmitter();
  contents.setFrameRate = () => {};
  contents.invalidate = () => {};
  contents.capturePage = () => { calls.readback++; throw new Error("Unexpected pixel readback"); };
  const window = { webContents: contents, isDestroyed: () => false, getContentSize: () => [64, 32] };
  const bridge = new exports.VrBridge();
  bridge.attachWindow(window);
  const bitmap = { getSize: () => ({ width: 64, height: 32 }), toBitmap() { calls.bitmap++; return Buffer.alloc(64 * 32 * 4); } };
  const texture = (metadata) => ({ textureInfo: metadata, release() { calls.released++; } });
  const paint = (payload, image = bitmap) => contents.emit("paint", { texture: payload }, {}, image);
  return { bridge, addon, calls, runtime, paint, texture };
}

for (const platform of ["win32", "linux"]) {
  test(`${platform}: failed GPU submission is reported without bitmap upload or readback`, () => {
    const f = fixture(platform, {}, () => false);
    const metadata = platform === "win32" ? { sharedTextureHandle: Buffer.alloc(8, 1) }
      : { codedSize: { width: 64, height: 32 }, pixelFormat: "bgra", planes: [{ fd: 12, stride: 256, offset: 0, size: 8192 }] };
    f.paint(f.texture(metadata));
    assert.equal(f.calls.shared.length, 1);
    assert.equal(f.calls.software.length, 0);
    assert.equal(f.calls.readback, 0);
    assert.equal(f.calls.bitmap, 0);
    assert.equal(f.calls.released, 1);
    assert.equal(f.calls.errors.length, 1);
    assert.match(String(f.calls.errors[0]), /injected shared-import failure/);
    f.bridge.shutdown();
  });

  test(`${platform}: bitmap-only paint cannot silently replace a shared-texture frame`, () => {
    const f = fixture(platform);
    f.paint(undefined);
    assert.equal(f.calls.shared.length, 0);
    assert.equal(f.calls.software.length, 0);
    assert.equal(f.calls.readback, 0);
    assert.equal(f.calls.bitmap, 0);
    assert.equal(f.calls.errors.length, 1);
    f.bridge.shutdown();
  });

  test(`${platform}: invalid shared metadata still releases the paint lease`, () => {
    const f = fixture(platform);
    f.paint(f.texture({}));
    assert.equal(f.calls.shared.length, 0);
    assert.equal(f.calls.software.length, 0);
    assert.equal(f.calls.readback, 0);
    assert.equal(f.calls.released, 1);
    assert.equal(f.calls.errors.length, 1);
    f.bridge.shutdown();
  });

  test(`${platform}: empty startup paint does not suppress a later real transport error`, () => {
    const f = fixture(platform);
    f.paint(undefined, { getSize: () => ({ width: 0, height: 0 }), toBitmap() { throw new Error("No bitmap exists"); } });
    assert.equal(f.calls.errors.length, 0);
    f.paint(undefined);
    assert.equal(f.calls.errors.length, 1);
    assert.equal(f.calls.software.length, 0);
    assert.equal(f.calls.readback, 0);
    f.bridge.shutdown();
  });
}

for (const api of ["opengl-xlib", "vulkan"]) {
  test(`Linux ${api} API layer uses the shared DMA-BUF, never a bitmap replacement`, () => {
    const f = fixture("linux", { selectedBackend: "openxr", openxrMode: "api-layer",
      openxrCompanionConnected: true, openxrHostGraphicsApi: api });
    const metadata = { codedSize: { width: 64, height: 32 }, pixelFormat: "bgra", modifier: "0",
      planes: [{ fd: 12, stride: 256, offset: 0, size: 8192 }] };
    f.paint(f.texture(metadata));
    assert.equal(f.calls.shared.length, 1);
    assert.notEqual(f.calls.shared[0], metadata);
    assert.notEqual(f.calls.shared[0].planes[0], metadata.planes[0]);
    assert.equal(f.calls.shared[0].planes[0].fd, 12);
    assert.equal(f.calls.software.length, 0);
    assert.equal(f.calls.readback, 0);
    assert.equal(f.calls.released, 1);
    f.paint(undefined);
    assert.equal(f.calls.software.length, 0);
    f.bridge.shutdown();
  });
}

for (const api of ["d3d11", "d3d12"]) test(`Windows OpenXR ${api} leases remain alive until the host acknowledges consumption`, () => {
  const f = fixture("win32", { selectedBackend: "openxr", openxrMode: "api-layer",
    openxrHostGraphicsApi: api, openxrSubmittedFrameSequence: 0, openxrConsumedFrameSequence: 0 },
  (runtime) => { runtime.openxrSubmittedFrameSequence++; return true; });
  f.paint(f.texture({ sharedTextureHandle: Buffer.alloc(8, 1) }));
  assert.equal(f.calls.released, 0);
  f.runtime.openxrConsumedFrameSequence = 1;
  f.paint(undefined);
  assert.equal(f.calls.released, 1);
  f.paint(f.texture({ sharedTextureHandle: Buffer.alloc(8, 2) }));
  assert.equal(f.calls.released, 1);
  f.bridge.shutdown();
  assert.equal(f.calls.released, 2);
});
