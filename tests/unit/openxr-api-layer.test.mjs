import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { parseOpenXRApiLayerStatus } from "../../packages/electron-vr/dist/openxrApiLayer.js";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const layerDirectory = resolve(root, "packages", "native-addon", "native", "openxr-api-layer");

test("implicit API-layer manifest has a recovery switch and matching name", async () => {
  const manifest = JSON.parse(await readFile(resolve(layerDirectory, "electron_vr_openxr_layer.json"), "utf8"));
  assert.equal(manifest.file_format_version, "1.0.0");
  assert.equal(manifest.api_layer.name, "XR_APILAYER_ELECTRON_VR_overlay");
  assert.equal(manifest.api_layer.api_version, "1.0");
  assert.equal(manifest.api_layer.disable_environment, "ELECTRON_VR_DISABLE_OPENXR_API_LAYER");
  assert.match(manifest.api_layer.library_path, /electron_vr_openxr_layer\.dll$/);
});

test("API-layer protocol and source retain the Direct3D single-overlay contract", async () => {
  const protocol = JSON.parse(await readFile(resolve(layerDirectory, "protocol.json"), "utf8"));
  const source = await readFile(resolve(layerDirectory, "layer.cc"), "utf8");
  const harness = await readFile(resolve(layerDirectory, "layer_test.cc"), "utf8");
  assert.equal(protocol.version, 2);
  assert.equal(protocol.textureSlots, 3);
  assert.deepEqual(protocol.graphicsApis, ["d3d11", "d3d12"]);
  assert.match(source, /xrNegotiateLoaderApiLayerInterface/);
  assert.match(source, /XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT/);
  assert.match(source, /XrCompositionLayerQuad/);
  assert.match(source, /XrGraphicsBindingD3D12KHR/);
  assert.match(source, /ID3D12CommandQueue/);
  assert.doesNotMatch(source, /\bxrWaitFrame\s*\(/);
  assert.doesNotMatch(source, /\bxrBeginFrame\s*\(/);
  assert.match(source, /constexpr XrDuration kSwapchainWaitTimeout = 0/);
  assert.match(harness, /PFN_xrNegotiateLoaderApiLayerInterface/);
  assert.match(harness, /no-companion frame is forwarded unchanged/);
});

test("Linux API layer uses private Unix IPC and desktop OpenGL pass-through", async () => {
  const manifest = JSON.parse(await readFile(resolve(layerDirectory, "electron_vr_openxr_layer_linux.json"), "utf8"));
  const source = await readFile(resolve(layerDirectory, "layer_linux.cc"), "utf8");
  const protocol = await readFile(resolve(root, "packages", "native-addon", "native", "openxr_api_layer_protocol_linux.h"), "utf8");
  assert.equal(manifest.api_layer.name, "XR_APILAYER_ELECTRON_VR_overlay");
  assert.match(manifest.api_layer.library_path, /libelectron_vr_openxr_layer\.so$/);
  assert.match(source, /XrGraphicsBindingOpenGLXlibKHR/);
  assert.match(source, /SCM_RIGHTS/);
  assert.match(source, /SO_PEERCRED/);
  assert.match(source, /glXMakeContextCurrent/);
  assert.match(source, /DRM_FORMAT_MOD_INVALID/);
  assert.match(source, /mmap/);
  assert.doesNotMatch(source, /glEGLImageTargetTexture2DOES|glEGLImageTargetTexStorageEXT/);
  assert.match(protocol, /SOCK_SEQPACKET|kSocketName/);
});

test("API-layer status parser handles Windows and Linux utility output", () => {
  assert.deepEqual(parseOpenXRApiLayerStatus([
    "installed=true",
    "registered=true",
    "enabled=false",
    "manifest=C:\\Users\\test\\electron_vr_openxr_layer.json",
    "scope=current-user (elevated OpenXR applications do not load HKCU layers)"
  ].join("\r\n")), {
    installed: true,
    enabled: false,
    registered: true,
    manifestPath: "C:\\Users\\test\\electron_vr_openxr_layer.json",
    scope: "current-user (elevated OpenXR applications do not load HKCU layers)"
  });

  assert.deepEqual(parseOpenXRApiLayerStatus([
    "installed=false",
    "enabled=false",
    "manifest=/home/test/.local/share/openxr/1/api_layers/implicit.d/electron_vr_openxr_layer.json"
  ].join("\n")), {
    installed: false,
    enabled: false,
    registered: null,
    manifestPath: "/home/test/.local/share/openxr/1/api_layers/implicit.d/electron_vr_openxr_layer.json",
    scope: "current-user"
  });
});

test("published Linux API-layer utility reports structured status details", async () => {
  const packagingSource = await readFile(resolve(root, "tools", "prepare-prebuilt-package.mjs"), "utf8");
  assert.match(packagingSource, /console\.log\(\"manifest=\" \+ manifest\)/);
  assert.match(packagingSource, /console\.log\(\"scope=current-user\"\)/);
});
