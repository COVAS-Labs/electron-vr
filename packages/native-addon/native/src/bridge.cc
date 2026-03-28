#include "bridge.h"

#include <utility>

#include "mock_backend.h"
#include "openvr_backend.h"
#include "openxr_backend.h"

namespace vrbridge {

BridgeState& GetBridgeState() {
  static BridgeState bridge_state;
  return bridge_state;
}

RuntimeInfo BridgeState::GetRuntimeInfo() const {
  return runtime_info_;
}

bool BridgeState::Initialize(const InitializeOptions& options) {
  runtime_info_ = ProbeRuntime();
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

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = InitializeOpenXRBackend(options_, &last_error_);
      break;
    case BackendKind::kOpenVR:
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

bool BridgeState::SubmitFrameWindows(uint64_t shared_handle) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (shared_handle == 0) {
    SetLastError("Shared texture handle must be non-zero.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = SubmitOpenXRFrameWindows(shared_handle, &last_error_);
      break;
    case BackendKind::kOpenVR:
      success = SubmitOpenVRFrameWindows(shared_handle, &last_error_);
      break;
    case BackendKind::kMock:
      success = SubmitMockFrameWindows(shared_handle, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for Windows frame submission.");
      return false;
  }

  if (success) {
    ++frame_count_;
  }

  return success;
}

bool BridgeState::SubmitFrameLinux(const LinuxTextureInfo& texture_info) {
  if (!initialized_) {
    SetLastError("Bridge is not initialized.");
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    SetLastError("Linux texture info must include a non-negative DMA-BUF file descriptor.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      success = SubmitOpenXRFrameLinux(texture_info, &last_error_);
      break;
    case BackendKind::kOpenVR:
      success = SubmitOpenVRFrameLinux(texture_info, &last_error_);
      break;
    case BackendKind::kMock:
      success = SubmitMockFrameLinux(texture_info, &last_error_);
      break;
    case BackendKind::kNone:
    default:
      SetLastError("No backend selected for Linux frame submission.");
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
    SetLastError("Software frame must include width, height, and RGBA pixel data.");
    return false;
  }

  bool success = false;
  switch (runtime_info_.selected_backend) {
    case BackendKind::kMock:
      success = SubmitMockSoftwareFrame(frame_info, &last_error_);
      break;
    case BackendKind::kOpenXR:
    case BackendKind::kOpenVR:
    case BackendKind::kNone:
    default:
      SetLastError("Software frame submission is only available for the mock backend.");
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
      success = SetOpenXRPlacement(placement, &last_error_);
      break;
    case BackendKind::kOpenVR:
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
      success = SetOpenXRVisible(visible, &last_error_);
      break;
    case BackendKind::kOpenVR:
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
      success = SetOpenXRSizeMeters(size_meters, &last_error_);
      break;
    case BackendKind::kOpenVR:
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

void BridgeState::Shutdown() {
  switch (runtime_info_.selected_backend) {
    case BackendKind::kOpenXR:
      ShutdownOpenXRBackend();
      break;
    case BackendKind::kOpenVR:
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
