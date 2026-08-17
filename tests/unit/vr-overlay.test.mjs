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
