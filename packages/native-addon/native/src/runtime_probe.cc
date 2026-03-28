#include "runtime_probe.h"

#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "openvr.h"

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

std::string DetectOpenVRRuntimePath(bool* installed) {
  if (installed != nullptr) {
    *installed = false;
  }

  if (!vr::VR_IsRuntimeInstalled()) {
    return std::string();
  }

  if (installed != nullptr) {
    *installed = true;
  }

  uint32_t required_size = 0;
  if (!vr::VR_GetRuntimePath(nullptr, 0, &required_size) || required_size == 0) {
    return std::string();
  }

  std::vector<char> buffer(required_size, '\0');
  if (!vr::VR_GetRuntimePath(buffer.data(), required_size, &required_size)) {
    return std::string();
  }

  return std::string(buffer.data());
}

}  // namespace

RuntimeInfo ProbeRuntime() {
  RuntimeInfo info;
  info.platform = DetectPlatform();
  info.probe_mode = "native";

#if defined(_WIN32)
  info.openxr_available = LibraryExists("openxr_loader.dll");
  info.openvr_available = LibraryExists("openvr_api.dll");
#else
  info.openxr_available = LibraryExists("libopenxr_loader.so.1", "libopenxr_loader.so");
  info.openvr_available = LibraryExists("libopenvr_api.so", "openvr_api.so");
#endif

  info.openxr_overlay_extension_available = false;
  info.openvr_runtime_path = DetectOpenVRRuntimePath(&info.openvr_runtime_installed);

  if (info.openvr_available && info.openvr_runtime_installed) {
    info.selected_backend = BackendKind::kOpenVR;
  } else {
    info.selected_backend = BackendKind::kMock;
  }

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
