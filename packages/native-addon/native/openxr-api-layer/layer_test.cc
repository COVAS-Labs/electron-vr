#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr char kLayerName[] = "XR_APILAYER_ELECTRON_VR_overlay";

XrInstance g_instance = (XrInstance)0x1001;
XrSession g_session = (XrSession)0x2001;
const XrFrameEndInfo* g_forwarded_frame = nullptr;
uint32_t g_create_instance_calls = 0;
uint32_t g_destroy_instance_calls = 0;
uint32_t g_create_session_calls = 0;
uint32_t g_destroy_session_calls = 0;
uint32_t g_end_frame_calls = 0;

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << std::endl;
  return false;
}

XrResult XRAPI_CALL StubDestroyInstance(XrInstance instance) {
  if (instance != g_instance) return XR_ERROR_HANDLE_INVALID;
  ++g_destroy_instance_calls;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
  if (properties == nullptr) return XR_ERROR_VALIDATION_FAILURE;
  properties->graphicsProperties.maxLayerCount = 16;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubCreateSession(XrInstance instance, const XrSessionCreateInfo*, XrSession* session) {
  if (instance != g_instance || session == nullptr) return XR_ERROR_VALIDATION_FAILURE;
  ++g_create_session_calls;
  *session = g_session;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubDestroySession(XrSession session) {
  if (session != g_session) return XR_ERROR_HANDLE_INVALID;
  ++g_destroy_session_calls;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubEnumerateReferenceSpaces(XrSession, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces) {
  if (count == nullptr) return XR_ERROR_VALIDATION_FAILURE;
  *count = 2;
  if (capacity >= 2 && spaces != nullptr) {
    spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*) {
  return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult XRAPI_CALL StubDestroySpace(XrSpace) { return XR_SUCCESS; }

XrResult XRAPI_CALL StubEnumerateSwapchainFormats(XrSession, uint32_t, uint32_t* count, int64_t*) {
  if (count != nullptr) *count = 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*) {
  return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult XRAPI_CALL StubDestroySwapchain(XrSwapchain) { return XR_SUCCESS; }

XrResult XRAPI_CALL StubEnumerateSwapchainImages(
    XrSwapchain, uint32_t, uint32_t* count, XrSwapchainImageBaseHeader*) {
  if (count != nullptr) *count = 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubAcquireSwapchainImage(
    XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*) {
  return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult XRAPI_CALL StubWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
  return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult XRAPI_CALL StubReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
  return XR_ERROR_FEATURE_UNSUPPORTED;
}

XrResult XRAPI_CALL StubEndFrame(XrSession session, const XrFrameEndInfo* frame) {
  if (session != g_session) return XR_ERROR_HANDLE_INVALID;
  ++g_end_frame_calls;
  g_forwarded_frame = frame;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubGetInstanceProcAddr(
    XrInstance,
    const char* name,
    PFN_xrVoidFunction* function) {
  if (name == nullptr || function == nullptr) return XR_ERROR_VALIDATION_FAILURE;
  struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
  };
  const Entry entries[] = {
    {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(StubDestroyInstance)},
    {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(StubGetSystemProperties)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(StubCreateSession)},
    {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(StubDestroySession)},
    {"xrEnumerateReferenceSpaces", reinterpret_cast<PFN_xrVoidFunction>(StubEnumerateReferenceSpaces)},
    {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(StubCreateReferenceSpace)},
    {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(StubDestroySpace)},
    {"xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction>(StubEnumerateSwapchainFormats)},
    {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(StubCreateSwapchain)},
    {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(StubDestroySwapchain)},
    {"xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction>(StubEnumerateSwapchainImages)},
    {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(StubAcquireSwapchainImage)},
    {"xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(StubWaitSwapchainImage)},
    {"xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(StubReleaseSwapchainImage)},
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(StubEndFrame)},
  };
  for (const Entry& entry : entries) {
    if (std::strcmp(name, entry.name) == 0) {
      *function = entry.function;
      return XR_SUCCESS;
    }
  }
  *function = nullptr;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL StubCreateApiLayerInstance(
    const XrInstanceCreateInfo*,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
  if (instance == nullptr || layer_info == nullptr || layer_info->nextInfo != nullptr) {
    return XR_ERROR_VALIDATION_FAILURE;
  }
  ++g_create_instance_calls;
  *instance = g_instance;
  return XR_SUCCESS;
}

std::wstring LayerPath() {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  std::wstring executable(path.data(), length);
  const size_t separator = executable.find_last_of(L"\\/");
  return executable.substr(0, separator + 1) + L"electron_vr_openxr_layer.dll";
}

}  // namespace

int wmain() {
  bool passed = true;
  HMODULE layer = LoadLibraryW(LayerPath().c_str());
  passed &= Expect(layer != nullptr, "API-layer DLL loads");
  if (layer == nullptr) return 1;

  const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
    GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface"));
  passed &= Expect(negotiate != nullptr, "negotiation export exists");
  if (negotiate == nullptr) {
    FreeLibrary(layer);
    return 1;
  }

  XrNegotiateLoaderInfo loader_info{};
  loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
  loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
  loader_info.structSize = sizeof(loader_info);
  loader_info.minInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
  loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

  XrNegotiateApiLayerRequest request{};
  request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
  request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
  request.structSize = sizeof(request);

  passed &= Expect(
    XR_FAILED(negotiate(&loader_info, "XR_APILAYER_INVALID", &request)),
    "negotiation rejects a different layer name");
  passed &= Expect(
    negotiate(&loader_info, kLayerName, &request) == XR_SUCCESS,
    "negotiation accepts the supported loader interface");
  passed &= Expect(request.getInstanceProcAddr != nullptr, "negotiation returns xrGetInstanceProcAddr");
  passed &= Expect(request.createApiLayerInstance != nullptr, "negotiation returns xrCreateApiLayerInstance");

  XrApiLayerNextInfo next_info{};
  next_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
  next_info.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
  next_info.structSize = sizeof(next_info);
  std::strncpy(next_info.layerName, "XR_APILAYER_INVALID", sizeof(next_info.layerName) - 1);
  next_info.nextGetInstanceProcAddr = StubGetInstanceProcAddr;
  next_info.nextCreateApiLayerInstance = StubCreateApiLayerInstance;

  XrApiLayerCreateInfo layer_info{};
  layer_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
  layer_info.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
  layer_info.structSize = sizeof(layer_info);
  layer_info.nextInfo = &next_info;

  XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(instance_info.applicationInfo.applicationName, "electron-vr-ci", XR_MAX_APPLICATION_NAME_SIZE - 1);
  instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
  instance_info.enabledExtensionCount = 1;
  instance_info.enabledExtensionNames = extensions;
  XrInstance instance = XR_NULL_HANDLE;
  passed &= Expect(
    XR_FAILED(request.createApiLayerInstance(&instance_info, &layer_info, &instance)) && g_create_instance_calls == 0,
    "instance creation rejects a mismatched layer chain node");
  std::memset(next_info.layerName, 0, sizeof(next_info.layerName));
  std::strncpy(next_info.layerName, kLayerName, sizeof(next_info.layerName) - 1);
  passed &= Expect(
    request.createApiLayerInstance(&instance_info, &layer_info, &instance) == XR_SUCCESS,
    "layer chains instance creation");
  passed &= Expect(instance == g_instance && g_create_instance_calls == 1, "downstream instance is preserved");

  PFN_xrVoidFunction function = nullptr;
  passed &= Expect(
    request.getInstanceProcAddr(instance, "xrCreateSession", &function) == XR_SUCCESS && function != nullptr,
    "layer exposes intercepted xrCreateSession");
  const auto create_session = reinterpret_cast<PFN_xrCreateSession>(function);
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp_adapter;
  ComPtr<ID3D12Device> d3d12_device;
  ComPtr<ID3D12CommandQueue> d3d12_queue;
  passed &= Expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "creates DXGI factory for D3D12 WARP test");
  passed &= Expect(factory != nullptr && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))), "finds WARP adapter");
  passed &= Expect(warp_adapter != nullptr && SUCCEEDED(D3D12CreateDevice(
    warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device))), "creates D3D12 WARP device");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  passed &= Expect(d3d12_device != nullptr && SUCCEEDED(d3d12_device->CreateCommandQueue(
    &queue_desc, IID_PPV_ARGS(&d3d12_queue))), "creates D3D12 direct command queue");
  XrGraphicsBindingD3D12KHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  graphics_binding.device = d3d12_device.Get();
  graphics_binding.queue = d3d12_queue.Get();
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &graphics_binding;
  session_info.systemId = 1;
  XrSession session = XR_NULL_HANDLE;
  passed &= Expect(create_session(instance, &session_info, &session) == XR_SUCCESS, "D3D12 session creation passes through");
  passed &= Expect(session == g_session && g_create_session_calls == 1, "downstream session is preserved");

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrEndFrame", &function);
  const auto end_frame = reinterpret_cast<PFN_xrEndFrame>(function);
  XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
  frame.displayTime = 123;
  frame.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  passed &= Expect(end_frame(session, &frame) == XR_SUCCESS, "frame submission passes through");
  passed &= Expect(g_end_frame_calls == 1 && g_forwarded_frame == &frame, "no-companion frame is forwarded unchanged");

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrDestroySession", &function);
  passed &= Expect(
    reinterpret_cast<PFN_xrDestroySession>(function)(session) == XR_SUCCESS && g_destroy_session_calls == 1,
    "session destruction passes through");

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrDestroyInstance", &function);
  passed &= Expect(
    reinterpret_cast<PFN_xrDestroyInstance>(function)(instance) == XR_SUCCESS && g_destroy_instance_calls == 1,
    "instance destruction passes through");

  FreeLibrary(layer);
  if (passed) std::cout << "OpenXR API-layer negotiation and pass-through tests passed." << std::endl;
  return passed ? 0 : 1;
}
