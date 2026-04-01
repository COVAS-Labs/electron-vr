#include "openxr_backend.h"

#include "openxr_loader_win.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

#if defined(__linux__)
#ifndef XR_USE_PLATFORM_EGL
#define XR_USE_PLATFORM_EGL
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif

#include <drm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <unistd.h>
#endif

namespace vrbridge {

namespace {

void SetError(std::string* error_message, const std::string& message) {
  if (error_message != nullptr) {
    *error_message = message;
  }
}

std::string ToString(XrResult result) {
  std::ostringstream stream;
  stream << static_cast<int32_t>(result);
  return stream.str();
}

const char* SessionStateToString(XrSessionState state) {
  switch (state) {
    case XR_SESSION_STATE_IDLE:
      return "idle";
    case XR_SESSION_STATE_READY:
      return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
      return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
      return "visible";
    case XR_SESSION_STATE_FOCUSED:
      return "focused";
    case XR_SESSION_STATE_STOPPING:
      return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
      return "loss-pending";
    case XR_SESSION_STATE_EXITING:
      return "exiting";
    case XR_SESSION_STATE_UNKNOWN:
    default:
      return "unknown";
  }
}

std::string XrResultToString(XrInstance instance, XrResult result) {
#if !defined(_WIN32)
  if (instance != XR_NULL_HANDLE) {
    char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
    if (XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
      return std::string(buffer);
    }
  }
#endif

  return ToString(result);
}

#if defined(__linux__)
std::string EglErrorString(EGLint error) {
  std::ostringstream stream;
  stream << "0x" << std::hex << static_cast<unsigned long>(error);
  return stream.str();
}

uint32_t ParseDrmFormat(const std::string& pixel_format) {
  if (pixel_format == "rgba") {
    return DRM_FORMAT_ABGR8888;
  }
  if (pixel_format == "bgra") {
    return DRM_FORMAT_ARGB8888;
  }
  if (pixel_format == "ARGB8888") {
    return DRM_FORMAT_ARGB8888;
  }
  if (pixel_format == "ABGR8888") {
    return DRM_FORMAT_ABGR8888;
  }
  if (pixel_format == "XRGB8888") {
    return DRM_FORMAT_XRGB8888;
  }
  if (pixel_format == "XBGR8888") {
    return DRM_FORMAT_XBGR8888;
  }

  return DRM_FORMAT_ARGB8888;
}

bool ParseModifier(const std::string& modifier_string, uint64_t* modifier_out) {
  if (modifier_out == nullptr || modifier_string.empty()) {
    return false;
  }

  try {
    *modifier_out = std::stoull(modifier_string, nullptr, 0);
    return true;
  } catch (...) {
    return false;
  }
}
#endif

XrPosef ToXrPose(const OverlayPlacement& placement) {
  XrPosef pose{};
  pose.orientation.x = placement.rotation.x;
  pose.orientation.y = placement.rotation.y;
  pose.orientation.z = placement.rotation.z;
  pose.orientation.w = placement.rotation.w;
  pose.position.x = placement.position.x;
  pose.position.y = placement.position.y;
  pose.position.z = placement.position.z;
  return pose;
}

float ComputeHeightMeters(uint32_t width, uint32_t height, float width_meters) {
  if (width == 0 || height == 0 || !std::isfinite(width_meters) || width_meters <= 0.0f) {
    return width_meters;
  }

  return width_meters * (static_cast<float>(height) / static_cast<float>(width));
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
  for (const XrExtensionProperties& extension : extensions) {
    if (std::string(extension.extensionName) == name) {
      return true;
    }
  }

  return false;
}

template <typename T, XrStructureType Type>
T MakeXrStruct() {
  T value{};
  value.type = Type;
  value.next = nullptr;
  return value;
}

struct OpenXRBackendState {
  bool initialized = false;
  bool visible = true;
  float size_meters = 1.0f;
  OverlayPlacement placement;
  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system_id = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace stage_space = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  int64_t swapchain_format = 0;
  XrEnvironmentBlendMode environment_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  bool session_running = false;
  bool logged_first_frame_submission = false;
#if defined(__linux__)
  std::vector<XrSwapchainImageOpenGLESKHR> swapchain_images;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext egl_context = EGL_NO_CONTEXT;
  EGLConfig egl_config = nullptr;
  PFNEGLCREATEIMAGEKHRPROC egl_create_image_khr = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_khr = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture_2d_oes = nullptr;
  PFN_xrGetOpenGLESGraphicsRequirementsKHR xr_get_opengl_graphics_requirements_khr = nullptr;
  GLuint imported_texture = 0;
  GLuint framebuffer = 0;
  GLuint program = 0;
  GLint sampler_uniform = -1;
#elif defined(_WIN32)
  std::vector<XrSwapchainImageD3D11KHR> swapchain_images;
  HMODULE openxr_loader = nullptr;
  PFN_xrGetInstanceProcAddr xr_get_instance_proc_addr = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties xr_enumerate_instance_extension_properties = nullptr;
  PFN_xrCreateInstance xr_create_instance = nullptr;
  PFN_xrDestroyInstance xr_destroy_instance = nullptr;
  PFN_xrGetSystem xr_get_system = nullptr;
  PFN_xrCreateSession xr_create_session = nullptr;
  PFN_xrDestroySession xr_destroy_session = nullptr;
  PFN_xrCreateReferenceSpace xr_create_reference_space = nullptr;
  PFN_xrDestroySpace xr_destroy_space = nullptr;
  PFN_xrEnumerateSwapchainFormats xr_enumerate_swapchain_formats = nullptr;
  PFN_xrCreateSwapchain xr_create_swapchain = nullptr;
  PFN_xrDestroySwapchain xr_destroy_swapchain = nullptr;
  PFN_xrEnumerateSwapchainImages xr_enumerate_swapchain_images = nullptr;
  PFN_xrEnumerateEnvironmentBlendModes xr_enumerate_environment_blend_modes = nullptr;
  PFN_xrPollEvent xr_poll_event = nullptr;
  PFN_xrBeginSession xr_begin_session = nullptr;
  PFN_xrEndSession xr_end_session = nullptr;
  PFN_xrWaitFrame xr_wait_frame = nullptr;
  PFN_xrBeginFrame xr_begin_frame = nullptr;
  PFN_xrEndFrame xr_end_frame = nullptr;
  PFN_xrAcquireSwapchainImage xr_acquire_swapchain_image = nullptr;
  PFN_xrWaitSwapchainImage xr_wait_swapchain_image = nullptr;
  PFN_xrReleaseSwapchainImage xr_release_swapchain_image = nullptr;
  PFN_xrResultToString xr_result_to_string = nullptr;
  PFN_xrGetD3D11GraphicsRequirementsKHR xr_get_d3d11_graphics_requirements_khr = nullptr;
  ID3D11Device* d3d_device = nullptr;
  ID3D11DeviceContext* d3d_context = nullptr;
  ID3D11Device1* d3d_device1 = nullptr;
  ID3D11Texture2D* shared_texture = nullptr;
  uint64_t shared_texture_handle_value = 0;
  bool logged_shared_texture_desc = false;
#endif
};

OpenXRBackendState g_state;

bool CheckXr(XrResult result, const char* message, std::string* error_message) {
  if (XR_SUCCEEDED(result)) {
    return true;
  }

  SetError(error_message, std::string(message) + ": " + XrResultToString(g_state.instance, result));
  return false;
}

XrResult DestroySwapchainHandle(XrSwapchain swapchain) {
#if defined(_WIN32)
  return g_state.xr_destroy_swapchain(swapchain);
#else
  return xrDestroySwapchain(swapchain);
#endif
}

XrResult DestroySpaceHandle(XrSpace space) {
#if defined(_WIN32)
  return g_state.xr_destroy_space(space);
#else
  return xrDestroySpace(space);
#endif
}

XrResult DestroySessionHandle(XrSession session) {
#if defined(_WIN32)
  return g_state.xr_destroy_session(session);
#else
  return xrDestroySession(session);
#endif
}

XrResult DestroyInstanceHandle(XrInstance instance) {
#if defined(_WIN32)
  return g_state.xr_destroy_instance(instance);
#else
  return xrDestroyInstance(instance);
#endif
}

XrResult EnumerateSwapchainFormatsHandle(XrSession session, uint32_t capacity_input, uint32_t* count_output, int64_t* formats) {
#if defined(_WIN32)
  return g_state.xr_enumerate_swapchain_formats(session, capacity_input, count_output, formats);
#else
  return xrEnumerateSwapchainFormats(session, capacity_input, count_output, formats);
#endif
}

XrResult EnumerateEnvironmentBlendModesHandle(
  XrInstance instance,
  XrSystemId system_id,
  XrViewConfigurationType view_type,
  uint32_t capacity_input,
  uint32_t* count_output,
  XrEnvironmentBlendMode* blend_modes) {
#if defined(_WIN32)
  return g_state.xr_enumerate_environment_blend_modes(instance, system_id, view_type, capacity_input, count_output, blend_modes);
#else
  return xrEnumerateEnvironmentBlendModes(instance, system_id, view_type, capacity_input, count_output, blend_modes);
#endif
}

XrResult BeginSessionHandle(XrSession session, const XrSessionBeginInfo* begin_info) {
#if defined(_WIN32)
  return g_state.xr_begin_session(session, begin_info);
#else
  return xrBeginSession(session, begin_info);
#endif
}

XrResult CreateInstanceHandle(const XrInstanceCreateInfo* create_info, XrInstance* instance) {
#if defined(_WIN32)
  return g_state.xr_create_instance(create_info, instance);
#else
  return xrCreateInstance(create_info, instance);
#endif
}

XrResult GetSystemHandle(XrInstance instance, const XrSystemGetInfo* get_info, XrSystemId* system_id) {
#if defined(_WIN32)
  return g_state.xr_get_system(instance, get_info, system_id);
#else
  return xrGetSystem(instance, get_info, system_id);
#endif
}

XrResult CreateSessionHandle(XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session) {
#if defined(_WIN32)
  return g_state.xr_create_session(instance, create_info, session);
#else
  return xrCreateSession(instance, create_info, session);
#endif
}

XrResult CreateReferenceSpaceHandle(XrSession session, const XrReferenceSpaceCreateInfo* create_info, XrSpace* space) {
#if defined(_WIN32)
  return g_state.xr_create_reference_space(session, create_info, space);
#else
  return xrCreateReferenceSpace(session, create_info, space);
#endif
}

XrResult CreateSwapchainHandle(XrSession session, const XrSwapchainCreateInfo* create_info, XrSwapchain* swapchain) {
#if defined(_WIN32)
  return g_state.xr_create_swapchain(session, create_info, swapchain);
#else
  return xrCreateSwapchain(session, create_info, swapchain);
#endif
}

XrResult EnumerateSwapchainImagesHandle(XrSwapchain swapchain, uint32_t capacity_input, uint32_t* count_output, XrSwapchainImageBaseHeader* images) {
#if defined(_WIN32)
  return g_state.xr_enumerate_swapchain_images(swapchain, capacity_input, count_output, images);
#else
  return xrEnumerateSwapchainImages(swapchain, capacity_input, count_output, images);
#endif
}

XrResult PollEventHandle(XrInstance instance, XrEventDataBuffer* event_buffer) {
#if defined(_WIN32)
  return g_state.xr_poll_event(instance, event_buffer);
#else
  return xrPollEvent(instance, event_buffer);
#endif
}

XrResult EndSessionHandle(XrSession session) {
#if defined(_WIN32)
  return g_state.xr_end_session(session);
#else
  return xrEndSession(session);
#endif
}

XrResult WaitFrameHandle(XrSession session, const XrFrameWaitInfo* wait_info, XrFrameState* frame_state) {
#if defined(_WIN32)
  return g_state.xr_wait_frame(session, wait_info, frame_state);
#else
  return xrWaitFrame(session, wait_info, frame_state);
#endif
}

XrResult BeginFrameHandle(XrSession session, const XrFrameBeginInfo* begin_info) {
#if defined(_WIN32)
  return g_state.xr_begin_frame(session, begin_info);
#else
  return xrBeginFrame(session, begin_info);
#endif
}

XrResult EndFrameHandle(XrSession session, const XrFrameEndInfo* end_info) {
#if defined(_WIN32)
  return g_state.xr_end_frame(session, end_info);
#else
  return xrEndFrame(session, end_info);
#endif
}

XrResult AcquireSwapchainImageHandle(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquire_info, uint32_t* image_index) {
#if defined(_WIN32)
  return g_state.xr_acquire_swapchain_image(swapchain, acquire_info, image_index);
#else
  return xrAcquireSwapchainImage(swapchain, acquire_info, image_index);
#endif
}

XrResult WaitSwapchainImageHandle(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* wait_info) {
#if defined(_WIN32)
  return g_state.xr_wait_swapchain_image(swapchain, wait_info);
#else
  return xrWaitSwapchainImage(swapchain, wait_info);
#endif
}

XrResult ReleaseSwapchainImageHandle(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* release_info) {
#if defined(_WIN32)
  return g_state.xr_release_swapchain_image(swapchain, release_info);
#else
  return xrReleaseSwapchainImage(swapchain, release_info);
#endif
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
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
    default:
      return "DXGI_FORMAT_OTHER";
  }
}

template <typename FunctionType>
bool LoadGlobalOpenXRFunction(const char* name, FunctionType* target, std::string* error_message) {
  if (target == nullptr || g_state.xr_get_instance_proc_addr == nullptr) {
    SetError(error_message, std::string("OpenXR global function loader is unavailable for ") + name + ".");
    return false;
  }

  PFN_xrVoidFunction function = nullptr;
  const XrResult result = g_state.xr_get_instance_proc_addr(XR_NULL_HANDLE, name, &function);
  if (XR_FAILED(result) || function == nullptr) {
    SetError(error_message, std::string("Failed to load global OpenXR function ") + name + ": " + ToString(result));
    return false;
  }

  *target = reinterpret_cast<FunctionType>(function);
  return true;
}

template <typename FunctionType>
bool LoadInstanceOpenXRFunction(const char* name, FunctionType* target, std::string* error_message) {
  if (target == nullptr || g_state.xr_get_instance_proc_addr == nullptr || g_state.instance == XR_NULL_HANDLE) {
    SetError(error_message, std::string("OpenXR instance function loader is unavailable for ") + name + ".");
    return false;
  }

  PFN_xrVoidFunction function = nullptr;
  const XrResult result = g_state.xr_get_instance_proc_addr(g_state.instance, name, &function);
  if (XR_FAILED(result) || function == nullptr) {
    SetError(error_message, std::string("Failed to load instance OpenXR function ") + name + ": " + ToString(result));
    return false;
  }

  *target = reinterpret_cast<FunctionType>(function);
  return true;
}

void ReleaseSharedTexture() {
  if (g_state.shared_texture != nullptr) {
    g_state.shared_texture->Release();
    g_state.shared_texture = nullptr;
  }
  g_state.shared_texture_handle_value = 0;
}

void ReleaseD3DResources() {
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

void LogSharedTextureDescOnce(const D3D11_TEXTURE2D_DESC& desc) {
  if (g_state.logged_shared_texture_desc) {
    return;
  }

  g_state.logged_shared_texture_desc = true;
  std::cout
    << "OpenXR shared texture desc: "
    << "width=" << desc.Width
    << ", height=" << desc.Height
    << ", format=" << DxgiFormatToString(desc.Format)
    << ", bindFlags=0x" << std::hex << desc.BindFlags
    << ", miscFlags=0x" << desc.MiscFlags
    << std::dec
    << ", sampleCount=" << desc.SampleDesc.Count
    << std::endl;
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

  SetError(error_message, "Failed to create a D3D11 device for OpenXR texture import (" + HResultToString(last_error) + ").");
  ReleaseD3DResources();
  return false;
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
      D3D11_TEXTURE2D_DESC shared_desc = {};
      g_state.shared_texture->GetDesc(&shared_desc);
      LogSharedTextureDescOnce(shared_desc);
      return true;
    }
  }

  open_result = g_state.d3d_device->OpenSharedResource(
    handle,
    __uuidof(ID3D11Texture2D),
    reinterpret_cast<void**>(&g_state.shared_texture)
  );

  if (FAILED(open_result) || g_state.shared_texture == nullptr) {
    SetError(error_message, "Failed to open Windows shared texture handle for OpenXR (" + HResultToString(open_result) + ").");
    ReleaseSharedTexture();
    return false;
  }

  g_state.shared_texture_handle_value = shared_handle;
  D3D11_TEXTURE2D_DESC shared_desc = {};
  g_state.shared_texture->GetDesc(&shared_desc);
  LogSharedTextureDescOnce(shared_desc);
  return true;
}

bool IsCompatibleOpenXRSwapchainFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool LoadOpenXRLoader(std::string* error_message) {
  if (g_state.openxr_loader != nullptr) {
    return true;
  }

  g_state.openxr_loader = openxrwin::LoadOpenXRLoaderModule();
  if (g_state.openxr_loader == nullptr) {
    SetError(
      error_message,
      "Failed to load openxr_loader.dll for the Windows OpenXR backend. Set ELECTRON_VR_OPENXR_LOADER_PATH if the loader is installed outside PATH."
    );
    return false;
  }

  g_state.xr_get_instance_proc_addr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_state.openxr_loader, "xrGetInstanceProcAddr"));
  if (g_state.xr_get_instance_proc_addr == nullptr) {
    SetError(error_message, "Failed to resolve xrGetInstanceProcAddr from openxr_loader.dll.");
    return false;
  }

  return LoadGlobalOpenXRFunction("xrEnumerateInstanceExtensionProperties", &g_state.xr_enumerate_instance_extension_properties, error_message) &&
         LoadGlobalOpenXRFunction("xrCreateInstance", &g_state.xr_create_instance, error_message);
}

bool LoadInstanceOpenXRFunctions(std::string* error_message) {
  return LoadInstanceOpenXRFunction("xrDestroyInstance", &g_state.xr_destroy_instance, error_message) &&
         LoadInstanceOpenXRFunction("xrGetSystem", &g_state.xr_get_system, error_message) &&
         LoadInstanceOpenXRFunction("xrCreateSession", &g_state.xr_create_session, error_message) &&
         LoadInstanceOpenXRFunction("xrDestroySession", &g_state.xr_destroy_session, error_message) &&
         LoadInstanceOpenXRFunction("xrCreateReferenceSpace", &g_state.xr_create_reference_space, error_message) &&
         LoadInstanceOpenXRFunction("xrDestroySpace", &g_state.xr_destroy_space, error_message) &&
         LoadInstanceOpenXRFunction("xrEnumerateSwapchainFormats", &g_state.xr_enumerate_swapchain_formats, error_message) &&
         LoadInstanceOpenXRFunction("xrCreateSwapchain", &g_state.xr_create_swapchain, error_message) &&
         LoadInstanceOpenXRFunction("xrDestroySwapchain", &g_state.xr_destroy_swapchain, error_message) &&
         LoadInstanceOpenXRFunction("xrEnumerateSwapchainImages", &g_state.xr_enumerate_swapchain_images, error_message) &&
         LoadInstanceOpenXRFunction("xrEnumerateEnvironmentBlendModes", &g_state.xr_enumerate_environment_blend_modes, error_message) &&
         LoadInstanceOpenXRFunction("xrPollEvent", &g_state.xr_poll_event, error_message) &&
         LoadInstanceOpenXRFunction("xrBeginSession", &g_state.xr_begin_session, error_message) &&
         LoadInstanceOpenXRFunction("xrEndSession", &g_state.xr_end_session, error_message) &&
         LoadInstanceOpenXRFunction("xrWaitFrame", &g_state.xr_wait_frame, error_message) &&
         LoadInstanceOpenXRFunction("xrBeginFrame", &g_state.xr_begin_frame, error_message) &&
         LoadInstanceOpenXRFunction("xrEndFrame", &g_state.xr_end_frame, error_message) &&
         LoadInstanceOpenXRFunction("xrAcquireSwapchainImage", &g_state.xr_acquire_swapchain_image, error_message) &&
         LoadInstanceOpenXRFunction("xrWaitSwapchainImage", &g_state.xr_wait_swapchain_image, error_message) &&
         LoadInstanceOpenXRFunction("xrReleaseSwapchainImage", &g_state.xr_release_swapchain_image, error_message) &&
         LoadInstanceOpenXRFunction("xrResultToString", &g_state.xr_result_to_string, error_message) &&
         LoadInstanceOpenXRFunction("xrGetD3D11GraphicsRequirementsKHR", &g_state.xr_get_d3d11_graphics_requirements_khr, error_message);
}
#endif

#if defined(__linux__)
GLuint CompileShader(GLenum shader_type, const char* source, std::string* error_message) {
  const GLuint shader = glCreateShader(shader_type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }

  GLint log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  std::string log(static_cast<size_t>(log_length > 1 ? log_length : 1), '\0');
  if (log_length > 1) {
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
  }
  glDeleteShader(shader);
  SetError(error_message, "OpenXR GL shader compilation failed: " + log);
  return 0;
}

GLuint LinkProgram(const char* vertex_source, const char* fragment_source, std::string* error_message) {
  const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex_source, error_message);
  if (vertex_shader == 0) {
    return 0;
  }

  const GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment_source, error_message);
  if (fragment_shader == 0) {
    glDeleteShader(vertex_shader);
    return 0;
  }

  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex_shader);
  glAttachShader(program, fragment_shader);
  glBindAttribLocation(program, 0, "a_position");
  glBindAttribLocation(program, 1, "a_tex_coord");
  glLinkProgram(program);
  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    return program;
  }

  GLint log_length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
  std::string log(static_cast<size_t>(log_length > 1 ? log_length : 1), '\0');
  if (log_length > 1) {
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
  }
  glDeleteProgram(program);
  SetError(error_message, "OpenXR GL program link failed: " + log);
  return 0;
}
#endif

void DestroySwapchain() {
  if (g_state.swapchain != XR_NULL_HANDLE) {
    DestroySwapchainHandle(g_state.swapchain);
    g_state.swapchain = XR_NULL_HANDLE;
  }
  g_state.swapchain_images.clear();
  g_state.frame_width = 0;
  g_state.frame_height = 0;
}

#if defined(__linux__)
void ShutdownGraphicsObjects() {
  if (g_state.framebuffer != 0) {
    glDeleteFramebuffers(1, &g_state.framebuffer);
    g_state.framebuffer = 0;
  }
  if (g_state.imported_texture != 0) {
    glDeleteTextures(1, &g_state.imported_texture);
    g_state.imported_texture = 0;
  }
  if (g_state.program != 0) {
    glDeleteProgram(g_state.program);
    g_state.program = 0;
  }
  g_state.sampler_uniform = -1;
}
#endif

void ResetState() {
#if defined(__linux__)
  if (g_state.egl_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(g_state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
#endif

#if defined(__linux__)
  ShutdownGraphicsObjects();
#endif
  DestroySwapchain();

  if (g_state.view_space != XR_NULL_HANDLE) {
    DestroySpaceHandle(g_state.view_space);
    g_state.view_space = XR_NULL_HANDLE;
  }
  if (g_state.stage_space != XR_NULL_HANDLE) {
    DestroySpaceHandle(g_state.stage_space);
    g_state.stage_space = XR_NULL_HANDLE;
  }
  if (g_state.local_space != XR_NULL_HANDLE) {
    DestroySpaceHandle(g_state.local_space);
    g_state.local_space = XR_NULL_HANDLE;
  }
  if (g_state.session != XR_NULL_HANDLE) {
    DestroySessionHandle(g_state.session);
    g_state.session = XR_NULL_HANDLE;
  }
  if (g_state.instance != XR_NULL_HANDLE) {
    DestroyInstanceHandle(g_state.instance);
    g_state.instance = XR_NULL_HANDLE;
  }

#if defined(__linux__)
  if (g_state.egl_context != EGL_NO_CONTEXT && g_state.egl_display != EGL_NO_DISPLAY) {
    eglDestroyContext(g_state.egl_display, g_state.egl_context);
    g_state.egl_context = EGL_NO_CONTEXT;
  }
  if (g_state.egl_surface != EGL_NO_SURFACE && g_state.egl_display != EGL_NO_DISPLAY) {
    eglDestroySurface(g_state.egl_display, g_state.egl_surface);
    g_state.egl_surface = EGL_NO_SURFACE;
  }
  if (g_state.egl_display != EGL_NO_DISPLAY) {
    eglTerminate(g_state.egl_display);
    g_state.egl_display = EGL_NO_DISPLAY;
  }
  g_state.egl_config = nullptr;
  g_state.egl_create_image_khr = nullptr;
  g_state.egl_destroy_image_khr = nullptr;
  g_state.gl_egl_image_target_texture_2d_oes = nullptr;
  g_state.xr_get_opengl_graphics_requirements_khr = nullptr;
#elif defined(_WIN32)
  if (g_state.openxr_loader != nullptr) {
    FreeLibrary(g_state.openxr_loader);
    g_state.openxr_loader = nullptr;
  }
  g_state.xr_get_instance_proc_addr = nullptr;
  g_state.xr_enumerate_instance_extension_properties = nullptr;
  g_state.xr_create_instance = nullptr;
  g_state.xr_destroy_instance = nullptr;
  g_state.xr_get_system = nullptr;
  g_state.xr_create_session = nullptr;
  g_state.xr_destroy_session = nullptr;
  g_state.xr_create_reference_space = nullptr;
  g_state.xr_destroy_space = nullptr;
  g_state.xr_enumerate_swapchain_formats = nullptr;
  g_state.xr_create_swapchain = nullptr;
  g_state.xr_destroy_swapchain = nullptr;
  g_state.xr_enumerate_swapchain_images = nullptr;
  g_state.xr_enumerate_environment_blend_modes = nullptr;
  g_state.xr_poll_event = nullptr;
  g_state.xr_begin_session = nullptr;
  g_state.xr_end_session = nullptr;
  g_state.xr_wait_frame = nullptr;
  g_state.xr_begin_frame = nullptr;
  g_state.xr_end_frame = nullptr;
  g_state.xr_acquire_swapchain_image = nullptr;
  g_state.xr_wait_swapchain_image = nullptr;
  g_state.xr_release_swapchain_image = nullptr;
  g_state.xr_result_to_string = nullptr;
  g_state.xr_get_d3d11_graphics_requirements_khr = nullptr;
  ReleaseD3DResources();
#endif
  g_state.swapchain_format = 0;
  g_state.environment_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  g_state.session_state = XR_SESSION_STATE_UNKNOWN;
  g_state.session_running = false;
  g_state.logged_first_frame_submission = false;
  g_state.initialized = false;
}

bool SelectSwapchainFormat(std::string* error_message) {
  uint32_t format_count = 0;
#if defined(_WIN32)
  if (!CheckXr(
        g_state.xr_enumerate_swapchain_formats(g_state.session, 0, &format_count, nullptr),
        "Failed to enumerate OpenXR swapchain format count",
        error_message)) {
    return false;
  }
#else
  if (!CheckXr(
        xrEnumerateSwapchainFormats(g_state.session, 0, &format_count, nullptr),
        "Failed to enumerate OpenXR swapchain format count",
        error_message)) {
    return false;
  }
#endif

  std::vector<int64_t> formats(format_count);
#if defined(_WIN32)
  if (!CheckXr(
        g_state.xr_enumerate_swapchain_formats(g_state.session, format_count, &format_count, formats.data()),
        "Failed to enumerate OpenXR swapchain formats",
        error_message)) {
    return false;
  }
#else
  if (!CheckXr(
        xrEnumerateSwapchainFormats(g_state.session, format_count, &format_count, formats.data()),
        "Failed to enumerate OpenXR swapchain formats",
        error_message)) {
    return false;
  }
#endif

#if defined(__linux__)
  static const int64_t kPreferredFormats[] = {
    GL_SRGB8_ALPHA8_EXT,
    GL_RGBA,
  };
#elif defined(_WIN32)
  static const int64_t kPreferredFormats[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
  };
#endif

  for (size_t index = 0; index < (sizeof(kPreferredFormats) / sizeof(kPreferredFormats[0])); ++index) {
    const int64_t preferred_format = kPreferredFormats[index];
    if (std::find(formats.begin(), formats.end(), preferred_format) != formats.end()) {
      g_state.swapchain_format = preferred_format;
      return true;
    }
  }

  if (!formats.empty()) {
    g_state.swapchain_format = formats.front();
    return true;
  }

  SetError(error_message, "OpenXR runtime reported no swapchain formats.");
  return false;
}

bool SelectEnvironmentBlendMode(std::string* error_message) {
  uint32_t blend_mode_count = 0;
#if defined(_WIN32)
  if (!CheckXr(
        g_state.xr_enumerate_environment_blend_modes(
          g_state.instance,
          g_state.system_id,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          0,
          &blend_mode_count,
          nullptr),
        "Failed to enumerate OpenXR environment blend mode count",
        error_message)) {
    return false;
  }
#else
  if (!CheckXr(
        xrEnumerateEnvironmentBlendModes(
          g_state.instance,
          g_state.system_id,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          0,
          &blend_mode_count,
          nullptr),
        "Failed to enumerate OpenXR environment blend mode count",
        error_message)) {
    return false;
  }
#endif

  std::vector<XrEnvironmentBlendMode> blend_modes(blend_mode_count);
#if defined(_WIN32)
  if (!CheckXr(
        g_state.xr_enumerate_environment_blend_modes(
          g_state.instance,
          g_state.system_id,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          blend_mode_count,
          &blend_mode_count,
          blend_modes.data()),
        "Failed to enumerate OpenXR environment blend modes",
        error_message)) {
    return false;
  }
#else
  if (!CheckXr(
        xrEnumerateEnvironmentBlendModes(
          g_state.instance,
          g_state.system_id,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          blend_mode_count,
          &blend_mode_count,
          blend_modes.data()),
        "Failed to enumerate OpenXR environment blend modes",
        error_message)) {
    return false;
  }
#endif

  static const XrEnvironmentBlendMode kPreferredBlendModes[] = {
    XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND,
    XR_ENVIRONMENT_BLEND_MODE_ADDITIVE,
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
  };

  for (size_t preferred_index = 0; preferred_index < (sizeof(kPreferredBlendModes) / sizeof(kPreferredBlendModes[0])); ++preferred_index) {
    const XrEnvironmentBlendMode preferred_mode = kPreferredBlendModes[preferred_index];
    if (std::find(blend_modes.begin(), blend_modes.end(), preferred_mode) != blend_modes.end()) {
      g_state.environment_blend_mode = preferred_mode;
      return true;
    }
  }

  if (!blend_modes.empty()) {
    g_state.environment_blend_mode = blend_modes.front();
    return true;
  }

  SetError(error_message, "OpenXR runtime reported no environment blend modes.");
  return false;
}

bool BeginSessionIfNeeded(std::string* error_message) {
  if (g_state.session_running) {
    return true;
  }

  if (g_state.session_state != XR_SESSION_STATE_READY && g_state.session_state != XR_SESSION_STATE_SYNCHRONIZED &&
      g_state.session_state != XR_SESSION_STATE_VISIBLE && g_state.session_state != XR_SESSION_STATE_FOCUSED) {
    SetError(error_message, "OpenXR session is not ready to begin frames yet.");
    return false;
  }

  auto begin_info = MakeXrStruct<XrSessionBeginInfo, XR_TYPE_SESSION_BEGIN_INFO>();
  begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
#if defined(_WIN32)
  if (!CheckXr(g_state.xr_begin_session(g_state.session, &begin_info), "Failed to begin OpenXR session", error_message)) {
#else
  if (!CheckXr(xrBeginSession(g_state.session, &begin_info), "Failed to begin OpenXR session", error_message)) {
#endif
    return false;
  }

  g_state.session_running = true;
  return true;
}

#if defined(__linux__)
bool InitializeEgl(std::string* error_message) {
  g_state.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_state.egl_display == EGL_NO_DISPLAY) {
    SetError(error_message, "Failed to acquire EGL display for OpenXR backend.");
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(g_state.egl_display, &major, &minor) != EGL_TRUE) {
    SetError(error_message, "Failed to initialize EGL for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    SetError(error_message, "Failed to bind OpenGL ES API for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  const EGLint config_attributes[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_NONE,
  };

  EGLint num_configs = 0;
  if (eglChooseConfig(g_state.egl_display, config_attributes, &g_state.egl_config, 1, &num_configs) != EGL_TRUE || num_configs <= 0) {
    SetError(error_message, "Failed to choose EGL config for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE,
  };

  const EGLint surface_attributes[] = {
    EGL_WIDTH, 16,
    EGL_HEIGHT, 16,
    EGL_NONE,
  };

  g_state.egl_surface = eglCreatePbufferSurface(g_state.egl_display, g_state.egl_config, surface_attributes);
  if (g_state.egl_surface == EGL_NO_SURFACE) {
    SetError(error_message, "Failed to create EGL pbuffer surface for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  g_state.egl_context = eglCreateContext(g_state.egl_display, g_state.egl_config, EGL_NO_CONTEXT, context_attributes);
  if (g_state.egl_context == EGL_NO_CONTEXT) {
    SetError(error_message, "Failed to create EGL OpenGL context for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  if (eglMakeCurrent(g_state.egl_display, g_state.egl_surface, g_state.egl_surface, g_state.egl_context) != EGL_TRUE) {
    SetError(error_message, "Failed to make EGL context current for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  g_state.egl_create_image_khr = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  g_state.egl_destroy_image_khr = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  g_state.gl_egl_image_target_texture_2d_oes = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (g_state.egl_create_image_khr == nullptr || g_state.egl_destroy_image_khr == nullptr ||
      g_state.gl_egl_image_target_texture_2d_oes == nullptr) {
    SetError(error_message, "Required EGL image extensions are unavailable for OpenXR backend.");
    return false;
  }

  return true;
}

bool InitializeGraphicsResources(std::string* error_message) {
  static constexpr char kVertexShaderSource[] = R"(
    attribute vec2 a_position;
    attribute vec2 a_tex_coord;
    varying vec2 v_tex_coord;
    void main() {
      v_tex_coord = a_tex_coord;
      gl_Position = vec4(a_position, 0.0, 1.0);
    }
  )";

  static constexpr char kFragmentShaderSource[] = R"(
    precision mediump float;
    varying vec2 v_tex_coord;
    uniform sampler2D u_texture;
    void main() {
      gl_FragColor = texture2D(u_texture, v_tex_coord);
    }
  )";

  g_state.program = LinkProgram(kVertexShaderSource, kFragmentShaderSource, error_message);
  if (g_state.program == 0) {
    return false;
  }

  glUseProgram(g_state.program);
  g_state.sampler_uniform = glGetUniformLocation(g_state.program, "u_texture");
  glUniform1i(g_state.sampler_uniform, 0);

  glGenTextures(1, &g_state.imported_texture);
  glBindTexture(GL_TEXTURE_2D, g_state.imported_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &g_state.framebuffer);

  if (g_state.imported_texture == 0 || g_state.framebuffer == 0) {
    SetError(error_message, "Failed to create OpenXR GL copy resources.");
    return false;
  }

  return true;
}
#endif

bool CreateInstance(std::string* error_message) {
  uint32_t extension_count = 0;
#if defined(_WIN32)
  if (!LoadOpenXRLoader(error_message)) {
    return false;
  }
  if (!CheckXr(g_state.xr_enumerate_instance_extension_properties(nullptr, 0, &extension_count, nullptr), "Failed to enumerate OpenXR instance extensions", error_message)) {
#else
  if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr), "Failed to enumerate OpenXR instance extensions", error_message)) {
#endif
    return false;
  }

  std::vector<XrExtensionProperties> extensions(extension_count);
  for (XrExtensionProperties& extension : extensions) {
    extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    extension.next = nullptr;
  }

#if defined(_WIN32)
  if (!CheckXr(g_state.xr_enumerate_instance_extension_properties(nullptr, extension_count, &extension_count, extensions.data()), "Failed to enumerate OpenXR instance extensions", error_message)) {
#else
  if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data()), "Failed to enumerate OpenXR instance extensions", error_message)) {
#endif
    return false;
  }

  if (!HasExtension(extensions, XR_EXTX_OVERLAY_EXTENSION_NAME)) {
    SetError(error_message, "OpenXR runtime does not expose XR_EXTX_overlay.");
    return false;
  }

#if defined(__linux__)
  if (!HasExtension(extensions, XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
    SetError(error_message, "OpenXR runtime does not expose XR_MNDX_egl_enable.");
    return false;
  }
  if (!HasExtension(extensions, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
    SetError(error_message, "OpenXR runtime does not expose XR_KHR_opengl_es_enable.");
    return false;
  }

  const char* enabled_extensions[] = {
    XR_EXTX_OVERLAY_EXTENSION_NAME,
    XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
  };
  constexpr uint32_t enabled_extension_count = 3;
#elif defined(_WIN32)
  if (!HasExtension(extensions, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
    SetError(error_message, "OpenXR runtime does not expose XR_KHR_D3D11_enable.");
    return false;
  }

  const char* enabled_extensions[] = {
    XR_EXTX_OVERLAY_EXTENSION_NAME,
    XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
  };
  constexpr uint32_t enabled_extension_count = 2;
#endif

  auto create_info = MakeXrStruct<XrInstanceCreateInfo, XR_TYPE_INSTANCE_CREATE_INFO>();
  std::strncpy(create_info.applicationInfo.applicationName, "electron-vr", XR_MAX_APPLICATION_NAME_SIZE - 1);
  create_info.applicationInfo.applicationVersion = 1;
  std::strncpy(create_info.applicationInfo.engineName, "electron-vr", XR_MAX_ENGINE_NAME_SIZE - 1);
  create_info.applicationInfo.engineVersion = 1;
  create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
  create_info.enabledExtensionCount = enabled_extension_count;
  create_info.enabledExtensionNames = enabled_extensions;

  if (!CheckXr(CreateInstanceHandle(&create_info, &g_state.instance), "Failed to create OpenXR instance", error_message)) {
    return false;
  }

#if defined(_WIN32)
  if (!LoadInstanceOpenXRFunctions(error_message)) {
    return false;
  }
#endif

  return true;
}

bool CreateSession(std::string* error_message) {
  auto system_get_info = MakeXrStruct<XrSystemGetInfo, XR_TYPE_SYSTEM_GET_INFO>();
  system_get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!CheckXr(GetSystemHandle(g_state.instance, &system_get_info, &g_state.system_id), "Failed to acquire OpenXR system", error_message)) {
    return false;
  }

#if defined(__linux__)
  if (!CheckXr(xrGetInstanceProcAddr(
        g_state.instance,
        "xrGetOpenGLESGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&g_state.xr_get_opengl_graphics_requirements_khr)),
      "Failed to load xrGetOpenGLESGraphicsRequirementsKHR", error_message) ||
      g_state.xr_get_opengl_graphics_requirements_khr == nullptr) {
    return false;
  }

  auto graphics_requirements = MakeXrStruct<XrGraphicsRequirementsOpenGLESKHR, XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR>();
  if (!CheckXr(g_state.xr_get_opengl_graphics_requirements_khr(g_state.instance, g_state.system_id, &graphics_requirements), "Failed to query OpenXR OpenGL ES graphics requirements", error_message)) {
    return false;
  }

  auto graphics_binding = MakeXrStruct<XrGraphicsBindingEGLMNDX, XR_TYPE_GRAPHICS_BINDING_EGL_MNDX>();
  graphics_binding.getProcAddress = eglGetProcAddress;
  graphics_binding.display = g_state.egl_display;
  graphics_binding.config = g_state.egl_config;
  graphics_binding.context = g_state.egl_context;

  auto overlay_info = MakeXrStruct<XrSessionCreateInfoOverlayEXTX, XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX>();
  overlay_info.createFlags = 0;
  overlay_info.sessionLayersPlacement = UINT32_MAX;

  graphics_binding.next = &overlay_info;

  auto session_create_info = MakeXrStruct<XrSessionCreateInfo, XR_TYPE_SESSION_CREATE_INFO>();
  session_create_info.next = &graphics_binding;
  session_create_info.systemId = g_state.system_id;
#elif defined(_WIN32)
  if (!EnsureD3D11Device(error_message)) {
    return false;
  }

  auto graphics_requirements = MakeXrStruct<XrGraphicsRequirementsD3D11KHR, XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR>();
  if (!CheckXr(g_state.xr_get_d3d11_graphics_requirements_khr(g_state.instance, g_state.system_id, &graphics_requirements), "Failed to query OpenXR D3D11 graphics requirements", error_message)) {
    return false;
  }

  auto graphics_binding = MakeXrStruct<XrGraphicsBindingD3D11KHR, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR>();
  graphics_binding.device = g_state.d3d_device;

  auto overlay_info = MakeXrStruct<XrSessionCreateInfoOverlayEXTX, XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX>();
  overlay_info.createFlags = 0;
  overlay_info.sessionLayersPlacement = UINT32_MAX;

  graphics_binding.next = &overlay_info;

  auto session_create_info = MakeXrStruct<XrSessionCreateInfo, XR_TYPE_SESSION_CREATE_INFO>();
  session_create_info.next = &graphics_binding;
  session_create_info.systemId = g_state.system_id;
#endif

  if (!CheckXr(CreateSessionHandle(g_state.instance, &session_create_info, &g_state.session), "Failed to create OpenXR overlay session", error_message)) {
    return false;
  }

  auto local_space_info = MakeXrStruct<XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO>();
  local_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  local_space_info.poseInReferenceSpace.orientation.w = 1.0f;
  if (!CheckXr(CreateReferenceSpaceHandle(g_state.session, &local_space_info, &g_state.local_space), "Failed to create OpenXR local reference space", error_message)) {
    return false;
  }

  auto view_space_info = MakeXrStruct<XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO>();
  view_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  view_space_info.poseInReferenceSpace.orientation.w = 1.0f;
  if (!CheckXr(CreateReferenceSpaceHandle(g_state.session, &view_space_info, &g_state.view_space), "Failed to create OpenXR view reference space", error_message)) {
    return false;
  }

  auto stage_space_info = MakeXrStruct<XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO>();
  stage_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  stage_space_info.poseInReferenceSpace.orientation.w = 1.0f;
  const XrResult stage_space_result = CreateReferenceSpaceHandle(g_state.session, &stage_space_info, &g_state.stage_space);
  if (XR_FAILED(stage_space_result)) {
    g_state.stage_space = XR_NULL_HANDLE;
  }

  if (!SelectSwapchainFormat(error_message)) {
    return false;
  }

  if (!SelectEnvironmentBlendMode(error_message)) {
    return false;
  }

  g_state.session_state = XR_SESSION_STATE_READY;
  g_state.session_running = false;
  return true;
}

bool CreateSwapchain(uint32_t width, uint32_t height, std::string* error_message) {
  if (width == 0 || height == 0) {
    SetError(error_message, "OpenXR swapchain requires non-zero width and height.");
    return false;
  }

  DestroySwapchain();

  auto swapchain_create_info = MakeXrStruct<XrSwapchainCreateInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO>();
  swapchain_create_info.createFlags = 0;
  swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  swapchain_create_info.format = g_state.swapchain_format;
  swapchain_create_info.sampleCount = 1;
  swapchain_create_info.width = width;
  swapchain_create_info.height = height;
  swapchain_create_info.faceCount = 1;
  swapchain_create_info.arraySize = 1;
  swapchain_create_info.mipCount = 1;

  if (!CheckXr(CreateSwapchainHandle(g_state.session, &swapchain_create_info, &g_state.swapchain), "Failed to create OpenXR swapchain", error_message)) {
    return false;
  }

  uint32_t image_count = 0;
  if (!CheckXr(EnumerateSwapchainImagesHandle(g_state.swapchain, 0, &image_count, nullptr), "Failed to enumerate OpenXR swapchain image count", error_message)) {
    DestroySwapchain();
    return false;
  }

#if defined(__linux__)
  g_state.swapchain_images.resize(image_count);
  for (XrSwapchainImageOpenGLESKHR& image : g_state.swapchain_images) {
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    image.next = nullptr;
    image.image = 0;
  }

  if (!CheckXr(
        EnumerateSwapchainImagesHandle(
          g_state.swapchain,
          image_count,
          &image_count,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(g_state.swapchain_images.data())),
        "Failed to enumerate OpenXR swapchain images",
        error_message)) {
    DestroySwapchain();
    return false;
  }
#elif defined(_WIN32)
  g_state.swapchain_images.resize(image_count);
  for (XrSwapchainImageD3D11KHR& image : g_state.swapchain_images) {
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    image.next = nullptr;
    image.texture = nullptr;
  }

  if (!CheckXr(
        EnumerateSwapchainImagesHandle(
          g_state.swapchain,
          image_count,
          &image_count,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(g_state.swapchain_images.data())),
        "Failed to enumerate OpenXR swapchain images",
        error_message)) {
    DestroySwapchain();
    return false;
  }
#endif

  g_state.frame_width = width;
  g_state.frame_height = height;
  return true;
}

XrSpace GetLayerSpaceForPlacement(const OverlayPlacement& placement) {
  if (placement.mode == OverlayPlacementMode::kHead) {
    return g_state.view_space;
  }

  if (g_state.stage_space != XR_NULL_HANDLE) {
    return g_state.stage_space;
  }

  return g_state.local_space;
}

const char* GetLayerSpaceNameForPlacement(const OverlayPlacement& placement) {
  if (placement.mode == OverlayPlacementMode::kHead) {
    return "view";
  }

  if (g_state.stage_space != XR_NULL_HANDLE) {
    return "stage";
  }

  return "local";
}

#if defined(__linux__)
bool RenderImportedFrameToSwapchain(GLuint destination_texture, const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (texture_info.planes.empty()) {
    SetError(error_message, "OpenXR backend requires at least one DMA-BUF plane.");
    return false;
  }

  const LinuxPlaneInfo& plane = texture_info.planes.front();
  if (plane.fd < 0) {
    SetError(error_message, "OpenXR backend received an invalid DMA-BUF fd.");
    return false;
  }

  std::array<EGLint, 32> attributes{};
  size_t attribute_index = 0;
  const auto push_attribute = [&](EGLint key, EGLint value) {
    attributes[attribute_index++] = key;
    attributes[attribute_index++] = value;
  };

  push_attribute(EGL_WIDTH, static_cast<EGLint>(texture_info.width));
  push_attribute(EGL_HEIGHT, static_cast<EGLint>(texture_info.height));
  push_attribute(EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(ParseDrmFormat(texture_info.pixel_format)));
  push_attribute(EGL_DMA_BUF_PLANE0_FD_EXT, plane.fd);
  push_attribute(EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(plane.offset));
  push_attribute(EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(plane.stride));

  uint64_t modifier = 0;
  if (ParseModifier(texture_info.modifier, &modifier)) {
    push_attribute(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, static_cast<EGLint>(modifier & 0xffffffffULL));
    push_attribute(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, static_cast<EGLint>((modifier >> 32U) & 0xffffffffULL));
  }
  attributes[attribute_index] = EGL_NONE;

  EGLImageKHR image = g_state.egl_create_image_khr(g_state.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  if (image == EGL_NO_IMAGE_KHR) {
    SetError(error_message, "Failed to import DMA-BUF into EGLImage for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  glBindTexture(GL_TEXTURE_2D, g_state.imported_texture);
  g_state.gl_egl_image_target_texture_2d_oes(GL_TEXTURE_2D, image);

  glBindFramebuffer(GL_FRAMEBUFFER, g_state.framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination_texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_state.egl_destroy_image_khr(g_state.egl_display, image);
    SetError(error_message, "OpenXR swapchain framebuffer is incomplete for textured rendering.");
    return false;
  }

  glViewport(0, 0, static_cast<GLsizei>(g_state.frame_width), static_cast<GLsizei>(g_state.frame_height));
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(g_state.program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_state.imported_texture);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);

  static constexpr GLfloat kVertices[] = {
      -1.0f, -1.0f, 0.0f, 1.0f,
       1.0f, -1.0f, 1.0f, 1.0f,
      -1.0f,  1.0f, 0.0f, 0.0f,
       1.0f,  1.0f, 1.0f, 0.0f,
  };
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices + 2);
  glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glFinish();

  if (g_state.egl_destroy_image_khr(g_state.egl_display, image) != EGL_TRUE) {
    SetError(error_message, "Failed to destroy imported EGLImage for OpenXR backend: " + EglErrorString(eglGetError()));
    return false;
  }

  return true;
}
#endif

bool PollEvents(std::string* error_message) {
  auto event_buffer = MakeXrStruct<XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER>();
  while (true) {
    const XrResult poll_result = PollEventHandle(g_state.instance, &event_buffer);
    if (poll_result == XR_EVENT_UNAVAILABLE) {
      return true;
    }

    if (XR_FAILED(poll_result)) {
      SetError(error_message, "OpenXR poll event failed: " + XrResultToString(g_state.instance, poll_result));
      return false;
    }

    if (event_buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto* session_state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event_buffer);
      g_state.session_state = session_state->state;

      if (session_state->state == XR_SESSION_STATE_READY) {
        if (!BeginSessionIfNeeded(error_message)) {
          return false;
        }
      } else if (session_state->state == XR_SESSION_STATE_STOPPING) {
        if (g_state.session_running) {
          if (!CheckXr(EndSessionHandle(g_state.session), "Failed to end OpenXR session", error_message)) {
            return false;
          }
          g_state.session_running = false;
        }
      } else if (session_state->state == XR_SESSION_STATE_EXITING || session_state->state == XR_SESSION_STATE_LOSS_PENDING) {
        SetError(error_message, "OpenXR session exited or was lost.");
        return false;
      }
    }

    event_buffer = MakeXrStruct<XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER>();
  }
}

#if defined(__linux__)
bool EnsureSwapchainForFrame(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (texture_info.width == 0 || texture_info.height == 0) {
    SetError(error_message, "Linux OpenXR texture submission requires codedSize width and height.");
    return false;
  }

  if (g_state.swapchain == XR_NULL_HANDLE || g_state.frame_width != texture_info.width || g_state.frame_height != texture_info.height) {
    if (!CreateSwapchain(texture_info.width, texture_info.height, error_message)) {
      return false;
    }
  }

  return true;
}

bool SubmitLayerForCurrentFrame(const LinuxTextureInfo& texture_info, std::string* error_message) {
  if (!PollEvents(error_message)) {
    return false;
  }

  if (g_state.session_state == XR_SESSION_STATE_EXITING || g_state.session_state == XR_SESSION_STATE_LOSS_PENDING) {
    SetError(error_message, "OpenXR session exited or was lost.");
    return false;
  }

  if (!EnsureSwapchainForFrame(texture_info, error_message)) {
    return false;
  }

  if (!BeginSessionIfNeeded(error_message)) {
    return false;
  }

  auto frame_wait_info = MakeXrStruct<XrFrameWaitInfo, XR_TYPE_FRAME_WAIT_INFO>();
  auto frame_state = MakeXrStruct<XrFrameState, XR_TYPE_FRAME_STATE>();
  if (!CheckXr(WaitFrameHandle(g_state.session, &frame_wait_info, &frame_state), "Failed to wait for OpenXR frame", error_message)) {
    return false;
  }

  auto frame_begin_info = MakeXrStruct<XrFrameBeginInfo, XR_TYPE_FRAME_BEGIN_INFO>();
  if (!CheckXr(BeginFrameHandle(g_state.session, &frame_begin_info), "Failed to begin OpenXR frame", error_message)) {
    return false;
  }

  std::vector<XrCompositionLayerBaseHeader*> layers;
  auto quad_layer = MakeXrStruct<XrCompositionLayerQuad, XR_TYPE_COMPOSITION_LAYER_QUAD>();

  if (g_state.visible) {
    uint32_t image_index = 0;
    auto acquire_info = MakeXrStruct<XrSwapchainImageAcquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO>();
    if (!CheckXr(AcquireSwapchainImageHandle(g_state.swapchain, &acquire_info, &image_index), "Failed to acquire OpenXR swapchain image", error_message)) {
      return false;
    }

    auto wait_info = MakeXrStruct<XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO>();
    wait_info.timeout = XR_INFINITE_DURATION;
    if (!CheckXr(WaitSwapchainImageHandle(g_state.swapchain, &wait_info), "Failed to wait for OpenXR swapchain image", error_message)) {
      return false;
    }

    const GLuint swapchain_texture = g_state.swapchain_images[image_index].image;
    if (!RenderImportedFrameToSwapchain(swapchain_texture, texture_info, error_message)) {
      auto release_info = MakeXrStruct<XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO>();
      ReleaseSwapchainImageHandle(g_state.swapchain, &release_info);
      return false;
    }

    auto release_info = MakeXrStruct<XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO>();
    if (!CheckXr(ReleaseSwapchainImageHandle(g_state.swapchain, &release_info), "Failed to release OpenXR swapchain image", error_message)) {
      return false;
    }

    quad_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad_layer.space = GetLayerSpaceForPlacement(g_state.placement);
    quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_layer.subImage.swapchain = g_state.swapchain;
    quad_layer.subImage.imageRect.offset = {0, 0};
    quad_layer.subImage.imageRect.extent = {static_cast<int32_t>(g_state.frame_width), static_cast<int32_t>(g_state.frame_height)};
    quad_layer.subImage.imageArrayIndex = 0;
    quad_layer.pose = ToXrPose(g_state.placement);
    quad_layer.size.width = g_state.size_meters;
    quad_layer.size.height = ComputeHeightMeters(g_state.frame_width, g_state.frame_height, g_state.size_meters);

    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));

    if (!g_state.logged_first_frame_submission) {
      g_state.logged_first_frame_submission = true;
      std::cout << "OpenXR submitted first quad layer: space="
                << GetLayerSpaceNameForPlacement(g_state.placement)
                << ", pose=(" << quad_layer.pose.position.x << ", " << quad_layer.pose.position.y << ", " << quad_layer.pose.position.z
                << "), size=(" << quad_layer.size.width << "m x " << quad_layer.size.height << "m), frame="
                << g_state.frame_width << "x" << g_state.frame_height
                << std::endl;
    }
  }

  auto frame_end_info = MakeXrStruct<XrFrameEndInfo, XR_TYPE_FRAME_END_INFO>();
  frame_end_info.displayTime = frame_state.predictedDisplayTime;
  frame_end_info.environmentBlendMode = g_state.environment_blend_mode;
  frame_end_info.layerCount = static_cast<uint32_t>(layers.size());
  frame_end_info.layers = layers.empty() ? nullptr : layers.data();

  if (!CheckXr(EndFrameHandle(g_state.session, &frame_end_info), "Failed to end OpenXR frame", error_message)) {
    return false;
  }

  return true;
}
#endif

bool g_initialized = false;

#if defined(_WIN32)
bool EnsureSwapchainForWindowsFrame(uint32_t width, uint32_t height, std::string* error_message) {
  if (width == 0 || height == 0) {
    SetError(error_message, "Windows OpenXR texture submission requires non-zero width and height.");
    return false;
  }

  if (g_state.swapchain == XR_NULL_HANDLE || g_state.frame_width != width || g_state.frame_height != height) {
    if (!CreateSwapchain(width, height, error_message)) {
      return false;
    }
  }

  return true;
}

bool SubmitLayerForCurrentFrameWindows(ID3D11Texture2D* source_texture, const D3D11_TEXTURE2D_DESC& source_desc, std::string* error_message) {
  if (source_texture == nullptr) {
    SetError(error_message, "Windows OpenXR frame submission requires a shared D3D11 texture.");
    return false;
  }

  if (!PollEvents(error_message)) {
    return false;
  }

  if (g_state.session_state == XR_SESSION_STATE_EXITING || g_state.session_state == XR_SESSION_STATE_LOSS_PENDING) {
    SetError(error_message, "OpenXR session exited or was lost.");
    return false;
  }

  if (!EnsureSwapchainForWindowsFrame(source_desc.Width, source_desc.Height, error_message)) {
    return false;
  }

  if (!BeginSessionIfNeeded(error_message)) {
    return false;
  }

  auto frame_wait_info = MakeXrStruct<XrFrameWaitInfo, XR_TYPE_FRAME_WAIT_INFO>();
  auto frame_state = MakeXrStruct<XrFrameState, XR_TYPE_FRAME_STATE>();
  if (!CheckXr(WaitFrameHandle(g_state.session, &frame_wait_info, &frame_state), "Failed to wait for OpenXR frame", error_message)) {
    return false;
  }

  auto frame_begin_info = MakeXrStruct<XrFrameBeginInfo, XR_TYPE_FRAME_BEGIN_INFO>();
  if (!CheckXr(BeginFrameHandle(g_state.session, &frame_begin_info), "Failed to begin OpenXR frame", error_message)) {
    return false;
  }

  std::vector<XrCompositionLayerBaseHeader*> layers;
  auto quad_layer = MakeXrStruct<XrCompositionLayerQuad, XR_TYPE_COMPOSITION_LAYER_QUAD>();

  if (g_state.visible) {
    uint32_t image_index = 0;
    auto acquire_info = MakeXrStruct<XrSwapchainImageAcquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO>();
    if (!CheckXr(AcquireSwapchainImageHandle(g_state.swapchain, &acquire_info, &image_index), "Failed to acquire OpenXR swapchain image", error_message)) {
      return false;
    }

    auto wait_info = MakeXrStruct<XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO>();
    wait_info.timeout = XR_INFINITE_DURATION;
    if (!CheckXr(WaitSwapchainImageHandle(g_state.swapchain, &wait_info), "Failed to wait for OpenXR swapchain image", error_message)) {
      return false;
    }

    ID3D11Texture2D* destination_texture = g_state.swapchain_images[image_index].texture;
    if (destination_texture == nullptr) {
      auto release_info = MakeXrStruct<XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO>();
      ReleaseSwapchainImageHandle(g_state.swapchain, &release_info);
      SetError(error_message, "OpenXR swapchain returned a null D3D11 texture.");
      return false;
    }

    g_state.d3d_context->CopyResource(destination_texture, source_texture);
    g_state.d3d_context->Flush();

    auto release_info = MakeXrStruct<XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO>();
    if (!CheckXr(ReleaseSwapchainImageHandle(g_state.swapchain, &release_info), "Failed to release OpenXR swapchain image", error_message)) {
      return false;
    }

    quad_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad_layer.space = GetLayerSpaceForPlacement(g_state.placement);
    quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_layer.subImage.swapchain = g_state.swapchain;
    quad_layer.subImage.imageRect.offset = {0, 0};
    quad_layer.subImage.imageRect.extent = {static_cast<int32_t>(g_state.frame_width), static_cast<int32_t>(g_state.frame_height)};
    quad_layer.subImage.imageArrayIndex = 0;
    quad_layer.pose = ToXrPose(g_state.placement);
    quad_layer.size.width = g_state.size_meters;
    quad_layer.size.height = ComputeHeightMeters(g_state.frame_width, g_state.frame_height, g_state.size_meters);

    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));

    if (!g_state.logged_first_frame_submission) {
      g_state.logged_first_frame_submission = true;
      std::cout << "OpenXR submitted first Windows quad layer: space="
                << GetLayerSpaceNameForPlacement(g_state.placement)
                << ", pose=(" << quad_layer.pose.position.x << ", " << quad_layer.pose.position.y << ", " << quad_layer.pose.position.z
                << "), size=(" << quad_layer.size.width << "m x " << quad_layer.size.height << "m), frame="
                << g_state.frame_width << "x" << g_state.frame_height
                << ", format=" << DxgiFormatToString(source_desc.Format)
                << std::endl;
    }
  }

  auto frame_end_info = MakeXrStruct<XrFrameEndInfo, XR_TYPE_FRAME_END_INFO>();
  frame_end_info.displayTime = frame_state.predictedDisplayTime;
  frame_end_info.environmentBlendMode = g_state.environment_blend_mode;
  frame_end_info.layerCount = static_cast<uint32_t>(layers.size());
  frame_end_info.layers = layers.empty() ? nullptr : layers.data();

  if (!CheckXr(EndFrameHandle(g_state.session, &frame_end_info), "Failed to end OpenXR frame", error_message)) {
    return false;
  }

  return true;
}
#endif

}  // namespace

bool InitializeOpenXRBackend(const InitializeOptions& options, std::string* error_message) {
  if (options.name.empty()) {
    SetError(error_message, "OpenXR backend requires a non-empty overlay name.");
    return false;
  }

#if defined(__linux__)
  ShutdownOpenXRBackend();

  g_state.visible = options.visible;
  g_state.size_meters = options.size_meters;
  g_state.placement = options.placement;

  if (!InitializeEgl(error_message) ||
      !InitializeGraphicsResources(error_message) ||
      !CreateInstance(error_message) ||
      !CreateSession(error_message)) {
    ShutdownOpenXRBackend();
    return false;
  }

  if (eglMakeCurrent(g_state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    SetError(error_message, "Failed to release EGL context from init thread for OpenXR backend: " + EglErrorString(eglGetError()));
    ShutdownOpenXRBackend();
    return false;
  }

  g_state.initialized = true;
  g_initialized = true;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#elif defined(_WIN32)
  ShutdownOpenXRBackend();

  g_state.visible = options.visible;
  g_state.size_meters = options.size_meters;
  g_state.placement = options.placement;

  if (!CreateInstance(error_message) ||
      !CreateSession(error_message)) {
    ShutdownOpenXRBackend();
    return false;
  }

  g_state.initialized = true;
  g_initialized = true;
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#else
  (void)options;
  SetError(error_message, "OpenXR backend is only implemented on Linux right now.");
  return false;
#endif
}

bool SubmitOpenXRFrameWindows(uint64_t shared_handle, std::string* error_message) {
#if defined(_WIN32)
  if (!g_state.initialized || !g_initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }

  if (shared_handle == 0) {
    SetError(error_message, "OpenXR backend received an invalid Windows shared handle.");
    return false;
  }

  if (!OpenSharedTextureFromHandle(shared_handle, error_message)) {
    return false;
  }

  D3D11_TEXTURE2D_DESC shared_desc = {};
  g_state.shared_texture->GetDesc(&shared_desc);
  if (shared_desc.Width == 0 || shared_desc.Height == 0) {
    SetError(error_message, "Windows OpenXR texture submission requires non-zero width and height.");
    return false;
  }

  if (!IsCompatibleOpenXRSwapchainFormat(shared_desc.Format)) {
    SetError(
      error_message,
      "Windows OpenXR received unsupported shared texture format " + std::string(DxgiFormatToString(shared_desc.Format)) + ". Expected BGRA8 or RGBA8.");
    return false;
  }

  if (!SubmitLayerForCurrentFrameWindows(g_state.shared_texture, shared_desc, error_message)) {
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#else
  (void)shared_handle;
  SetError(error_message, "OpenXR Windows frame submission is only available on Windows builds.");
  return false;
#endif
}

bool SubmitOpenXRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message) {
#if defined(__linux__)
  if (!g_state.initialized || !g_initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }

  if (texture_info.planes.empty() || texture_info.planes.front().fd < 0) {
    SetError(error_message, "OpenXR backend received an invalid DMA-BUF fd.");
    return false;
  }

  if (eglMakeCurrent(g_state.egl_display, g_state.egl_surface, g_state.egl_surface, g_state.egl_context) != EGL_TRUE) {
    SetError(error_message, "Failed to make EGL context current for OpenXR frame submission: " + EglErrorString(eglGetError()));
    return false;
  }

  if (!SubmitLayerForCurrentFrame(texture_info, error_message)) {
    eglMakeCurrent(g_state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return false;
  }

  if (eglMakeCurrent(g_state.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
    SetError(error_message, "Failed to release EGL context after OpenXR frame submission: " + EglErrorString(eglGetError()));
    return false;
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
#else
  (void)texture_info;
  SetError(error_message, "OpenXR Linux frame submission is only available on Linux builds.");
  return false;
#endif
}

bool SetOpenXRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  if (!g_initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }

#if defined(__linux__) || defined(_WIN32)
  g_state.placement = placement;
  g_state.logged_first_frame_submission = false;
#else
  (void)placement;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetOpenXRVisible(bool visible, std::string* error_message) {
  if (!g_initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }

#if defined(__linux__) || defined(_WIN32)
  g_state.visible = visible;
#else
  (void)visible;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool SetOpenXRSizeMeters(float size_meters, std::string* error_message) {
  if (!g_initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }
  if (!std::isfinite(size_meters) || size_meters <= 0.0f) {
    SetError(error_message, "OpenXR overlay size must be greater than zero.");
    return false;
  }

#if defined(__linux__) || defined(_WIN32)
  g_state.size_meters = size_meters;
#endif

  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

void PopulateOpenXRRuntimeInfo(RuntimeInfo* runtime_info) {
  if (runtime_info == nullptr) {
    return;
  }

  runtime_info->openxr_session_state = SessionStateToString(g_state.session_state);
  runtime_info->openxr_session_running = g_state.session_running;
}

void ShutdownOpenXRBackend() {
#if defined(__linux__) || defined(_WIN32)
  if (g_state.session != XR_NULL_HANDLE) {
    EndSessionHandle(g_state.session);
  }
  ResetState();
#endif
  g_initialized = false;
}

}  // namespace vrbridge
