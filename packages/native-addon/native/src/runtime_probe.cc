#include "runtime_probe.h"

#include "openxr_loader_win.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "openvr.h"

#if defined(_WIN32)
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#else
#include <dlfcn.h>
#endif

#if defined(__linux__)
#ifndef XR_USE_PLATFORM_EGL
#define XR_USE_PLATFORM_EGL
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

namespace vrbridge {

namespace {

bool LibraryExists(const char* primary_name, const char* secondary_name = nullptr) {
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(primary_name);
  if (handle != nullptr) {
    FreeLibrary(handle);
    return true;
  }

  if (secondary_name == nullptr) {
    return false;
  }

  handle = LoadLibraryA(secondary_name);
  if (handle != nullptr) {
    FreeLibrary(handle);
    return true;
  }

  return false;
#else
  void* handle = dlopen(primary_name, RTLD_LAZY | RTLD_LOCAL);
  if (handle != nullptr) {
    dlclose(handle);
    return true;
  }

  if (secondary_name == nullptr) {
    return false;
  }

  handle = dlopen(secondary_name, RTLD_LAZY | RTLD_LOCAL);
  if (handle != nullptr) {
    dlclose(handle);
    return true;
  }

  return false;
#endif
}

void AppendProbeMode(RuntimeInfo* info, const std::string& suffix) {
  if (info == nullptr || suffix.empty()) {
    return;
  }

  if (info->probe_mode.empty()) {
    info->probe_mode = suffix;
    return;
  }

  info->probe_mode += ":" + suffix;
}

std::string DetectPlatform() {
#if defined(_WIN32)
  return "win32";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "darwin";
#else
  return "unknown";
#endif
}

bool IsTruthyEnvVar(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }

  const std::string normalized(value);
  return normalized == "1" || normalized == "true" || normalized == "TRUE" || normalized == "yes" || normalized == "YES" || normalized == "on" || normalized == "ON";
}

#if defined(_WIN32)
bool InitializeOpenVRProbe(vr::IVRSystem** system, vr::EVRInitError* init_error) {
  __try {
    *system = vr::VR_Init(init_error, vr::VRApplication_Background);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (system != nullptr) {
      *system = nullptr;
    }
    if (init_error != nullptr) {
      *init_error = vr::VRInitError_Init_Internal;
    }
    return false;
  }
}

bool AcquireOpenVRApplications(vr::IVRApplications** applications) {
  __try {
    *applications = vr::VRApplications();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (applications != nullptr) {
      *applications = nullptr;
    }
    return false;
  }
}
#else
bool InitializeOpenVRProbe(vr::IVRSystem** system, vr::EVRInitError* init_error) {
  *system = vr::VR_Init(init_error, vr::VRApplication_Background);
  return true;
}

bool AcquireOpenVRApplications(vr::IVRApplications** applications) {
  *applications = vr::VRApplications();
  return true;
}
#endif

std::string ReadOpenVRApplicationPropertyString(
  vr::IVRApplications* applications,
  const std::string& application_key,
  vr::EVRApplicationProperty property) {
  if (applications == nullptr || application_key.empty()) {
    return std::string();
  }

  std::vector<char> buffer(256, '\0');
  vr::EVRApplicationError application_error = vr::VRApplicationError_None;
  uint32_t required_size = applications->GetApplicationPropertyString(
    application_key.c_str(),
    property,
    buffer.data(),
    static_cast<uint32_t>(buffer.size()),
    &application_error);
  if (application_error == vr::VRApplicationError_BufferTooSmall && required_size > buffer.size()) {
    buffer.assign(required_size, '\0');
    application_error = vr::VRApplicationError_None;
    required_size = applications->GetApplicationPropertyString(
      application_key.c_str(),
      property,
      buffer.data(),
      static_cast<uint32_t>(buffer.size()),
      &application_error);
  }

  if (application_error != vr::VRApplicationError_None || required_size == 0) {
    return std::string();
  }

  return std::string(buffer.data());
}

void QueryOpenVRSceneApplication(RuntimeInfo* info) {
  if (info == nullptr || !info->openvr_available || !info->openvr_runtime_installed) {
    return;
  }

  vr::EVRInitError init_error = vr::VRInitError_None;
  vr::IVRSystem* system = nullptr;
  if (!InitializeOpenVRProbe(&system, &init_error) || init_error != vr::VRInitError_None || system == nullptr) {
    return;
  }

  vr::IVRApplications* applications = nullptr;
  if (!AcquireOpenVRApplications(&applications) || applications == nullptr) {
    vr::VR_Shutdown();
    return;
  }

  const vr::EVRSceneApplicationState scene_state = applications->GetSceneApplicationState();
  const char* scene_state_name = applications->GetSceneApplicationStateNameFromEnum(scene_state);
  if (scene_state_name != nullptr) {
    info->openvr_scene_application_state = scene_state_name;
  }

  info->openvr_scene_process_id = applications->GetCurrentSceneProcessId();
  if (info->openvr_scene_process_id != 0) {
    std::array<char, vr::k_unMaxApplicationKeyLength> application_key_buffer = {};
    const vr::EVRApplicationError application_key_error = applications->GetApplicationKeyByProcessId(
      info->openvr_scene_process_id,
      application_key_buffer.data(),
      static_cast<uint32_t>(application_key_buffer.size()));
    if (application_key_error == vr::VRApplicationError_None) {
      info->openvr_scene_application_key = application_key_buffer.data();
      info->openvr_scene_application_name = ReadOpenVRApplicationPropertyString(
        applications,
        info->openvr_scene_application_key,
        vr::VRApplicationProperty_Name_String);
      info->openvr_scene_application_binary_path = ReadOpenVRApplicationPropertyString(
        applications,
        info->openvr_scene_application_key,
        vr::VRApplicationProperty_BinaryPath_String);
    }
  }

  vr::VR_Shutdown();
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return std::string();
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::string UnescapeJsonString(const std::string& value) {
  std::string result;
  result.reserve(value.size());

  for (size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character != '\\' || index + 1 >= value.size()) {
      result.push_back(character);
      continue;
    }

    const char escaped = value[++index];
    switch (escaped) {
      case '\\':
      case '/':
      case '"':
        result.push_back(escaped);
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      default:
        result.push_back(escaped);
        break;
    }
  }

  return result;
}

std::string ExtractFirstRuntimePath(const std::string& json_text) {
  const std::string runtime_key = "\"runtime\"";
  const size_t runtime_key_index = json_text.find(runtime_key);
  if (runtime_key_index == std::string::npos) {
    return std::string();
  }

  const size_t array_start = json_text.find('[', runtime_key_index + runtime_key.size());
  if (array_start == std::string::npos) {
    return std::string();
  }

  const size_t string_start = json_text.find('"', array_start + 1);
  if (string_start == std::string::npos) {
    return std::string();
  }

  std::string raw_value;
  bool escaping = false;
  for (size_t index = string_start + 1; index < json_text.size(); ++index) {
    const char character = json_text[index];
    if (!escaping && character == '"') {
      return UnescapeJsonString(raw_value);
    }

    raw_value.push_back(character);
    escaping = !escaping && character == '\\';
    if (character != '\\') {
      escaping = false;
    }
  }

  return std::string();
}

std::string GetOpenVRPathsFilePath() {
#if defined(_WIN32)
  const char* local_app_data = std::getenv("LOCALAPPDATA");
  if (local_app_data == nullptr || local_app_data[0] == '\0') {
    return std::string();
  }

  return std::string(local_app_data) + "\\openvr\\openvrpaths.vrpath";
#elif defined(__linux__)
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return std::string();
  }

  return std::string(home) + "/.config/openvr/openvrpaths.vrpath";
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return std::string();
  }

  return std::string(home) + "/Library/Application Support/OpenVR/openvrpaths.vrpath";
#else
  return std::string();
#endif
}

std::string DetectOpenVRRuntimePath(bool* installed) {
  if (installed != nullptr) {
    *installed = false;
  }

  const std::string registry_path = GetOpenVRPathsFilePath();
  if (registry_path.empty()) {
    return std::string();
  }

  const std::string registry_contents = ReadTextFile(registry_path);
  if (registry_contents.empty()) {
    return std::string();
  }

  const std::string runtime_path = ExtractFirstRuntimePath(registry_contents);
  if (runtime_path.empty()) {
    return std::string();
  }

  if (installed != nullptr) {
    *installed = true;
  }

  return runtime_path;
}

#if defined(_WIN32) || defined(__linux__)
bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
  for (const XrExtensionProperties& extension : extensions) {
    if (std::string(extension.extensionName) == name) {
      return true;
    }
  }

  return false;
}

bool QueryOpenXRExtensions(RuntimeInfo* info) {
  if (info == nullptr) {
    return false;
  }

#if defined(_WIN32)
  HMODULE loader_module = openxrwin::LoadOpenXRLoaderModule();
  if (loader_module == nullptr) {
    info->probe_mode = "openxr-loader-load-failed";
    return false;
  }

  const auto enumerate_extensions = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(
    GetProcAddress(loader_module, "xrEnumerateInstanceExtensionProperties")
  );
  if (enumerate_extensions == nullptr) {
    info->probe_mode = "openxr-loader-proc-missing";
    FreeLibrary(loader_module);
    return false;
  }

  uint32_t extension_count = 0;
  const XrResult enumerate_count_result = enumerate_extensions(nullptr, 0, &extension_count, nullptr);
  if (XR_FAILED(enumerate_count_result)) {
    info->probe_mode = "openxr-loader-enumerate-failed";
    FreeLibrary(loader_module);
    return false;
  }

  std::vector<XrExtensionProperties> extensions(extension_count);
  for (XrExtensionProperties& extension : extensions) {
    extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    extension.next = nullptr;
  }

  const XrResult enumerate_result = enumerate_extensions(
    nullptr,
    extension_count,
    &extension_count,
    extensions.data());
  if (XR_FAILED(enumerate_result)) {
    info->probe_mode = "openxr-loader-enumerate-failed";
    FreeLibrary(loader_module);
    return false;
  }

  info->openxr_available = true;
  info->openxr_overlay_extension_available = HasExtension(extensions, XR_EXTX_OVERLAY_EXTENSION_NAME);
  info->openxr_windows_d3d11_binding_available = HasExtension(extensions, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
  info->probe_mode = "openxr-extension-enumeration";
  FreeLibrary(loader_module);
  return true;
#else
  uint32_t extension_count = 0;
  const XrResult enumerate_count_result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
  if (XR_FAILED(enumerate_count_result)) {
    info->probe_mode = "openxr-loader-enumerate-failed";
    return false;
  }

  std::vector<XrExtensionProperties> extensions(extension_count);
  for (XrExtensionProperties& extension : extensions) {
    extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    extension.next = nullptr;
  }

  const XrResult enumerate_result = xrEnumerateInstanceExtensionProperties(
    nullptr,
    extension_count,
    &extension_count,
    extensions.data());
  if (XR_FAILED(enumerate_result)) {
    info->probe_mode = "openxr-loader-enumerate-failed";
    return false;
  }

  info->openxr_available = true;
  info->openxr_overlay_extension_available = HasExtension(extensions, XR_EXTX_OVERLAY_EXTENSION_NAME);
  info->openxr_linux_egl_binding_available = HasExtension(extensions, XR_MNDX_EGL_ENABLE_EXTENSION_NAME);
  info->probe_mode = "openxr-extension-enumeration";
  return true;
#endif
}
#endif

}  // namespace

RuntimeInfo ProbeRuntime() {
  RuntimeInfo info;
  info.platform = DetectPlatform();
  info.probe_mode = "filesystem";

#if defined(_WIN32)
  info.openxr_runtime_name = openxrwin::GetActiveRuntimeName();
  info.openxr_runtime_manifest_path = openxrwin::GetActiveRuntimeManifestPath();
  info.openxr_runtime_library_path = openxrwin::GetActiveRuntimeLibraryPath();
  info.openxr_available = openxrwin::CanLoadOpenXRLoader(&info.openxr_loader_path);
  info.openvr_available = LibraryExists("openvr_api.dll");
#else
  info.openxr_available = LibraryExists("libopenxr_loader.so.1", "libopenxr_loader.so");
  info.openvr_available = LibraryExists("libopenvr_api.so", "openvr_api.so");
#endif

  info.openvr_runtime_path = DetectOpenVRRuntimePath(&info.openvr_runtime_installed);
  QueryOpenVRSceneApplication(&info);

#if defined(__linux__)
  if (info.openxr_available) {
    const bool queried_extensions = QueryOpenXRExtensions(&info);
    if (!queried_extensions) {
      info.openxr_available = false;
      info.openxr_overlay_extension_available = false;
      info.openxr_linux_egl_binding_available = false;
    }
  }

  const bool openxr_ready = info.openxr_available && info.openxr_overlay_extension_available && info.openxr_linux_egl_binding_available;
  const bool openxr_disabled_by_env = IsTruthyEnvVar("ELECTRON_VR_DISABLE_OPENXR");
  const bool openxr_enabled = openxr_ready && !openxr_disabled_by_env;

  if (openxr_enabled) {
    info.selected_backend = BackendKind::kOpenXR;
    AppendProbeMode(&info, "selected-openxr");
  } else if (info.openvr_available && info.openvr_runtime_installed) {
    info.selected_backend = BackendKind::kOpenVR;
    if (openxr_disabled_by_env) {
      AppendProbeMode(&info, "openxr-disabled-by-env");
    } else if (info.openxr_available && !openxr_ready) {
      AppendProbeMode(&info, "openxr-missing-overlay-or-egl");
    }
    AppendProbeMode(&info, "selected-openvr");
  } else {
    info.selected_backend = BackendKind::kMock;
    if (openxr_disabled_by_env) {
      AppendProbeMode(&info, "openxr-disabled-by-env");
    } else if (info.openxr_available && !openxr_ready) {
      AppendProbeMode(&info, "openxr-missing-overlay-or-egl");
    }
    if (!info.openvr_runtime_installed) {
      AppendProbeMode(&info, "openvr-runtime-not-installed");
    } else if (!info.openvr_available) {
      AppendProbeMode(&info, "openvr-library-unavailable");
    }
    AppendProbeMode(&info, "selected-mock");
  }
#elif defined(_WIN32)
  if (info.openxr_available) {
    const bool queried_extensions = QueryOpenXRExtensions(&info);
    if (!queried_extensions) {
      info.openxr_available = false;
      info.openxr_overlay_extension_available = false;
      info.openxr_windows_d3d11_binding_available = false;
    }
  }

  info.openxr_linux_egl_binding_available = false;

  const bool openxr_ready = info.openxr_available && info.openxr_overlay_extension_available && info.openxr_windows_d3d11_binding_available;
  const bool openxr_disabled_by_env = IsTruthyEnvVar("ELECTRON_VR_DISABLE_OPENXR");
  const bool openxr_enabled_by_env = !openxr_disabled_by_env && IsTruthyEnvVar("ELECTRON_VR_ENABLE_OPENXR");

  if (openxr_enabled_by_env && openxr_ready) {
    info.selected_backend = BackendKind::kOpenXR;
    AppendProbeMode(&info, "openxr-enabled-by-env");
    AppendProbeMode(&info, "selected-openxr");
  } else if (info.openvr_available && info.openvr_runtime_installed) {
    info.selected_backend = BackendKind::kOpenVR;
    if (openxr_disabled_by_env) {
      AppendProbeMode(&info, "openxr-disabled-by-env");
    } else if (openxr_enabled_by_env && !openxr_ready) {
      AppendProbeMode(&info, "openxr-enable-requested-but-unavailable");
    } else if (openxr_ready) {
      AppendProbeMode(&info, "openxr-available-not-default");
    } else if (info.openxr_available) {
      AppendProbeMode(&info, "openxr-missing-overlay-or-d3d11");
    }
    AppendProbeMode(&info, "selected-openvr");
  } else if (openxr_enabled_by_env && openxr_ready) {
    info.selected_backend = BackendKind::kOpenXR;
    AppendProbeMode(&info, "openxr-enabled-by-env");
    AppendProbeMode(&info, "selected-openxr");
  } else {
    info.selected_backend = BackendKind::kMock;
    if (openxr_disabled_by_env) {
      AppendProbeMode(&info, "openxr-disabled-by-env");
    } else if (openxr_enabled_by_env && !openxr_ready) {
      AppendProbeMode(&info, "openxr-enable-requested-but-unavailable");
    } else if (openxr_ready) {
      AppendProbeMode(&info, "openxr-available-not-default");
    } else if (info.openxr_available) {
      AppendProbeMode(&info, "openxr-missing-overlay-or-d3d11");
    }
    if (!info.openvr_runtime_installed) {
      AppendProbeMode(&info, "openvr-runtime-not-installed");
    } else if (!info.openvr_available) {
      AppendProbeMode(&info, "openvr-library-unavailable");
    }
    AppendProbeMode(&info, "selected-mock");
  }
#else
  info.openxr_overlay_extension_available = false;
  info.openxr_linux_egl_binding_available = false;
  info.openxr_windows_d3d11_binding_available = false;

  if (info.openvr_available && info.openvr_runtime_installed) {
    info.selected_backend = BackendKind::kOpenVR;
    AppendProbeMode(&info, "selected-openvr");
  } else {
    info.selected_backend = BackendKind::kMock;
    if (!info.openvr_runtime_installed) {
      AppendProbeMode(&info, "openvr-runtime-not-installed");
    } else if (!info.openvr_available) {
      AppendProbeMode(&info, "openvr-library-unavailable");
    }
    AppendProbeMode(&info, "selected-mock");
  }
#endif

  return info;
}

const char* BackendKindToString(BackendKind kind) {
  switch (kind) {
    case BackendKind::kOpenXR:
      return "openxr";
    case BackendKind::kOpenVR:
      return "openvr";
    case BackendKind::kMock:
      return "mock";
    case BackendKind::kNone:
    default:
      return "none";
  }
}

}  // namespace vrbridge
