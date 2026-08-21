#include "openxr_companion.h"

#include <cmath>

#if defined(_WIN32)
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <windows.h>

#include "../openxr_api_layer_protocol.h"
#endif

namespace vrbridge {

namespace {

#if defined(_WIN32)
std::filesystem::path ManifestLibraryPath(const std::filesystem::path& manifest) {
  std::ifstream stream(manifest, std::ios::binary);
  if (!stream) return {};
  const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  const size_t key_position = contents.find("\"library_path\"");
  const size_t colon = key_position == std::string::npos ? std::string::npos : contents.find(':', key_position);
  const size_t value_start = colon == std::string::npos ? std::string::npos : contents.find('"', colon + 1);
  const size_t value_end = value_start == std::string::npos ? std::string::npos : contents.find('"', value_start + 1);
  if (value_start == std::string::npos || value_end == std::string::npos) return {};
  std::string value = contents.substr(value_start + 1, value_end - value_start - 1);
  for (size_t position = 0; (position = value.find("\\\\", position)) != std::string::npos;) {
    value.replace(position, 2, "\\");
    ++position;
  }
  return manifest.parent_path() / value;
}
#endif

void SetError(std::string* error_message, const std::string& message) {
  if (error_message != nullptr) {
    *error_message = message;
  }
}

#if defined(_WIN32)

using electron_vr::openxr_layer::AdapterLuid;
using electron_vr::openxr_layer::GraphicsApi;
using electron_vr::openxr_layer::LayerHello;
using electron_vr::openxr_layer::OverlaySnapshot;
using electron_vr::openxr_layer::PlacementMode;
using electron_vr::openxr_layer::kPipeName;
using electron_vr::openxr_layer::kProtocolMagic;
using electron_vr::openxr_layer::kProtocolVersion;
using electron_vr::openxr_layer::kTextureSlotCount;

template <typename T>
void ReleaseCom(T** value) {
  if (value != nullptr && *value != nullptr) {
    (*value)->Release();
    *value = nullptr;
  }
}

std::string HResultString(HRESULT result) {
  std::ostringstream stream;
  stream << "0x" << std::hex << static_cast<unsigned long>(result);
  return stream.str();
}

struct TextureSlotState {
  ID3D11Texture2D* texture = nullptr;
  IDXGIKeyedMutex* keyed_mutex = nullptr;
  ID3D11Fence* fence = nullptr;
  HANDLE local_handle = nullptr;
  HANDLE local_fence_handle = nullptr;
  uint64_t remote_handle = 0;
  uint64_t remote_fence_handle = 0;
  uint64_t fence_value = 1;
  uint64_t sequence = 0;
};

struct CompanionState {
  std::mutex mutex;
  std::thread server_thread;
  std::atomic<bool> stop_requested{false};
  HANDLE stop_event = nullptr;
  bool initialized = false;
  bool connected = false;
  LayerHello hello;
  OverlaySnapshot snapshot;
  ID3D11Device* device = nullptr;
  ID3D11Device1* device1 = nullptr;
  ID3D11Device5* device5 = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11DeviceContext4* context4 = nullptr;
  std::array<TextureSlotState, kTextureSlotCount> slots;
  uint32_t next_slot = 0;
  uint64_t connection_generation = 0;
  uint64_t resource_connection_generation = 0;
  uint64_t source_handle = 0;
  ID3D11Texture2D* source_texture = nullptr;
};

CompanionState g_state;

bool WriteExact(HANDLE pipe, const void* data, DWORD size) {
  const uint8_t* cursor = static_cast<const uint8_t*>(data);
  DWORD remaining = size;
  while (remaining > 0) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    DWORD written = 0;
    BOOL completed = WriteFile(pipe, cursor, remaining, &written, &overlapped);
    if (!completed && GetLastError() == ERROR_IO_PENDING) {
      HANDLE events[] = {overlapped.hEvent, g_state.stop_event};
      if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
        completed = GetOverlappedResult(pipe, &overlapped, &written, FALSE);
      } else {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &written, TRUE);
      }
    }
    CloseHandle(overlapped.hEvent);
    if (!completed || written == 0) return false;
    cursor += written;
    remaining -= written;
  }
  return true;
}

bool ReadExact(HANDLE pipe, void* data, DWORD size) {
  uint8_t* cursor = static_cast<uint8_t*>(data);
  DWORD remaining = size;
  while (remaining > 0) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    DWORD read = 0;
    BOOL completed = ReadFile(pipe, cursor, remaining, &read, &overlapped);
    if (!completed && GetLastError() == ERROR_IO_PENDING) {
      HANDLE events[] = {overlapped.hEvent, g_state.stop_event};
      if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
        completed = GetOverlappedResult(pipe, &overlapped, &read, FALSE);
      } else {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &read, TRUE);
      }
    }
    CloseHandle(overlapped.hEvent);
    if (!completed || read == 0) return false;
    cursor += read;
    remaining -= read;
  }
  return true;
}

PSECURITY_DESCRIPTOR CreatePipeSecurityDescriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return nullptr;
  }

  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> buffer(size);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    CloseHandle(token);
    return nullptr;
  }
  CloseHandle(token);

  const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(token_user->User.Sid, &sid)) {
    return nullptr;
  }

  const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return nullptr;
  }
  return descriptor;
}

void CloseTextureResourcesLocked() {
  ReleaseCom(&g_state.source_texture);
  g_state.source_handle = 0;
  for (TextureSlotState& slot : g_state.slots) {
    ReleaseCom(&slot.keyed_mutex);
    ReleaseCom(&slot.fence);
    ReleaseCom(&slot.texture);
    if (slot.local_handle != nullptr) {
      CloseHandle(slot.local_handle);
      slot.local_handle = nullptr;
    }
    if (slot.local_fence_handle != nullptr) {
      CloseHandle(slot.local_fence_handle);
      slot.local_fence_handle = nullptr;
    }
    slot.remote_handle = 0;
    slot.remote_fence_handle = 0;
    slot.sequence = 0;
  }
  ReleaseCom(&g_state.context);
  ReleaseCom(&g_state.context4);
  ReleaseCom(&g_state.device5);
  ReleaseCom(&g_state.device1);
  ReleaseCom(&g_state.device);
  g_state.snapshot.texture_generation++;
  g_state.snapshot.latest_sequence = 0;
  g_state.snapshot.width = 0;
  g_state.snapshot.height = 0;
  g_state.snapshot.dxgi_format = 0;
  for (auto& slot : g_state.snapshot.slots) {
    slot = {};
  }
}

bool SameLuid(const LUID& left, const AdapterLuid& right) {
  return left.LowPart == right.low_part && left.HighPart == right.high_part;
}

bool CreateDeviceForHostLocked(std::string* error_message) {
  if (g_state.device != nullptr && g_state.resource_connection_generation == g_state.connection_generation) {
    return true;
  }

  CloseTextureResourcesLocked();
  IDXGIFactory1* factory = nullptr;
  HRESULT result = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
  if (FAILED(result)) {
    SetError(error_message, "Failed to create DXGI factory for API-layer transport (" + HResultString(result) + ").");
    return false;
  }

  IDXGIAdapter1* selected_adapter = nullptr;
  for (UINT index = 0; ; ++index) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && SameLuid(desc.AdapterLuid, g_state.hello.adapter_luid)) {
      selected_adapter = adapter;
      break;
    }
    adapter->Release();
  }
  factory->Release();

  if (selected_adapter == nullptr) {
    SetError(error_message, "Could not find the D3D11 adapter used by the connected OpenXR application.");
    return false;
  }

  D3D_FEATURE_LEVEL feature_level{};
  result = D3D11CreateDevice(
    selected_adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    nullptr,
    0,
    D3D11_SDK_VERSION,
    &g_state.device,
    &feature_level,
    &g_state.context);
  selected_adapter->Release();
  if (FAILED(result)) {
    SetError(error_message, "Failed to create a D3D11 device on the OpenXR host adapter (" + HResultString(result) + ").");
    CloseTextureResourcesLocked();
    return false;
  }

  result = g_state.device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_state.device1));
  if (FAILED(result)) {
    SetError(error_message, "The OpenXR host adapter does not expose ID3D11Device1 (" + HResultString(result) + ").");
    CloseTextureResourcesLocked();
    return false;
  }

  if (g_state.hello.graphics_api == GraphicsApi::kD3D12) {
    result = g_state.device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&g_state.device5));
    if (SUCCEEDED(result)) {
      result = g_state.context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_state.context4));
    }
    if (FAILED(result) || g_state.device5 == nullptr || g_state.context4 == nullptr) {
      SetError(error_message, "D3D12 OpenXR transport requires D3D11 fence interoperability.");
      CloseTextureResourcesLocked();
      return false;
    }
  }

  g_state.resource_connection_generation = g_state.connection_generation;
  return true;
}

bool OpenSourceTextureLocked(uint64_t shared_handle, std::string* error_message) {
  if (g_state.source_texture != nullptr && g_state.source_handle == shared_handle) {
    return true;
  }
  ReleaseCom(&g_state.source_texture);
  g_state.source_handle = 0;

  const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(shared_handle));
  HRESULT result = g_state.device1->OpenSharedResource1(
    handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_state.source_texture));
  if (FAILED(result)) {
    result = g_state.device->OpenSharedResource(
      handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_state.source_texture));
  }
  if (FAILED(result) || g_state.source_texture == nullptr) {
    SetError(error_message, "Failed to open Electron's D3D11 shared texture on the OpenXR host adapter (" + HResultString(result) + ").");
    return false;
  }
  g_state.source_handle = shared_handle;
  return true;
}

bool CreateTextureRingLocked(const D3D11_TEXTURE2D_DESC& source_desc, std::string* error_message) {
  if (g_state.snapshot.width == source_desc.Width &&
      g_state.snapshot.height == source_desc.Height &&
      g_state.snapshot.dxgi_format == static_cast<uint32_t>(source_desc.Format) &&
      g_state.slots[0].texture != nullptr) {
    return true;
  }

  for (TextureSlotState& slot : g_state.slots) {
    ReleaseCom(&slot.keyed_mutex);
    ReleaseCom(&slot.fence);
    ReleaseCom(&slot.texture);
    if (slot.local_handle != nullptr) CloseHandle(slot.local_handle);
    if (slot.local_fence_handle != nullptr) CloseHandle(slot.local_fence_handle);
    slot = {};
  }

  HANDLE target_process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, g_state.hello.process_id);
  if (target_process == nullptr) {
    SetError(error_message, "Failed to open the OpenXR host process for shared-handle duplication.");
    return false;
  }

  D3D11_TEXTURE2D_DESC desc = source_desc;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  if (g_state.hello.graphics_api == GraphicsApi::kD3D11) {
    desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  } else {
    desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
  }

  bool success = true;
  const auto close_remote_handle = [&](uint64_t remote_value) {
    if (remote_value == 0) return;
    HANDLE local_duplicate = nullptr;
    if (DuplicateHandle(
          target_process,
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remote_value)),
          GetCurrentProcess(),
          &local_duplicate,
          0,
          FALSE,
          DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS) && local_duplicate != nullptr) {
      CloseHandle(local_duplicate);
    }
  };
  for (uint32_t index = 0; index < kTextureSlotCount; ++index) {
    TextureSlotState& slot = g_state.slots[index];
    HRESULT result = g_state.device->CreateTexture2D(&desc, nullptr, &slot.texture);
    if (FAILED(result)) {
      SetError(error_message, "Failed to create API-layer transport texture (" + HResultString(result) + ").");
      success = false;
      break;
    }
    if (g_state.hello.graphics_api == GraphicsApi::kD3D11) {
      result = slot.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&slot.keyed_mutex));
    }
    IDXGIResource1* resource = nullptr;
    if (SUCCEEDED(result)) {
      result = slot.texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource));
    }
    if (SUCCEEDED(result)) {
      result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &slot.local_handle);
    }
    ReleaseCom(&resource);
    HANDLE remote_handle = nullptr;
    if (SUCCEEDED(result) && !DuplicateHandle(
          GetCurrentProcess(), slot.local_handle, target_process, &remote_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      result = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(result)) {
      SetError(error_message, "Failed to publish API-layer transport texture (" + HResultString(result) + ").");
      success = false;
      break;
    }
    slot.remote_handle = reinterpret_cast<uint64_t>(remote_handle);
    if (g_state.hello.graphics_api == GraphicsApi::kD3D12) {
      result = g_state.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&slot.fence));
      if (SUCCEEDED(result)) {
        result = slot.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &slot.local_fence_handle);
      }
      HANDLE remote_fence_handle = nullptr;
      if (SUCCEEDED(result) && !DuplicateHandle(
            GetCurrentProcess(), slot.local_fence_handle, target_process, &remote_fence_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        result = HRESULT_FROM_WIN32(GetLastError());
      }
      if (FAILED(result)) {
        SetError(error_message, "Failed to publish API-layer transport fence (" + HResultString(result) + ").");
        success = false;
        break;
      }
      slot.remote_fence_handle = reinterpret_cast<uint64_t>(remote_fence_handle);
      slot.fence_value = 1;
    }
  }
  if (!success) {
    for (TextureSlotState& slot : g_state.slots) {
      close_remote_handle(slot.remote_handle);
      close_remote_handle(slot.remote_fence_handle);
      ReleaseCom(&slot.keyed_mutex);
      ReleaseCom(&slot.fence);
      ReleaseCom(&slot.texture);
      if (slot.local_handle != nullptr) CloseHandle(slot.local_handle);
      if (slot.local_fence_handle != nullptr) CloseHandle(slot.local_fence_handle);
      slot = {};
    }
    CloseHandle(target_process);
    return false;
  }
  CloseHandle(target_process);

  g_state.snapshot.texture_generation++;
  g_state.snapshot.width = desc.Width;
  g_state.snapshot.height = desc.Height;
  g_state.snapshot.dxgi_format = static_cast<uint32_t>(desc.Format);
  for (uint32_t index = 0; index < kTextureSlotCount; ++index) {
    g_state.snapshot.slots[index].handle = g_state.slots[index].remote_handle;
    g_state.snapshot.slots[index].fence_handle = g_state.slots[index].remote_fence_handle;
    g_state.snapshot.slots[index].fence_value = 0;
    g_state.snapshot.slots[index].sequence = 0;
  }
  g_state.next_slot = 0;
  return true;
}

void ServerThreadMain() {
  while (!g_state.stop_requested.load()) {
    PSECURITY_DESCRIPTOR descriptor = CreatePipeSecurityDescriptor();
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE pipe = CreateNamedPipeW(
      kPipeName,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,
      sizeof(OverlaySnapshot),
      sizeof(LayerHello),
      0,
      descriptor != nullptr ? &attributes : nullptr);
    if (descriptor != nullptr) LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    OVERLAPPED connect_overlapped{};
    connect_overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = FALSE;
    if (connect_overlapped.hEvent != nullptr) {
      connected = ConnectNamedPipe(pipe, &connect_overlapped);
      if (!connected) {
        const DWORD connect_error = GetLastError();
        if (connect_error == ERROR_PIPE_CONNECTED) {
          connected = TRUE;
        } else if (connect_error == ERROR_IO_PENDING) {
          HANDLE events[] = {connect_overlapped.hEvent, g_state.stop_event};
          const DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
          if (wait_result == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            connected = GetOverlappedResult(pipe, &connect_overlapped, &transferred, FALSE);
          } else {
            CancelIoEx(pipe, &connect_overlapped);
            DWORD transferred = 0;
            GetOverlappedResult(pipe, &connect_overlapped, &transferred, TRUE);
          }
        }
      }
      CloseHandle(connect_overlapped.hEvent);
    }
    if (!connected || g_state.stop_requested.load()) {
      CloseHandle(pipe);
      continue;
    }

    LayerHello hello{};
    if (!ReadExact(pipe, &hello, sizeof(hello)) ||
        hello.magic != kProtocolMagic || hello.version != kProtocolVersion ||
        hello.size != sizeof(LayerHello) ||
        (hello.graphics_api != GraphicsApi::kD3D11 && hello.graphics_api != GraphicsApi::kD3D12)) {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      continue;
    }

    uint64_t sent_revision = UINT64_MAX;
    {
      std::lock_guard<std::mutex> lock(g_state.mutex);
      g_state.hello = hello;
      g_state.connected = true;
      g_state.connection_generation++;
      g_state.snapshot.revision++;
    }

    while (!g_state.stop_requested.load()) {
      OverlaySnapshot snapshot{};
      {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        snapshot = g_state.snapshot;
      }
      if (snapshot.revision != sent_revision) {
        if (!WriteExact(pipe, &snapshot, sizeof(snapshot))) break;
        sent_revision = snapshot.revision;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    {
      std::lock_guard<std::mutex> lock(g_state.mutex);
      g_state.connected = false;
      g_state.snapshot.revision++;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void ApplyOptionsLocked(const InitializeOptions& options) {
  g_state.snapshot.visible = options.visible ? 1U : 0U;
  g_state.snapshot.size_meters = options.size_meters;
  g_state.snapshot.curvature = options.curvature;
  g_state.snapshot.placement_mode = options.placement.mode == OverlayPlacementMode::kHead
    ? PlacementMode::kHead : PlacementMode::kWorld;
  g_state.snapshot.position[0] = options.placement.position.x;
  g_state.snapshot.position[1] = options.placement.position.y;
  g_state.snapshot.position[2] = options.placement.position.z;
  g_state.snapshot.rotation[0] = options.placement.rotation.x;
  g_state.snapshot.rotation[1] = options.placement.rotation.y;
  g_state.snapshot.rotation[2] = options.placement.rotation.z;
  g_state.snapshot.rotation[3] = options.placement.rotation.w;
  g_state.snapshot.revision++;
}

#endif

}  // namespace

bool IsOpenXRApiLayerInstalled(bool* enabled, std::string* manifest_path) {
#if defined(_WIN32)
  if (enabled != nullptr) *enabled = false;
  if (manifest_path != nullptr) manifest_path->clear();
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD index = 0;
  bool found = false;
  while (true) {
    wchar_t name[32768] = {};
    DWORD name_size = static_cast<DWORD>(std::size(name));
    DWORD type = 0;
    DWORD value = 1;
    DWORD value_size = sizeof(value);
    const LONG result = RegEnumValueW(key, index++, name, &name_size, nullptr, &type, reinterpret_cast<BYTE*>(&value), &value_size);
    if (result == ERROR_NO_MORE_ITEMS) break;
    if (result != ERROR_SUCCESS || type != REG_DWORD) continue;
    const std::wstring path(name, name_size);
    if (path.find(L"electron_vr_openxr_layer.json") == std::wstring::npos) continue;
    const std::filesystem::path manifest(path);
    if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(ManifestLibraryPath(manifest).c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    found = true;
    if (enabled != nullptr) *enabled = value == 0;
    if (manifest_path != nullptr) {
      const int length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
      manifest_path->resize(length);
      WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), manifest_path->data(), length, nullptr, nullptr);
    }
    break;
  }
  RegCloseKey(key);
  return found;
#else
  if (enabled != nullptr) *enabled = false;
  if (manifest_path != nullptr) manifest_path->clear();
  return false;
#endif
}

bool InitializeOpenXRCompanion(const InitializeOptions& options, std::string* error_message) {
#if defined(_WIN32)
  if (options.curvature != 0.0f) {
    SetError(error_message, "Curvature is not supported by the first API-layer milestone.");
    return false;
  }
  ShutdownOpenXRCompanion();
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.snapshot = {};
    ApplyOptionsLocked(options);
    g_state.initialized = true;
    g_state.stop_requested.store(false);
    g_state.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_state.stop_event == nullptr) {
      g_state.initialized = false;
      SetError(error_message, "Failed to create the API-layer companion shutdown event.");
      return false;
    }
  }
  g_state.server_thread = std::thread(ServerThreadMain);
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)options;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

bool SubmitOpenXRCompanionFrame(uint64_t shared_handle, std::string* error_message) {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.initialized) {
    SetError(error_message, "OpenXR API-layer companion is not initialized.");
    return false;
  }
  if (!g_state.connected) {
    SetError(error_message, "Waiting for a D3D11 or D3D12 OpenXR application to connect to the API-layer companion.");
    return false;
  }
  if (!CreateDeviceForHostLocked(error_message) || !OpenSourceTextureLocked(shared_handle, error_message)) {
    return false;
  }
  D3D11_TEXTURE2D_DESC source_desc{};
  g_state.source_texture->GetDesc(&source_desc);
  if (source_desc.Width == 0 || source_desc.Height == 0 || source_desc.SampleDesc.Count != 1) {
    SetError(error_message, "Electron shared texture has unsupported dimensions or multisampling.");
    return false;
  }
  if (source_desc.MipLevels != 1 || source_desc.ArraySize != 1) {
    SetError(error_message, "Electron shared texture must contain one mip level and one array slice.");
    return false;
  }
  if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
      source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
      source_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      source_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
    SetError(error_message, "Electron shared texture must use a BGRA8 or RGBA8 format for the D3D11 API-layer transport.");
    return false;
  }
  if (!CreateTextureRingLocked(source_desc, error_message)) return false;

  TextureSlotState& slot = g_state.slots[g_state.next_slot];
  if (g_state.hello.graphics_api == GraphicsApi::kD3D11) {
    const HRESULT acquire_result = slot.keyed_mutex->AcquireSync(0, 0);
    if (acquire_result == WAIT_TIMEOUT) {
      g_state.next_slot = (g_state.next_slot + 1) % kTextureSlotCount;
      if (error_message != nullptr) error_message->clear();
      return true;
    }
    if (FAILED(acquire_result)) {
      SetError(error_message, "Failed to acquire API-layer transport texture (" + HResultString(acquire_result) + ").");
      return false;
    }
  } else if (slot.fence->GetCompletedValue() < slot.fence_value - 1) {
    g_state.next_slot = (g_state.next_slot + 1) % kTextureSlotCount;
    if (error_message != nullptr) error_message->clear();
    return true;
  }
  g_state.context->CopyResource(slot.texture, g_state.source_texture);
  if (g_state.hello.graphics_api == GraphicsApi::kD3D12) {
    const HRESULT signal_result = g_state.context4->Signal(slot.fence, slot.fence_value);
    g_state.context->Flush();
    if (FAILED(signal_result)) {
      SetError(error_message, "Failed to signal the D3D12 transport fence (" + HResultString(signal_result) + ").");
      return false;
    }
  } else {
    g_state.context->Flush();
    const HRESULT release_result = slot.keyed_mutex->ReleaseSync(1);
    if (FAILED(release_result)) {
      SetError(error_message, "Failed to release the D3D11 transport texture (" + HResultString(release_result) + ").");
      return false;
    }
  }

  const uint64_t sequence = ++g_state.snapshot.latest_sequence;
  slot.sequence = sequence;
  g_state.snapshot.latest_slot = g_state.next_slot;
  g_state.snapshot.slots[g_state.next_slot].sequence = sequence;
  g_state.snapshot.slots[g_state.next_slot].fence_value =
    g_state.hello.graphics_api == GraphicsApi::kD3D12 ? slot.fence_value : 0;
  if (g_state.hello.graphics_api == GraphicsApi::kD3D12) slot.fence_value += 2;
  g_state.snapshot.revision++;
  g_state.next_slot = (g_state.next_slot + 1) % kTextureSlotCount;
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)shared_handle;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

bool SetOpenXRCompanionPlacement(const OverlayPlacement& placement, std::string* error_message) {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.snapshot.placement_mode = placement.mode == OverlayPlacementMode::kHead ? PlacementMode::kHead : PlacementMode::kWorld;
  g_state.snapshot.position[0] = placement.position.x;
  g_state.snapshot.position[1] = placement.position.y;
  g_state.snapshot.position[2] = placement.position.z;
  g_state.snapshot.rotation[0] = placement.rotation.x;
  g_state.snapshot.rotation[1] = placement.rotation.y;
  g_state.snapshot.rotation[2] = placement.rotation.z;
  g_state.snapshot.rotation[3] = placement.rotation.w;
  g_state.snapshot.revision++;
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)placement;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

bool SetOpenXRCompanionVisible(bool visible, std::string* error_message) {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.snapshot.visible = visible ? 1U : 0U;
  g_state.snapshot.revision++;
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)visible;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

bool SetOpenXRCompanionSizeMeters(float size_meters, std::string* error_message) {
#if defined(_WIN32)
  if (!std::isfinite(size_meters) || size_meters <= 0.0f) {
    SetError(error_message, "Overlay size must be greater than zero.");
    return false;
  }
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.snapshot.size_meters = size_meters;
  g_state.snapshot.revision++;
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)size_meters;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

bool SetOpenXRCompanionCurvature(float curvature, std::string* error_message) {
#if defined(_WIN32)
  if (curvature != 0.0f) {
    SetError(error_message, "Curvature is not supported by the first API-layer milestone.");
    return false;
  }
  std::lock_guard<std::mutex> lock(g_state.mutex);
  g_state.snapshot.curvature = curvature;
  g_state.snapshot.revision++;
  if (error_message != nullptr) error_message->clear();
  return true;
#else
  (void)curvature;
  SetError(error_message, "The OpenXR API-layer companion is available only on Windows.");
  return false;
#endif
}

void PopulateOpenXRCompanionRuntimeInfo(RuntimeInfo* runtime_info) {
#if defined(_WIN32)
  if (runtime_info == nullptr) return;
  std::lock_guard<std::mutex> lock(g_state.mutex);
  runtime_info->openxr_companion_connected = g_state.connected;
  if (g_state.connected) {
    runtime_info->openxr_host_detected = true;
    runtime_info->openxr_host_process_id = g_state.hello.process_id;
    runtime_info->openxr_host_application_name = g_state.hello.application_name;
    runtime_info->openxr_host_graphics_api = g_state.hello.graphics_api == GraphicsApi::kD3D12 ? "d3d12" : "d3d11";
    std::ostringstream luid;
    luid << std::hex << static_cast<uint32_t>(g_state.hello.adapter_luid.high_part)
         << ":" << g_state.hello.adapter_luid.low_part;
    runtime_info->openxr_host_adapter_luid = luid.str();
    runtime_info->openxr_protocol_version = kProtocolVersion;
  }
#else
  (void)runtime_info;
#endif
}

void ShutdownOpenXRCompanion() {
#if defined(_WIN32)
  g_state.stop_requested.store(true);
  if (g_state.stop_event != nullptr) SetEvent(g_state.stop_event);
  if (g_state.server_thread.joinable()) {
    CancelSynchronousIo(g_state.server_thread.native_handle());
    g_state.server_thread.join();
  }
  std::lock_guard<std::mutex> lock(g_state.mutex);
  CloseTextureResourcesLocked();
  g_state.connected = false;
  g_state.initialized = false;
  g_state.hello = {};
  g_state.snapshot = {};
  if (g_state.stop_event != nullptr) {
    CloseHandle(g_state.stop_event);
    g_state.stop_event = nullptr;
  }
#endif
}

}  // namespace vrbridge
