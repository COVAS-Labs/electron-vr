import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { assertCurvature, assertSizeMeters, normalizePlacement } from "../../packages/electron-vr/dist/overlayOptions.js";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

test("overlay placement defaults to a head-locked transform", () => {
  const placement = normalizePlacement();

  assert.deepEqual(placement, {
    mode: "head",
    position: { x: 0, y: 0, z: -1.2 },
    rotation: { x: 0, y: 0, z: 0, w: 1 }
  });
});

test("overlay size validation rejects invalid values", () => {
  assert.doesNotThrow(() => assertSizeMeters(1.25));
  assert.throws(() => assertSizeMeters(Number.NaN), /sizeMeters/);
  assert.throws(() => assertSizeMeters(-1), /sizeMeters/);
  assert.throws(() => assertSizeMeters(0), /sizeMeters/);
});

test("overlay curvature validation rejects invalid values", () => {
  assert.doesNotThrow(() => assertCurvature(0));
  assert.doesNotThrow(() => assertCurvature(0.5));
  assert.doesNotThrow(() => assertCurvature(1));
  assert.throws(() => assertCurvature(Number.NaN), /curvature/);
  assert.throws(() => assertCurvature(-0.1), /curvature/);
  assert.throws(() => assertCurvature(1.1), /curvature/);
});

test("overlay placement validation rejects bad mode and coordinates", () => {
  assert.throws(
    () => normalizePlacement({ mode: "sideways", position: { x: 0, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0, w: 1 } }),
    /placement.mode/
  );

  assert.throws(
    () => normalizePlacement({ mode: "world", position: { x: Number.POSITIVE_INFINITY, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0, w: 1 } }),
    /placement.position/
  );
  assert.throws(
    () => normalizePlacement({ mode: "world", position: { x: 0, y: 0, z: -2 }, rotation: { x: 0, y: 0, z: 0, w: Number.NaN } }),
    /placement.rotation/
  );
});

test("runtime probe can disable the OpenVR fallback", async () => {
  const probe = await readFile(resolve(root, "packages", "native-addon", "native", "src", "runtime_probe.cc"), "utf8");
  assert.equal((probe.match(/ELECTRON_VR_DISABLE_OPENVR/g) ?? []).length, 2);
  assert.equal((probe.match(/openvr-disabled-by-env/g) ?? []).length, 2);
  assert.match(probe, /!openvr_disabled_by_env && info\.openvr_available && info\.openvr_runtime_installed/);
});

test("Windows VDXR avoids probing or selecting OpenVR", async () => {
  const probe = await readFile(resolve(root, "packages", "native-addon", "native", "src", "runtime_probe.cc"), "utf8");
  assert.match(probe, /IsVirtualDesktopOpenXRRuntime/);
  assert.match(probe, /openvr_unsafe_for_active_openxr_runtime = virtual_desktop_openxr && !openxr_enabled/);
  assert.match(probe, /!openvr_disabled_by_env && !openvr_unsafe_for_active_openxr_runtime/);
  assert.equal((probe.match(/QueryOpenVRSceneApplication\(&info\);/g) ?? []).length, 3);
});

test("Windows OpenXR layer publishes host presence before companion connection", async () => {
  const protocol = await readFile(resolve(root, "packages", "native-addon", "native", "openxr_api_layer_protocol.h"), "utf8");
  const layer = await readFile(resolve(root, "packages", "native-addon", "native", "openxr-api-layer", "layer.cc"), "utf8");
  const probe = await readFile(resolve(root, "packages", "native-addon", "native", "src", "runtime_probe.cc"), "utf8");
  const bridge = await readFile(resolve(root, "packages", "native-addon", "native", "src", "bridge.cc"), "utf8");
  assert.match(protocol, /kHostPresenceMappingPrefix/);
  assert.match(layer, /CreateFileMappingW/);
  assert.match(layer, /host_presence_ = hello/);
  assert.match(probe, /OpenFileMappingW/);
  assert.match(probe, /openxr_host_detected = true/);
  assert.match(bridge, /PopulateOpenXRHostPresence/);
});

test("Windows D3D11 companion submission never spin-waits for GPU completion", async () => {
  const companion = await readFile(resolve(root, "packages", "native-addon", "native", "src", "openxr_companion.cc"), "utf8");
  assert.doesNotMatch(companion, /while \(completion_result == S_FALSE\)/);
  assert.doesNotMatch(companion, /D3D11_ASYNC_GETDATA_DONOTFLUSH/);
  assert.match(companion, /slot\.keyed_mutex->ReleaseSync\(1\)/);
});

test("Windows OpenXR layer retains the last frame while a newer texture is pending", async () => {
  const layer = await readFile(resolve(root, "packages", "native-addon", "native", "openxr-api-layer", "layer.cc"), "utf8");
  assert.match(layer, /const bool copied_latest_frame = CopyLatestFrame/);
  assert.match(layer, /!copied_latest_frame && state->consumed_sequence == 0/);
});

test("Windows OpenComposite is not selected for unsupported OpenVR overlays", async () => {
  const probe = await readFile(resolve(root, "packages", "native-addon", "native", "src", "runtime_probe.cc"), "utf8");
  assert.match(probe, /IsOpenCompositeOpenVRRuntime/);
  assert.match(probe, /!opencomposite_openvr &&/);
  assert.match(probe, /openvr-overlay-unsupported-by-opencomposite/);
});

test("Linux OpenVR defaults to Vulkan with software and OpenGL fallbacks", async () => {
  const bridge = await readFile(resolve(root, "packages", "electron-vr", "src", "bridge.ts"), "utf8");
  const backend = await readFile(resolve(root, "packages", "native-addon", "native", "src", "openvr_backend.cc"), "utf8");
  assert.match(backend, /GetDmabufFormats/);
  assert.match(backend, /GetDmabufModifiers/);
  assert.doesNotMatch(backend, /RefResource\(imported_texture/);
  assert.match(backend, /direct_import_error/);
  assert.match(backend, /sync_error == vr::VROverlayError_TimedOut/);
  assert.match(backend, /WaitFrameSync\(100\)/);
  assert.match(backend, /ELECTRON_VR_DISABLE_OPENVR_VULKAN/);
  assert.match(backend, /GetVulkanInstanceExtensionsRequired/);
  assert.match(backend, /GetOutputDevice\(&output_device, vr::TextureType_Vulkan/);
  assert.match(backend, /VK_IMAGE_USAGE_TRANSFER_SRC_BIT \| VK_IMAGE_USAGE_SAMPLED_BIT/);
  assert.match(backend, /VK_IMAGE_USAGE_TRANSFER_DST_BIT/);
  assert.match(backend, /VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL/);
  assert.match(backend, /VK_QUEUE_FAMILY_FOREIGN_EXT/);
  assert.match(backend, /DMA_BUF_IOCTL_EXPORT_SYNC_FILE/);
  assert.match(backend, /WaitForDmabufProducer/);
  assert.match(backend, /EnsureLinuxOpenVRVulkanSubmitImage/);
  assert.match(backend, /CheckLinuxOpenVRVulkanSubmitImageWritable/);
  assert.match(backend, /if \(!writable\) return true/);
  assert.match(backend, /vulkan_visible_safe_to_write = sync_error == vr::VROverlayError_None/);
  assert.match(backend, /vulkan_dmabuf_import_enabled = std::all_of/);
  assert.match(backend, /does not support DMA-BUF import; use software upload/);
  assert.doesNotMatch(backend, /lacks required DMA-BUF extension/);
  assert.match(backend, /CopyLinuxOpenVRVulkanImage/);
  assert.match(backend, /SubmitLinuxOpenVRVulkanSoftwareFrame/);
  assert.match(bridge, /OpenVR Vulkan software upload/);
  assert.match(backend, /vkCmdCopyBufferToImage/);
  assert.match(backend, /requirements\.size, 0, &g_state\.vulkan_staging_mapping/);
  assert.match(backend, /LogLinuxFrameSyncResult\(sync_error, "Vulkan software texture submission"\)/);
  assert.match(backend, /vkCmdCopyImage/);
  assert.match(backend, /reinterpret_cast<uintptr_t>\(g_state\.vulkan_visible_image\)/);
  assert.doesNotMatch(backend, /g_state\.vulkan_visible_image = image/);
  assert.match(backend, /vr::VRVulkanTextureData_t/);
  assert.match(backend, /texture\.eType = vr::TextureType_Vulkan/);
  assert.match(backend, /vkQueueWaitIdle/);
  assert.doesNotMatch(backend, /glEGLImageTargetTexture2DOES/);
  assert.doesNotMatch(bridge, /submitLinuxSoftwareFrame/);
  assert.match(backend, /ELECTRON_VR_OPENVR_GL_UPLOAD/);
  assert.match(backend, /TextureType_OpenGL/);
  assert.match(bridge, /isTruthyEnvironmentVariable\("ELECTRON_VR_OPENVR_GL_UPLOAD"\)/);
  assert.match(bridge, /submitLinuxOpenGLFrame/);
  assert.match(bridge, /scheduleLinuxCaptureFallback/);
  assert.match(bridge, /bgraToVerticallyFlippedRgba/);
  assert.match(bridge, /linuxOpenVRVisibleTexture = texture/);
  assert.match(bridge, /!isTruthyEnvironmentVariable\("ELECTRON_VR_DISABLE_OPENVR_VULKAN"\)/);
  assert.match(bridge, /releaseTexture\(previousTexture\)/);
});
