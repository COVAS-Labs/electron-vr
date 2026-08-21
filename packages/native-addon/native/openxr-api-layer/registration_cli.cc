#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRegistryPath[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
constexpr wchar_t kManifestName[] = L"electron_vr_openxr_layer.json";
constexpr wchar_t kDllName[] = L"electron_vr_openxr_layer.dll";
constexpr wchar_t kProtocolName[] = L"protocol.json";

std::filesystem::path ExecutableDirectory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return length == 0 ? std::filesystem::path() : std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::filesystem::path InstallDirectory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length)) / L"ElectronVR" / L"OpenXR";
}

std::filesystem::path InstalledManifestPath() {
  return InstallDirectory() / kManifestName;
}

bool SetRegistration(DWORD value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring path = std::filesystem::absolute(InstalledManifestPath()).wstring();
  const LONG result = RegSetValueExW(
    key, path.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool DeleteRegistration() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
  const std::wstring path = std::filesystem::absolute(InstalledManifestPath()).wstring();
  const LONG result = RegDeleteValueW(key, path.c_str());
  RegCloseKey(key);
  return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
}

bool ReadRegistration(DWORD* value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  const std::wstring path = std::filesystem::absolute(InstalledManifestPath()).wstring();
  DWORD type = 0;
  DWORD size = sizeof(*value);
  const LONG result = RegQueryValueExW(key, path.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
  RegCloseKey(key);
  return result == ERROR_SUCCESS && type == REG_DWORD;
}

bool CopyAsset(const std::filesystem::path& source_directory, const std::filesystem::path& install_directory, const wchar_t* name) {
  std::error_code error;
  std::filesystem::copy_file(source_directory / name, install_directory / name, std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    std::wcerr << L"Failed to copy " << name << L": " << error.message().c_str() << L"\n";
    return false;
  }
  return true;
}

bool CopyAssetTo(const std::filesystem::path& source, const std::filesystem::path& target) {
  std::error_code error;
  std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
  if (error) {
    std::wcerr << L"Failed to copy " << source.filename().wstring() << L": " << error.message().c_str() << L"\n";
    return false;
  }
  return true;
}

bool FilesMatch(const std::filesystem::path& first, const std::filesystem::path& second) {
  std::error_code error;
  if (!std::filesystem::exists(first) || !std::filesystem::exists(second) ||
      std::filesystem::file_size(first, error) != std::filesystem::file_size(second, error) || error) {
    return false;
  }
  std::ifstream first_stream(first, std::ios::binary);
  std::ifstream second_stream(second, std::ios::binary);
  return first_stream && second_stream && std::equal(
    std::istreambuf_iterator<char>(first_stream), std::istreambuf_iterator<char>(),
    std::istreambuf_iterator<char>(second_stream));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return stream ? std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()) : std::string();
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

std::string ManifestLibraryName(const std::filesystem::path& manifest) {
  const std::string contents = ReadFile(manifest);
  const std::string key = "\"library_path\"";
  const size_t key_position = contents.find(key);
  const size_t value_start = key_position == std::string::npos ? std::string::npos : contents.find('"', contents.find(':', key_position) + 1);
  const size_t value_end = value_start == std::string::npos ? std::string::npos : contents.find('"', value_start + 1);
  if (value_start == std::string::npos || value_end == std::string::npos) return {};
  std::string value = contents.substr(value_start + 1, value_end - value_start - 1);
  const size_t separator = value.find_last_of("\\/");
  return separator == std::string::npos ? value : value.substr(separator + 1);
}

bool WriteManifest(const std::filesystem::path& source, const std::filesystem::path& target, const std::string& dll_name) {
  std::string contents = ReadFile(source);
  const std::string original_name = "electron_vr_openxr_layer.dll";
  const size_t position = contents.find(original_name);
  if (position == std::string::npos) return false;
  contents.replace(position, original_name.size(), dll_name);
  std::ofstream stream(target, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return stream.good();
}

int Install() {
  const std::filesystem::path source = ExecutableDirectory();
  const std::filesystem::path target = InstallDirectory();
  if (source.empty() || target.empty()) {
    std::wcerr << L"Could not determine the layer source or per-user installation directory.\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::create_directories(target, error);
  const std::string dll_name = VersionedDllName(source / kDllName);
  const std::filesystem::path versioned_dll = target / dll_name;
  if (error || !CopyAssetTo(source / kDllName, versioned_dll) ||
      !WriteManifest(source / kManifestName, target / kManifestName, dll_name) || !CopyAsset(source, target, kProtocolName)) return 1;
  if (!SetRegistration(0)) {
    std::wcerr << L"Failed to register the implicit API layer in HKCU.\n";
    return 1;
  }
  std::wcout << L"Installed and enabled: " << InstalledManifestPath().wstring() << L"\n";
  return 0;
}

int SetEnabled(bool enabled) {
  if (!std::filesystem::exists(InstalledManifestPath())) {
    std::wcerr << L"The API layer is not installed. Run install first.\n";
    return 1;
  }
  if (!SetRegistration(enabled ? 0 : 1)) {
    std::wcerr << L"Failed to update the implicit API-layer registration.\n";
    return 1;
  }
  std::wcout << (enabled ? L"Enabled.\n" : L"Disabled.\n");
  return 0;
}

int Status() {
  DWORD value = 1;
  const bool registered = ReadRegistration(&value);
  const std::filesystem::path source = ExecutableDirectory();
  const std::string installed_dll_name = ManifestLibraryName(InstalledManifestPath());
  const std::string expected_dll_name = VersionedDllName(source / kDllName);
  const std::filesystem::path installed_dll = InstallDirectory() / installed_dll_name;
  const bool files_present = std::filesystem::exists(InstalledManifestPath()) && !installed_dll_name.empty() &&
                              std::filesystem::exists(installed_dll);
  const bool current = files_present && installed_dll_name == expected_dll_name &&
                       FilesMatch(source / kDllName, installed_dll) &&
                       FilesMatch(source / kProtocolName, InstallDirectory() / kProtocolName);
  std::wcout << L"installed=" << (files_present ? L"true" : L"false") << L"\n";
  std::wcout << L"registered=" << (registered ? L"true" : L"false") << L"\n";
  std::wcout << L"enabled=" << (registered && value == 0 ? L"true" : L"false") << L"\n";
  std::wcout << L"requires_update=" << (files_present && !current ? L"true" : L"false") << L"\n";
  std::wcout << L"manifest=" << InstalledManifestPath().wstring() << L"\n";
  std::wcout << L"scope=current-user (elevated OpenXR applications do not load HKCU layers)\n";
  return 0;
}

int Uninstall() {
  if (!DeleteRegistration()) {
    std::wcerr << L"Failed to remove the implicit API-layer registration.\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::remove_all(InstallDirectory(), error);
  if (error) {
    std::wcerr << L"Registration was removed, but installed files could not be deleted: " << error.message().c_str() << L"\n";
    return 1;
  }
  std::wcout << L"Uninstalled.\n";
  return 0;
}

void PrintUsage() {
  std::wcout << L"Usage: electron_vr_openxr_layer_cli.exe <install|enable|disable|status|uninstall>\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    PrintUsage();
    return 2;
  }
  const std::wstring command = argv[1];
  if (command == L"install") return Install();
  if (command == L"enable") return SetEnabled(true);
  if (command == L"disable") return SetEnabled(false);
  if (command == L"status") return Status();
  if (command == L"uninstall") return Uninstall();
  PrintUsage();
  return 2;
}
