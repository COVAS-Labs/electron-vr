#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <iostream>
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

int Install() {
  const std::filesystem::path source = ExecutableDirectory();
  const std::filesystem::path target = InstallDirectory();
  if (source.empty() || target.empty()) {
    std::wcerr << L"Could not determine the layer source or per-user installation directory.\n";
    return 1;
  }
  std::error_code error;
  std::filesystem::create_directories(target, error);
  if (error || !CopyAsset(source, target, kDllName) || !CopyAsset(source, target, kManifestName) || !CopyAsset(source, target, kProtocolName)) return 1;
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
  const bool files_present = std::filesystem::exists(InstalledManifestPath()) &&
                             std::filesystem::exists(InstallDirectory() / kDllName);
  std::wcout << L"installed=" << (files_present ? L"true" : L"false") << L"\n";
  std::wcout << L"registered=" << (registered ? L"true" : L"false") << L"\n";
  std::wcout << L"enabled=" << (registered && value == 0 ? L"true" : L"false") << L"\n";
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
