#include "openvr_backend.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "openvr.h"

#if defined(_WIN32)
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#endif

#if defined(__linux__)
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#endif

namespace vrbridge {

namespace {

struct OpenVRState {
  vr::IVRSystem* system = nullptr;
  vr::IVROverlay* overlay = nullptr;
#if defined(__linux__)
  struct ImportedTexture {
    dev_t device = 0;
    ino_t inode = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t modifier = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t size = 0;
    vr::SharedTextureHandle_t handle = 0;
  };

  vr::IVRIPCResourceManagerClient* ipc = nullptr;
  std::vector<ImportedTexture> imported_textures;
  bool logged_frame_sync_timeout = false;
  Display* glx_display = nullptr;
  GLXContext glx_context = nullptr;
  GLXPbuffer glx_pbuffer = 0;
  GLuint gl_upload_texture = 0;
  uint32_t gl_upload_width = 0;
  uint32_t gl_upload_height = 0;
  uint64_t gl_upload_frame_count = 0;
  uint64_t gl_upload_changed_frame_count = 0;
  uint64_t gl_upload_last_checksum = 0;
  bool logged_gl_upload = false;
  VkInstance vulkan_instance = VK_NULL_HANDLE;
  VkPhysicalDevice vulkan_physical_device = VK_NULL_HANDLE;
  VkDevice vulkan_device = VK_NULL_HANDLE;
  VkQueue vulkan_queue = VK_NULL_HANDLE;
  VkCommandPool vulkan_command_pool = VK_NULL_HANDLE;
  uint32_t vulkan_queue_family = 0;
  VkPhysicalDeviceMemoryProperties vulkan_memory_properties = {};
  PFN_vkGetMemoryFdPropertiesKHR vulkan_get_memory_fd_properties = nullptr;
  bool vulkan_dmabuf_import_enabled = false;
  VkImage vulkan_visible_image = VK_NULL_HANDLE;
  VkDeviceMemory vulkan_visible_memory = VK_NULL_HANDLE;
  std::vector<VkImage> vulkan_retired_images;
  std::vector<VkDeviceMemory> vulkan_retired_memory;
  uint32_t vulkan_visible_width = 0;
  uint32_t vulkan_visible_height = 0;
  VkFormat vulkan_visible_format = VK_FORMAT_UNDEFINED;
  bool vulkan_visible_initialized = false;
  bool vulkan_visible_safe_to_write = true;
  VkBuffer vulkan_staging_buffer = VK_NULL_HANDLE;
  VkDeviceMemory vulkan_staging_memory = VK_NULL_HANDLE;
  void* vulkan_staging_mapping = nullptr;
  VkDeviceSize vulkan_staging_size = 0;
  bool vulkan_staging_coherent = false;
  uint64_t vulkan_frame_count = 0;
  bool logged_vulkan_submission = false;
  bool vulkan_disabled = false;
#endif
  vr::VROverlayHandle_t overlay_handle = vr::k_ulOverlayHandleInvalid;
#if defined(_WIN32)
  ID3D11Device* d3d_device = nullptr;
  ID3D11DeviceContext* d3d_context = nullptr;
  ID3D11Device1* d3d_device1 = nullptr;
  ID3D11Texture2D* shared_texture = nullptr;
  ID3D11Texture2D* submit_texture = nullptr;
  HANDLE submit_texture_shared_handle = nullptr;
  uint64_t shared_texture_handle_value = 0;
  DXGI_FORMAT submit_texture_format = DXGI_FORMAT_UNKNOWN;
  uint32_t submit_texture_width = 0;
  uint32_t submit_texture_height = 0;
  bool logged_shared_texture_desc = false;
#endif
  bool initialized = false;
};

OpenVRState g_state;
std::atomic<uint64_t> g_overlay_key_counter{0};

void SetError(std::string* error_message, std::string message) {
  if (error_message != nullptr) {
    *error_message = std::move(message);
  }
}

bool CheckOverlayError(vr::EVROverlayError error, std::string context, std::string* error_message);
#if defined(_WIN32)
bool EnsureD3D11Device(std::string* error_message);
bool EnsureSubmitTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, std::string* error_message);
bool ConfigureOverlayTextureFromSubmitTexture(vr::Texture_t* texture, std::string* error_message);
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

void PopulateOpenVRSceneApplication(RuntimeInfo* runtime_info) {
  if (runtime_info == nullptr || g_state.system == nullptr) {
    return;
  }

  vr::IVRApplications* applications = vr::VRApplications();
  if (applications == nullptr) {
    return;
  }

  const vr::EVRSceneApplicationState scene_state = applications->GetSceneApplicationState();
  const char* scene_state_name = applications->GetSceneApplicationStateNameFromEnum(scene_state);
  if (scene_state_name != nullptr) {
    runtime_info->openvr_scene_application_state = scene_state_name;
  }

  runtime_info->openvr_scene_process_id = applications->GetCurrentSceneProcessId();
  if (runtime_info->openvr_scene_process_id == 0) {
    return;
  }

  std::vector<char> application_key_buffer(vr::k_unMaxApplicationKeyLength, '\0');
  const vr::EVRApplicationError application_key_error = applications->GetApplicationKeyByProcessId(
    runtime_info->openvr_scene_process_id,
    application_key_buffer.data(),
    static_cast<uint32_t>(application_key_buffer.size()));
  if (application_key_error != vr::VRApplicationError_None) {
    return;
  }

  runtime_info->openvr_scene_application_key = application_key_buffer.data();
  runtime_info->openvr_scene_application_name = ReadOpenVRApplicationPropertyString(
    applications,
    runtime_info->openvr_scene_application_key,
    vr::VRApplicationProperty_Name_String);
  runtime_info->openvr_scene_application_binary_path = ReadOpenVRApplicationPropertyString(
    applications,
    runtime_info->openvr_scene_application_key,
    vr::VRApplicationProperty_BinaryPath_String);
}

std::string BuildOverlayKey(const std::string& overlay_name) {
  const uint64_t instance_id = g_overlay_key_counter.fetch_add(1, std::memory_order_relaxed);
  const uint64_t timestamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

  std::ostringstream stream;
  stream << "electron_vr." << overlay_name << "." << timestamp << "." << instance_id;
  return stream.str();
}

#if defined(_WIN32)
std::string HResultToString(HRESULT value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << static_cast<unsigned long>(value);
  return stream.str();
}

const char* DxgiFormatToString(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM:
      return "DXGI_FORMAT_B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
    default:
      return "DXGI_FORMAT_OTHER";
  }
}

bool IsOpenVRFriendlyFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_B8G8R8X8_UNORM ||
         format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

void ReleaseSubmitTexture() {
  if (g_state.submit_texture != nullptr) {
    g_state.submit_texture->Release();
    g_state.submit_texture = nullptr;
  }
  g_state.submit_texture_shared_handle = nullptr;
  g_state.submit_texture_format = DXGI_FORMAT_UNKNOWN;
  g_state.submit_texture_width = 0;
  g_state.submit_texture_height = 0;
}

void ReleaseSharedTexture() {
  if (g_state.shared_texture != nullptr) {
    g_state.shared_texture->Release();
    g_state.shared_texture = nullptr;
  }
  g_state.shared_texture_handle_value = 0;
}

void ReleaseD3DResources() {
  ReleaseSubmitTexture();
  ReleaseSharedTexture();

  if (g_state.d3d_context != nullptr) {
    g_state.d3d_context->Release();
    g_state.d3d_context = nullptr;
  }

  if (g_state.d3d_device1 != nullptr) {
    g_state.d3d_device1->Release();
    g_state.d3d_device1 = nullptr;
  }

  if (g_state.d3d_device != nullptr) {
    g_state.d3d_device->Release();
    g_state.d3d_device = nullptr;
  }
}

bool EnsureD3D11Device(std::string* error_message) {
  if (g_state.d3d_device != nullptr) {
    return true;
  }

  const D3D_FEATURE_LEVEL requested_feature_levels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0
  };
  const D3D_DRIVER_TYPE driver_types[] = {
    D3D_DRIVER_TYPE_HARDWARE,
    D3D_DRIVER_TYPE_WARP
  };

  HRESULT last_error = E_FAIL;
  if (g_state.system != nullptr) {
    int32_t adapter_index = -1;
    g_state.system->GetDXGIOutputInfo(&adapter_index);

    if (adapter_index >= 0) {
      IDXGIFactory1* factory = nullptr;
      const HRESULT factory_result = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory)
      );

      if (SUCCEEDED(factory_result) && factory != nullptr) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT adapter_result = factory->EnumAdapters1(static_cast<UINT>(adapter_index), &adapter);
        if (SUCCEEDED(adapter_result) && adapter != nullptr) {
          DXGI_ADAPTER_DESC1 adapter_desc = {};
          (void)adapter->GetDesc1(&adapter_desc);

          D3D_FEATURE_LEVEL acquired_feature_level = D3D_FEATURE_LEVEL_10_0;
          last_error = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            requested_feature_levels,
            ARRAYSIZE(requested_feature_levels),
            D3D11_SDK_VERSION,
            &g_state.d3d_device,
            &acquired_feature_level,
            &g_state.d3d_context
          );

          if (SUCCEEDED(last_error)) {
            std::cout
              << "OpenVR D3D11 device created on SteamVR adapter index "
              << adapter_index
              << " (vendorId=0x" << std::hex << adapter_desc.VendorId
              << ", deviceId=0x" << adapter_desc.DeviceId
              << std::dec << ")"
              << std::endl;
            if (g_state.d3d_device != nullptr) {
              (void)g_state.d3d_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_state.d3d_device1));
            }
            adapter->Release();
            factory->Release();
            return true;
          }

          std::cout
            << "OpenVR D3D11 device creation failed on SteamVR adapter index "
            << adapter_index
            << " with error "
            << HResultToString(last_error)
            << "; falling back to default adapter selection."
            << std::endl;
          adapter->Release();
        } else {
          std::cout
            << "OpenVR DXGI adapter index "
            << adapter_index
            << " was not available via EnumAdapters1 ("
            << HResultToString(adapter_result)
            << "); falling back to default adapter selection."
            << std::endl;
        }

        factory->Release();
      } else {
        std::cout
          << "CreateDXGIFactory1 failed while preparing the OpenVR D3D11 device ("
          << HResultToString(factory_result)
          << "); falling back to default adapter selection."
          << std::endl;
      }
    } else {
      std::cout << "OpenVR did not report a DXGI adapter index; falling back to default adapter selection." << std::endl;
    }
  }

  for (const D3D_DRIVER_TYPE driver_type : driver_types) {
    D3D_FEATURE_LEVEL acquired_feature_level = D3D_FEATURE_LEVEL_10_0;
    last_error = D3D11CreateDevice(
      nullptr,
      driver_type,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      requested_feature_levels,
      ARRAYSIZE(requested_feature_levels),
      D3D11_SDK_VERSION,
      &g_state.d3d_device,
      &acquired_feature_level,
      &g_state.d3d_context
    );

    if (SUCCEEDED(last_error)) {
      if (g_state.d3d_device != nullptr) {
        (void)g_state.d3d_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&g_state.d3d_device1));
      }
      return true;
    }
  }

  SetError(error_message, "Failed to create a D3D11 device for OpenVR texture import (" + HResultToString(last_error) + ").");
  ReleaseD3DResources();
  return false;
}

void LogSharedTextureDescOnce(const D3D11_TEXTURE2D_DESC& desc) {
  if (g_state.logged_shared_texture_desc) {
    return;
  }

  g_state.logged_shared_texture_desc = true;
  std::cout
    << "OpenVR shared texture desc: "
    << "width=" << desc.Width
    << ", height=" << desc.Height
    << ", format=" << DxgiFormatToString(desc.Format)
    << ", bindFlags=0x" << std::hex << desc.BindFlags
    << ", miscFlags=0x" << desc.MiscFlags
    << std::dec
    << ", sampleCount=" << desc.SampleDesc.Count
    << std::endl;
}

bool EnsureSubmitTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, std::string* error_message) {
  if (g_state.submit_texture != nullptr &&
      g_state.submit_texture_format == format &&
      g_state.submit_texture_width == width &&
      g_state.submit_texture_height == height) {
    return true;
  }

  ReleaseSubmitTexture();

  D3D11_TEXTURE2D_DESC submit_desc = {};
  submit_desc.Width = width;
  submit_desc.Height = height;
  submit_desc.MipLevels = 1;
  submit_desc.ArraySize = 1;
  submit_desc.Format = format;
  submit_desc.SampleDesc.Count = 1;
  submit_desc.SampleDesc.Quality = 0;
  submit_desc.Usage = D3D11_USAGE_DEFAULT;
  submit_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  submit_desc.CPUAccessFlags = 0;
  submit_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  const HRESULT create_result = g_state.d3d_device->CreateTexture2D(&submit_desc, nullptr, &g_state.submit_texture);
  if (FAILED(create_result) || g_state.submit_texture == nullptr) {
    SetError(error_message, "Failed to create an intermediate D3D11 texture for OpenVR submission (" + HResultToString(create_result) + ").");
    ReleaseSubmitTexture();
    return false;
  }

  IDXGIResource* dxgi_resource = nullptr;
  const HRESULT query_result = g_state.submit_texture->QueryInterface(
    __uuidof(IDXGIResource),
    reinterpret_cast<void**>(&dxgi_resource)
  );
  if (FAILED(query_result) || dxgi_resource == nullptr) {
    SetError(error_message, "Failed to query an IDXGIResource for the OpenVR submit texture (" + HResultToString(query_result) + ").");
    ReleaseSubmitTexture();
    return false;
  }

  const HRESULT shared_handle_result = dxgi_resource->GetSharedHandle(&g_state.submit_texture_shared_handle);
  dxgi_resource->Release();
  if (FAILED(shared_handle_result) || g_state.submit_texture_shared_handle == nullptr) {
    SetError(error_message, "Failed to get a DXGI shared handle for the OpenVR submit texture (" + HResultToString(shared_handle_result) + ").");
    ReleaseSubmitTexture();
    return false;
  }

  g_state.submit_texture_format = submit_desc.Format;
  g_state.submit_texture_width = submit_desc.Width;
  g_state.submit_texture_height = submit_desc.Height;
  return true;
}

bool ConfigureOverlayTextureFromSubmitTexture(vr::Texture_t* texture, std::string* error_message) {
  if (texture == nullptr || g_state.submit_texture == nullptr) {
    SetError(error_message, "OpenVR submit texture is unavailable.");
    return false;
  }

  if (g_state.submit_texture_shared_handle == nullptr) {
    SetError(error_message, "OpenVR submit texture does not expose a DXGI shared handle.");
    return false;
  }

  texture->handle = g_state.submit_texture_shared_handle;
  texture->eType = vr::TextureType_DXGISharedHandle;
  texture->eColorSpace = vr::ColorSpace_Auto;
  return true;
}

bool EnsureSubmitTexture(const D3D11_TEXTURE2D_DESC& source_desc, std::string* error_message) {
  return EnsureSubmitTexture(source_desc.Width, source_desc.Height, source_desc.Format, error_message);
}

bool OpenSharedTextureFromHandle(uint64_t shared_handle, std::string* error_message) {
  if (g_state.shared_texture != nullptr && g_state.shared_texture_handle_value == shared_handle) {
    return true;
  }

  ReleaseSharedTexture();

  if (!EnsureD3D11Device(error_message)) {
    return false;
  }

  const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(shared_handle));
  HRESULT open_result = E_FAIL;

  if (g_state.d3d_device1 != nullptr) {
    open_result = g_state.d3d_device1->OpenSharedResource1(
      handle,
      __uuidof(ID3D11Texture2D),
      reinterpret_cast<void**>(&g_state.shared_texture)
    );

    if (SUCCEEDED(open_result) && g_state.shared_texture != nullptr) {
      g_state.shared_texture_handle_value = shared_handle;
      return true;
    }
  }

  open_result = g_state.d3d_device->OpenSharedResource(
    handle,
    __uuidof(ID3D11Texture2D),
    reinterpret_cast<void**>(&g_state.shared_texture)
  );

  if (FAILED(open_result) || g_state.shared_texture == nullptr) {
    SetError(error_message, "Failed to open Windows shared texture handle for OpenVR (" + HResultToString(open_result) + ").");
    ReleaseSharedTexture();
    return false;
  }

  g_state.shared_texture_handle_value = shared_handle;
  D3D11_TEXTURE2D_DESC shared_desc = {};
  g_state.shared_texture->GetDesc(&shared_desc);
  LogSharedTextureDescOnce(shared_desc);
  return true;
}
#endif

#if defined(_WIN32)
bool InitializeVRSystem(vr::IVRSystem** system, vr::EVRInitError* init_error) {
  __try {
    *system = vr::VR_Init(init_error, vr::VRApplication_Overlay);
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

bool AcquireOverlayInterface(vr::IVROverlay** overlay) {
  __try {
    *overlay = vr::VROverlay();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (overlay != nullptr) {
      *overlay = nullptr;
    }
    return false;
  }
}

bool CreateOverlayHandle(
  vr::IVROverlay* overlay,
  const char* overlay_key,
  const char* overlay_name,
  vr::VROverlayHandle_t* overlay_handle,
  vr::EVROverlayError* overlay_error) {
  __try {
    *overlay_error = overlay->CreateOverlay(overlay_key, overlay_name, overlay_handle);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (overlay_handle != nullptr) {
      *overlay_handle = vr::k_ulOverlayHandleInvalid;
    }
    if (overlay_error != nullptr) {
      *overlay_error = vr::VROverlayError_UnknownOverlay;
    }
    return false;
  }
}

bool SetPremultipliedOverlayFlag(vr::IVROverlay* overlay, vr::VROverlayHandle_t overlay_handle, bool enabled, vr::EVROverlayError* overlay_error) {
  __try {
    *overlay_error = overlay->SetOverlayFlag(overlay_handle, vr::VROverlayFlags_IsPremultiplied, enabled);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (overlay_error != nullptr) {
      *overlay_error = vr::VROverlayError_UnknownOverlay;
    }
    return false;
  }
}
#else
bool InitializeVRSystem(vr::IVRSystem** system, vr::EVRInitError* init_error) {
  *system = vr::VR_Init(init_error, vr::VRApplication_Overlay);
  return true;
}

bool AcquireOverlayInterface(vr::IVROverlay** overlay) {
  *overlay = vr::VROverlay();
  return true;
}

bool CreateOverlayHandle(
  vr::IVROverlay* overlay,
  const char* overlay_key,
  const char* overlay_name,
  vr::VROverlayHandle_t* overlay_handle,
  vr::EVROverlayError* overlay_error) {
  *overlay_error = overlay->CreateOverlay(overlay_key, overlay_name, overlay_handle);
  return true;
}

bool SetPremultipliedOverlayFlag(vr::IVROverlay* overlay, vr::VROverlayHandle_t overlay_handle, bool enabled, vr::EVROverlayError* overlay_error) {
  *overlay_error = overlay->SetOverlayFlag(overlay_handle, vr::VROverlayFlags_IsPremultiplied, enabled);
  return true;
}
#endif

std::string OverlayErrorToString(vr::IVROverlay* overlay, vr::EVROverlayError error) {
  if (overlay == nullptr) {
    return "Unknown OpenVR overlay error";
  }

  const char* name = overlay->GetOverlayErrorNameFromEnum(error);
  return name != nullptr ? std::string(name) : std::string("Unknown OpenVR overlay error");
}

bool CheckOverlayError(vr::EVROverlayError error, std::string context, std::string* error_message) {
  if (error == vr::VROverlayError_None) {
    return true;
  }

  SetError(error_message, context + ": " + OverlayErrorToString(g_state.overlay, error));
  return false;
}

vr::HmdMatrix34_t ToHmdMatrix34(const OverlayPlacement& placement) {
  const float x = placement.rotation.x;
  const float y = placement.rotation.y;
  const float z = placement.rotation.z;
  const float w = placement.rotation.w;

  vr::HmdMatrix34_t matrix = {};
  matrix.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
  matrix.m[0][1] = 2.0f * (x * y - z * w);
  matrix.m[0][2] = 2.0f * (x * z + y * w);
  matrix.m[0][3] = placement.position.x;

  matrix.m[1][0] = 2.0f * (x * y + z * w);
  matrix.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
  matrix.m[1][2] = 2.0f * (y * z - x * w);
  matrix.m[1][3] = placement.position.y;

  matrix.m[2][0] = 2.0f * (x * z - y * w);
  matrix.m[2][1] = 2.0f * (y * z + x * w);
  matrix.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
  matrix.m[2][3] = placement.position.z;
  return matrix;
}

bool ApplyPlacement(const OverlayPlacement& placement, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  const vr::HmdMatrix34_t matrix = ToHmdMatrix34(placement);
  if (placement.mode == OverlayPlacementMode::kHead) {
    return CheckOverlayError(
      g_state.overlay->SetOverlayTransformTrackedDeviceRelative(
        g_state.overlay_handle,
        vr::k_unTrackedDeviceIndex_Hmd,
        &matrix),
      "Failed to set head-locked overlay transform",
      error_message);
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayTransformAbsolute(
      g_state.overlay_handle,
      vr::TrackingUniverseStanding,
      &matrix),
    "Failed to set world-locked overlay transform",
    error_message);
}

bool ApplyVisible(bool visible, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  return CheckOverlayError(
    visible ? g_state.overlay->ShowOverlay(g_state.overlay_handle)
            : g_state.overlay->HideOverlay(g_state.overlay_handle),
    visible ? "Failed to show OpenVR overlay" : "Failed to hide OpenVR overlay",
    error_message);
}

bool ApplySizeMeters(float size_meters, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayWidthInMeters(g_state.overlay_handle, size_meters),
    "Failed to set OpenVR overlay width",
    error_message);
}

bool ApplyCurvature(float curvature, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  if (curvature < 0.0f || curvature > 1.0f || !std::isfinite(curvature)) {
    SetError(error_message, "OpenVR overlay curvature must be between 0 and 1.");
    return false;
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayCurvature(g_state.overlay_handle, curvature),
    "Failed to set OpenVR overlay curvature",
    error_message);
}

#if defined(__linux__)
bool IsTruthyEnvironmentVariable(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' &&
         std::strcmp(value, "0") != 0 &&
         std::strcmp(value, "false") != 0 &&
         std::strcmp(value, "off") != 0;
}

bool WaitForDmabufProducer(int fd, std::string* error_message) {
  dma_buf_export_sync_file export_sync{};
  export_sync.flags = DMA_BUF_SYNC_READ;
  export_sync.fd = -1;
  if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export_sync) != 0 || export_sync.fd < 0) {
    SetError(
      error_message,
      "Failed to export Electron's DMA-BUF producer fence: " +
        std::string(std::strerror(errno)) + ".");
    return false;
  }

  pollfd producer_fence{};
  producer_fence.fd = export_sync.fd;
  producer_fence.events = POLLIN;
  int poll_result = -1;
  do {
    poll_result = poll(&producer_fence, 1, 1000);
  } while (poll_result < 0 && errno == EINTR);
  close(export_sync.fd);
  if (poll_result <= 0 || (producer_fence.revents & (POLLERR | POLLNVAL)) != 0) {
    SetError(
      error_message,
      poll_result == 0
        ? "Timed out waiting for Electron's DMA-BUF producer fence."
        : "Failed while waiting for Electron's DMA-BUF producer fence.");
    return false;
  }
  return true;
}

void LogLinuxFrameSyncResult(vr::EVROverlayError sync_error, const char* submission_name) {
  if (sync_error == vr::VROverlayError_None) {
    return;
  }
  if (sync_error == vr::VROverlayError_TimedOut) {
    if (!g_state.logged_frame_sync_timeout) {
      g_state.logged_frame_sync_timeout = true;
      std::cerr << "Linux OpenVR warning: WaitFrameSync timed out after " << submission_name
                << "; submitted resources will not be reused until synchronization succeeds." << std::endl;
    }
    return;
  }

  const char* sync_error_name = g_state.overlay->GetOverlayErrorNameFromEnum(sync_error);
  std::cerr << "Linux OpenVR warning: WaitFrameSync failed after " << submission_name << ": "
            << (sync_error_name != nullptr ? sync_error_name : "unknown error") << std::endl;
}

bool ParseInteger(const std::string& value, uint64_t* result) {
  if (value.empty() || result == nullptr) {
    return false;
  }

  try {
    *result = static_cast<uint64_t>(std::stoull(value, nullptr, 0));
    return true;
  } catch (...) {
    return false;
  }
}

bool TryParsePixelFormat(const std::string& pixel_format, uint32_t* result) {
  if (result == nullptr) {
    return false;
  }

  uint64_t parsed_format = 0;
  if (ParseInteger(pixel_format, &parsed_format)) {
    *result = static_cast<uint32_t>(parsed_format);
    return true;
  }

  if (pixel_format == "rgba") {
    *result = DRM_FORMAT_ABGR8888;
    return true;
  }
  if (pixel_format == "bgra") {
    *result = DRM_FORMAT_ARGB8888;
    return true;
  }
  if (pixel_format == "ARGB8888") {
    *result = DRM_FORMAT_ARGB8888;
    return true;
  }
  if (pixel_format == "ABGR8888") {
    *result = DRM_FORMAT_ABGR8888;
    return true;
  }
  if (pixel_format == "XRGB8888") {
    *result = DRM_FORMAT_XRGB8888;
    return true;
  }
  if (pixel_format == "XBGR8888") {
    *result = DRM_FORMAT_XBGR8888;
    return true;
  }

  return false;
}

bool TryParseModifier(const std::string& modifier, uint64_t* result) {
  if (result == nullptr) {
    return false;
  }

  if (modifier.empty()) {
    *result = DRM_FORMAT_MOD_INVALID;
    return true;
  }

  return ParseInteger(modifier, result);
}

std::vector<std::string> SplitVulkanExtensions(const std::vector<char>& buffer) {
  std::vector<std::string> extensions;
  if (buffer.empty()) {
    return extensions;
  }
  std::istringstream stream(buffer.data());
  std::string extension;
  while (stream >> extension) {
    extensions.push_back(extension);
  }
  return extensions;
}

bool HasVulkanDeviceExtension(VkPhysicalDevice physical_device, const char* name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
    return false;
  }
  return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

void AppendUniqueExtension(std::vector<std::string>* extensions, const char* name) {
  const auto found = std::find(extensions->begin(), extensions->end(), name);
  if (found == extensions->end()) {
    extensions->emplace_back(name);
  }
}

void ReleaseLinuxOpenVRVulkanResources() {
  if (g_state.vulkan_device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(g_state.vulkan_device);
    if (g_state.vulkan_staging_mapping != nullptr) {
      vkUnmapMemory(g_state.vulkan_device, g_state.vulkan_staging_memory);
    }
    if (g_state.vulkan_staging_buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(g_state.vulkan_device, g_state.vulkan_staging_buffer, nullptr);
    }
    if (g_state.vulkan_staging_memory != VK_NULL_HANDLE) {
      vkFreeMemory(g_state.vulkan_device, g_state.vulkan_staging_memory, nullptr);
    }
    if (g_state.vulkan_visible_image != VK_NULL_HANDLE) {
      vkDestroyImage(g_state.vulkan_device, g_state.vulkan_visible_image, nullptr);
    }
    if (g_state.vulkan_visible_memory != VK_NULL_HANDLE) {
      vkFreeMemory(g_state.vulkan_device, g_state.vulkan_visible_memory, nullptr);
    }
    for (VkImage image : g_state.vulkan_retired_images) {
      vkDestroyImage(g_state.vulkan_device, image, nullptr);
    }
    for (VkDeviceMemory memory : g_state.vulkan_retired_memory) {
      vkFreeMemory(g_state.vulkan_device, memory, nullptr);
    }
    if (g_state.vulkan_command_pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(g_state.vulkan_device, g_state.vulkan_command_pool, nullptr);
    }
    vkDestroyDevice(g_state.vulkan_device, nullptr);
  }
  if (g_state.vulkan_instance != VK_NULL_HANDLE) {
    vkDestroyInstance(g_state.vulkan_instance, nullptr);
  }
  g_state.vulkan_instance = VK_NULL_HANDLE;
  g_state.vulkan_physical_device = VK_NULL_HANDLE;
  g_state.vulkan_device = VK_NULL_HANDLE;
  g_state.vulkan_queue = VK_NULL_HANDLE;
  g_state.vulkan_command_pool = VK_NULL_HANDLE;
  g_state.vulkan_queue_family = 0;
  g_state.vulkan_memory_properties = {};
  g_state.vulkan_get_memory_fd_properties = nullptr;
  g_state.vulkan_dmabuf_import_enabled = false;
  g_state.vulkan_visible_image = VK_NULL_HANDLE;
  g_state.vulkan_visible_memory = VK_NULL_HANDLE;
  g_state.vulkan_retired_images.clear();
  g_state.vulkan_retired_memory.clear();
  g_state.vulkan_visible_width = 0;
  g_state.vulkan_visible_height = 0;
  g_state.vulkan_visible_format = VK_FORMAT_UNDEFINED;
  g_state.vulkan_visible_initialized = false;
  g_state.vulkan_visible_safe_to_write = true;
  g_state.vulkan_staging_buffer = VK_NULL_HANDLE;
  g_state.vulkan_staging_memory = VK_NULL_HANDLE;
  g_state.vulkan_staging_mapping = nullptr;
  g_state.vulkan_staging_size = 0;
  g_state.vulkan_staging_coherent = false;
  g_state.vulkan_frame_count = 0;
  g_state.logged_vulkan_submission = false;
}

bool EnsureLinuxOpenVRVulkan(std::string* error_message) {
  if (g_state.vulkan_device != VK_NULL_HANDLE) {
    return true;
  }

  vr::IVRCompositor* compositor = vr::VRCompositor();
  if (compositor == nullptr) {
    SetError(error_message, "OpenVR compositor is unavailable for Vulkan submission.");
    return false;
  }

  const uint32_t instance_extension_size = compositor->GetVulkanInstanceExtensionsRequired(nullptr, 0);
  std::vector<char> instance_extension_buffer(instance_extension_size, '\0');
  if (instance_extension_size > 0 &&
      compositor->GetVulkanInstanceExtensionsRequired(instance_extension_buffer.data(), instance_extension_size) == 0) {
    SetError(error_message, "OpenVR failed to report its required Vulkan instance extensions.");
    return false;
  }
  std::vector<std::string> instance_extensions = SplitVulkanExtensions(instance_extension_buffer);
  std::vector<const char*> instance_extension_names;
  instance_extension_names.reserve(instance_extensions.size());
  for (const std::string& extension : instance_extensions) {
    instance_extension_names.push_back(extension.c_str());
  }

  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "electron-vr OpenVR overlay";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &application;
  instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extension_names.size());
  instance_info.ppEnabledExtensionNames = instance_extension_names.data();
  if (vkCreateInstance(&instance_info, nullptr, &g_state.vulkan_instance) != VK_SUCCESS) {
    SetError(error_message, "Failed to create the Vulkan instance required for OpenVR submission.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }

  uint64_t output_device = 0;
  g_state.system->GetOutputDevice(&output_device, vr::TextureType_Vulkan, g_state.vulkan_instance);
  g_state.vulkan_physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(output_device));
  if (g_state.vulkan_physical_device == VK_NULL_HANDLE) {
    SetError(error_message, "OpenVR did not report a Vulkan physical device for the overlay.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }

  const uint32_t device_extension_size = compositor->GetVulkanDeviceExtensionsRequired(
    g_state.vulkan_physical_device, nullptr, 0);
  std::vector<char> device_extension_buffer(device_extension_size, '\0');
  if (device_extension_size > 0 &&
      compositor->GetVulkanDeviceExtensionsRequired(
        g_state.vulkan_physical_device, device_extension_buffer.data(), device_extension_size) == 0) {
    SetError(error_message, "OpenVR failed to report its required Vulkan device extensions.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }
  std::vector<std::string> device_extensions = SplitVulkanExtensions(device_extension_buffer);
  const char* import_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
  };
  g_state.vulkan_dmabuf_import_enabled = std::all_of(
    std::begin(import_extensions), std::end(import_extensions), [](const char* extension) {
      return HasVulkanDeviceExtension(g_state.vulkan_physical_device, extension);
    });
  if (g_state.vulkan_dmabuf_import_enabled) {
    for (const char* extension : import_extensions) {
      AppendUniqueExtension(&device_extensions, extension);
    }
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g_state.vulkan_physical_device, &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(
    g_state.vulkan_physical_device, &queue_family_count, queue_families.data());
  bool found_queue = false;
  for (uint32_t index = 0; index < queue_family_count; ++index) {
    if (queue_families[index].queueCount > 0 &&
        (queue_families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      g_state.vulkan_queue_family = index;
      found_queue = true;
      break;
    }
  }
  if (!found_queue) {
    SetError(error_message, "OpenVR's Vulkan device does not expose a graphics queue.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }

  std::vector<const char*> device_extension_names;
  device_extension_names.reserve(device_extensions.size());
  for (const std::string& extension : device_extensions) {
    device_extension_names.push_back(extension.c_str());
  }
  float queue_priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = g_state.vulkan_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &queue_priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(device_extension_names.size());
  device_info.ppEnabledExtensionNames = device_extension_names.data();
  if (vkCreateDevice(g_state.vulkan_physical_device, &device_info, nullptr, &g_state.vulkan_device) != VK_SUCCESS) {
    SetError(error_message, "Failed to create OpenVR's Vulkan device for DMA-BUF import.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }
  vkGetDeviceQueue(g_state.vulkan_device, g_state.vulkan_queue_family, 0, &g_state.vulkan_queue);
  vkGetPhysicalDeviceMemoryProperties(g_state.vulkan_physical_device, &g_state.vulkan_memory_properties);
  if (g_state.vulkan_dmabuf_import_enabled) {
    g_state.vulkan_get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
      vkGetDeviceProcAddr(g_state.vulkan_device, "vkGetMemoryFdPropertiesKHR"));
    if (g_state.vulkan_get_memory_fd_properties == nullptr) {
      g_state.vulkan_dmabuf_import_enabled = false;
    }
  }

  VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool_info.queueFamilyIndex = g_state.vulkan_queue_family;
  command_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  if (g_state.vulkan_queue == VK_NULL_HANDLE ||
      vkCreateCommandPool(g_state.vulkan_device, &command_pool_info, nullptr, &g_state.vulkan_command_pool) != VK_SUCCESS) {
    SetError(error_message, "Failed to initialize Vulkan commands for OpenVR.");
    ReleaseLinuxOpenVRVulkanResources();
    return false;
  }

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(g_state.vulkan_physical_device, &properties);
  std::cout << "Linux OpenVR Vulkan submission initialized on " << properties.deviceName
            << " (queue family " << g_state.vulkan_queue_family << ")." << std::endl;
  return true;
}

bool FindOpenVRVulkanMemoryType(uint32_t type_bits, uint32_t* memory_type) {
  for (uint32_t index = 0; index < g_state.vulkan_memory_properties.memoryTypeCount; ++index) {
    if ((type_bits & (1U << index)) != 0) {
      *memory_type = index;
      return true;
    }
  }
  return false;
}

bool FindOpenVRVulkanMemoryType(
  uint32_t type_bits,
  VkMemoryPropertyFlags required_properties,
  uint32_t* memory_type) {
  for (uint32_t index = 0; index < g_state.vulkan_memory_properties.memoryTypeCount; ++index) {
    const VkMemoryPropertyFlags properties =
      g_state.vulkan_memory_properties.memoryTypes[index].propertyFlags;
    if ((type_bits & (1U << index)) != 0 &&
        (properties & required_properties) == required_properties) {
      *memory_type = index;
      return true;
    }
  }
  return false;
}

VkFormat OpenVRVulkanFormat(uint32_t drm_format) {
  if (drm_format == DRM_FORMAT_ARGB8888) {
    return VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (drm_format == DRM_FORMAT_ABGR8888) {
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

bool ImportLinuxOpenVRVulkanImage(
  const LinuxTextureInfo& texture_info,
  uint32_t drm_format,
  uint64_t modifier,
  VkImage* imported_image,
  VkDeviceMemory* imported_memory,
  VkFormat* imported_format,
  std::string* error_message) {
  if (!g_state.vulkan_dmabuf_import_enabled) {
    SetError(error_message, "OpenVR's Vulkan device does not support DMA-BUF import; use software upload.");
    return false;
  }
  const LinuxPlaneInfo& plane = texture_info.planes.front();
  if ((modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR) ||
      plane.stride < texture_info.width * 4ULL ||
      plane.offset > plane.size ||
      static_cast<uint64_t>(plane.stride) * texture_info.height > plane.size - plane.offset) {
    SetError(error_message, "Linux OpenVR Vulkan import requires Electron's single-plane linear DMA-BUF contract.");
    return false;
  }
  const VkFormat format = OpenVRVulkanFormat(drm_format);
  if (format == VK_FORMAT_UNDEFINED) {
    SetError(error_message, "Linux OpenVR Vulkan import received an unsupported DRM format.");
    return false;
  }

  VkDrmFormatModifierPropertiesListEXT modifier_list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 format_properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  format_properties.pNext = &modifier_list;
  vkGetPhysicalDeviceFormatProperties2(g_state.vulkan_physical_device, format, &format_properties);
  std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(modifier_list.drmFormatModifierCount);
  modifier_list.pDrmFormatModifierProperties = modifiers.data();
  vkGetPhysicalDeviceFormatProperties2(g_state.vulkan_physical_device, format, &format_properties);
  const auto linear_modifier = std::find_if(modifiers.begin(), modifiers.end(), [](const auto& candidate) {
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    return candidate.drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
      candidate.drmFormatModifierPlaneCount == 1 &&
      (candidate.drmFormatModifierTilingFeatures & required) == required;
  });
  if (linear_modifier == modifiers.end()) {
    SetError(error_message, "OpenVR's Vulkan device cannot sample and transfer Electron's linear DMA-BUF format.");
    return false;
  }

  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int import_fd = -1;
  const auto cleanup = [&]() {
    if (fence != VK_NULL_HANDLE) vkDestroyFence(g_state.vulkan_device, fence, nullptr);
    if (command != VK_NULL_HANDLE) vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
    if (image != VK_NULL_HANDLE) vkDestroyImage(g_state.vulkan_device, image, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(g_state.vulkan_device, memory, nullptr);
    if (import_fd >= 0) close(import_fd);
  };
  const auto fail = [&](const char* message) {
    SetError(error_message, message);
    cleanup();
    return false;
  };

  VkSubresourceLayout plane_layout{};
  plane_layout.offset = plane.offset;
  plane_layout.rowPitch = plane.stride;
  VkExternalMemoryImageCreateInfo external_image{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_modifier{
    VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
  explicit_modifier.pNext = &external_image;
  explicit_modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  explicit_modifier.drmFormatModifierPlaneCount = 1;
  explicit_modifier.pPlaneLayouts = &plane_layout;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.pNext = &explicit_modifier;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {texture_info.width, texture_info.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(g_state.vulkan_device, &image_info, nullptr, &image) != VK_SUCCESS) {
    return fail("Failed to create the OpenVR Vulkan DMA-BUF image.");
  }

  VkMemoryDedicatedRequirements dedicated_requirements{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
  requirements.pNext = &dedicated_requirements;
  VkImageMemoryRequirementsInfo2 requirements_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
  requirements_info.image = image;
  vkGetImageMemoryRequirements2(g_state.vulkan_device, &requirements_info, &requirements);
  import_fd = dup(plane.fd);
  if (import_fd < 0) {
    return fail("Failed to duplicate the OpenVR Vulkan DMA-BUF descriptor.");
  }
  VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  if (g_state.vulkan_get_memory_fd_properties(
        g_state.vulkan_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        import_fd, &fd_properties) != VK_SUCCESS) {
    return fail("vkGetMemoryFdPropertiesKHR failed for the OpenVR DMA-BUF.");
  }
  uint32_t memory_type = 0;
  if (!FindOpenVRVulkanMemoryType(
        requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits, &memory_type)) {
    return fail("The OpenVR Vulkan image and DMA-BUF have no common memory type.");
  }
  VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated_info.image = image;
  VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  import_info.fd = import_fd;
  import_info.pNext = &dedicated_info;
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.pNext = &import_info;
  allocation.allocationSize = requirements.memoryRequirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(g_state.vulkan_device, &allocation, nullptr, &memory) != VK_SUCCESS) {
    return fail("Failed to allocate imported OpenVR Vulkan DMA-BUF memory.");
  }
  import_fd = -1;
  if (vkBindImageMemory(g_state.vulkan_device, image, memory, 0) != VK_SUCCESS) {
    return fail("Failed to bind the OpenVR Vulkan DMA-BUF image.");
  }

  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = g_state.vulkan_command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(g_state.vulkan_device, &command_info, &command) != VK_SUCCESS) {
    return fail("Failed to allocate an OpenVR Vulkan transition command.");
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    return fail("Failed to begin the OpenVR Vulkan transition command.");
  }
  VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  acquire.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  acquire.dstQueueFamilyIndex = g_state.vulkan_queue_family;
  acquire.image = image;
  acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(
    command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
    0, nullptr, 0, nullptr, 1, &acquire);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    return fail("Failed to finish the OpenVR Vulkan transition command.");
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(g_state.vulkan_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    return fail("Failed to create the OpenVR Vulkan transition fence.");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(g_state.vulkan_queue, 1, &submit, fence) != VK_SUCCESS ||
      vkWaitForFences(g_state.vulkan_device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    return fail("Failed to acquire the OpenVR Vulkan DMA-BUF image.");
  }

  vkDestroyFence(g_state.vulkan_device, fence, nullptr);
  fence = VK_NULL_HANDLE;
  vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
  command = VK_NULL_HANDLE;
  *imported_image = image;
  *imported_memory = memory;
  *imported_format = format;
  image = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;
  return true;
}

bool EnsureLinuxOpenVRVulkanSubmitImage(
  uint32_t width,
  uint32_t height,
  VkFormat format,
  std::string* error_message) {
  if (g_state.vulkan_visible_image != VK_NULL_HANDLE &&
      g_state.vulkan_visible_width == width &&
      g_state.vulkan_visible_height == height &&
      g_state.vulkan_visible_format == format) {
    return true;
  }

  if (vkQueueWaitIdle(g_state.vulkan_queue) != VK_SUCCESS) {
    SetError(error_message, "Failed to synchronize before replacing the OpenVR Vulkan submit image.");
    return false;
  }
  if (g_state.vulkan_visible_image != VK_NULL_HANDLE) {
    g_state.vulkan_retired_images.push_back(g_state.vulkan_visible_image);
    g_state.vulkan_visible_image = VK_NULL_HANDLE;
  }
  if (g_state.vulkan_visible_memory != VK_NULL_HANDLE) {
    g_state.vulkan_retired_memory.push_back(g_state.vulkan_visible_memory);
    g_state.vulkan_visible_memory = VK_NULL_HANDLE;
  }

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(
        g_state.vulkan_device, &image_info, nullptr,
        &g_state.vulkan_visible_image) != VK_SUCCESS) {
    SetError(error_message, "Failed to create the persistent OpenVR Vulkan submit image.");
    return false;
  }

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(g_state.vulkan_device, g_state.vulkan_visible_image, &requirements);
  uint32_t memory_type = 0;
  if (!FindOpenVRVulkanMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) &&
      !FindOpenVRVulkanMemoryType(requirements.memoryTypeBits, &memory_type)) {
    vkDestroyImage(g_state.vulkan_device, g_state.vulkan_visible_image, nullptr);
    g_state.vulkan_visible_image = VK_NULL_HANDLE;
    SetError(error_message, "No compatible memory type exists for the persistent OpenVR Vulkan submit image.");
    return false;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(
        g_state.vulkan_device, &allocation, nullptr,
        &g_state.vulkan_visible_memory) != VK_SUCCESS ||
      vkBindImageMemory(
        g_state.vulkan_device, g_state.vulkan_visible_image,
        g_state.vulkan_visible_memory, 0) != VK_SUCCESS) {
    if (g_state.vulkan_visible_memory != VK_NULL_HANDLE) {
      vkFreeMemory(g_state.vulkan_device, g_state.vulkan_visible_memory, nullptr);
    }
    vkDestroyImage(g_state.vulkan_device, g_state.vulkan_visible_image, nullptr);
    g_state.vulkan_visible_image = VK_NULL_HANDLE;
    g_state.vulkan_visible_memory = VK_NULL_HANDLE;
    SetError(error_message, "Failed to allocate the persistent OpenVR Vulkan submit image.");
    return false;
  }

  g_state.vulkan_visible_width = width;
  g_state.vulkan_visible_height = height;
  g_state.vulkan_visible_format = format;
  g_state.vulkan_visible_initialized = false;
  g_state.vulkan_visible_safe_to_write = true;
  return true;
}

bool CheckLinuxOpenVRVulkanSubmitImageWritable(bool* writable, std::string* error_message) {
  *writable = false;
  if (g_state.vulkan_visible_safe_to_write || std::getenv("VR_OVERRIDE") != nullptr) {
    *writable = true;
    return true;
  }
  const vr::EVROverlayError sync_error = g_state.overlay->WaitFrameSync(100);
  LogLinuxFrameSyncResult(sync_error, "pending Vulkan texture submission");
  if (sync_error == vr::VROverlayError_None) {
    g_state.vulkan_visible_safe_to_write = true;
    *writable = true;
    return true;
  }
  if (sync_error == vr::VROverlayError_TimedOut) {
    if (error_message != nullptr) error_message->clear();
    return true;
  }
  SetError(error_message, "OpenVR has not released the previous Vulkan submit image.");
  return false;
}

bool CopyLinuxOpenVRVulkanImage(
  VkImage source,
  uint32_t width,
  uint32_t height,
  std::string* error_message) {
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  const auto cleanup = [&]() {
    if (fence != VK_NULL_HANDLE) vkDestroyFence(g_state.vulkan_device, fence, nullptr);
    if (command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
    }
  };
  const auto fail = [&](const char* message) {
    SetError(error_message, message);
    cleanup();
    return false;
  };

  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = g_state.vulkan_command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(g_state.vulkan_device, &command_info, &command) != VK_SUCCESS) {
    return fail("Failed to allocate the OpenVR Vulkan submit-copy command.");
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    return fail("Failed to begin the OpenVR Vulkan submit-copy command.");
  }

  VkImageMemoryBarrier destination_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  destination_barrier.srcAccessMask = g_state.vulkan_visible_initialized
    ? VK_ACCESS_TRANSFER_READ_BIT
    : 0;
  destination_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  destination_barrier.oldLayout = g_state.vulkan_visible_initialized
    ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
    : VK_IMAGE_LAYOUT_UNDEFINED;
  destination_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  destination_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  destination_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  destination_barrier.image = g_state.vulkan_visible_image;
  destination_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(
    command,
    g_state.vulkan_visible_initialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &destination_barrier);

  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {width, height, 1};
  vkCmdCopyImage(
    command,
    source,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    g_state.vulkan_visible_image,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    1,
    &copy);

  destination_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  destination_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  destination_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  destination_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(
    command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
    0, 0, nullptr, 0, nullptr, 1, &destination_barrier);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    return fail("Failed to finish the OpenVR Vulkan submit-copy command.");
  }

  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(g_state.vulkan_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    return fail("Failed to create the OpenVR Vulkan submit-copy fence.");
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(g_state.vulkan_queue, 1, &submit, fence) != VK_SUCCESS ||
      vkWaitForFences(g_state.vulkan_device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    return fail("Failed to copy into the persistent OpenVR Vulkan submit image.");
  }

  cleanup();
  g_state.vulkan_visible_initialized = true;
  return true;
}

bool UploadLinuxOpenVRVulkanPixels(const SoftwareFrameInfo& frame, std::string* error_message) {
  const VkDeviceSize size = static_cast<VkDeviceSize>(frame.rgba_pixels.size());
  if (g_state.vulkan_staging_buffer == VK_NULL_HANDLE || g_state.vulkan_staging_size != size) {
    if (g_state.vulkan_staging_mapping != nullptr) vkUnmapMemory(g_state.vulkan_device, g_state.vulkan_staging_memory);
    if (g_state.vulkan_staging_buffer != VK_NULL_HANDLE) vkDestroyBuffer(g_state.vulkan_device, g_state.vulkan_staging_buffer, nullptr);
    if (g_state.vulkan_staging_memory != VK_NULL_HANDLE) vkFreeMemory(g_state.vulkan_device, g_state.vulkan_staging_memory, nullptr);
    g_state.vulkan_staging_mapping = nullptr;
    g_state.vulkan_staging_buffer = VK_NULL_HANDLE;
    g_state.vulkan_staging_memory = VK_NULL_HANDLE;

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_state.vulkan_device, &buffer_info, nullptr, &g_state.vulkan_staging_buffer) != VK_SUCCESS) {
      SetError(error_message, "Failed to create the OpenVR Vulkan software staging buffer."); return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_state.vulkan_device, g_state.vulkan_staging_buffer, &requirements);
    uint32_t memory_type = 0;
    if (!FindOpenVRVulkanMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type)) {
      SetError(error_message, "No host-visible memory exists for OpenVR Vulkan software upload."); return false;
    }
    const VkMemoryPropertyFlags properties = g_state.vulkan_memory_properties.memoryTypes[memory_type].propertyFlags;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(g_state.vulkan_device, &allocation, nullptr, &g_state.vulkan_staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(g_state.vulkan_device, g_state.vulkan_staging_buffer, g_state.vulkan_staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(
          g_state.vulkan_device, g_state.vulkan_staging_memory, 0,
          requirements.size, 0, &g_state.vulkan_staging_mapping) != VK_SUCCESS) {
      SetError(error_message, "Failed to allocate the OpenVR Vulkan software staging buffer."); return false;
    }
    g_state.vulkan_staging_size = size;
    g_state.vulkan_staging_coherent = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  }

  std::memcpy(g_state.vulkan_staging_mapping, frame.rgba_pixels.data(), frame.rgba_pixels.size());
  if (!g_state.vulkan_staging_coherent) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = g_state.vulkan_staging_memory;
    range.size = VK_WHOLE_SIZE;
    if (vkFlushMappedMemoryRanges(g_state.vulkan_device, 1, &range) != VK_SUCCESS) {
      SetError(error_message, "Failed to flush OpenVR Vulkan software pixels."); return false;
    }
  }

  VkCommandBuffer command = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = g_state.vulkan_command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(g_state.vulkan_device, &command_info, &command) != VK_SUCCESS) {
    SetError(error_message, "Failed to allocate the OpenVR Vulkan software upload command.");
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
    SetError(error_message, "Failed to begin the OpenVR Vulkan software upload command.");
    return false;
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = g_state.vulkan_visible_initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = g_state.vulkan_visible_initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = g_state.vulkan_visible_image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {frame.width, frame.height, 1};
  vkCmdCopyBufferToImage(command, g_state.vulkan_staging_buffer, g_state.vulkan_visible_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
    SetError(error_message, "Failed to finish the OpenVR Vulkan software upload command.");
    return false;
  }
  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(g_state.vulkan_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
    SetError(error_message, "Failed to create the OpenVR Vulkan software upload fence.");
    return false;
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  const bool copied = vkQueueSubmit(g_state.vulkan_queue, 1, &submit, fence) == VK_SUCCESS &&
    vkWaitForFences(g_state.vulkan_device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
  vkDestroyFence(g_state.vulkan_device, fence, nullptr);
  vkFreeCommandBuffers(g_state.vulkan_device, g_state.vulkan_command_pool, 1, &command);
  if (!copied) { SetError(error_message, "Failed to upload OpenVR Vulkan software pixels."); return false; }
  g_state.vulkan_visible_initialized = true;
  return true;
}

bool SubmitLinuxOpenVRVulkanSoftwareFrame(const SoftwareFrameInfo& frame, std::string* error_message) {
  if (!EnsureLinuxOpenVRVulkan(error_message) ||
      !EnsureLinuxOpenVRVulkanSubmitImage(frame.width, frame.height, VK_FORMAT_R8G8B8A8_UNORM, error_message)) return false;
  bool writable = false;
  if (!CheckLinuxOpenVRVulkanSubmitImageWritable(&writable, error_message)) return false;
  if (!writable) return true;
  if (!UploadLinuxOpenVRVulkanPixels(frame, error_message)) return false;
  vr::VRVulkanTextureData_t data{};
  data.m_nImage = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_state.vulkan_visible_image));
  data.m_pDevice = g_state.vulkan_device; data.m_pPhysicalDevice = g_state.vulkan_physical_device;
  data.m_pInstance = g_state.vulkan_instance; data.m_pQueue = g_state.vulkan_queue;
  data.m_nQueueFamilyIndex = g_state.vulkan_queue_family; data.m_nWidth = frame.width; data.m_nHeight = frame.height;
  data.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM; data.m_nSampleCount = 1;
  vr::Texture_t texture{&data, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
  if (!CheckOverlayError(g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
        "Failed to submit Vulkan software texture to OpenVR overlay", error_message)) return false;
  if (std::getenv("VR_OVERRIDE") == nullptr) {
    g_state.vulkan_visible_safe_to_write = false;
    const vr::EVROverlayError sync_error = g_state.overlay->WaitFrameSync(100);
    LogLinuxFrameSyncResult(sync_error, "Vulkan software texture submission");
    g_state.vulkan_visible_safe_to_write = sync_error == vr::VROverlayError_None;
  }
  if (
      vkQueueWaitIdle(g_state.vulkan_queue) != VK_SUCCESS) return false;
  ++g_state.vulkan_frame_count;
  if (!g_state.logged_vulkan_submission) {
    g_state.logged_vulkan_submission = true;
    std::cout << "Linux OpenVR submitted first software frame through TextureType_Vulkan." << std::endl;
  }
  if (g_state.vulkan_frame_count % 300 == 0) std::cout << "Linux OpenVR Vulkan submission frame count=" << g_state.vulkan_frame_count << std::endl;
  if (error_message) error_message->clear();
  return true;
}

bool SubmitLinuxOpenVRVulkanTexture(
  const LinuxTextureInfo& texture_info,
  uint32_t drm_format,
  uint64_t modifier,
  std::string* error_message) {
  if (!EnsureLinuxOpenVRVulkan(error_message)) {
    return false;
  }

  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  if (!WaitForDmabufProducer(texture_info.planes.front().fd, error_message) ||
      !ImportLinuxOpenVRVulkanImage(
        texture_info, drm_format, modifier, &image, &memory, &format, error_message)) {
    return false;
  }
  const auto release_import = [&]() {
    vkDestroyImage(g_state.vulkan_device, image, nullptr);
    vkFreeMemory(g_state.vulkan_device, memory, nullptr);
  };
  if (!EnsureLinuxOpenVRVulkanSubmitImage(
        texture_info.width, texture_info.height, format, error_message)) {
    release_import();
    return false;
  }
  bool writable = false;
  if (!CheckLinuxOpenVRVulkanSubmitImageWritable(&writable, error_message)) {
    release_import();
    return false;
  }
  if (!writable) {
    release_import();
    return true;
  }
  if (!CopyLinuxOpenVRVulkanImage(
        image, texture_info.width, texture_info.height, error_message)) {
    release_import();
    return false;
  }

  vr::VRVulkanTextureData_t vulkan_texture{};
  vulkan_texture.m_nImage = static_cast<uint64_t>(
    reinterpret_cast<uintptr_t>(g_state.vulkan_visible_image));
  vulkan_texture.m_pDevice = g_state.vulkan_device;
  vulkan_texture.m_pPhysicalDevice = g_state.vulkan_physical_device;
  vulkan_texture.m_pInstance = g_state.vulkan_instance;
  vulkan_texture.m_pQueue = g_state.vulkan_queue;
  vulkan_texture.m_nQueueFamilyIndex = g_state.vulkan_queue_family;
  vulkan_texture.m_nWidth = texture_info.width;
  vulkan_texture.m_nHeight = texture_info.height;
  vulkan_texture.m_nFormat = static_cast<uint32_t>(format);
  vulkan_texture.m_nSampleCount = 1;
  vr::Texture_t texture{};
  texture.handle = &vulkan_texture;
  texture.eType = vr::TextureType_Vulkan;
  texture.eColorSpace = vr::ColorSpace_Auto;
  const vr::EVROverlayError overlay_error = g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture);
  if (!CheckOverlayError(
        overlay_error, "Failed to submit Vulkan texture to OpenVR overlay", error_message)) {
    release_import();
    return false;
  }

  if (std::getenv("VR_OVERRIDE") == nullptr) {
    g_state.vulkan_visible_safe_to_write = false;
    const vr::EVROverlayError sync_error = g_state.overlay->WaitFrameSync(100);
    LogLinuxFrameSyncResult(sync_error, "Vulkan texture submission");
    g_state.vulkan_visible_safe_to_write = sync_error == vr::VROverlayError_None;
  }
  if (vkQueueWaitIdle(g_state.vulkan_queue) != VK_SUCCESS) {
    release_import();
    SetError(error_message, "OpenVR Vulkan queue synchronization failed after texture submission.");
    return false;
  }
  release_import();
  ++g_state.vulkan_frame_count;
  if (!g_state.logged_vulkan_submission) {
    g_state.logged_vulkan_submission = true;
    std::cout << "Linux OpenVR submitted first DMA-BUF through TextureType_Vulkan." << std::endl;
  }
  if (g_state.vulkan_frame_count % 300 == 0) {
    std::cout << "Linux OpenVR Vulkan submission frame count=" << g_state.vulkan_frame_count << std::endl;
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SupportsDmabufFormatModifier(uint32_t format, uint64_t modifier, std::string* error_message) {
  uint32_t format_count = 0;
  if (!g_state.ipc->GetDmabufFormats(&format_count, nullptr) || format_count == 0) {
    SetError(error_message, "SteamVR did not report any supported DMA-BUF formats.");
    return false;
  }

  std::vector<uint32_t> formats(format_count);
  if (!g_state.ipc->GetDmabufFormats(&format_count, formats.data()) ||
      std::find(formats.begin(), formats.begin() + format_count, format) == formats.begin() + format_count) {
    SetError(error_message, "SteamVR does not support Electron's DMA-BUF pixel format.");
    return false;
  }

  uint32_t modifier_count = 0;
  if (!g_state.ipc->GetDmabufModifiers(
        vr::VRApplication_Overlay,
        format,
        &modifier_count,
        nullptr) || modifier_count == 0) {
    SetError(error_message, "SteamVR did not report any supported modifiers for Electron's DMA-BUF pixel format.");
    return false;
  }

  std::vector<uint64_t> modifiers(modifier_count);
  if (!g_state.ipc->GetDmabufModifiers(
        vr::VRApplication_Overlay,
        format,
        &modifier_count,
        modifiers.data()) ||
      std::find(modifiers.begin(), modifiers.begin() + modifier_count, modifier) == modifiers.begin() + modifier_count) {
    std::ostringstream message;
    message << "SteamVR does not support Electron's DMA-BUF modifier " << modifier << "; advertised modifiers:";
    for (uint32_t index = 0; index < modifier_count; ++index) {
      message << " " << modifiers[index];
    }
    message << ".";
    SetError(error_message, message.str());
    return false;
  }

  return true;
}

void ReleaseLinuxOpenGLUploadResources() {
  if (g_state.glx_display == nullptr) {
    return;
  }

  if (g_state.glx_context != nullptr && g_state.glx_pbuffer != 0 &&
      glXMakeContextCurrent(g_state.glx_display, g_state.glx_pbuffer, g_state.glx_pbuffer, g_state.glx_context)) {
    if (g_state.gl_upload_texture != 0) {
      glDeleteTextures(1, &g_state.gl_upload_texture);
    }
    glXMakeContextCurrent(g_state.glx_display, None, None, nullptr);
  }

  g_state.gl_upload_texture = 0;
  if (g_state.glx_context != nullptr) {
    glXDestroyContext(g_state.glx_display, g_state.glx_context);
    g_state.glx_context = nullptr;
  }
  if (g_state.glx_pbuffer != 0) {
    glXDestroyPbuffer(g_state.glx_display, g_state.glx_pbuffer);
    g_state.glx_pbuffer = 0;
  }
  XCloseDisplay(g_state.glx_display);
  g_state.glx_display = nullptr;
  g_state.gl_upload_width = 0;
  g_state.gl_upload_height = 0;
  g_state.gl_upload_frame_count = 0;
  g_state.gl_upload_changed_frame_count = 0;
  g_state.gl_upload_last_checksum = 0;
  g_state.logged_gl_upload = false;
}

bool EnsureLinuxOpenGLUploadTexture(uint32_t width, uint32_t height, std::string* error_message) {
  if (g_state.glx_display == nullptr) {
    g_state.glx_display = XOpenDisplay(nullptr);
    if (g_state.glx_display == nullptr) {
      SetError(error_message, "Failed to open the X11 display for Linux OpenVR OpenGL upload.");
      return false;
    }

    const int framebuffer_attributes[] = {
      GLX_X_RENDERABLE, True,
      GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
      GLX_RENDER_TYPE, GLX_RGBA_BIT,
      GLX_RED_SIZE, 8,
      GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8,
      GLX_ALPHA_SIZE, 8,
      None
    };
    int framebuffer_count = 0;
    GLXFBConfig* framebuffer_configs = glXChooseFBConfig(
      g_state.glx_display,
      DefaultScreen(g_state.glx_display),
      framebuffer_attributes,
      &framebuffer_count);
    if (framebuffer_configs == nullptr || framebuffer_count == 0) {
      if (framebuffer_configs != nullptr) {
        XFree(framebuffer_configs);
      }
      SetError(error_message, "Failed to choose a GLX framebuffer configuration for Linux OpenVR upload.");
      ReleaseLinuxOpenGLUploadResources();
      return false;
    }

    const GLXFBConfig framebuffer_config = framebuffer_configs[0];
    XFree(framebuffer_configs);
    const int pbuffer_attributes[] = {
      GLX_PBUFFER_WIDTH, 1,
      GLX_PBUFFER_HEIGHT, 1,
      None
    };
    g_state.glx_pbuffer = glXCreatePbuffer(g_state.glx_display, framebuffer_config, pbuffer_attributes);
    g_state.glx_context = glXCreateNewContext(
      g_state.glx_display,
      framebuffer_config,
      GLX_RGBA_TYPE,
      nullptr,
      True);
    if (g_state.glx_pbuffer == 0 || g_state.glx_context == nullptr) {
      SetError(error_message, "Failed to create the GLX upload context for Linux OpenVR.");
      ReleaseLinuxOpenGLUploadResources();
      return false;
    }
  }

  if (!glXMakeContextCurrent(
        g_state.glx_display,
        g_state.glx_pbuffer,
        g_state.glx_pbuffer,
        g_state.glx_context)) {
    SetError(error_message, "Failed to activate the GLX upload context for Linux OpenVR.");
    return false;
  }

  if (g_state.gl_upload_texture == 0) {
    glGenTextures(1, &g_state.gl_upload_texture);
    glBindTexture(GL_TEXTURE_2D, g_state.gl_upload_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  } else {
    glBindTexture(GL_TEXTURE_2D, g_state.gl_upload_texture);
  }

  if (g_state.gl_upload_width != width || g_state.gl_upload_height != height) {
    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_RGBA8,
      static_cast<GLsizei>(width),
      static_cast<GLsizei>(height),
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      nullptr);
    g_state.gl_upload_width = width;
    g_state.gl_upload_height = height;
  }

  if (g_state.gl_upload_texture == 0 || glGetError() != GL_NO_ERROR) {
    glBindTexture(GL_TEXTURE_2D, 0);
    SetError(error_message, "Failed to allocate the Linux OpenVR OpenGL upload texture.");
    return false;
  }

  return true;
}

bool SubmitLinuxOpenGLSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message) {
  Display* previous_display = glXGetCurrentDisplay();
  GLXContext previous_context = glXGetCurrentContext();
  const GLXDrawable previous_drawable = glXGetCurrentDrawable();
  const GLXDrawable previous_read_drawable = glXGetCurrentReadDrawable();
  const auto restore_context = [&]() {
    if (previous_display != nullptr && previous_context != nullptr) {
      glXMakeContextCurrent(previous_display, previous_drawable, previous_read_drawable, previous_context);
    } else if (g_state.glx_display != nullptr) {
      glXMakeContextCurrent(g_state.glx_display, None, None, nullptr);
    }
  };

  if (!EnsureLinuxOpenGLUploadTexture(frame_info.width, frame_info.height, error_message)) {
    restore_context();
    return false;
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0,
    0,
    static_cast<GLsizei>(frame_info.width),
    static_cast<GLsizei>(frame_info.height),
    GL_RGBA,
    GL_UNSIGNED_BYTE,
    frame_info.rgba_pixels.data());
  glBindTexture(GL_TEXTURE_2D, 0);
  glFlush();
  if (glGetError() != GL_NO_ERROR) {
    restore_context();
    SetError(error_message, "Failed to upload the Linux OpenVR software frame to OpenGL.");
    return false;
  }

  vr::Texture_t texture = {};
  texture.handle = reinterpret_cast<void*>(static_cast<intptr_t>(g_state.gl_upload_texture));
  texture.eType = vr::TextureType_OpenGL;
  texture.eColorSpace = vr::ColorSpace_Auto;
  const vr::EVROverlayError overlay_error = g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture);
  restore_context();

  if (!CheckOverlayError(
        overlay_error,
        "Failed to submit Linux OpenGL texture to OpenVR overlay",
        error_message)) {
    return false;
  }

  vr::IVRCompositor* compositor = vr::VRCompositor();
  if (compositor == nullptr) {
    SetError(error_message, "Failed to acquire the OpenVR compositor needed to present the OpenGL overlay.");
    return false;
  }
  const vr::EVRCompositorError compositor_error = compositor->WaitGetPoses(nullptr, 0, nullptr, 0);
  if (compositor_error != vr::VRCompositorError_None) {
    SetError(
      error_message,
      "Failed to begin the OpenVR OpenGL overlay frame: compositor error " +
        std::to_string(static_cast<int>(compositor_error)) + ".");
    return false;
  }
  compositor->PostPresentHandoff();

  uint64_t checksum = 1469598103934665603ull;
  for (size_t index = 0; index < frame_info.rgba_pixels.size(); index += 16) {
    checksum ^= frame_info.rgba_pixels[index];
    checksum *= 1099511628211ull;
  }
  ++g_state.gl_upload_frame_count;
  if (g_state.gl_upload_frame_count == 1 || checksum != g_state.gl_upload_last_checksum) {
    ++g_state.gl_upload_changed_frame_count;
  }
  g_state.gl_upload_last_checksum = checksum;

  if (!g_state.logged_gl_upload) {
    g_state.logged_gl_upload = true;
    std::cout << "Linux OpenVR submitted first software frame through OpenGL upload." << std::endl;
  }
  if (g_state.gl_upload_frame_count % 30 == 0) {
    std::cout << "Linux OpenVR OpenGL upload frame count=" << g_state.gl_upload_frame_count
              << " changed=" << g_state.gl_upload_changed_frame_count << std::endl;
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

void ReleaseLinuxImportedTextures() {
  if (g_state.ipc != nullptr) {
    for (const OpenVRState::ImportedTexture& texture : g_state.imported_textures) {
      g_state.ipc->UnrefResource(texture.handle);
    }
  }
  g_state.imported_textures.clear();
}
#endif

void ResetState() {
#if defined(_WIN32)
  ReleaseD3DResources();
  g_state.logged_shared_texture_desc = false;
#endif
#if defined(__linux__)
  ReleaseLinuxImportedTextures();
  ReleaseLinuxOpenGLUploadResources();
  ReleaseLinuxOpenVRVulkanResources();
  g_state.ipc = nullptr;
  g_state.logged_frame_sync_timeout = false;
  g_state.vulkan_disabled = false;
#endif
  g_state.system = nullptr;
  g_state.overlay = nullptr;
  g_state.overlay_handle = vr::k_ulOverlayHandleInvalid;
  g_state.initialized = false;
}

}  // namespace

bool InitializeOpenVRBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    SetError(error_message, "OpenVR backend requires a non-empty overlay name.");
    return false;
  }

  ShutdownOpenVRBackend();

  vr::EVRInitError init_error = vr::VRInitError_None;
  if (!InitializeVRSystem(&g_state.system, &init_error)) {
    SetError(error_message, "OpenVR initialization triggered a structured exception on Windows.");
    ResetState();
    return false;
  }

  if (init_error != vr::VRInitError_None || g_state.system == nullptr) {
    SetError(error_message, std::string("Failed to initialize OpenVR: ") + vr::VR_GetVRInitErrorAsEnglishDescription(init_error));
    ResetState();
    return false;
  }

  if (!AcquireOverlayInterface(&g_state.overlay)) {
    SetError(error_message, "OpenVR overlay interface acquisition triggered a structured exception on Windows.");
    ShutdownOpenVRBackend();
    return false;
  }

  if (g_state.overlay == nullptr) {
    SetError(error_message, "Failed to acquire OpenVR overlay interface.");
    ShutdownOpenVRBackend();
    return false;
  }

#if defined(__linux__)
  g_state.ipc = vr::VRIPCResourceManager();
#endif

  const std::string overlay_key = BuildOverlayKey(options.name);
  vr::EVROverlayError create_error = vr::VROverlayError_None;
  if (!CreateOverlayHandle(
        g_state.overlay,
        overlay_key.c_str(),
        options.name.c_str(),
        &g_state.overlay_handle,
        &create_error)) {
    SetError(error_message, "OpenVR overlay creation triggered a structured exception on Windows.");
    ShutdownOpenVRBackend();
    return false;
  }

  if (!CheckOverlayError(create_error, "Failed to create OpenVR overlay", error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  g_state.initialized = true;

  vr::EVROverlayError flag_error = vr::VROverlayError_None;
  if (!SetPremultipliedOverlayFlag(
        g_state.overlay,
        g_state.overlay_handle,
        true,
        &flag_error)) {
    SetError(error_message, "OpenVR overlay configuration triggered a structured exception on Windows.");
    ShutdownOpenVRBackend();
    return false;
  }

  if (!CheckOverlayError(flag_error, "Failed to configure OpenVR overlay alpha mode", error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  if (!ApplySizeMeters(options.size_meters, error_message) ||
      (options.curvature > 0.0f && !ApplyCurvature(options.curvature, error_message)) ||
      !ApplyPlacement(options.placement, error_message) ||
      !ApplyVisible(options.visible, error_message)) {
    ShutdownOpenVRBackend();
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SubmitOpenVRFrameWindows(uint64_t shared_handle, std::string* error_message) {
#if defined(_WIN32)
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  if (shared_handle == 0) {
    SetError(error_message, "OpenVR backend received an invalid Windows shared handle.");
    return false;
  }

  if (!OpenSharedTextureFromHandle(shared_handle, error_message)) {
    return false;
  }

  D3D11_TEXTURE2D_DESC shared_desc = {};
  g_state.shared_texture->GetDesc(&shared_desc);

  if (!EnsureSubmitTexture(shared_desc, error_message)) {
    return false;
  }

  g_state.d3d_context->CopyResource(g_state.submit_texture, g_state.shared_texture);
  g_state.d3d_context->Flush();

  if (!IsOpenVRFriendlyFormat(shared_desc.Format) && error_message != nullptr && error_message->empty()) {
    std::cout << "OpenVR warning: submitting non-BGRA DirectX texture format " << DxgiFormatToString(shared_desc.Format) << std::endl;
  }

  vr::Texture_t texture = {};
  if (!ConfigureOverlayTextureFromSubmitTexture(&texture, error_message)) {
    return false;
  }

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
        "Failed to submit Windows texture to OpenVR overlay",
        error_message)) {
    return false;
  }

  g_state.d3d_context->Flush();

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#else
  (void)shared_handle;
  SetError(error_message, "OpenVR Windows frame submission is only available on Windows builds.");
  return false;
#endif
}

bool SubmitOpenVRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

#if defined(__linux__)
  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    SetError(error_message, "OpenVR backend received an invalid DMA-BUF fd.");
    return false;
  }

  if (texture_info.width == 0 || texture_info.height == 0) {
    SetError(error_message, "Linux OpenVR texture submission requires non-zero width and height.");
    return false;
  }

  if (texture_info.planes.size() != 1) {
    SetError(
      error_message,
      "Linux OpenVR currently supports only single-plane DMA-BUF textures from Electron; received " + std::to_string(texture_info.planes.size()) + " planes.");
    return false;
  }

  const LinuxPlaneInfo& plane = texture_info.planes.front();
  if (plane.fd < 0) {
    SetError(error_message, "Linux OpenVR texture submission requires a non-negative DMA-BUF fd.");
    return false;
  }

  if (plane.stride == 0) {
    SetError(error_message, "Linux OpenVR texture submission requires a non-zero DMA-BUF plane stride.");
    return false;
  }

  uint32_t pixel_format = 0;
  if (!TryParsePixelFormat(texture_info.pixel_format, &pixel_format)) {
    const std::string format_name = texture_info.pixel_format.empty() ? std::string("<empty>") : texture_info.pixel_format;
    SetError(
      error_message,
      "Linux OpenVR received unsupported DMA-BUF pixel format '" + format_name + "'. Expected Electron's single-plane bgra/rgba or the equivalent DRM fourcc string.");
    return false;
  }

  uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  if (!TryParseModifier(texture_info.modifier, &modifier)) {
    SetError(
      error_message,
      "Linux OpenVR received an invalid DMA-BUF modifier '" + texture_info.modifier + "'. Expected an integer string or an empty modifier.");
    return false;
  }

  std::string vulkan_error;
  if (!IsTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN") && !g_state.vulkan_disabled) {
    if (SubmitLinuxOpenVRVulkanTexture(texture_info, pixel_format, modifier, &vulkan_error)) {
      return true;
    }
    g_state.vulkan_disabled = true;
    std::cerr << "Linux OpenVR Vulkan submission disabled after failure: " << vulkan_error << std::endl;
  }

  std::string direct_import_error;
  const bool direct_import_supported = g_state.ipc != nullptr &&
    SupportsDmabufFormatModifier(pixel_format, modifier, &direct_import_error);
  if (!direct_import_supported) {
    SetError(
      error_message,
      (vulkan_error.empty() ? std::string() : "Vulkan submission failed: " + vulkan_error + " ") +
        (direct_import_error.empty()
          ? "OpenVR IPC resource manager is unavailable on Linux."
          : direct_import_error));
    return false;
  }

  struct stat buffer_stat = {};
  if (fstat(plane.fd, &buffer_stat) != 0) {
    SetError(error_message, "Failed to identify Linux OpenVR DMA-BUF texture.");
    return false;
  }

  const auto imported = std::find_if(
    g_state.imported_textures.begin(),
    g_state.imported_textures.end(),
    [&](const OpenVRState::ImportedTexture& candidate) {
      return candidate.device == buffer_stat.st_dev &&
             candidate.inode == buffer_stat.st_ino &&
             candidate.width == texture_info.width &&
             candidate.height == texture_info.height &&
             candidate.format == pixel_format &&
             candidate.modifier == modifier &&
             candidate.offset == plane.offset &&
             candidate.stride == plane.stride &&
             candidate.size == plane.size;
    });

  vr::SharedTextureHandle_t imported_texture = static_cast<vr::SharedTextureHandle_t>(0);
  if (imported != g_state.imported_textures.end()) {
    imported_texture = imported->handle;
  } else {
    if (g_state.imported_textures.size() >= 16) {
      SetError(error_message, "Linux OpenVR DMA-BUF cache reached its 16-resource diagnostic limit.");
      return false;
    }

    vr::DmabufAttributes_t attributes = {};
    attributes.pNext = nullptr;
    attributes.unWidth = texture_info.width;
    attributes.unHeight = texture_info.height;
    attributes.unDepth = 1;
    attributes.unMipLevels = 1;
    attributes.unArrayLayers = 1;
    attributes.unSampleCount = 1;
    attributes.unFormat = pixel_format;
    attributes.ulModifier = modifier;
    attributes.unPlaneCount = 1;
    attributes.plane[0].nFd = plane.fd;
    attributes.plane[0].unOffset = plane.offset;
    attributes.plane[0].unStride = plane.stride;

    if (!g_state.ipc->ImportDmabuf(vr::VRApplication_Overlay, &attributes, &imported_texture) ||
        imported_texture == static_cast<vr::SharedTextureHandle_t>(0)) {
      SetError(error_message, "Failed to import DMA-BUF texture into OpenVR.");
      return false;
    }

    g_state.imported_textures.push_back({
      buffer_stat.st_dev,
      buffer_stat.st_ino,
      texture_info.width,
      texture_info.height,
      pixel_format,
      modifier,
      plane.offset,
      plane.stride,
      plane.size,
      imported_texture});

    std::cout << "Linux OpenVR imported DMA-BUF "
              << buffer_stat.st_dev << ":" << buffer_stat.st_ino
              << " as shared resource " << imported_texture
              << " (cache size=" << g_state.imported_textures.size() << ")." << std::endl;
  }

  vr::Texture_t texture = {};
  texture.handle = &imported_texture;
  texture.eType = vr::TextureType_SharedTextureHandle;
  texture.eColorSpace = vr::ColorSpace_Auto;

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
        "Failed to submit Linux texture to OpenVR overlay",
        error_message)) {
    return false;
  }

  const vr::EVROverlayError sync_error = g_state.overlay->WaitFrameSync(100);
  LogLinuxFrameSyncResult(sync_error, "direct texture submission");

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#else
  (void)texture_info;
  SetError(error_message, "OpenVR Linux frame submission is only available on Linux builds.");
  return false;
#endif
}

bool SubmitOpenVRSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  if (frame_info.width == 0 || frame_info.height == 0) {
    SetError(error_message, "OpenVR software frame submission requires non-zero width and height.");
    return false;
  }

  const size_t expected_size = static_cast<size_t>(frame_info.width) * static_cast<size_t>(frame_info.height) * 4;
  if (frame_info.rgba_pixels.size() != expected_size) {
    SetError(error_message, "OpenVR software frame submission received an unexpected RGBA buffer size.");
    return false;
  }

#if defined(__linux__)
  if (IsTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_GL_UPLOAD")) {
    return SubmitLinuxOpenGLSoftwareFrame(frame_info, error_message);
  }
  if (!IsTruthyEnvironmentVariable("ELECTRON_VR_DISABLE_OPENVR_VULKAN") ||
      IsTruthyEnvironmentVariable("ELECTRON_VR_OPENVR_VULKAN_SOFTWARE")) {
    return SubmitLinuxOpenVRVulkanSoftwareFrame(frame_info, error_message);
  }
  SetError(error_message, "Linux OpenVR Vulkan submission is disabled and no software transport is enabled.");
  return false;
#else
  if (!CheckOverlayError(
        g_state.overlay->SetOverlayRaw(
          g_state.overlay_handle,
          const_cast<uint8_t*>(frame_info.rgba_pixels.data()),
          frame_info.width,
          frame_info.height,
          4),
        "Failed to submit software frame to OpenVR overlay",
        error_message)) {
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#endif
}

bool SetOpenVRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  return ApplyPlacement(placement, error_message);
}

bool SetOpenVRVisible(bool visible, std::string* error_message) {
  return ApplyVisible(visible, error_message);
}

bool SetOpenVRSizeMeters(float size_meters, std::string* error_message) {
  if (size_meters <= 0.0f || !std::isfinite(size_meters)) {
    SetError(error_message, "OpenVR overlay size must be greater than zero.");
    return false;
  }

  return ApplySizeMeters(size_meters, error_message);
}

bool SetOpenVRCurvature(float curvature, std::string* error_message) {
  return ApplyCurvature(curvature, error_message);
}

void PopulateOpenVRRuntimeInfo(RuntimeInfo* runtime_info) {
  PopulateOpenVRSceneApplication(runtime_info);
}

void ShutdownOpenVRBackend() {
  if (g_state.overlay != nullptr && g_state.overlay_handle != vr::k_ulOverlayHandleInvalid) {
    g_state.overlay->DestroyOverlay(g_state.overlay_handle);
  }

#if defined(__linux__)
  ReleaseLinuxImportedTextures();
#endif

  if (g_state.system != nullptr || g_state.overlay != nullptr) {
    vr::VR_Shutdown();
  }

  ResetState();
}

}  // namespace vrbridge
