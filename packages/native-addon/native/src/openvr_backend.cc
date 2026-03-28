#include "openvr_backend.h"

namespace vrbridge {

namespace {

bool g_initialized = false;

}  // namespace

bool InitializeOpenVRBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    if (error_message != nullptr) {
      *error_message = "OpenVR backend requires a non-empty overlay name.";
    }
    return false;
  }

  g_initialized = true;

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

bool SubmitOpenVRFrameWindows(uint64_t shared_handle, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenVR backend is not initialized.";
    }
    return false;
  }

  if (shared_handle == 0) {
    if (error_message != nullptr) {
      *error_message = "OpenVR backend received an invalid Windows shared handle.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

bool SubmitOpenVRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenVR backend is not initialized.";
    }
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    if (error_message != nullptr) {
      *error_message = "OpenVR backend received an invalid DMA-BUF fd.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

void ShutdownOpenVRBackend() {
  g_initialized = false;
}

}  // namespace vrbridge
