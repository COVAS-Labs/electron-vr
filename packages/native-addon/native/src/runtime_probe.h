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
  bool openxr_linux_egl_binding_available = false;
  bool openxr_windows_d3d11_binding_available = false;
  std::string openxr_runtime_name;
  std::string openxr_runtime_manifest_path;
  std::string openxr_runtime_library_path;
  std::string openxr_loader_path;
  bool openvr_available = false;
  bool openvr_runtime_installed = false;
  std::string openvr_runtime_path;
  BackendKind selected_backend = BackendKind::kNone;
  std::string probe_mode = "stub";
};

RuntimeInfo ProbeRuntime();
const char* BackendKindToString(BackendKind kind);

}  // namespace vrbridge
