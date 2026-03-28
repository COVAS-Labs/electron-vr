#pragma once

#include <string>

namespace vrbridge {

enum class BackendKind {
  kNone = 0,
  kOpenXR,
  kOpenVR,
  kMock
};

struct RuntimeInfo {
  std::string platform;
  bool openxr_available = false;
  bool openxr_overlay_extension_available = false;
  bool openvr_available = false;
  BackendKind selected_backend = BackendKind::kNone;
  std::string probe_mode = "stub";
};

RuntimeInfo ProbeRuntime();
const char* BackendKindToString(BackendKind kind);

}  // namespace vrbridge
