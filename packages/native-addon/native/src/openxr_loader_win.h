#pragma once

#if defined(_WIN32)

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

namespace vrbridge::openxrwin {

inline std::string NormalizeWindowsSeparators(const std::string& path) {
  std::string normalized = path;
  for (char& character : normalized) {
    if (character == '/') {
      character = '\\';
    }
  }
  return normalized;
}

inline std::string NormalizeWindowsPath(const std::string& path) {
  if (path.empty()) {
    return std::string();
  }

  const std::string normalized_separators = NormalizeWindowsSeparators(path);
  const DWORD required_size = GetFullPathNameA(normalized_separators.c_str(), 0, nullptr, nullptr);
  if (required_size == 0) {
    return normalized_separators;
  }

  std::string full_path(static_cast<size_t>(required_size), '\0');
  const DWORD written = GetFullPathNameA(normalized_separators.c_str(), required_size, full_path.data(), nullptr);
  if (written == 0) {
    return normalized_separators;
  }

  full_path.resize(static_cast<size_t>(written));
  return full_path;
}

inline bool FileExists(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  const DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool DirectoryExists(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  const DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::string JoinPath(const std::string& left, const std::string& right) {
  if (left.empty()) {
    return right;
  }
  if (right.empty()) {
    return left;
  }
  if (left.back() == '\\' || left.back() == '/') {
    return NormalizeWindowsSeparators(left + right);
  }
  return NormalizeWindowsSeparators(left + "\\" + right);
}

inline std::string DirectoryName(const std::string& path) {
  const size_t separator_index = path.find_last_of("\\/");
  if (separator_index == std::string::npos) {
    return std::string();
  }
  return path.substr(0, separator_index);
}

inline std::string ReadTextFile(const std::string& path) {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return std::string();
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

inline std::string UnescapeJsonString(const std::string& value) {
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

inline std::string ExtractJsonStringValue(const std::string& json_text, const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return std::string();
  }

  const std::string key_token = std::string("\"") + key + "\"";
  const size_t key_index = json_text.find(key_token);
  if (key_index == std::string::npos) {
    return std::string();
  }

  const size_t colon_index = json_text.find(':', key_index + key_token.size());
  if (colon_index == std::string::npos) {
    return std::string();
  }

  const size_t string_start = json_text.find('"', colon_index + 1);
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

inline std::string GetEnvironmentVariableString(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return std::string();
  }

  const DWORD required_size = GetEnvironmentVariableA(name, nullptr, 0);
  if (required_size == 0) {
    return std::string();
  }

  std::string value(static_cast<size_t>(required_size), '\0');
  const DWORD written = GetEnvironmentVariableA(name, value.data(), required_size);
  if (written == 0) {
    return std::string();
  }

  value.resize(static_cast<size_t>(written));
  return value;
}

inline std::string ExpandEnvironmentVariables(const std::string& value) {
  if (value.empty()) {
    return std::string();
  }

  const DWORD required_size = ExpandEnvironmentStringsA(value.c_str(), nullptr, 0);
  if (required_size == 0) {
    return value;
  }

  std::string expanded(static_cast<size_t>(required_size), '\0');
  const DWORD written = ExpandEnvironmentStringsA(value.c_str(), expanded.data(), required_size);
  if (written == 0) {
    return value;
  }

  if (!expanded.empty() && expanded.back() == '\0') {
    expanded.pop_back();
  }
  return expanded;
}

inline std::string ReadRegistryString(HKEY hive, const char* subkey, const char* value_name) {
  if (subkey == nullptr || value_name == nullptr) {
    return std::string();
  }

  HKEY key_handle = nullptr;
  if (RegOpenKeyExA(hive, subkey, 0, KEY_READ, &key_handle) != ERROR_SUCCESS) {
    return std::string();
  }

  DWORD value_type = 0;
  DWORD value_size = 0;
  const LONG query_result = RegQueryValueExA(key_handle, value_name, nullptr, &value_type, nullptr, &value_size);
  if (query_result != ERROR_SUCCESS || value_size == 0 || (value_type != REG_SZ && value_type != REG_EXPAND_SZ)) {
    RegCloseKey(key_handle);
    return std::string();
  }

  std::string value(static_cast<size_t>(value_size), '\0');
  const LONG read_result = RegQueryValueExA(
    key_handle,
    value_name,
    nullptr,
    &value_type,
    reinterpret_cast<LPBYTE>(value.data()),
    &value_size);
  RegCloseKey(key_handle);
  if (read_result != ERROR_SUCCESS) {
    return std::string();
  }

  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }

  if (value_type == REG_EXPAND_SZ) {
    return ExpandEnvironmentVariables(value);
  }

  return value;
}

inline bool IsAbsoluteWindowsPath(const std::string& path) {
  return path.size() >= 2 && path[1] == ':';
}

inline bool IsUncPath(const std::string& path) {
  return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
}

inline std::string ResolvePathRelativeTo(const std::string& base_dir, const std::string& path) {
  if (path.empty()) {
    return std::string();
  }
  if (IsAbsoluteWindowsPath(path) || IsUncPath(path)) {
    return NormalizeWindowsPath(path);
  }
  return NormalizeWindowsPath(JoinPath(base_dir, path));
}

inline std::string NormalizeLoaderCandidate(const std::string& path) {
  if (path.empty()) {
    return std::string();
  }
  if (DirectoryExists(path)) {
    return NormalizeWindowsPath(JoinPath(path, "openxr_loader.dll"));
  }
  return NormalizeWindowsPath(path);
}

inline void AppendUnique(std::vector<std::string>* values, const std::string& value) {
  if (values == nullptr || value.empty()) {
    return;
  }

  for (const std::string& existing : *values) {
    if (existing == value) {
      return;
    }
  }

  values->push_back(value);
}

inline std::string GetActiveRuntimeManifestPath() {
  const std::string env_override = GetEnvironmentVariableString("XR_RUNTIME_JSON");
  if (!env_override.empty()) {
    return NormalizeWindowsPath(env_override);
  }

  const std::string current_user_runtime = ReadRegistryString(HKEY_CURRENT_USER, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime");
  if (!current_user_runtime.empty()) {
    return NormalizeWindowsPath(current_user_runtime);
  }

  return NormalizeWindowsPath(ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime"));
}

inline std::string GetActiveRuntimeLibraryPath() {
  const std::string manifest_path = GetActiveRuntimeManifestPath();
  if (manifest_path.empty()) {
    return std::string();
  }

  const std::string manifest_contents = ReadTextFile(manifest_path);
  if (manifest_contents.empty()) {
    return std::string();
  }

  const std::string library_path = ExtractJsonStringValue(manifest_contents, "library_path");
  if (library_path.empty()) {
    return std::string();
  }

  return ResolvePathRelativeTo(DirectoryName(manifest_path), library_path);
}

inline std::string GetActiveRuntimeName() {
  const std::string manifest_path = GetActiveRuntimeManifestPath();
  if (manifest_path.empty()) {
    return std::string();
  }

  const std::string manifest_contents = ReadTextFile(manifest_path);
  if (manifest_contents.empty()) {
    return std::string();
  }

  return ExtractJsonStringValue(manifest_contents, "name");
}

inline std::string GetSteamVrLoaderPathFromRegistry() {
  const std::string current_user_steam = ReadRegistryString(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", "SteamPath");
  if (!current_user_steam.empty()) {
    return NormalizeWindowsPath(JoinPath(current_user_steam, "steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll"));
  }

  const std::string local_machine_steam = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
  if (!local_machine_steam.empty()) {
    return NormalizeWindowsPath(JoinPath(local_machine_steam, "steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll"));
  }

  return std::string();
}

inline std::vector<std::string> CollectOpenXRLoaderCandidates() {
  std::vector<std::string> candidates;

  AppendUnique(&candidates, NormalizeLoaderCandidate(GetEnvironmentVariableString("ELECTRON_VR_OPENXR_LOADER_PATH")));
  AppendUnique(&candidates, NormalizeLoaderCandidate(GetEnvironmentVariableString("OPENXR_LOADER_PATH")));

  const std::string active_runtime_manifest_path = GetActiveRuntimeManifestPath();
  if (!active_runtime_manifest_path.empty()) {
    const std::string manifest_dir = DirectoryName(active_runtime_manifest_path);
    const std::string manifest_parent_dir = DirectoryName(manifest_dir);
    AppendUnique(&candidates, JoinPath(manifest_dir, "openxr_loader.dll"));
    AppendUnique(&candidates, JoinPath(manifest_dir, "bin\\win64\\openxr_loader.dll"));
    if (!manifest_parent_dir.empty()) {
      AppendUnique(&candidates, JoinPath(manifest_parent_dir, "openxr_loader.dll"));
      AppendUnique(&candidates, JoinPath(manifest_parent_dir, "bin\\win64\\openxr_loader.dll"));
    }
  }

  const std::string active_runtime_library_path = GetActiveRuntimeLibraryPath();
  if (!active_runtime_library_path.empty()) {
    const std::string runtime_library_dir = DirectoryName(active_runtime_library_path);
    const std::string runtime_library_parent_dir = DirectoryName(runtime_library_dir);
    AppendUnique(&candidates, JoinPath(runtime_library_dir, "openxr_loader.dll"));
    AppendUnique(&candidates, JoinPath(runtime_library_dir, "bin\\win64\\openxr_loader.dll"));
    if (!runtime_library_parent_dir.empty()) {
      AppendUnique(&candidates, JoinPath(runtime_library_parent_dir, "openxr_loader.dll"));
      AppendUnique(&candidates, JoinPath(runtime_library_parent_dir, "bin\\win64\\openxr_loader.dll"));
    }
  }

  const std::string steam_loader_path = GetSteamVrLoaderPathFromRegistry();
  if (!steam_loader_path.empty()) {
    AppendUnique(&candidates, steam_loader_path);
  }

  const std::string program_files_x86 = GetEnvironmentVariableString("ProgramFiles(x86)");
  if (!program_files_x86.empty()) {
    AppendUnique(&candidates, JoinPath(program_files_x86, "Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll"));
  }

  const std::string program_files = GetEnvironmentVariableString("ProgramFiles");
  if (!program_files.empty()) {
    AppendUnique(&candidates, JoinPath(program_files, "Oculus\\Support\\oculus-runtime\\openxr_loader.dll"));
    AppendUnique(&candidates, JoinPath(program_files, "Virtual Desktop Streamer\\OpenXR\\openxr_loader.dll"));
  }

  return candidates;
}

inline HMODULE LoadOpenXRLoaderModule(std::string* loaded_path = nullptr) {
  HMODULE module = LoadLibraryA("openxr_loader.dll");
  if (module != nullptr) {
    if (loaded_path != nullptr) {
      *loaded_path = "openxr_loader.dll";
    }
    return module;
  }

  const std::vector<std::string> candidates = CollectOpenXRLoaderCandidates();
  for (const std::string& candidate : candidates) {
    if (!FileExists(candidate)) {
      continue;
    }

    module = LoadLibraryExA(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
      module = LoadLibraryA(candidate.c_str());
    }
    if (module != nullptr) {
      if (loaded_path != nullptr) {
        *loaded_path = candidate;
      }
      return module;
    }
  }

  return nullptr;
}

inline bool CanLoadOpenXRLoader(std::string* loaded_path = nullptr) {
  HMODULE module = LoadOpenXRLoaderModule(loaded_path);
  if (module == nullptr) {
    return false;
  }

  FreeLibrary(module);
  return true;
}

}  // namespace vrbridge::openxrwin

#endif
