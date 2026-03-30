#include "openvr_backend.h"

#include "curved_geometry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <chrono>
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
#include <drm/drm_fourcc.h>
#endif

namespace vrbridge {

namespace {

struct OpenVRState {
  vr::IVRSystem* system = nullptr;
  vr::IVROverlay* overlay = nullptr;
#if defined(__linux__)
  vr::IVRIPCResourceManagerClient* ipc = nullptr;
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
  bool visible = true;
  float size_meters = 1.0f;
  OverlayPlacement placement;
  OverlayCurvature curvature;
  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  std::string overlay_name;
  std::vector<vr::VROverlayHandle_t> segment_handles;
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

vr::HmdMatrix34_t MultiplyMatrices(const vr::HmdMatrix34_t& left, const vr::HmdMatrix34_t& right) {
  vr::HmdMatrix34_t result = {};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      result.m[row][column] = left.m[row][0] * right.m[0][column] +
                              left.m[row][1] * right.m[1][column] +
                              left.m[row][2] * right.m[2][column];
    }
    result.m[row][3] = left.m[row][0] * right.m[0][3] +
                       left.m[row][1] * right.m[1][3] +
                       left.m[row][2] * right.m[2][3] +
                       left.m[row][3];
  }
  return result;
}

vr::HmdMatrix34_t BuildSegmentLocalMatrix(const CurvedQuadSegment& segment) {
  vr::HmdMatrix34_t matrix = {};
  matrix.m[0][0] = segment.right_axis.x;
  matrix.m[0][1] = segment.up_axis.x;
  matrix.m[0][2] = segment.forward_axis.x;
  matrix.m[0][3] = segment.position.x;

  matrix.m[1][0] = segment.right_axis.y;
  matrix.m[1][1] = segment.up_axis.y;
  matrix.m[1][2] = segment.forward_axis.y;
  matrix.m[1][3] = segment.position.y;

  matrix.m[2][0] = segment.right_axis.z;
  matrix.m[2][1] = segment.up_axis.z;
  matrix.m[2][2] = segment.forward_axis.z;
  matrix.m[2][3] = segment.position.z;
  return matrix;
}

bool ApplySegmentedOverlayConfig(
  vr::VROverlayHandle_t overlay_handle,
  const CurvedQuadSegment& segment,
  uint32_t sort_order,
  std::string* error_message) {
  const vr::HmdMatrix34_t base_transform = ToHmdMatrix34(g_state.placement);
  const vr::HmdMatrix34_t local_transform = BuildSegmentLocalMatrix(segment);
  const vr::HmdMatrix34_t final_transform = MultiplyMatrices(base_transform, local_transform);

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayWidthInMeters(overlay_handle, segment.width_meters),
        "Failed to set OpenVR segment width",
        error_message)) {
    return false;
  }

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTexelAspect(overlay_handle, 1.0f),
        "Failed to set OpenVR segment texel aspect",
        error_message)) {
    return false;
  }

  vr::VRTextureBounds_t bounds = {};
  bounds.uMin = segment.u_min;
  bounds.uMax = segment.u_max;
  bounds.vMin = 1.0f - segment.v_max;
  bounds.vMax = 1.0f - segment.v_min;
  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTextureBounds(overlay_handle, &bounds),
        "Failed to set OpenVR segment texture bounds",
        error_message)) {
    return false;
  }

  if (!CheckOverlayError(
        g_state.overlay->SetOverlaySortOrder(overlay_handle, sort_order),
        "Failed to set OpenVR segment sort order",
        error_message)) {
    return false;
  }

  if (g_state.placement.mode == OverlayPlacementMode::kHead) {
    return CheckOverlayError(
      g_state.overlay->SetOverlayTransformTrackedDeviceRelative(overlay_handle, vr::k_unTrackedDeviceIndex_Hmd, &final_transform),
      "Failed to set head-locked OpenVR segment transform",
      error_message);
  }

  return CheckOverlayError(
    g_state.overlay->SetOverlayTransformAbsolute(overlay_handle, vr::TrackingUniverseStanding, &final_transform),
    "Failed to set world-locked OpenVR segment transform",
    error_message);
}

bool ApplyVisible(bool visible, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  auto apply_to_handle = [&](vr::VROverlayHandle_t handle) {
    return CheckOverlayError(
      visible ? g_state.overlay->ShowOverlay(handle)
              : g_state.overlay->HideOverlay(handle),
      visible ? "Failed to show OpenVR overlay" : "Failed to hide OpenVR overlay",
      error_message);
  };

  if (!apply_to_handle(g_state.overlay_handle)) {
    return false;
  }

  for (vr::VROverlayHandle_t handle : g_state.segment_handles) {
    if (!apply_to_handle(handle)) {
      return false;
    }
  }

  return true;
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

bool EnsureOverlaySegmentCount(size_t desired_count, std::string* error_message) {
  if (!g_state.initialized || g_state.overlay == nullptr || g_state.overlay_handle == vr::k_ulOverlayHandleInvalid) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  while (g_state.segment_handles.size() < desired_count) {
    const size_t segment_index = g_state.segment_handles.size();
    const std::string segment_key = BuildOverlayKey(g_state.overlay_name + ".segment." + std::to_string(segment_index));
    const std::string segment_name = g_state.overlay_name + " Segment " + std::to_string(segment_index + 1);
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError create_error = vr::VROverlayError_None;
    if (!CreateOverlayHandle(g_state.overlay, segment_key.c_str(), segment_name.c_str(), &handle, &create_error)) {
      SetError(error_message, "OpenVR segment overlay creation triggered a structured exception on Windows.");
      return false;
    }
    if (!CheckOverlayError(create_error, "Failed to create OpenVR segment overlay", error_message)) {
      return false;
    }

    vr::EVROverlayError flag_error = vr::VROverlayError_None;
    if (!SetPremultipliedOverlayFlag(g_state.overlay, handle, true, &flag_error)) {
      SetError(error_message, "OpenVR segment overlay configuration triggered a structured exception on Windows.");
      return false;
    }
    if (!CheckOverlayError(flag_error, "Failed to configure OpenVR segment alpha mode", error_message)) {
      return false;
    }

    if (!CheckOverlayError(
          g_state.visible ? g_state.overlay->ShowOverlay(handle)
                          : g_state.overlay->HideOverlay(handle),
          g_state.visible ? "Failed to show OpenVR segment overlay"
                          : "Failed to hide OpenVR segment overlay",
          error_message)) {
      return false;
    }

    g_state.segment_handles.push_back(handle);
  }

  while (g_state.segment_handles.size() > desired_count) {
    const vr::VROverlayHandle_t handle = g_state.segment_handles.back();
    g_state.segment_handles.pop_back();
    if (!CheckOverlayError(g_state.overlay->DestroyOverlay(handle), "Failed to destroy OpenVR segment overlay", error_message)) {
      return false;
    }
  }

  return true;
}

#if defined(__linux__)
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
#endif

void ResetState() {
#if defined(_WIN32)
  ReleaseD3DResources();
  g_state.logged_shared_texture_desc = false;
#endif
#if defined(__linux__)
  g_state.ipc = nullptr;
#endif
  g_state.system = nullptr;
  g_state.overlay = nullptr;
  g_state.overlay_handle = vr::k_ulOverlayHandleInvalid;
  g_state.segment_handles.clear();
  g_state.overlay_name.clear();
  g_state.size_meters = 1.0f;
  g_state.placement = OverlayPlacement{};
  g_state.curvature = OverlayCurvature{};
  g_state.frame_width = 0;
  g_state.frame_height = 0;
  g_state.visible = true;
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
  g_state.overlay_name = options.name;
  g_state.visible = options.visible;
  g_state.size_meters = options.size_meters;
  g_state.placement = options.placement;
  g_state.curvature = options.curvature;

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
  g_state.frame_width = shared_desc.Width;
  g_state.frame_height = shared_desc.Height;

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

  const CurvedQuadLayout layout = BuildCurvedQuadLayout(g_state.frame_width, g_state.frame_height, g_state.size_meters, g_state.curvature);
  if (!EnsureOverlaySegmentCount(layout.segments.size() > 0 ? layout.segments.size() - 1U : 0U, error_message)) {
    return false;
  }

  if (!layout.segments.empty()) {
    const CurvedQuadSegment& first_segment = layout.segments.front();
    const CurvedQuadSegment& last_segment = layout.segments.back();
    std::cout << "OpenVR curved layout: segments=" << layout.horizontal_segments << "x" << layout.vertical_segments
              << ", curvature=(h=" << (g_state.curvature.has_horizontal ? std::to_string(g_state.curvature.horizontal) : std::string("flat"))
              << ", v=" << (g_state.curvature.has_vertical ? std::to_string(g_state.curvature.vertical) : std::string("flat"))
              << "), first.z=" << first_segment.position.z
              << ", last.z=" << last_segment.position.z
              << std::endl;
  }

  if (!CheckOverlayError(
        g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
        "Failed to submit Windows texture to OpenVR overlay",
        error_message)) {
    return false;
  }

  if (!ApplySegmentedOverlayConfig(g_state.overlay_handle, layout.segments.front(), 0, error_message)) {
    return false;
  }

  for (size_t index = 1; index < layout.segments.size(); ++index) {
    const vr::VROverlayHandle_t handle = g_state.segment_handles[index - 1U];
    if (!CheckOverlayError(
          g_state.overlay->SetOverlayTexture(handle, &texture),
          "Failed to submit Windows texture to OpenVR segment overlay",
          error_message)) {
      return false;
    }
    if (!ApplySegmentedOverlayConfig(handle, layout.segments[index], static_cast<uint32_t>(index), error_message)) {
      return false;
    }
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
  if (g_state.ipc == nullptr) {
    SetError(error_message, "OpenVR IPC resource manager is unavailable on Linux.");
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    SetError(error_message, "OpenVR backend received an invalid DMA-BUF fd.");
    return false;
  }

  if (texture_info.width == 0 || texture_info.height == 0) {
    SetError(error_message, "Linux OpenVR texture submission requires non-zero width and height.");
    return false;
  }
  g_state.frame_width = texture_info.width;
  g_state.frame_height = texture_info.height;

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

  vr::SharedTextureHandle_t imported_texture = static_cast<vr::SharedTextureHandle_t>(0);
  if (!g_state.ipc->ImportDmabuf(vr::VRApplication_Overlay, &attributes, &imported_texture) ||
      imported_texture == static_cast<vr::SharedTextureHandle_t>(0)) {
    SetError(error_message, "Failed to import DMA-BUF texture into OpenVR.");
    return false;
  }

  vr::Texture_t texture = {};
  texture.handle = &imported_texture;
  texture.eType = vr::TextureType_SharedTextureHandle;
  texture.eColorSpace = vr::ColorSpace_Auto;

  const CurvedQuadLayout layout = BuildCurvedQuadLayout(g_state.frame_width, g_state.frame_height, g_state.size_meters, g_state.curvature);
  if (!EnsureOverlaySegmentCount(layout.segments.size() > 0 ? layout.segments.size() - 1U : 0U, error_message)) {
    g_state.ipc->UnrefResource(imported_texture);
    return false;
  }

  if (!layout.segments.empty()) {
    const CurvedQuadSegment& first_segment = layout.segments.front();
    const CurvedQuadSegment& last_segment = layout.segments.back();
    std::cout << "OpenVR curved layout: segments=" << layout.horizontal_segments << "x" << layout.vertical_segments
              << ", curvature=(h=" << (g_state.curvature.has_horizontal ? std::to_string(g_state.curvature.horizontal) : std::string("flat"))
              << ", v=" << (g_state.curvature.has_vertical ? std::to_string(g_state.curvature.vertical) : std::string("flat"))
              << "), first.z=" << first_segment.position.z
              << ", last.z=" << last_segment.position.z
              << std::endl;
  }

  const bool submitted = CheckOverlayError(
    g_state.overlay->SetOverlayTexture(g_state.overlay_handle, &texture),
    "Failed to submit Linux texture to OpenVR overlay",
    error_message);

  bool configured = submitted;
  if (configured) {
    configured = ApplySegmentedOverlayConfig(g_state.overlay_handle, layout.segments.front(), 0, error_message);
  }
  for (size_t index = 1; configured && index < layout.segments.size(); ++index) {
    const vr::VROverlayHandle_t handle = g_state.segment_handles[index - 1U];
    configured = CheckOverlayError(
      g_state.overlay->SetOverlayTexture(handle, &texture),
      "Failed to submit Linux texture to OpenVR segment overlay",
      error_message);
    if (configured) {
      configured = ApplySegmentedOverlayConfig(handle, layout.segments[index], static_cast<uint32_t>(index), error_message);
    }
  }

  g_state.ipc->UnrefResource(imported_texture);
  return submitted && configured;
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

  g_state.frame_width = frame_info.width;
  g_state.frame_height = frame_info.height;

  const CurvedQuadLayout layout = BuildCurvedQuadLayout(g_state.frame_width, g_state.frame_height, g_state.size_meters, g_state.curvature);
  if (!EnsureOverlaySegmentCount(layout.segments.size() > 0 ? layout.segments.size() - 1U : 0U, error_message)) {
    return false;
  }

  if (!layout.segments.empty()) {
    const CurvedQuadSegment& first_segment = layout.segments.front();
    const CurvedQuadSegment& last_segment = layout.segments.back();
    std::cout << "OpenVR curved layout: segments=" << layout.horizontal_segments << "x" << layout.vertical_segments
              << ", curvature=(h=" << (g_state.curvature.has_horizontal ? std::to_string(g_state.curvature.horizontal) : std::string("flat"))
              << ", v=" << (g_state.curvature.has_vertical ? std::to_string(g_state.curvature.vertical) : std::string("flat"))
              << "), first.z=" << first_segment.position.z
              << ", last.z=" << last_segment.position.z
              << std::endl;
  }

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

  if (!ApplySegmentedOverlayConfig(g_state.overlay_handle, layout.segments.front(), 0, error_message)) {
    return false;
  }

  for (size_t index = 1; index < layout.segments.size(); ++index) {
    const vr::VROverlayHandle_t handle = g_state.segment_handles[index - 1U];
    if (!CheckOverlayError(
          g_state.overlay->SetOverlayRaw(
            handle,
            const_cast<uint8_t*>(frame_info.rgba_pixels.data()),
            frame_info.width,
            frame_info.height,
            4),
          "Failed to submit software frame to OpenVR segment overlay",
          error_message)) {
      return false;
    }
    if (!ApplySegmentedOverlayConfig(handle, layout.segments[index], static_cast<uint32_t>(index), error_message)) {
      return false;
    }
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetOpenVRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  g_state.placement = placement;
  return ApplyPlacement(placement, error_message);
}

bool SetOpenVRCurvature(const OverlayCurvature& curvature, std::string* error_message) {
  if (!g_state.initialized) {
    SetError(error_message, "OpenVR backend is not initialized.");
    return false;
  }

  g_state.curvature = curvature;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetOpenVRVisible(bool visible, std::string* error_message) {
  g_state.visible = visible;
  return ApplyVisible(visible, error_message);
}

bool SetOpenVRSizeMeters(float size_meters, std::string* error_message) {
  if (size_meters <= 0.0f || !std::isfinite(size_meters)) {
    SetError(error_message, "OpenVR overlay size must be greater than zero.");
    return false;
  }

  g_state.size_meters = size_meters;
  return ApplySizeMeters(size_meters, error_message);
}

void ShutdownOpenVRBackend() {
  for (vr::VROverlayHandle_t handle : g_state.segment_handles) {
    if (g_state.overlay != nullptr && handle != vr::k_ulOverlayHandleInvalid) {
      g_state.overlay->DestroyOverlay(handle);
    }
  }

  if (g_state.overlay != nullptr && g_state.overlay_handle != vr::k_ulOverlayHandleInvalid) {
    g_state.overlay->DestroyOverlay(g_state.overlay_handle);
  }

  if (g_state.system != nullptr || g_state.overlay != nullptr) {
    vr::VR_Shutdown();
  }

  ResetState();
}

}  // namespace vrbridge
