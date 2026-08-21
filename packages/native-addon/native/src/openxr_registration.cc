#include "openxr_registration.h"

#if defined(_WIN32)
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace vrbridge {
namespace {

constexpr wchar_t kRegistryPath[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
constexpr wchar_t kManifestName[] = L"electron_vr_openxr_layer.json";
constexpr wchar_t kDllName[] = L"electron_vr_openxr_layer.dll";
constexpr wchar_t kProtocolName[] = L"protocol.json";

std::filesystem::path InstallDirectory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  return length == 0 || length >= buffer.size()
    ? std::filesystem::path()
    : std::filesystem::path(std::wstring(buffer.data(), length)) / L"ElectronVR" / L"OpenXR";
}

std::filesystem::path ManifestPath() { return InstallDirectory() / kManifestName; }

bool SetRegistration(DWORD value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  const std::wstring path = std::filesystem::absolute(ManifestPath()).wstring();
  const LONG result = RegSetValueExW(key, path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool ReadRegistration(DWORD* value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  const std::wstring path = std::filesystem::absolute(ManifestPath()).wstring();
  DWORD type = 0;
  DWORD size = sizeof(*value);
  const LONG result = RegQueryValueExW(key, path.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
  RegCloseKey(key);
  return result == ERROR_SUCCESS && type == REG_DWORD;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return stream ? std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()) : std::string();
}

bool FilesMatch(const std::filesystem::path& first, const std::filesystem::path& second) {
  std::error_code error;
  if (!std::filesystem::exists(first) || !std::filesystem::exists(second) ||
      std::filesystem::file_size(first, error) != std::filesystem::file_size(second, error) || error) return false;
  return ReadFile(first) == ReadFile(second);
}

std::string VersionedDllName(const std::filesystem::path& source) {
  std::ifstream stream(source, std::ios::binary);
  uint64_t hash = 14695981039346656037ull;
  char buffer[64 * 1024];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    for (std::streamsize index = 0; index < stream.gcount(); ++index) {
      hash ^= static_cast<unsigned char>(buffer[index]);
      hash *= 1099511628211ull;
    }
  }
  std::ostringstream name;
  name << "electron_vr_openxr_layer_" << std::hex << hash << ".dll";
  return name.str();
}

std::string ManifestLibraryName() {
  const std::string contents = ReadFile(ManifestPath());
  const size_t key = contents.find("\"library_path\"");
  const size_t colon = key == std::string::npos ? std::string::npos : contents.find(':', key);
  const size_t start = colon == std::string::npos ? std::string::npos : contents.find('"', colon + 1);
  const size_t end = start == std::string::npos ? std::string::npos : contents.find('"', start + 1);
  if (start == std::string::npos || end == std::string::npos) return {};
  const std::string value = contents.substr(start + 1, end - start - 1);
  const size_t separator = value.find_last_of("\\/");
  return separator == std::string::npos ? value : value.substr(separator + 1);
}

bool WriteManifest(const std::filesystem::path& source, const std::string& dll_name) {
  std::string contents = ReadFile(source);
  const std::string original = "electron_vr_openxr_layer.dll";
  const size_t position = contents.find(original);
  if (position == std::string::npos) return false;
  contents.replace(position, original.size(), dll_name);
  std::ofstream stream(ManifestPath(), std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return stream.good();
}

void SetError(std::string* error, const std::string& message) { if (error) *error = message; }

}  // namespace

OpenXRApiLayerStatus GetOpenXRApiLayerStatus(const std::string& source_directory) {
  OpenXRApiLayerStatus status;
  DWORD value = 1;
  status.registered = ReadRegistration(&value);
  status.enabled = status.registered && value == 0;
  status.manifest_path = ManifestPath().string();
  const std::filesystem::path source(source_directory);
  const std::string installed_name = ManifestLibraryName();
  const std::filesystem::path installed_dll = InstallDirectory() / installed_name;
  status.installed = std::filesystem::exists(ManifestPath()) && !installed_name.empty() && std::filesystem::exists(installed_dll);
  const std::string expected_name = VersionedDllName(source / kDllName);
  status.requires_update = status.installed && (installed_name != expected_name ||
    !FilesMatch(source / kDllName, installed_dll) || !FilesMatch(source / kProtocolName, InstallDirectory() / kProtocolName));
  return status;
}

bool InstallOpenXRApiLayer(const std::string& source_directory, std::string* error) {
  const std::filesystem::path source(source_directory);
  const std::filesystem::path target = InstallDirectory();
  std::error_code filesystem_error;
  std::filesystem::create_directories(target, filesystem_error);
  const std::string dll_name = VersionedDllName(source / kDllName);
  std::filesystem::copy_file(source / kDllName, target / dll_name, std::filesystem::copy_options::overwrite_existing, filesystem_error);
  if (filesystem_error || !WriteManifest(source / kManifestName, dll_name)) {
    SetError(error, "Failed to copy the OpenXR API-layer assets.");
    return false;
  }
  std::filesystem::copy_file(source / kProtocolName, target / kProtocolName, std::filesystem::copy_options::overwrite_existing, filesystem_error);
  if (filesystem_error || !SetRegistration(0)) {
    SetError(error, "Failed to register the OpenXR API layer.");
    return false;
  }
  if (error) error->clear();
  return true;
}

bool SetOpenXRApiLayerEnabled(bool enabled, std::string* error) {
  if (!std::filesystem::exists(ManifestPath()) || !SetRegistration(enabled ? 0 : 1)) {
    SetError(error, "Failed to update OpenXR API-layer registration.");
    return false;
  }
  if (error) error->clear();
  return true;
}

bool UninstallOpenXRApiLayer(std::string* error) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
    const std::wstring path = std::filesystem::absolute(ManifestPath()).wstring();
    RegDeleteValueW(key, path.c_str());
    RegCloseKey(key);
  }
  std::error_code filesystem_error;
  std::filesystem::remove_all(InstallDirectory(), filesystem_error);
  if (filesystem_error) {
    SetError(error, "Failed to remove the installed OpenXR API-layer files.");
    return false;
  }
  if (error) error->clear();
  return true;
}

}  // namespace vrbridge
#endif
