#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../openxr_api_layer_protocol.h"

namespace {

using Microsoft::WRL::ComPtr;
using electron_vr::openxr_layer::GraphicsApi;
using electron_vr::openxr_layer::LayerHello;
using electron_vr::openxr_layer::OverlaySnapshot;
using electron_vr::openxr_layer::PlacementMode;
using electron_vr::openxr_layer::kHostPresenceMappingPrefix;
using electron_vr::openxr_layer::kPipeName;
using electron_vr::openxr_layer::kProtocolMagic;
using electron_vr::openxr_layer::kProtocolVersion;
using electron_vr::openxr_layer::kTextureSlotCount;

constexpr char kLayerName[] = "XR_APILAYER_ELECTRON_VR_overlay";
constexpr XrDuration kSwapchainWaitTimeout = 0;
constexpr DWORD kMutexWaitTimeoutMs = 0;

std::wstring HostPresenceMappingName() {
  return std::wstring(kHostPresenceMappingPrefix) + std::to_wstring(GetCurrentProcessId());
}

struct DispatchTable {
  PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_xrDestroyInstance DestroyInstance = nullptr;
  PFN_xrGetSystemProperties GetSystemProperties = nullptr;
  PFN_xrCreateSession CreateSession = nullptr;
  PFN_xrDestroySession DestroySession = nullptr;
  PFN_xrEnumerateReferenceSpaces EnumerateReferenceSpaces = nullptr;
  PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
  PFN_xrDestroySpace DestroySpace = nullptr;
  PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
  PFN_xrCreateSwapchain CreateSwapchain = nullptr;
  PFN_xrDestroySwapchain DestroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
  PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
  PFN_xrEndFrame EndFrame = nullptr;
};

struct InstanceState {
  XrInstance instance = XR_NULL_HANDLE;
  PFN_xrGetInstanceProcAddr next_get_instance_proc_addr = nullptr;
  DispatchTable dispatch;
  std::string application_name;
  bool d3d11_enabled = false;
  bool d3d12_enabled = false;
};

struct ImportedSlot {
  ComPtr<ID3D11Texture2D> texture11;
  ComPtr<IDXGIKeyedMutex> keyed_mutex;
  ComPtr<ID3D12Resource> texture12;
  ComPtr<ID3D12Fence> fence12;
  uint64_t acknowledged_fence_value = 0;
};

struct D3D12AllocatorState {
  ComPtr<ID3D12CommandAllocator> allocator;
  uint64_t completion_value = 0;
};

struct SessionState {
  XrSession session = XR_NULL_HANDLE;
  std::shared_ptr<InstanceState> instance;
  GraphicsApi graphics_api = GraphicsApi::kUnknown;
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11Device1> device11_1;
  ComPtr<ID3D11DeviceContext> context11;
  ComPtr<ID3D12Device> device12;
  ComPtr<ID3D12CommandQueue> queue12;
  ComPtr<ID3D12GraphicsCommandList> command_list12;
  ComPtr<ID3D12Fence> completion_fence12;
  std::array<D3D12AllocatorState, kTextureSlotCount> allocators12;
  uint64_t next_completion_value12 = 1;
  LUID adapter_luid{};
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace stage_space = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageD3D11KHR> images11;
  std::vector<XrSwapchainImageD3D12KHR> images12;
  std::array<ImportedSlot, kTextureSlotCount> imported_slots;
  uint64_t imported_generation = 0;
  uint64_t rejected_generation = 0;
  uint64_t pending_generation = 0;
  uint64_t resource_retirement_value12 = 0;
  uint64_t consumed_sequence = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t max_layer_count = 0;
  bool compatible = false;
  bool image_acquired = false;
  bool image_waited = false;
  uint32_t acquired_image_index = 0;
};

std::mutex g_state_mutex;
std::unordered_map<XrInstance, std::shared_ptr<InstanceState>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::mutex g_pipe_lifecycle_mutex;
XrSession g_pipe_session = XR_NULL_HANDLE;

bool ReadExact(HANDLE pipe, void* data, DWORD size) {
  uint8_t* cursor = static_cast<uint8_t*>(data);
  DWORD remaining = size;
  while (remaining > 0) {
    DWORD read = 0;
    if (!ReadFile(pipe, cursor, remaining, &read, nullptr) || read == 0) return false;
    cursor += read;
    remaining -= read;
  }
  return true;
}

bool WriteExact(HANDLE pipe, const void* data, DWORD size) {
  const uint8_t* cursor = static_cast<const uint8_t*>(data);
  DWORD remaining = size;
  while (remaining > 0) {
    DWORD written = 0;
    if (!WriteFile(pipe, cursor, remaining, &written, nullptr) || written == 0) return false;
    cursor += written;
    remaining -= written;
  }
  return true;
}

class PipeClient {
 public:
  ~PipeClient() { Stop(); }

  void Start(const LayerHello& hello) {
    Stop();
    const std::wstring host_presence_mapping_name = HostPresenceMappingName();
    host_presence_mapping_ = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LayerHello), host_presence_mapping_name.c_str());
    if (host_presence_mapping_ != nullptr) {
      host_presence_ = static_cast<LayerHello*>(MapViewOfFile(
        host_presence_mapping_, FILE_MAP_WRITE, 0, 0, sizeof(LayerHello)));
      if (host_presence_ != nullptr) *host_presence_ = hello;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hello_ = hello;
      snapshot_ = {};
    }
    stop_requested_.store(false);
    thread_ = std::thread([this] { Run(); });
  }

  void Stop() {
    stop_requested_.store(true);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pipe = pipe_;
      pipe_ = INVALID_HANDLE_VALUE;
    }
    if (pipe != INVALID_HANDLE_VALUE) {
      if (thread_.joinable()) CancelSynchronousIo(thread_.native_handle());
      CancelIoEx(pipe, nullptr);
      CloseHandle(pipe);
    }
    if (thread_.joinable()) thread_.join();
    if (host_presence_ != nullptr) {
      UnmapViewOfFile(host_presence_);
      host_presence_ = nullptr;
    }
    if (host_presence_mapping_ != nullptr) {
      CloseHandle(host_presence_mapping_);
      host_presence_mapping_ = nullptr;
    }
  }

  OverlaySnapshot Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
  }

 private:
  void Run() {
    while (!stop_requested_.load()) {
      if (!WaitNamedPipeW(kPipeName, 250)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) continue;
      LayerHello hello{};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pipe_ = pipe;
        hello = hello_;
      }
      if (!WriteExact(pipe, &hello, sizeof(hello))) {
        ClearPipe(pipe);
        continue;
      }
      while (!stop_requested_.load()) {
        OverlaySnapshot snapshot{};
        if (!ReadExact(pipe, &snapshot, sizeof(snapshot))) break;
        if (snapshot.magic != kProtocolMagic || snapshot.version != kProtocolVersion || snapshot.size != sizeof(snapshot)) break;
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = snapshot;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
      }
      ClearPipe(pipe);
    }
  }

  void ClearPipe(HANDLE pipe) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipe_ == pipe) {
      pipe_ = INVALID_HANDLE_VALUE;
      CloseHandle(pipe);
    }
  }

  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> stop_requested_{true};
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE host_presence_mapping_ = nullptr;
  LayerHello* host_presence_ = nullptr;
  LayerHello hello_{};
  OverlaySnapshot snapshot_{};
};

PipeClient* const g_pipe_client = new PipeClient();

template <typename FunctionType>
bool LoadFunction(const std::shared_ptr<InstanceState>& state, const char* name, FunctionType* target) {
  PFN_xrVoidFunction function = nullptr;
  const XrResult result = state->next_get_instance_proc_addr(state->instance, name, &function);
  if (XR_FAILED(result) || function == nullptr) return false;
  *target = reinterpret_cast<FunctionType>(function);
  return true;
}

bool PopulateDispatch(const std::shared_ptr<InstanceState>& state) {
  state->dispatch.GetInstanceProcAddr = state->next_get_instance_proc_addr;
  return LoadFunction(state, "xrDestroyInstance", &state->dispatch.DestroyInstance) &&
         LoadFunction(state, "xrGetSystemProperties", &state->dispatch.GetSystemProperties) &&
         LoadFunction(state, "xrCreateSession", &state->dispatch.CreateSession) &&
         LoadFunction(state, "xrDestroySession", &state->dispatch.DestroySession) &&
         LoadFunction(state, "xrEnumerateReferenceSpaces", &state->dispatch.EnumerateReferenceSpaces) &&
         LoadFunction(state, "xrCreateReferenceSpace", &state->dispatch.CreateReferenceSpace) &&
         LoadFunction(state, "xrDestroySpace", &state->dispatch.DestroySpace) &&
         LoadFunction(state, "xrEnumerateSwapchainFormats", &state->dispatch.EnumerateSwapchainFormats) &&
         LoadFunction(state, "xrCreateSwapchain", &state->dispatch.CreateSwapchain) &&
         LoadFunction(state, "xrDestroySwapchain", &state->dispatch.DestroySwapchain) &&
         LoadFunction(state, "xrEnumerateSwapchainImages", &state->dispatch.EnumerateSwapchainImages) &&
         LoadFunction(state, "xrAcquireSwapchainImage", &state->dispatch.AcquireSwapchainImage) &&
         LoadFunction(state, "xrWaitSwapchainImage", &state->dispatch.WaitSwapchainImage) &&
         LoadFunction(state, "xrReleaseSwapchainImage", &state->dispatch.ReleaseSwapchainImage) &&
         LoadFunction(state, "xrEndFrame", &state->dispatch.EndFrame);
}

std::shared_ptr<InstanceState> FindInstance(XrInstance instance) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const auto iterator = g_instances.find(instance);
  return iterator == g_instances.end() ? nullptr : iterator->second;
}

std::shared_ptr<SessionState> FindSession(XrSession session) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const auto iterator = g_sessions.find(session);
  return iterator == g_sessions.end() ? nullptr : iterator->second;
}

bool HasEnabledExtension(const XrInstanceCreateInfo* info, const char* name) {
  if (info == nullptr || name == nullptr) return false;
  for (uint32_t index = 0; index < info->enabledExtensionCount; ++index) {
    if (info->enabledExtensionNames[index] != nullptr && std::strcmp(info->enabledExtensionNames[index], name) == 0) return true;
  }
  return false;
}

const XrGraphicsBindingD3D11KHR* FindD3D11Binding(const XrSessionCreateInfo* info) {
  if (info == nullptr) return nullptr;
  const XrBaseInStructure* node = static_cast<const XrBaseInStructure*>(info->next);
  while (node != nullptr) {
    if (node->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
      return reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(node);
    }
    node = node->next;
  }
  return nullptr;
}

const XrGraphicsBindingD3D12KHR* FindD3D12Binding(const XrSessionCreateInfo* info) {
  if (info == nullptr) return nullptr;
  const XrBaseInStructure* node = static_cast<const XrBaseInStructure*>(info->next);
  while (node != nullptr) {
    if (node->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
      return reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(node);
    }
    node = node->next;
  }
  return nullptr;
}

void DestroyOverlayResources(SessionState& state) {
  const DispatchTable& dispatch = state.instance->dispatch;
  if (state.image_acquired && state.swapchain != XR_NULL_HANDLE) {
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(dispatch.WaitSwapchainImage(state.swapchain, &wait_info))) {
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      dispatch.ReleaseSwapchainImage(state.swapchain, &release_info);
    }
  }
  state.image_acquired = false;
  state.image_waited = false;
  for (ImportedSlot& slot : state.imported_slots) {
    slot.keyed_mutex.Reset();
    slot.texture11.Reset();
    slot.texture12.Reset();
    slot.fence12.Reset();
    slot.acknowledged_fence_value = 0;
  }
  state.imported_generation = 0;
  state.rejected_generation = 0;
  state.pending_generation = 0;
  state.resource_retirement_value12 = 0;
  state.consumed_sequence = 0;
  state.images11.clear();
  state.images12.clear();
  if (state.swapchain != XR_NULL_HANDLE) {
    dispatch.DestroySwapchain(state.swapchain);
    state.swapchain = XR_NULL_HANDLE;
  }
  state.width = 0;
  state.height = 0;
  state.format = DXGI_FORMAT_UNKNOWN;
}

void DestroySessionResources(SessionState& state) {
  if (state.queue12 != nullptr && state.completion_fence12 != nullptr) {
    const uint64_t completion_value = state.next_completion_value12++;
    if (SUCCEEDED(state.queue12->Signal(state.completion_fence12.Get(), completion_value)) &&
        state.completion_fence12->GetCompletedValue() < completion_value) {
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (event != nullptr) {
        if (SUCCEEDED(state.completion_fence12->SetEventOnCompletion(completion_value, event))) {
          WaitForSingleObject(event, INFINITE);
        }
        CloseHandle(event);
      }
    }
  }
  DestroyOverlayResources(state);
  const DispatchTable& dispatch = state.instance->dispatch;
  if (state.stage_space != XR_NULL_HANDLE) dispatch.DestroySpace(state.stage_space);
  if (state.local_space != XR_NULL_HANDLE) dispatch.DestroySpace(state.local_space);
  if (state.view_space != XR_NULL_HANDLE) dispatch.DestroySpace(state.view_space);
  state.stage_space = XR_NULL_HANDLE;
  state.local_space = XR_NULL_HANDLE;
  state.view_space = XR_NULL_HANDLE;
  state.command_list12.Reset();
  for (auto& allocator : state.allocators12) allocator.allocator.Reset();
  state.completion_fence12.Reset();
  state.queue12.Reset();
  state.device12.Reset();
  state.context11.Reset();
  state.device11_1.Reset();
  state.device11.Reset();
}

void CreateReferenceSpaces(SessionState& state) {
  uint32_t count = 0;
  std::vector<XrReferenceSpaceType> supported;
  if (XR_SUCCEEDED(state.instance->dispatch.EnumerateReferenceSpaces(state.session, 0, &count, nullptr)) && count > 0) {
    supported.resize(count);
    if (XR_FAILED(state.instance->dispatch.EnumerateReferenceSpaces(state.session, count, &count, supported.data()))) supported.clear();
  }
  const auto create_space = [&](XrReferenceSpaceType type, XrSpace* space) {
    if (!supported.empty() && std::find(supported.begin(), supported.end(), type) == supported.end()) return;
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = type;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(state.instance->dispatch.CreateReferenceSpace(state.session, &info, space))) *space = XR_NULL_HANDLE;
  };
  create_space(XR_REFERENCE_SPACE_TYPE_VIEW, &state.view_space);
  create_space(XR_REFERENCE_SPACE_TYPE_LOCAL, &state.local_space);
  create_space(XR_REFERENCE_SPACE_TYPE_STAGE, &state.stage_space);
}

bool QueryAdapterLuid(ID3D11Device* device, LUID* luid) {
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc{};
  return device != nullptr && luid != nullptr &&
         SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
         SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
         SUCCEEDED(adapter->GetDesc(&desc)) &&
         ((*luid = desc.AdapterLuid), true);
}

bool OpenImportedTextures(SessionState& state, const OverlaySnapshot& snapshot) {
  if (state.imported_generation == snapshot.texture_generation) {
    return std::all_of(state.imported_slots.begin(), state.imported_slots.end(), [](const ImportedSlot& slot) {
      return (slot.texture11 != nullptr && slot.keyed_mutex != nullptr) ||
             (slot.texture12 != nullptr && slot.fence12 != nullptr);
    });
  }
  if (state.rejected_generation == snapshot.texture_generation) return false;
  if (snapshot.texture_generation == 0) return false;

  std::array<ImportedSlot, kTextureSlotCount> imported_slots;
  bool valid = true;
  for (uint32_t index = 0; index < kTextureSlotCount; ++index) {
    const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(snapshot.slots[index].handle));
    if (handle == nullptr) {
      valid = false;
      continue;
    }
    if (state.graphics_api == GraphicsApi::kD3D11 && state.device11_1 != nullptr) {
      const HRESULT open_result = state.device11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&imported_slots[index].texture11));
      CloseHandle(handle);
      if (FAILED(open_result) || imported_slots[index].texture11 == nullptr ||
          FAILED(imported_slots[index].texture11.As(&imported_slots[index].keyed_mutex))) {
        valid = false;
        continue;
      }
      D3D11_TEXTURE2D_DESC desc{};
      imported_slots[index].texture11->GetDesc(&desc);
      if (desc.Width != snapshot.width || desc.Height != snapshot.height ||
          desc.Format != static_cast<DXGI_FORMAT>(snapshot.dxgi_format) ||
          desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
        valid = false;
      }
    } else if (state.graphics_api == GraphicsApi::kD3D12 && state.device12 != nullptr) {
      const HRESULT open_result = state.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&imported_slots[index].texture12));
      CloseHandle(handle);
      const HANDLE fence_handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(snapshot.slots[index].fence_handle));
      HRESULT fence_result = E_INVALIDARG;
      if (fence_handle != nullptr) {
        fence_result = state.device12->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&imported_slots[index].fence12));
        CloseHandle(fence_handle);
      }
      if (FAILED(open_result) || FAILED(fence_result) || imported_slots[index].texture12 == nullptr || imported_slots[index].fence12 == nullptr) {
        valid = false;
        continue;
      }
      const D3D12_RESOURCE_DESC desc = imported_slots[index].texture12->GetDesc();
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != snapshot.width ||
          desc.Height != snapshot.height || desc.Format != static_cast<DXGI_FORMAT>(snapshot.dxgi_format) ||
          desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1) {
        valid = false;
      }
    } else {
      CloseHandle(handle);
      valid = false;
    }
  }
  if (!valid) {
    state.rejected_generation = snapshot.texture_generation;
    return false;
  }
  state.imported_slots = std::move(imported_slots);
  state.imported_generation = snapshot.texture_generation;
  state.rejected_generation = 0;
  return true;
}

bool CreateOverlaySwapchain(SessionState& state, const OverlaySnapshot& snapshot) {
  uint32_t format_count = 0;
  const DispatchTable& dispatch = state.instance->dispatch;
  if (XR_FAILED(dispatch.EnumerateSwapchainFormats(state.session, 0, &format_count, nullptr)) || format_count == 0) return false;
  std::vector<int64_t> formats(format_count);
  if (XR_FAILED(dispatch.EnumerateSwapchainFormats(state.session, format_count, &format_count, formats.data()))) return false;
  const int64_t desired_format = snapshot.dxgi_format;
  if (std::find(formats.begin(), formats.end(), desired_format) == formats.end()) return false;

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  if (state.graphics_api == GraphicsApi::kD3D12) {
    info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  }
  info.format = desired_format;
  info.sampleCount = 1;
  info.width = snapshot.width;
  info.height = snapshot.height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (XR_FAILED(dispatch.CreateSwapchain(state.session, &info, &state.swapchain))) return false;

  uint32_t image_count = 0;
  if (XR_FAILED(dispatch.EnumerateSwapchainImages(state.swapchain, 0, &image_count, nullptr)) || image_count == 0) {
    dispatch.DestroySwapchain(state.swapchain);
    state.swapchain = XR_NULL_HANDLE;
    return false;
  }
  XrSwapchainImageBaseHeader* images = nullptr;
  if (state.graphics_api == GraphicsApi::kD3D11) {
    state.images11.resize(image_count);
    for (auto& image : state.images11) image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    images = reinterpret_cast<XrSwapchainImageBaseHeader*>(state.images11.data());
  } else {
    state.images12.resize(image_count);
    for (auto& image : state.images12) image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    images = reinterpret_cast<XrSwapchainImageBaseHeader*>(state.images12.data());
  }
  if (XR_FAILED(dispatch.EnumerateSwapchainImages(state.swapchain, image_count, &image_count, images))) {
    state.images11.clear();
    state.images12.clear();
    dispatch.DestroySwapchain(state.swapchain);
    state.swapchain = XR_NULL_HANDLE;
    return false;
  }
  state.width = snapshot.width;
  state.height = snapshot.height;
  state.format = static_cast<DXGI_FORMAT>(snapshot.dxgi_format);
  return true;
}

bool EnsureOverlayResources(SessionState& state, const OverlaySnapshot& snapshot) {
  if (!state.compatible || snapshot.width == 0 || snapshot.height == 0 || snapshot.latest_sequence == 0) return false;
  if (state.rejected_generation == snapshot.texture_generation) return false;
  const bool changed = state.width != snapshot.width || state.height != snapshot.height ||
                       state.format != static_cast<DXGI_FORMAT>(snapshot.dxgi_format) ||
                       state.imported_generation != snapshot.texture_generation;
  if (changed) {
    if (state.image_acquired) {
      XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait_info.timeout = 0;
      const XrResult wait_result = state.instance->dispatch.WaitSwapchainImage(state.swapchain, &wait_info);
      if (wait_result == XR_TIMEOUT_EXPIRED || XR_FAILED(wait_result)) return false;
      XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      if (XR_FAILED(state.instance->dispatch.ReleaseSwapchainImage(state.swapchain, &release_info))) return false;
      state.image_acquired = false;
      state.image_waited = false;
    }
    if (state.graphics_api == GraphicsApi::kD3D12 && state.swapchain != XR_NULL_HANDLE) {
      if (state.pending_generation != snapshot.texture_generation) {
        const uint64_t retirement_value = state.next_completion_value12++;
        if (FAILED(state.queue12->Signal(state.completion_fence12.Get(), retirement_value))) return false;
        state.pending_generation = snapshot.texture_generation;
        state.resource_retirement_value12 = retirement_value;
        return false;
      }
      if (state.completion_fence12->GetCompletedValue() < state.resource_retirement_value12) return false;
    }
    DestroyOverlayResources(state);
  }
  if (!OpenImportedTextures(state, snapshot)) return false;
  return state.swapchain != XR_NULL_HANDLE || CreateOverlaySwapchain(state, snapshot);
}

D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

bool CopyLatestFrameD3D12(SessionState& state, const OverlaySnapshot& snapshot) {
  ImportedSlot& source = state.imported_slots[snapshot.latest_slot];
  if (source.texture12 == nullptr || source.fence12 == nullptr ||
      state.acquired_image_index >= state.images12.size() || state.images12[state.acquired_image_index].texture == nullptr) {
    return false;
  }

  D3D12AllocatorState* allocator_state = nullptr;
  const uint64_t completed = state.completion_fence12->GetCompletedValue();
  for (auto& candidate : state.allocators12) {
    if (candidate.completion_value == 0 || candidate.completion_value <= completed) {
      allocator_state = &candidate;
      break;
    }
  }
  if (allocator_state == nullptr || FAILED(allocator_state->allocator->Reset()) ||
      FAILED(state.command_list12->Reset(allocator_state->allocator.Get(), nullptr))) {
    return false;
  }

  ID3D12Resource* destination = state.images12[state.acquired_image_index].texture;
  const D3D12_RESOURCE_BARRIER barriers_before[] = {
    TransitionBarrier(source.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
    TransitionBarrier(destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST),
  };
  state.command_list12->ResourceBarrier(2, barriers_before);
  state.command_list12->CopyResource(destination, source.texture12.Get());
  const D3D12_RESOURCE_BARRIER barriers_after[] = {
    TransitionBarrier(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET),
    TransitionBarrier(source.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
  };
  state.command_list12->ResourceBarrier(2, barriers_after);
  if (FAILED(state.command_list12->Close()) ||
      FAILED(state.queue12->Wait(source.fence12.Get(), snapshot.slots[snapshot.latest_slot].fence_value))) {
    return false;
  }
  ID3D12CommandList* lists[] = {state.command_list12.Get()};
  state.queue12->ExecuteCommandLists(1, lists);
  const uint64_t completion_value = state.next_completion_value12++;
  allocator_state->completion_value = completion_value;
  const HRESULT completion_signal = state.queue12->Signal(state.completion_fence12.Get(), completion_value);
  const HRESULT acknowledgement_signal = state.queue12->Signal(
    source.fence12.Get(), snapshot.slots[snapshot.latest_slot].fence_value + 1);
  if (FAILED(completion_signal) || FAILED(acknowledgement_signal)) {
    if (FAILED(completion_signal)) allocator_state->completion_value = UINT64_MAX;
    return false;
  }
  source.acknowledged_fence_value = snapshot.slots[snapshot.latest_slot].fence_value + 1;
  return true;
}

void AcknowledgeUnusedD3D12Slots(SessionState& state, const OverlaySnapshot& snapshot) {
  if (state.graphics_api != GraphicsApi::kD3D12 || state.queue12 == nullptr) return;
  for (uint32_t index = 0; index < kTextureSlotCount; ++index) {
    if (index == snapshot.latest_slot) continue;
    ImportedSlot& slot = state.imported_slots[index];
    const uint64_t ready_value = snapshot.slots[index].fence_value;
    if (ready_value == 0 || slot.fence12 == nullptr || slot.acknowledged_fence_value >= ready_value + 1) continue;
    if (SUCCEEDED(state.queue12->Wait(slot.fence12.Get(), ready_value)) &&
        SUCCEEDED(state.queue12->Signal(slot.fence12.Get(), ready_value + 1))) {
      slot.acknowledged_fence_value = ready_value + 1;
    }
  }
}

bool CopyLatestFrame(SessionState& state, const OverlaySnapshot& snapshot) {
  if (snapshot.latest_sequence == state.consumed_sequence) return state.consumed_sequence != 0;
  if (snapshot.latest_slot >= kTextureSlotCount || snapshot.slots[snapshot.latest_slot].sequence != snapshot.latest_sequence) return false;
  const DispatchTable& dispatch = state.instance->dispatch;
  AcknowledgeUnusedD3D12Slots(state, snapshot);

  if (!state.image_acquired) {
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(dispatch.AcquireSwapchainImage(state.swapchain, &acquire_info, &state.acquired_image_index))) return false;
    state.image_acquired = true;
    state.image_waited = false;
  }
  if (!state.image_waited) {
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = kSwapchainWaitTimeout;
    const XrResult wait_result = dispatch.WaitSwapchainImage(state.swapchain, &wait_info);
    if (wait_result == XR_TIMEOUT_EXPIRED) return false;
    if (XR_FAILED(wait_result)) return false;
    state.image_waited = true;
  }

  bool copied = false;
  ImportedSlot& source = state.imported_slots[snapshot.latest_slot];
  if (state.graphics_api == GraphicsApi::kD3D11 && source.keyed_mutex != nullptr && source.texture11 != nullptr) {
    const HRESULT mutex_result = source.keyed_mutex->AcquireSync(1, kMutexWaitTimeoutMs);
    if (mutex_result == S_OK) {
      if (state.acquired_image_index < state.images11.size() && state.images11[state.acquired_image_index].texture != nullptr) {
        state.context11->CopyResource(state.images11[state.acquired_image_index].texture, source.texture11.Get());
        state.context11->Flush();
        copied = true;
      }
      source.keyed_mutex->ReleaseSync(0);
    }
  } else if (state.graphics_api == GraphicsApi::kD3D12) {
    copied = CopyLatestFrameD3D12(state, snapshot);
  }
  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult release_result = dispatch.ReleaseSwapchainImage(state.swapchain, &release_info);
  state.image_acquired = false;
  state.image_waited = false;
  if (copied && XR_SUCCEEDED(release_result)) state.consumed_sequence = snapshot.latest_sequence;
  return copied && XR_SUCCEEDED(release_result);
}

bool IsFinitePose(const OverlaySnapshot& snapshot) {
  for (float value : snapshot.position) if (!std::isfinite(value)) return false;
  float length_squared = 0.0f;
  for (float value : snapshot.rotation) {
    if (!std::isfinite(value)) return false;
    length_squared += value * value;
  }
  return length_squared > 0.000001f && std::isfinite(snapshot.size_meters) && snapshot.size_meters > 0.0f;
}

XrSpace SelectSpace(const SessionState& state, PlacementMode mode) {
  if (mode == PlacementMode::kHead) return state.view_space;
  return state.stage_space != XR_NULL_HANDLE ? state.stage_space : state.local_space;
}

LayerHello MakeLayerHello(const SessionState& state) {
  LayerHello hello{};
  hello.process_id = GetCurrentProcessId();
  hello.graphics_api = state.graphics_api;
  hello.adapter_luid.low_part = state.adapter_luid.LowPart;
  hello.adapter_luid.high_part = state.adapter_luid.HighPart;
  std::strncpy(hello.application_name, state.instance->application_name.c_str(), sizeof(hello.application_name) - 1);
  return hello;
}

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
XrResult XRAPI_CALL LayerCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info, XrInstance* instance);

XrResult XRAPI_CALL LayerDestroyInstance(XrInstance instance) {
  try {
    const auto state = FindInstance(instance);
    if (state == nullptr) return XR_ERROR_HANDLE_INVALID;
    std::vector<std::shared_ptr<SessionState>> sessions;
    std::shared_ptr<SessionState> replacement;
    bool selected_session_removed = false;
    std::lock_guard<std::mutex> pipe_lock(g_pipe_lifecycle_mutex);
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      for (auto iterator = g_sessions.begin(); iterator != g_sessions.end();) {
        if (iterator->second->instance == state) {
          sessions.push_back(iterator->second);
          iterator = g_sessions.erase(iterator);
        } else {
          ++iterator;
        }
      }
      g_instances.erase(instance);
      if (g_pipe_session != XR_NULL_HANDLE &&
          std::any_of(sessions.begin(), sessions.end(), [](const auto& session) { return session->session == g_pipe_session; })) {
        selected_session_removed = true;
        g_pipe_session = XR_NULL_HANDLE;
        for (const auto& entry : g_sessions) {
          if (entry.second->compatible) {
            replacement = entry.second;
            g_pipe_session = entry.first;
            break;
          }
        }
      }
    }
    for (auto& session : sessions) DestroySessionResources(*session);
    if (selected_session_removed) {
      g_pipe_client->Stop();
      if (replacement != nullptr) g_pipe_client->Start(MakeLayerHello(*replacement));
    }
    return state->dispatch.DestroyInstance(instance);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL LayerCreateSession(XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session) {
  try {
    const auto instance_state = FindInstance(instance);
    if (instance_state == nullptr || create_info == nullptr || session == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    const XrGraphicsBindingD3D11KHR* binding = FindD3D11Binding(create_info);
    const XrGraphicsBindingD3D12KHR* binding12 = FindD3D12Binding(create_info);
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    if (binding != nullptr && binding->device != nullptr) device11 = binding->device;
    if (binding12 != nullptr && binding12->device != nullptr && binding12->queue != nullptr) {
      device12 = binding12->device;
      queue12 = binding12->queue;
    }

    const XrResult result = instance_state->dispatch.CreateSession(instance, create_info, session);
    if (XR_FAILED(result)) return result;

    auto state = std::make_shared<SessionState>();
    state->session = *session;
    state->instance = instance_state;
    state->device11 = device11;
    state->device12 = device12;
    state->queue12 = queue12;
    if (state->device11 != nullptr) {
      state->graphics_api = GraphicsApi::kD3D11;
      state->device11.As(&state->device11_1);
      state->device11->GetImmediateContext(&state->context11);
      ComPtr<ID3D11Multithread> multithread;
      if (state->context11 != nullptr && SUCCEEDED(state->context11.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
      }
      state->compatible = state->device11_1 != nullptr && QueryAdapterLuid(state->device11.Get(), &state->adapter_luid);
    } else if (state->device12 != nullptr && state->queue12 != nullptr &&
               state->queue12->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
      state->graphics_api = GraphicsApi::kD3D12;
      state->adapter_luid = state->device12->GetAdapterLuid();
      state->compatible = SUCCEEDED(state->device12->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->completion_fence12)));
      for (auto& allocator : state->allocators12) {
        if (state->compatible) {
          state->compatible = SUCCEEDED(state->device12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.allocator)));
        }
      }
      if (state->compatible) {
        state->compatible = SUCCEEDED(state->device12->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          state->allocators12[0].allocator.Get(),
          nullptr,
          IID_PPV_ARGS(&state->command_list12)));
        if (state->compatible) state->compatible = SUCCEEDED(state->command_list12->Close());
      }
    }
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(instance_state->dispatch.GetSystemProperties(instance, create_info->systemId, &properties))) {
      state->max_layer_count = properties.graphicsProperties.maxLayerCount;
    }
    if (state->compatible) CreateReferenceSpaces(*state);
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      g_sessions[*session] = state;
    }
    if (state->compatible) {
      std::lock_guard<std::mutex> pipe_lock(g_pipe_lifecycle_mutex);
      if (g_pipe_session == XR_NULL_HANDLE) {
        g_pipe_session = *session;
        g_pipe_client->Start(MakeLayerHello(*state));
      }
    }
    return result;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL LayerDestroySession(XrSession session) {
  try {
    const auto state = FindSession(session);
    if (state == nullptr) return XR_ERROR_HANDLE_INVALID;
    std::shared_ptr<SessionState> replacement;
    std::lock_guard<std::mutex> pipe_lock(g_pipe_lifecycle_mutex);
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      g_sessions.erase(session);
      if (g_pipe_session == session) {
        g_pipe_session = XR_NULL_HANDLE;
        for (const auto& entry : g_sessions) {
          if (entry.second->compatible) {
            replacement = entry.second;
            g_pipe_session = entry.first;
            break;
          }
        }
      }
    }
    DestroySessionResources(*state);
    if (state->session == session && (replacement != nullptr || g_pipe_session == XR_NULL_HANDLE)) {
      g_pipe_client->Stop();
      if (replacement != nullptr) g_pipe_client->Start(MakeLayerHello(*replacement));
    }
    return state->instance->dispatch.DestroySession(session);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL LayerEndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
  try {
    const auto state = FindSession(session);
    if (state == nullptr) return XR_ERROR_HANDLE_INVALID;
    const auto forward_original = [&] { return state->instance->dispatch.EndFrame(session, frame_end_info); };
    if (frame_end_info == nullptr || frame_end_info->type != XR_TYPE_FRAME_END_INFO ||
        (frame_end_info->layerCount > 0 && frame_end_info->layers == nullptr) ||
        frame_end_info->layerCount == UINT32_MAX ||
        (state->max_layer_count > 0 && frame_end_info->layerCount >= state->max_layer_count)) {
      return forward_original();
    }

    const OverlaySnapshot snapshot = g_pipe_client->Snapshot();
    if (!snapshot.visible || !IsFinitePose(snapshot) || !EnsureOverlayResources(*state, snapshot)) {
      return forward_original();
    }
    const bool copied_latest_frame = CopyLatestFrame(*state, snapshot);
    if (!copied_latest_frame && state->consumed_sequence == 0) {
      return forward_original();
    }
    const XrSpace space = SelectSpace(*state, snapshot.placement_mode);
    if (space == XR_NULL_HANDLE) return forward_original();

    float quaternion_length = 0.0f;
    for (float value : snapshot.rotation) quaternion_length += value * value;
    quaternion_length = std::sqrt(quaternion_length);
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = state->swapchain;
    quad.subImage.imageRect.extent = {static_cast<int32_t>(state->width), static_cast<int32_t>(state->height)};
    quad.pose.orientation = {
      snapshot.rotation[0] / quaternion_length,
      snapshot.rotation[1] / quaternion_length,
      snapshot.rotation[2] / quaternion_length,
      snapshot.rotation[3] / quaternion_length};
    quad.pose.position = {snapshot.position[0], snapshot.position[1], snapshot.position[2]};
    quad.size.width = snapshot.size_meters;
    quad.size.height = snapshot.size_meters * static_cast<float>(state->height) / static_cast<float>(state->width);

    std::vector<const XrCompositionLayerBaseHeader*> layers(
      frame_end_info->layers, frame_end_info->layers + frame_end_info->layerCount);
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    XrFrameEndInfo modified = *frame_end_info;
    modified.layerCount = static_cast<uint32_t>(layers.size());
    modified.layers = layers.data();
    return state->instance->dispatch.EndFrame(session, &modified);
  } catch (...) {
    const auto state = FindSession(session);
    return state != nullptr ? state->instance->dispatch.EndFrame(session, frame_end_info) : XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
  try {
    if (name == nullptr || function == nullptr) return XR_ERROR_VALIDATION_FAILURE;
    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(LayerGetInstanceProcAddr);
      return XR_SUCCESS;
    }
    const auto state = FindInstance(instance);
    if (state == nullptr) {
      if (instance == XR_NULL_HANDLE) {
        PFN_xrGetInstanceProcAddr next = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_state_mutex);
          if (!g_instances.empty()) next = g_instances.begin()->second->next_get_instance_proc_addr;
        }
        if (next != nullptr) return next(instance, name, function);
        return XR_ERROR_FUNCTION_UNSUPPORTED;
      }
      return XR_ERROR_HANDLE_INVALID;
    }
    if (std::strcmp(name, "xrDestroyInstance") == 0) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(LayerDestroyInstance);
      return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrCreateSession") == 0) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(LayerCreateSession);
      return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrDestroySession") == 0) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(LayerDestroySession);
      return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrEndFrame") == 0) {
      *function = reinterpret_cast<PFN_xrVoidFunction>(LayerEndFrame);
      return XR_SUCCESS;
    }
    return state->next_get_instance_proc_addr(instance, name, function);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL LayerCreateApiLayerInstance(
    const XrInstanceCreateInfo* info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
  try {
    if (info == nullptr || layer_info == nullptr || instance == nullptr || layer_info->nextInfo == nullptr) return XR_ERROR_INITIALIZATION_FAILED;
    const XrApiLayerNextInfo* next = layer_info->nextInfo;
    if (layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize < sizeof(XrApiLayerCreateInfo) ||
        next->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        next->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        next->structSize < sizeof(XrApiLayerNextInfo) ||
        std::strcmp(next->layerName, kLayerName) != 0 ||
        next->nextGetInstanceProcAddr == nullptr || next->nextCreateApiLayerInstance == nullptr) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }

    auto state = std::make_shared<InstanceState>();
    state->next_get_instance_proc_addr = next->nextGetInstanceProcAddr;
    state->application_name = info->applicationInfo.applicationName;
    state->d3d11_enabled = HasEnabledExtension(info, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    state->d3d12_enabled = HasEnabledExtension(info, XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
    XrApiLayerCreateInfo downstream_info = *layer_info;
    downstream_info.nextInfo = next->next;
    const XrResult result = next->nextCreateApiLayerInstance(info, &downstream_info, instance);
    if (XR_FAILED(result)) return result;
    state->instance = *instance;
    if (!PopulateDispatch(state)) {
      PFN_xrVoidFunction destroy_function = nullptr;
      if (XR_SUCCEEDED(state->next_get_instance_proc_addr(*instance, "xrDestroyInstance", &destroy_function)) && destroy_function != nullptr) {
        reinterpret_cast<PFN_xrDestroyInstance>(destroy_function)(*instance);
      }
      *instance = XR_NULL_HANDLE;
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      g_instances[*instance] = state;
    }
    return XR_SUCCESS;
  } catch (...) {
    return XR_ERROR_OUT_OF_MEMORY;
  }
}

}  // namespace

extern "C" __declspec(dllexport)
XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* request) {
  try {
    if (loader_info == nullptr || layer_name == nullptr || request == nullptr || std::strcmp(layer_name, kLayerName) != 0) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loader_info->structSize < sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize < sizeof(XrNegotiateApiLayerRequest) ||
        loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    constexpr XrVersion kApiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (loader_info->minApiVersion > kApiVersion || loader_info->maxApiVersion < kApiVersion) return XR_ERROR_API_VERSION_UNSUPPORTED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = kApiVersion;
    request->getInstanceProcAddr = LayerGetInstanceProcAddr;
    request->createApiLayerInstance = LayerCreateApiLayerInstance;
    return XR_SUCCESS;
  } catch (...) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
}
