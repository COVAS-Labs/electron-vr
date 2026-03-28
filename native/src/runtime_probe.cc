#include "runtime_probe.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
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

}  // namespace

RuntimeInfo ProbeRuntime() {
  RuntimeInfo info;
  info.platform = DetectPlatform();

#if defined(_WIN32)
  info.openxr_available = LibraryExists("openxr_loader.dll");
  info.openvr_available = LibraryExists("openvr_api.dll");
#else
  info.openxr_available = LibraryExists("libopenxr_loader.so.1", "libopenxr_loader.so");
  info.openvr_available = LibraryExists("libopenvr_api.so", "openvr_api.so");
#endif

  info.openxr_overlay_extension_available = false;

  if (info.openxr_available) {
    info.selected_backend = BackendKind::kOpenXR;
  } else if (info.openvr_available) {
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
