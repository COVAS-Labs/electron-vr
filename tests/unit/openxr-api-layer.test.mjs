import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

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
