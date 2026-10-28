#include "bridge.h"

#include <cmath>
#include <utility>

#include "mock_backend.h"
#include "openvr_backend.h"
#include "openxr_backend.h"
#include "openxr_companion.h"
#include "openxr_companion_linux.h"

namespace vrbridge {

namespace {

bool ValidateSharedTextureSubmission(const SharedTextureSubmission& texture_submission, std::string* error_message) {
#if defined(_WIN32)
  if (texture_submission.windows_handle == 0) {
    if (error_message != nullptr) {
      *error_message = "Windows shared texture handle must be non-zero.";
    }
    return false;
  }
  return true;
#elif defined(__linux__)
  if (texture_submission.linux_texture.planes.empty() || texture_submission.linux_texture.planes.front().fd < 0) {
    if (error_message != nullptr) {
      *error_message = "Linux shared texture submission requires a non-negative DMA-BUF file descriptor.";
    }
    return false;
  }
  return true;
#elif defined(__APPLE__)
  if (texture_submission.mac_texture.io_surface == 0 || texture_submission.mac_texture.width == 0 || texture_submission.mac_texture.height == 0) {
    if (error_message != nullptr) {
      *error_message = "macOS shared texture submission requires an IOSurface and non-zero dimensions.";
    }
    return false;
  }
  return true;
#else
  if (error_message != nullptr) {
    *error_message = "Shared texture submission is not supported on this platform.";
  }
  return false;
#endif
}

bool SubmitOpenXRSharedTexture(const SharedTextureSubmission& texture_submission, std::string* error_message) {
#if defined(_WIN32)
  return SubmitOpenXRFrameWindows(texture_submission.windows_handle, error_message);
#elif defined(__linux__)
  return SubmitOpenXRFrameLinux(texture_submission.linux_texture, error_message);
#elif defined(__APPLE__)
  return SubmitOpenXRFrameMac(texture_submission.mac_texture, error_message);
#else
  if (error_message != nullptr) {
    *error_message = "OpenXR shared texture submission is not supported on this platform.";
  }
  return false;
#endif
}

bool SubmitOpenVRSharedTexture(const SharedTextureSubmission& texture_submission, std::string* error_message) {
#if defined(_WIN32)
  return SubmitOpenVRFrameWindows(texture_submission.windows_handle, error_message);
#elif defined(__linux__)
  return SubmitOpenVRFrameLinux(texture_submission.linux_texture, error_message);
#else
  if (error_message != nullptr) {
    *error_message = "OpenVR shared texture submission is not supported on this platform.";
  }
  return false;
#endif
}

bool SubmitMockSharedTexture(const SharedTextureSubmission& texture_submission, std::string* error_message) {
#if defined(_WIN32)
  return SubmitMockFrameWindows(texture_submission.windows_handle, error_message);
#elif defined(__linux__)
  return SubmitMockFrameLinux(texture_submission.linux_texture, error_message);
#else
  if (error_message != nullptr) {
    *error_message = "Mock shared texture submission is not supported on this platform.";
  }
  return false;
#endif
}

}  // namespace

BridgeState& GetBridgeState() {
  static BridgeState bridge_state;
  return bridge_state;
}

RuntimeInfo BridgeState::GetRuntimeInfo() {
  RefreshLinuxBackend();
  return GetLiveRuntimeInfo();
}

void BridgeState::RefreshLinuxBackend() {
#if defined(__linux__)
  if (!initialized_) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_backend_check_ < std::chrono::seconds(2)) return;
  last_backend_check_ = now;
  RuntimeInfo live = GetLiveRuntimeInfo();
  bool can_switch = false;
  if (live.selected_backend == BackendKind::kOpenXR && live.openxr_mode == OpenXRMode::kApiLayer) {
    can_switch = !live.openxr_host_detected && !live.openxr_companion_connected;
  } else if (live.selected_backend == BackendKind::kOpenVR) {
    // Never background-init/shutdown OpenVR while its overlay owns the client.
    // Query the read-only OpenXR host endpoint first instead.
    PopulateOpenXRHostPresenceLinux(&live);
    can_switch = live.openxr_host_detected;
  }
  if (!can_switch) return;
  const RuntimeInfo target = ProbeRuntime(live.selected_backend != BackendKind::kOpenVR);
  if (live.selected_backend == BackendKind::kOpenVR && !target.openxr_host_detected) return;
  if (target.selected_backend == runtime_info_.selected_backend) return;
  const RuntimeInfo previous = runtime_info_;
  const InitializeOptions options = options_;
  InitializeOptions target_options = options;
  if (target.selected_backend == BackendKind::kOpenXR && target.openxr_mode == OpenXRMode::kApiLayer) {
    target_options.curvature = 0.0f;
  }
  Shutdown();
  if (!InitializeWithRuntime(target_options, target)) {
    // A racing scene/runtime shutdown must not leave an otherwise usable
    // waiting overlay destroyed.
    Shutdown();
    InitializeWithRuntime(options, previous);
  } else {
    // The API-layer quad is flat, but retain the user's preferred curvature
    // for a later return to a backend that supports it.
    options_.curvature = options.curvature;
  }
#endif
}

RuntimeInfo BridgeState::GetLiveRuntimeInfo() const {
  RuntimeInfo runtime_info = initialized_ ? runtime_info_ : ProbeRuntime();

  switch (runtime_info.selected_backend) {
    case BackendKind::kOpenXR:
      if (runtime_info.openxr_mode == OpenXRMode::kApiLayer) {
#if defined(__linux__)
        PopulateOpenXRHostPresenceLinux(&runtime_info);
        PopulateOpenXRCompanionRuntimeInfoLinux(&runtime_info);
#else
        if (!initialized_) PopulateOpenXRHostPresence(&runtime_info);
        PopulateOpenXRCompanionRuntimeInfo(&runtime_info);
#endif
      } else {
        PopulateOpenXRRuntimeInfo(&runtime_info);
      }
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info.openvr_host_relay_available) {
        PopulateOpenVRRelayRuntimeInfoLinux(&runtime_info);
        break;
      }
#endif
      PopulateOpenVRRuntimeInfo(&runtime_info);
      break;
    case BackendKind::kMock:
    case BackendKind::kNone:
    default:
      break;
  }

  return runtime_info;
}

bool BridgeState::Initialize(const InitializeOptions& options) {
  return InitializeWithRuntime(options, ProbeRuntime());
}

bool BridgeState::InitializeWithRuntime(const InitializeOptions& options, const RuntimeInfo& runtime) {
  runtime_info_ = runtime;
  options_ = options;
  frame_count_ = 0;
  last_error_.clear();

  if (options.name.empty()) {
    SetLastError("Overlay name is required.");
    initialized_ = false;
    return false;
  }

  if (options.width == 0 || options.height == 0) {
    SetLastError("Overlay dimensions must be greater than zero.");
    initialized_ = false;
    return false;
  }

  if (options.size_meters <= 0.0f) {
    SetLastError("Overlay sizeMeters must be greater than zero.");
    initialized_ = false;
    return false;
  }

  if (!std::isfinite(options.curvature) || options.curvature < 0.0f || options.curvature > 1.0f) {
    SetLastError("Overlay curvature must be between 0 and 1.");
    initialized_ = false;
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? InitializeOpenXRCompanionLinux(options_, &last_error_)
#else
        ? InitializeOpenXRCompanion(options_, &last_error_)
#endif
        : InitializeOpenXRBackend(options_, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = InitializeOpenXRCompanionLinux(options_, &last_error_, true);
        break;
      }
#endif
      success = InitializeOpenVRBackend(options_, &last_error_);
      break;
    case BackendKind::kMock:
      success = InitializeMockBackend(options_, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend was selected for this system.");
      success = false;
      break;
  }

  initialized_ = success;
  return initialized_;
}

bool BridgeState::SubmitSharedTexture(const SharedTextureSubmission& texture_submission) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (!ValidateSharedTextureSubmission(texture_submission, &last_error_)) {
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? SubmitOpenXRCompanionFrameLinux(texture_submission.linux_texture, &last_error_)
#else
        ? SubmitOpenXRCompanionFrame(texture_submission.windows_handle, &last_error_)
#endif
        : SubmitOpenXRSharedTexture(texture_submission, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SubmitOpenXRCompanionFrameLinux(texture_submission.linux_texture, &last_error_);
        break;
      }
#endif
      success = SubmitOpenVRSharedTexture(texture_submission, &last_error_);
      break;
    case BackendKind::kMock:
      success = SubmitMockSharedTexture(texture_submission, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for shared-texture submission.");
      return false;
  }

  if (success) {
    ++frame_count_;
  }

  return success;
}

bool BridgeState::SubmitSoftwareFrame(const SoftwareFrameInfo& frame_info) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (frame_info.width == 0 || frame_info.height == 0 || frame_info.rgba_pixels.empty()) {
    SetLastError("Software frame submission requires width, height, and RGBA pixel data.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kMock:
      success = SubmitMockSoftwareFrame(frame_info, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SubmitOpenXRCompanionSoftwareFrameLinux(frame_info, &last_error_);
        break;
      }
#endif
      success = SubmitOpenVRSoftwareFrame(frame_info, &last_error_);
      break;
    case BackendKind::kOpenXR:
#if defined(__linux__)
      if (runtime_info_.openxr_mode == OpenXRMode::kApiLayer) {
        success = SubmitOpenXRCompanionSoftwareFrameLinux(frame_info, &last_error_);
        break;
      }
#endif
      SetLastError("Software frame submission is only available for the OpenXR API layer, mock, and OpenVR backends.");
      return false;
    case BackendKind::kNone:
    default:
      SetLastError("Software frame submission is only available for the OpenXR API layer, mock, and OpenVR backends.");
      return false;
  }

  if (success) {
    ++frame_count_;
  }

  return success;
}

bool BridgeState::SetOverlayPlacement(const OverlayPlacement& placement) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? SetOpenXRCompanionPlacementLinux(placement, &last_error_)
#else
        ? SetOpenXRCompanionPlacement(placement, &last_error_)
#endif
        : SetOpenXRPlacement(placement, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SetOpenXRCompanionPlacementLinux(placement, &last_error_);
        break;
      }
#endif
      success = SetOpenVRPlacement(placement, &last_error_);
      break;
    case BackendKind::kMock:
      success = SetMockPlacement(placement, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for overlay placement.");
      return false;
  }

  if (success) {
    options_.placement = placement;
  }

  return success;
}

bool BridgeState::SetOverlayVisible(bool visible) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? SetOpenXRCompanionVisibleLinux(visible, &last_error_)
#else
        ? SetOpenXRCompanionVisible(visible, &last_error_)
#endif
        : SetOpenXRVisible(visible, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SetOpenXRCompanionVisibleLinux(visible, &last_error_);
        break;
      }
#endif
      success = SetOpenVRVisible(visible, &last_error_);
      break;
    case BackendKind::kMock:
      success = SetMockVisible(visible, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for overlay visibility.");
      return false;
  }

  if (success) {
    options_.visible = visible;
  }

  return success;
}

bool BridgeState::SetOverlaySizeMeters(float size_meters) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (size_meters <= 0.0f) {
    SetLastError("Overlay sizeMeters must be greater than zero.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? SetOpenXRCompanionSizeMetersLinux(size_meters, &last_error_)
#else
        ? SetOpenXRCompanionSizeMeters(size_meters, &last_error_)
#endif
        : SetOpenXRSizeMeters(size_meters, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SetOpenXRCompanionSizeMetersLinux(size_meters, &last_error_);
        break;
      }
#endif
      success = SetOpenVRSizeMeters(size_meters, &last_error_);
      break;
    case BackendKind::kMock:
      success = SetMockSizeMeters(size_meters, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for overlay sizing.");
      return false;
  }

  if (success) {
    options_.size_meters = size_meters;
  }

  return success;
}

bool BridgeState::SetOverlayCurvature(float curvature) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (!std::isfinite(curvature) || curvature < 0.0f || curvature > 1.0f) {
    SetLastError("Overlay curvature must be between 0 and 1.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = runtime_info_.openxr_mode == OpenXRMode::kApiLayer
#if defined(__linux__)
        ? SetOpenXRCompanionCurvatureLinux(curvature, &last_error_)
#else
        ? SetOpenXRCompanionCurvature(curvature, &last_error_)
#endif
        : SetOpenXRCurvature(curvature, &last_error_);
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        success = SetOpenXRCompanionCurvatureLinux(curvature, &last_error_);
        break;
      }
#endif
      success = SetOpenVRCurvature(curvature, &last_error_);
      break;
    case BackendKind::kMock:
      success = SetMockCurvature(curvature, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for overlay curvature.");
      return false;
  }

  if (success) {
    options_.curvature = curvature;
  }

  return success;
}

void BridgeState::Shutdown() {
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      if (runtime_info_.openxr_mode == OpenXRMode::kApiLayer) {
#if defined(__linux__)
        ShutdownOpenXRCompanionLinux();
#else
        ShutdownOpenXRCompanion();
#endif
      } else {
        ShutdownOpenXRBackend();
      }
      break;
    case BackendKind::kOpenVR:
#if defined(__linux__)
      if (runtime_info_.openvr_host_relay_available) {
        ShutdownOpenXRCompanionLinux();
        break;
      }
#endif
      ShutdownOpenVRBackend();
      break;
    case BackendKind::kMock:
      ShutdownMockBackend();
      break;
    case BackendKind::kNone:
    default:
      break;
  }

  initialized_ = false;
  frame_count_ = 0;
  last_error_.clear();
}

bool BridgeState::IsInitialized() const {
  return initialized_;
}

std::string BridgeState::GetLastError() const {
  return last_error_;
}

void BridgeState::SetLastError(std::string message) {
  last_error_ = std::move(message);
}

}  // namespace vrbridge
