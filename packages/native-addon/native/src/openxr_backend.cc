#include "openxr_backend.h"

namespace vrbridge {

namespace {

bool g_initialized = false;

}  // namespace

bool InitializeOpenXRBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend requires a non-empty overlay name.";
    }
    return false;
  }

  g_initialized = true;

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

bool SubmitOpenXRFrameWindows(uint64_t shared_handle, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend is not initialized.";
    }
    return false;
  }

  if (shared_handle == 0) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend received an invalid Windows shared handle.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

bool SubmitOpenXRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend is not initialized.";
    }
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend received an invalid DMA-BUF fd.";
    }
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }

  return true;
}

bool SetOpenXRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  (void)placement;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    *error_message = "OpenXR overlay placement is not implemented yet.";
  }
  return false;
}

bool SetOpenXRVisible(bool visible, std::string* error_message) {
  (void)visible;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    *error_message = "OpenXR overlay visibility control is not implemented yet.";
  }
  return false;
}

bool SetOpenXRSizeMeters(float size_meters, std::string* error_message) {
  (void)size_meters;
  if (!g_initialized) {
    if (error_message != nullptr) {
      *error_message = "OpenXR backend is not initialized.";
    }
    return false;
  }

  if (error_message != nullptr) {
    *error_message = "OpenXR overlay sizing is not implemented yet.";
  }
  return false;
}

void ShutdownOpenXRBackend() {
  g_initialized = false;
}

}  // namespace vrbridge
