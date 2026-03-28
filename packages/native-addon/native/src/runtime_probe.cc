#include "runtime_probe.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
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

}  // namespace

RuntimeInfo ProbeRuntime() {
  RuntimeInfo info;
  info.platform = DetectPlatform();
  info.probe_mode = "filesystem";

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
