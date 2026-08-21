#pragma once

#include <cstdint>
#include <string>

namespace vrbridge {

enum class BackendKind {
  kNone = 0,
  kOpenXR,
  kOpenVR,
  kMock
};

enum class OpenXRMode {
  kNone = 0,
  kOverlaySession,
  kApiLayer,
  kStandardTestSession
};

struct RuntimeInfo {
  std::string platform;
  bool openxr_available = false;
  bool openxr_overlay_extension_available = false;
  bool openxr_linux_egl_binding_available = false;
  bool openxr_linux_opengl_es_binding_available = false;
  bool openxr_windows_d3d11_binding_available = false;
  bool openxr_windows_d3d12_binding_available = false;
  bool openxr_macos_metal_binding_available = false;
  std::string openxr_runtime_name;
  std::string openxr_runtime_manifest_path;
  std::string openxr_runtime_library_path;
  std::string openxr_loader_path;
  std::string openxr_session_state = "unknown";
  bool openxr_session_running = false;
  OpenXRMode openxr_mode = OpenXRMode::kNone;
  bool openxr_api_layer_installed = false;
  bool openxr_api_layer_enabled = false;
  std::string openxr_api_layer_manifest_path;
  bool openxr_companion_connected = false;
  bool openxr_host_detected = false;
  uint32_t openxr_host_process_id = 0;
  std::string openxr_host_application_name;
  std::string openxr_host_graphics_api;
  std::string openxr_host_adapter_luid;
  uint32_t openxr_protocol_version = 0;
  bool openvr_available = false;
  bool openvr_runtime_installed = false;
  std::string openvr_runtime_path;
  std::string openvr_scene_application_state;
  uint32_t openvr_scene_process_id = 0;
  std::string openvr_scene_application_key;
  std::string openvr_scene_application_name;
  std::string openvr_scene_application_binary_path;
  BackendKind selected_backend = BackendKind::kNone;
  std::string probe_mode = "stub";
};

RuntimeInfo ProbeRuntime();
void PopulateOpenXRHostPresence(RuntimeInfo* info);
const char* BackendKindToString(BackendKind kind);
const char* OpenXRModeToString(OpenXRMode mode);

}  // namespace vrbridge
