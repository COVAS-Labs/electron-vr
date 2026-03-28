#include "openvr_backend.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "openvr.h"

#if defined(__linux__)
#include <drm/drm_fourcc.h>
#endif

namespace vrbridge {

namespace {

struct OpenVRState {
  vr::IVRSystem* system = nullptr;
  vr::IVROverlay* overlay = nullptr;
#if defined(__linux__)
  vr::IVRIPCResourceManagerClient* ipc = nullptr;
#endif
  vr::VROverlayHandle_t overlay_handle = vr::k_ulOverlayHandleInvalid;
  bool initialized = false;
};

OpenVRState g_state;

void SetError(std::string* error_message, std::string message) {
  if (error_message != nullptr) {
    *error_message = std::move(message);
  }
}

std::string OverlayErrorToString(vr::IVROverlay* overlay, vr::EVROverlayError error) {
  if (overlay == nullptr) {
    return "Unknown OpenVR overlay error";
  }

  const char* name = overlay->GetOverlayErrorNameFromEnum(error);
  return name != nullptr ? std::string(name) : std::string("Unknown OpenVR overlay error");
}

bool CheckOverlayError(vr::EVROverlayError error, std::string context, std::string* error_message) {
  if (error == vr::VROverlayError_None) {
    return true;
  }

  SetError(error_message, context + ": " + OverlayErrorToString(g_state.overlay, error));
  return false;
}

vr::HmdMatrix34_t ToHmdMatrix34(const OverlayPlacement& placement) {
  const float x = placement.rotation.x;
  const float y = placement.rotation.y;
  const float z = placement.rotation.z;
  const float w = placement.rotation.w;

  vr::HmdMatrix34_t matrix = {};
  matrix.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
  matrix.m[0][1] = 2.0f * (x * y - z * w);
  matrix.m[0][2] = 2.0f * (x * z + y * w);
  matrix.m[0][3] = placement.position.x;

  matrix.m[1][0] = 2.0f * (x * y + z * w);
  matrix.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
  matrix.m[1][2] = 2.0f * (y * z - x * w);
  matrix.m[1][3] = placement.position.y;

  matrix.m[2][0] = 2.0f * (x * z - y * w);
  matrix.m[2][1] = 2.0f * (y * z + x * w);
  matrix.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
  matrix.m[2][3] = placement.position.z;
  return matrix;
}

bool ApplyPlacement(const OverlayPlacement& placement, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  const vr::HmdMatrix34_t matrix = ToHmdMatrix34(placement);
  if (placement.mode == OverlayPlacementMode::kHead) {
    return CheckOverlayError(
      g_state.overlay->SetOverlayTransformTrackedDeviceRelative(
        g_state.overlay_handle,
        vr::k_unTrackedDeviceIndex_Hmd,
        &matrix),
      "Failed to set head-locked overlay transform",
      error_message);
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayTransformAbsolute(
      g_state.overlay_handle,
      vr::TrackingUniverseStanding,
      &matrix),
    "Failed to set world-locked overlay transform",
    error_message);
}

bool ApplyVisible(bool visible, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  return CheckOverlayError(
    visible ? g_state.overlay->ShowOverlay(g_state.overlay_handle)
            : g_state.overlay->HideOverlay(g_state.overlay_handle),
    visible ? "Failed to show OpenVR overlay" : "Failed to hide OpenVR overlay",
    error_message);
}

bool ApplySizeMeters(float size_meters, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayWidthInMeters(g_state.overlay_handle, size_meters),
    "Failed to set OpenVR overlay width",
    error_message);
}

#if defined(__linux__)
bool ParseInteger(const std::string& value, uint64_t* result) {
  if (value.empty() || result == nullptr) {
    return false;
  }

  try {
    *result = static_cast<uint64_t>(std::stoull(value, nullptr, 0));
    return true;
  } catch (...) {
    return false;
  }
}

uint32_t ParsePixelFormat(const std::string& pixel_format) {
  uint64_t parsed_format = 0;
  if (ParseInteger(pixel_format, &parsed_format)) {
    return static_cast<uint32_t>(parsed_format);
  }

  if (pixel_format == "rgba") {
    return DRM_FORMAT_ABGR8888;
  }
  if (pixel_format == "bgra") {
    return DRM_FORMAT_ARGB8888;
  }
  if (pixel_format == "ARGB8888") {
    return DRM_FORMAT_ARGB8888;
  }
  if (pixel_format == "ABGR8888") {
    return DRM_FORMAT_ABGR8888;
  }
  if (pixel_format == "XRGB8888") {
    return DRM_FORMAT_XRGB8888;
  }
  if (pixel_format == "XBGR8888") {
    return DRM_FORMAT_XBGR8888;
  }
  return DRM_FORMAT_ARGB8888;
}

uint64_t ParseModifier(const std::string& modifier) {
  uint64_t parsed_modifier = 0;
  if (!ParseInteger(modifier, &parsed_modifier)) {
    return DRM_FORMAT_MOD_INVALID;
  }

  return parsed_modifier;
}
#endif

void ResetState() {
#if defined(__linux__)
  g_state.ipc = nullptr;
#endif
  g_state.system = nullptr;
  g_state.overlay = nullptr;
  g_state.overlay_handle = vr::k_ulOverlayHandleInvalid;
  g_state.initialized = false;
}

}  // namespace

bool InitializeOpenVRBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    SetError(error_message, "OpenVR backend requires a non-empty overlay name.");
    return false;
  }

  ShutdownOpenVRBackend();

  vr::EVRInitError init_error = vr::VRInitError_None;
  g_state.system = vr::VR_Init(&init_error, vr::VRApplication_Overlay);
  if (init_error != vr::VRInitError_None || g_state.system == nullptr) {
    SetError(error_message, std::string("Failed to initialize OpenVR: ") + vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
    ResetState();
    return false;
  }

  g_state.overlay = vr::VROverlay();
  if (g_state.overlay == nullptr) {
    SetError(error_message, "Failed to acquire OpenVR overlay interface.");
    ShutdownOpenVRBackend();
    return false;
  }

#if defined(__linux__)
  g_state.ipc = vr::VRIPCResourceManager();
#endif

  const std::string overlay_key = "electron_vr." + options.name;
  const vr::EVROverlayError create_error = g_state.overlay->CreateOverlay(
    overlay_key.c_str(),
    options.name.c_str(),
    &g_state.overlay_handle);
  if (!CheckOverlayError(create_error, "Failed to create OpenVR overlay", error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  g_state.initialized = true;

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayFlag(g_state.overlay_handle, vr::VROverlayFlags_IsPremultiplied, true),
        "Failed to configure OpenVR overlay alpha mode",
        error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  if (!ApplySizeMeters(options.size_meters, error_message) ||
      !ApplyPlacement(options.placement, error_message) ||
      !ApplyVisible(options.visible, error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SubmitOpenVRFrameWindows(uint64_t shared_handle, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  if (shared_handle == 0) {
    SetError(error_message, "OpenVR backend received an invalid Windows shared handle.");
    return false;
  }

  vr::Texture_t texture = {};
  texture.handle = reinterpret_cast<void*>(shared_handle);
  texture.eType = vr::TextureType_DXGISharedHandle;
  texture.eColorSpace = vr::ColorSpace_Auto;

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
        "Failed to submit Windows texture to OpenVR overlay",
        error_message)) {
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SubmitOpenVRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

#if defined(__linux__)
  if (g_state.ipc == nullptr) {
    SetError(error_message, "OpenVR IPC resource manager is unavailable on Linux.");
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    SetError(error_message, "OpenVR backend received an invalid DMA-BUF fd.");
    return false;
  }

  vr::DmabufAttributes_t attributes = {};
  attributes.pNext = nullptr;
  attributes.unWidth = texture_info.width;
  attributes.unHeight = texture_info.height;
  attributes.unDepth = 1;
  attributes.unMipLevels = 1;
  attributes.unArrayLayers = 1;
  attributes.unSampleCount = 1;
  attributes.unFormat = ParsePixelFormat(texture_info.pixel_format);
  attributes.ulModifier = ParseModifier(texture_info.modifier);
  attributes.unPlaneCount = static_cast<uint32_t>(std::min<size_t>(texture_info.planes.size(), vr::MaxDmabufPlaneCount));

  if (attributes.unWidth == 0 || attributes.unHeight == 0) {
    SetError(error_message, "Linux texture submission requires non-zero width and height.");
    return false;
  }

  for (uint32_t index = 0; index < attributes.unPlaneCount; ++index) {
    const LinuxPlaneInfo& plane = texture_info.planes[index];
    attributes.plane[index].nFd = plane.fd;
    attributes.plane[index].unOffset = plane.offset;
    attributes.plane[index].unStride = plane.stride;
  }

  vr::SharedTextureHandle_t imported_texture = static_cast<vr::SharedTextureHandle_t>(0);
  if (!g_state.ipc->ImportDmabuf(vr::VRApplication_Overlay, &attributes, &imported_texture) ||
      imported_texture == static_cast<vr::SharedTextureHandle_t>(0)) {
    SetError(error_message, "Failed to import DMA-BUF texture into OpenVR.");
    return false;
  }

  vr::Texture_t texture = {};
  texture.handle = &imported_texture;
  texture.eType = vr::TextureType_SharedTextureHandle;
  texture.eColorSpace = vr::ColorSpace_Auto;

  const bool submitted = CheckOverlayError(
    g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
    "Failed to submit Linux texture to OpenVR overlay",
    error_message);

  g_state.ipc->UnrefResource(imported_texture);
  return submitted;
#else
  (void)texture_info;
  SetError(error_message, "OpenVR Linux frame submission is only available on Linux builds.");
  return false;
#endif
}

bool SetOpenVRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  return ApplyPlacement(placement, error_message);
}

bool SetOpenVRVisible(bool visible, std::string* error_message) {
  return ApplyVisible(visible, error_message);
}

bool SetOpenVRSizeMeters(float size_meters, std::string* error_message) {
  if (size_meters <= 0.0f || !std::isfinite(size_meters)) {
    SetError(error_message, "OpenVR overlay size must be greater than zero.");
    return false;
  }

  return ApplySizeMeters(size_meters, error_message);
}

void ShutdownOpenVRBackend() {
  if (g_state.overlay != nullptr && g_state.overlay_handle != vr::k_ulOverlayHandleInvalid) {
    g_state.overlay->DestroyOverlay(g_state.overlay_handle);
  }

  if (g_state.system != nullptr || g_state.overlay != nullptr) {
    vr::VR_Shutdown();
  }

  ResetState();
}

}  // namespace vrbridge
