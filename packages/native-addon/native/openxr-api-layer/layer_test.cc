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

#include "../openxr_api_layer_protocol.h"
#include "swapchain_format.h"
#include "../app_anchor_windows.h"

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
XrPosef g_application_pose{{0,0,0,1},{0.3f,1.2f,-0.4f}};
bool g_anchor_valid=true;

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
XrResult XRAPI_CALL StubGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
  std::strcpy(properties->runtimeName,"SteamVR test runtime");
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
  *count = 3;
  if (capacity >= 3 && spaces != nullptr) {
    spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
  *space=reinterpret_cast<XrSpace>(0x3000+static_cast<uintptr_t>(info->referenceSpaceType));
  return XR_SUCCESS;
}

XrResult XRAPI_CALL StubDestroySpace(XrSpace) { return XR_SUCCESS; }
XrResult XRAPI_CALL StubLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  location->pose=g_application_pose;
  location->locationFlags=g_anchor_valid ? XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT : 0;
  return XR_SUCCESS;
}
XrResult XRAPI_CALL StubPollEvent(XrInstance, XrEventDataBuffer*) { return XR_EVENT_UNAVAILABLE; }

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
    {"xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction>(StubGetInstanceProperties)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(StubCreateSession)},
    {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(StubDestroySession)},
    {"xrEnumerateReferenceSpaces", reinterpret_cast<PFN_xrVoidFunction>(StubEnumerateReferenceSpaces)},
    {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(StubCreateReferenceSpace)},
      {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(StubLocateSpace)},
      {"xrPollEvent", reinterpret_cast<PFN_xrVoidFunction>(StubPollEvent)},
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
  using electron_vr::SelectOverlaySwapchainFormat;
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}) == DXGI_FORMAT_B8G8R8A8_UNORM,
    "prefer the exact source format when supported");
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
    {29, 91, 2, 10, 24, 40, 55, 45, 20}) == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    "negotiate Chromium BGRA into SteamVR's sRGB-only swapchain list");
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_R8G8B8A8_UNORM,
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    "negotiate the matching RGBA sRGB variant");
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) == DXGI_FORMAT_UNKNOWN,
    "reject channel-order changes without a shader conversion");
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_R8G8B8A8_UNORM, {}) == DXGI_FORMAT_UNKNOWN,
    "reject an empty format list");
  passed &= Expect(SelectOverlaySwapchainFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    {DXGI_FORMAT_B8G8R8A8_UNORM}) == DXGI_FORMAT_UNKNOWN,
    "do not silently change sRGB input into a linear destination");
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
  const std::wstring host_mapping_name = std::wstring(electron_vr::openxr_layer::kHostPresenceMappingPrefix) +
    std::to_wstring(GetCurrentProcessId());
  HANDLE host_mapping = OpenFileMappingW(
    FILE_MAP_READ, FALSE, host_mapping_name.c_str());
  passed &= Expect(host_mapping != nullptr, "compatible session publishes host presence");
  const auto* host = host_mapping == nullptr ? nullptr : static_cast<const electron_vr::openxr_layer::HostPresence*>(
    MapViewOfFile(host_mapping, FILE_MAP_READ, 0, 0, sizeof(electron_vr::openxr_layer::HostPresence)));
  passed &= Expect(host != nullptr && host->hello.process_id == GetCurrentProcessId() &&
    host->hello.graphics_api == electron_vr::openxr_layer::GraphicsApi::kD3D12 &&
    std::strcmp(host->hello.application_name, "electron-vr-ci") == 0,
    "host presence identifies the OpenXR application and graphics API");
  if (host != nullptr) UnmapViewOfFile(host);
  if (host_mapping != nullptr) CloseHandle(host_mapping);

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrEndFrame", &function);
  const auto end_frame = reinterpret_cast<PFN_xrEndFrame>(function);
  XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
  frame.displayTime = 123;
  frame.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  passed &= Expect(end_frame(session, &frame) == XR_SUCCESS, "frame submission passes through");
  passed &= Expect(g_end_frame_calls == 1 && g_forwarded_frame == &frame, "no-companion frame is forwarded unchanged");
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  projection.space=reinterpret_cast<XrSpace>(0x9999);
  const XrCompositionLayerBaseHeader* projection_layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  frame.layerCount=1; frame.layers=projection_layers;
  passed &= Expect(end_frame(session,&frame)==XR_SUCCESS && g_forwarded_frame==&frame,
    "anchor observation leaves game composition untouched without an OpenXR overlay");
  electron_vr::anchor::Sample observation;
  passed &= Expect(electron_vr::anchor::ReadProcess(GetCurrentProcessId(),&observation) &&
    std::abs(observation.stage_from_application.m[0][3]-0.3f)<0.0001f,
    "publishes application projection-space pose through the read-only bridge");
  const auto first_session=observation.session;
  g_application_pose.position.x=2.f;
  passed &= Expect(end_frame(session,&frame)==XR_SUCCESS &&
    electron_vr::anchor::ReadProcess(GetCurrentProcessId(),&observation) && observation.session==first_session &&
    std::abs(observation.stage_from_application.m[0][3]-2.f)<0.0001f,
    "recenter pose changes reach OpenVR without changing the anchor source identity");
  g_anchor_valid=false;
  end_frame(session,&frame);
  passed &= Expect(!electron_vr::anchor::ReadProcess(GetCurrentProcessId(),&observation),
    "tracking loss invalidates the shared pose instead of publishing an origin jump");

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrDestroySession", &function);
  passed &= Expect(
    reinterpret_cast<PFN_xrDestroySession>(function)(session) == XR_SUCCESS && g_destroy_session_calls == 1,
    "session destruction passes through");
  host_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, host_mapping_name.c_str());
  passed &= Expect(host_mapping == nullptr, "session destruction clears host presence");
  if (host_mapping != nullptr) CloseHandle(host_mapping);

  function = nullptr;
  request.getInstanceProcAddr(instance, "xrDestroyInstance", &function);
  passed &= Expect(
    reinterpret_cast<PFN_xrDestroyInstance>(function)(instance) == XR_SUCCESS && g_destroy_instance_calls == 1,
    "instance destruction passes through");

  FreeLibrary(layer);
  if (passed) std::cout << "OpenXR API-layer negotiation and pass-through tests passed." << std::endl;
  return passed ? 0 : 1;
}
