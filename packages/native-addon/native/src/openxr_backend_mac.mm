#include "openxr_backend.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace vrbridge {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kMinimumCurvature = 0.0001f;

struct MacOpenXRState {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system_id = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace stage_space = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageMetalKHR> swapchain_images;
  PFN_xrGetMetalGraphicsRequirementsKHR get_metal_requirements = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  int64_t swapchain_format = 0;
  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  bool initialized = false;
  bool session_running = false;
  bool overlay_session = false;
  bool cylinder_enabled = false;
  bool visible = true;
  bool logged_first_frame = false;
  float size_meters = 1.0f;
  float curvature = 0.0f;
  OverlayPlacement placement;
};

MacOpenXRState g_state;

template <typename T, XrStructureType Type>
T MakeXrStruct() {
  T value{};
  value.type = Type;
  value.next = nullptr;
  return value;
}

void SetError(std::string* error_message, const std::string& message) {
  if (error_message != nullptr) {
    *error_message = message;
  }
}

std::string ResultString(XrResult result) {
  if (g_state.instance != XR_NULL_HANDLE) {
    char result_string[XR_MAX_RESULT_STRING_SIZE] = {};
    if (XR_SUCCEEDED(xrResultToString(g_state.instance, result, result_string))) {
      return result_string;
    }
  }
  return std::to_string(static_cast<int32_t>(result));
}

bool CheckXr(XrResult result, const char* context, std::string* error_message) {
  if (XR_SUCCEEDED(result)) {
    return true;
  }
  SetError(error_message, std::string(context) + ": " + ResultString(result));
  return false;
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
  return std::any_of(extensions.begin(), extensions.end(), [name](const XrExtensionProperties& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

const char* SessionStateName(XrSessionState state) {
  switch (state) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss-pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
  }
}

XrPosef ToXrPose(const OverlayPlacement& placement) {
  XrPosef pose{};
  pose.orientation = {placement.rotation.x, placement.rotation.y, placement.rotation.z, placement.rotation.w};
  pose.position = {placement.position.x, placement.position.y, placement.position.z};
  return pose;
}

XrSpace LayerSpace() {
  if (g_state.placement.mode == OverlayPlacementMode::kHead) {
    return g_state.view_space;
  }
  return g_state.stage_space != XR_NULL_HANDLE ? g_state.stage_space : g_state.local_space;
}

float HeightMeters() {
  if (g_state.frame_width == 0) {
    return g_state.size_meters;
  }
  return g_state.size_meters * static_cast<float>(g_state.frame_height) / static_cast<float>(g_state.frame_width);
}

void DestroySwapchain() {
  if (g_state.swapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(g_state.swapchain);
    g_state.swapchain = XR_NULL_HANDLE;
  }
  g_state.swapchain_images.clear();
  g_state.frame_width = 0;
  g_state.frame_height = 0;
}

void ResetState() {
  DestroySwapchain();
  if (g_state.stage_space != XR_NULL_HANDLE) xrDestroySpace(g_state.stage_space);
  if (g_state.view_space != XR_NULL_HANDLE) xrDestroySpace(g_state.view_space);
  if (g_state.local_space != XR_NULL_HANDLE) xrDestroySpace(g_state.local_space);
  if (g_state.session != XR_NULL_HANDLE) xrDestroySession(g_state.session);
  if (g_state.instance != XR_NULL_HANDLE) xrDestroyInstance(g_state.instance);
  g_state = MacOpenXRState{};
}

bool CreateInstance(std::string* error_message) {
  uint32_t extension_count = 0;
  if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr),
      "Failed to enumerate OpenXR extensions", error_message)) {
    return false;
  }

  std::vector<XrExtensionProperties> extensions(extension_count);
  for (auto& extension : extensions) {
    extension.type = XR_TYPE_EXTENSION_PROPERTIES;
  }
  if (!CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data()),
      "Failed to enumerate OpenXR extensions", error_message)) {
    return false;
  }

  if (!HasExtension(extensions, XR_KHR_METAL_ENABLE_EXTENSION_NAME)) {
    SetError(error_message, "OpenXR runtime does not expose XR_KHR_metal_enable.");
    return false;
  }

  g_state.overlay_session = HasExtension(extensions, XR_EXTX_OVERLAY_EXTENSION_NAME);
  g_state.cylinder_enabled = HasExtension(extensions, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
  if (g_state.curvature > 0.0f && !g_state.cylinder_enabled) {
    SetError(error_message, "OpenXR runtime does not expose XR_KHR_composition_layer_cylinder.");
    return false;
  }

  std::vector<const char*> enabled_extensions{XR_KHR_METAL_ENABLE_EXTENSION_NAME};
  if (g_state.overlay_session) enabled_extensions.push_back(XR_EXTX_OVERLAY_EXTENSION_NAME);
  if (g_state.cylinder_enabled) enabled_extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);

  auto create_info = MakeXrStruct<XrInstanceCreateInfo, XR_TYPE_INSTANCE_CREATE_INFO>();
  std::strncpy(create_info.applicationInfo.applicationName, "electron-vr", XR_MAX_APPLICATION_NAME_SIZE - 1);
  create_info.applicationInfo.applicationVersion = 1;
  std::strncpy(create_info.applicationInfo.engineName, "electron-vr", XR_MAX_ENGINE_NAME_SIZE - 1);
  create_info.applicationInfo.engineVersion = 1;
  create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
  create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
  create_info.enabledExtensionNames = enabled_extensions.data();
  return CheckXr(xrCreateInstance(&create_info, &g_state.instance), "Failed to create OpenXR instance", error_message);
}

bool SelectSwapchainFormat(std::string* error_message) {
  uint32_t count = 0;
  if (!CheckXr(xrEnumerateSwapchainFormats(g_state.session, 0, &count, nullptr),
      "Failed to enumerate Metal swapchain formats", error_message)) {
    return false;
  }
  std::vector<int64_t> formats(count);
  if (!CheckXr(xrEnumerateSwapchainFormats(g_state.session, count, &count, formats.data()),
      "Failed to enumerate Metal swapchain formats", error_message)) {
    return false;
  }

  const int64_t preferred[] = {
    MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatBGRA8Unorm,
    MTLPixelFormatRGBA8Unorm_sRGB,
    MTLPixelFormatRGBA8Unorm,
  };
  for (int64_t format : preferred) {
    if (std::find(formats.begin(), formats.end(), format) != formats.end()) {
      g_state.swapchain_format = format;
      return true;
    }
  }
  SetError(error_message, "OpenXR runtime did not report a compatible BGRA/RGBA Metal swapchain format.");
  return false;
}

bool SelectBlendMode(std::string* error_message) {
  uint32_t count = 0;
  if (!CheckXr(xrEnumerateEnvironmentBlendModes(g_state.instance, g_state.system_id,
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
      "Failed to enumerate OpenXR blend modes", error_message)) {
    return false;
  }
  std::vector<XrEnvironmentBlendMode> modes(count);
  if (!CheckXr(xrEnumerateEnvironmentBlendModes(g_state.instance, g_state.system_id,
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()),
      "Failed to enumerate OpenXR blend modes", error_message)) {
    return false;
  }
  const XrEnvironmentBlendMode preferred[] = {
    XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND,
    XR_ENVIRONMENT_BLEND_MODE_ADDITIVE,
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
  };
  for (XrEnvironmentBlendMode mode : preferred) {
    if (std::find(modes.begin(), modes.end(), mode) != modes.end()) {
      g_state.blend_mode = mode;
      return true;
    }
  }
  SetError(error_message, "OpenXR runtime reported no environment blend modes.");
  return false;
}

bool CreateReferenceSpace(XrReferenceSpaceType type, XrSpace* space, bool required, std::string* error_message) {
  auto create_info = MakeXrStruct<XrReferenceSpaceCreateInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO>();
  create_info.referenceSpaceType = type;
  create_info.poseInReferenceSpace.orientation.w = 1.0f;
  const XrResult result = xrCreateReferenceSpace(g_state.session, &create_info, space);
  if (XR_SUCCEEDED(result) || !required) {
    if (XR_FAILED(result)) *space = XR_NULL_HANDLE;
    return true;
  }
  return CheckXr(result, "Failed to create OpenXR reference space", error_message);
}

bool CreateSession(std::string* error_message) {
  auto system_info = MakeXrStruct<XrSystemGetInfo, XR_TYPE_SYSTEM_GET_INFO>();
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!CheckXr(xrGetSystem(g_state.instance, &system_info, &g_state.system_id),
      "Failed to acquire OpenXR system", error_message)) {
    return false;
  }

  if (!CheckXr(xrGetInstanceProcAddr(g_state.instance, "xrGetMetalGraphicsRequirementsKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&g_state.get_metal_requirements)),
      "Failed to load xrGetMetalGraphicsRequirementsKHR", error_message) || g_state.get_metal_requirements == nullptr) {
    return false;
  }

  auto requirements = MakeXrStruct<XrGraphicsRequirementsMetalKHR, XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR>();
  if (!CheckXr(g_state.get_metal_requirements(g_state.instance, g_state.system_id, &requirements),
      "Failed to query OpenXR Metal requirements", error_message)) {
    return false;
  }
  g_state.device = (__bridge id<MTLDevice>)requirements.metalDevice;
  g_state.command_queue = [g_state.device newCommandQueue];
  if (g_state.device == nil || g_state.command_queue == nil) {
    SetError(error_message, "OpenXR runtime did not provide a usable Metal device and command queue.");
    return false;
  }

  auto binding = MakeXrStruct<XrGraphicsBindingMetalKHR, XR_TYPE_GRAPHICS_BINDING_METAL_KHR>();
  binding.commandQueue = (__bridge void*)g_state.command_queue;
  auto overlay_info = MakeXrStruct<XrSessionCreateInfoOverlayEXTX, XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX>();
  overlay_info.sessionLayersPlacement = UINT32_MAX;
  if (g_state.overlay_session) binding.next = &overlay_info;

  auto session_info = MakeXrStruct<XrSessionCreateInfo, XR_TYPE_SESSION_CREATE_INFO>();
  session_info.next = &binding;
  session_info.systemId = g_state.system_id;
  if (!CheckXr(xrCreateSession(g_state.instance, &session_info, &g_state.session),
      "Failed to create OpenXR session", error_message)) {
    return false;
  }

  return CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, &g_state.local_space, true, error_message) &&
         CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, &g_state.view_space, true, error_message) &&
         CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, &g_state.stage_space, false, error_message) &&
         SelectSwapchainFormat(error_message) && SelectBlendMode(error_message);
}

bool CreateSwapchain(uint32_t width, uint32_t height, std::string* error_message) {
  DestroySwapchain();
  auto create_info = MakeXrStruct<XrSwapchainCreateInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO>();
  create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  create_info.format = g_state.swapchain_format;
  create_info.sampleCount = 1;
  create_info.width = width;
  create_info.height = height;
  create_info.faceCount = 1;
  create_info.arraySize = 1;
  create_info.mipCount = 1;
  if (!CheckXr(xrCreateSwapchain(g_state.session, &create_info, &g_state.swapchain),
      "Failed to create OpenXR Metal swapchain", error_message)) {
    return false;
  }

  uint32_t count = 0;
  if (!CheckXr(xrEnumerateSwapchainImages(g_state.swapchain, 0, &count, nullptr),
      "Failed to enumerate Metal swapchain images", error_message)) {
    return false;
  }
  g_state.swapchain_images.resize(count);
  for (auto& image : g_state.swapchain_images) {
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
    image.next = nullptr;
    image.texture = nullptr;
  }
  if (!CheckXr(xrEnumerateSwapchainImages(g_state.swapchain, count, &count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(g_state.swapchain_images.data())),
      "Failed to enumerate Metal swapchain images", error_message)) {
    return false;
  }
  g_state.frame_width = width;
  g_state.frame_height = height;
  return true;
}

bool BeginSession(std::string* error_message) {
  if (g_state.session_running) return true;
  if (g_state.session_state != XR_SESSION_STATE_READY) return true;
  auto begin_info = MakeXrStruct<XrSessionBeginInfo, XR_TYPE_SESSION_BEGIN_INFO>();
  begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  if (!CheckXr(xrBeginSession(g_state.session, &begin_info), "Failed to begin OpenXR session", error_message)) {
    return false;
  }
  g_state.session_running = true;
  return true;
}

bool PollEvents(std::string* error_message) {
  auto event = MakeXrStruct<XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER>();
  while (true) {
    const XrResult result = xrPollEvent(g_state.instance, &event);
    if (result == XR_EVENT_UNAVAILABLE) return true;
    if (XR_FAILED(result)) return CheckXr(result, "Failed to poll OpenXR event", error_message);
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      g_state.session_state = changed->state;
      if (changed->state == XR_SESSION_STATE_READY) {
        if (!BeginSession(error_message)) return false;
      } else if (changed->state == XR_SESSION_STATE_STOPPING && g_state.session_running) {
        if (!CheckXr(xrEndSession(g_state.session), "Failed to end OpenXR session", error_message)) return false;
        g_state.session_running = false;
      } else if (changed->state == XR_SESSION_STATE_EXITING || changed->state == XR_SESSION_STATE_LOSS_PENDING) {
        SetError(error_message, "OpenXR session exited or was lost.");
        return false;
      }
    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      SetError(error_message, "OpenXR instance loss is pending.");
      return false;
    }
    event = MakeXrStruct<XrEventDataBuffer, XR_TYPE_EVENT_DATA_BUFFER>();
  }
}

bool CopyIOSurfaceToSwapchain(const MacTextureInfo& texture_info, id<MTLTexture> destination, std::string* error_message) {
  IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(texture_info.io_surface);
  const size_t surface_width = IOSurfaceGetWidth(surface);
  const size_t surface_height = IOSurfaceGetHeight(surface);
  if (surface_width < texture_info.width || surface_height < texture_info.height) {
    SetError(error_message, "Electron IOSurface dimensions are smaller than its coded size.");
    return false;
  }

  const MTLPixelFormat source_format = texture_info.pixel_format == "rgba"
    ? MTLPixelFormatRGBA8Unorm
    : MTLPixelFormatBGRA8Unorm;
  MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
    texture2DDescriptorWithPixelFormat:source_format
    width:texture_info.width
    height:texture_info.height
    mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> source = [g_state.device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
  if (source == nil || destination == nil) {
    SetError(error_message, "Failed to import Electron IOSurface as a Metal texture.");
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [g_state.command_queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  if (command_buffer == nil || blit == nil) {
    SetError(error_message, "Failed to create Metal blit commands for the OpenXR frame.");
    return false;
  }
  const MTLSize size = MTLSizeMake(texture_info.width, texture_info.height, 1);
  [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
    sourceSize:size toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status == MTLCommandBufferStatusError) {
    const char* description = command_buffer.error.localizedDescription.UTF8String;
    SetError(error_message, std::string("Metal IOSurface copy failed: ") + (description == nullptr ? "unknown error" : description));
    return false;
  }
  return true;
}

void ConfigureSubImage(XrSwapchainSubImage* sub_image) {
  sub_image->swapchain = g_state.swapchain;
  sub_image->imageRect.offset = {0, 0};
  sub_image->imageRect.extent = {static_cast<int32_t>(g_state.frame_width), static_cast<int32_t>(g_state.frame_height)};
}

bool SubmitFrame(const MacTextureInfo& texture_info, std::string* error_message) {
  if (!PollEvents(error_message) || !BeginSession(error_message)) return false;
  if (!g_state.session_running) return true;
  if (g_state.swapchain == XR_NULL_HANDLE || g_state.frame_width != texture_info.width || g_state.frame_height != texture_info.height) {
    if (!CreateSwapchain(texture_info.width, texture_info.height, error_message)) return false;
  }

  auto wait_info = MakeXrStruct<XrFrameWaitInfo, XR_TYPE_FRAME_WAIT_INFO>();
  auto frame_state = MakeXrStruct<XrFrameState, XR_TYPE_FRAME_STATE>();
  if (!CheckXr(xrWaitFrame(g_state.session, &wait_info, &frame_state), "Failed to wait for OpenXR frame", error_message)) return false;
  auto begin_info = MakeXrStruct<XrFrameBeginInfo, XR_TYPE_FRAME_BEGIN_INFO>();
  if (!CheckXr(xrBeginFrame(g_state.session, &begin_info), "Failed to begin OpenXR frame", error_message)) return false;

  std::vector<XrCompositionLayerBaseHeader*> layers;
  auto quad = MakeXrStruct<XrCompositionLayerQuad, XR_TYPE_COMPOSITION_LAYER_QUAD>();
  auto cylinder = MakeXrStruct<XrCompositionLayerCylinderKHR, XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR>();
  bool image_acquired = false;

  if (frame_state.shouldRender && g_state.visible) {
    uint32_t image_index = 0;
    auto acquire_info = MakeXrStruct<XrSwapchainImageAcquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO>();
    XrResult result = xrAcquireSwapchainImage(g_state.swapchain, &acquire_info, &image_index);
    if (XR_SUCCEEDED(result)) {
      image_acquired = true;
      auto image_wait = MakeXrStruct<XrSwapchainImageWaitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO>();
      image_wait.timeout = XR_INFINITE_DURATION;
      result = xrWaitSwapchainImage(g_state.swapchain, &image_wait);
    }
    if (XR_SUCCEEDED(result)) {
      id<MTLTexture> destination = (__bridge id<MTLTexture>)g_state.swapchain_images[image_index].texture;
      if (!CopyIOSurfaceToSwapchain(texture_info, destination, error_message)) result = XR_ERROR_RUNTIME_FAILURE;
    }
    if (image_acquired) {
      auto release_info = MakeXrStruct<XrSwapchainImageReleaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO>();
      const XrResult release_result = xrReleaseSwapchainImage(g_state.swapchain, &release_info);
      if (XR_SUCCEEDED(result)) result = release_result;
    }
    if (XR_FAILED(result)) {
      auto end_info = MakeXrStruct<XrFrameEndInfo, XR_TYPE_FRAME_END_INFO>();
      end_info.displayTime = frame_state.predictedDisplayTime;
      end_info.environmentBlendMode = g_state.blend_mode;
      xrEndFrame(g_state.session, &end_info);
      if (error_message != nullptr && error_message->empty()) SetError(error_message, "Failed to populate OpenXR Metal swapchain image: " + ResultString(result));
      return false;
    }

    const XrPosef pose = ToXrPose(g_state.placement);
    if (g_state.cylinder_enabled && g_state.curvature >= kMinimumCurvature) {
      const float angle = std::min(kTwoPi * g_state.curvature, std::nextafter(kTwoPi, 0.0f));
      cylinder.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      cylinder.space = LayerSpace();
      cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      ConfigureSubImage(&cylinder.subImage);
      cylinder.pose = pose;
      cylinder.radius = g_state.size_meters / angle;
      cylinder.centralAngle = angle;
      cylinder.aspectRatio = static_cast<float>(g_state.frame_width) / static_cast<float>(g_state.frame_height);
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&cylinder));
    } else {
      quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      quad.space = LayerSpace();
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      ConfigureSubImage(&quad.subImage);
      quad.pose = pose;
      quad.size = {g_state.size_meters, HeightMeters()};
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }
  }

  auto end_info = MakeXrStruct<XrFrameEndInfo, XR_TYPE_FRAME_END_INFO>();
  end_info.displayTime = frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = g_state.blend_mode;
  end_info.layerCount = static_cast<uint32_t>(layers.size());
  end_info.layers = layers.empty() ? nullptr : layers.data();
  if (!CheckXr(xrEndFrame(g_state.session, &end_info), "Failed to end OpenXR frame", error_message)) return false;

  if (!g_state.logged_first_frame && !layers.empty()) {
    g_state.logged_first_frame = true;
    std::cout << "OpenXR submitted first macOS Metal "
              << (g_state.curvature >= kMinimumCurvature ? "cylinder" : "quad")
              << " layer: frame=" << g_state.frame_width << "x" << g_state.frame_height
              << ", session=" << (g_state.overlay_session ? "overlay" : "standard") << std::endl;
  }
  return true;
}

}  // namespace

bool InitializeOpenXRBackend(const InitializeOptions& options, std::string* error_message) {
  ShutdownOpenXRBackend();
  if (options.name.empty()) {
    SetError(error_message, "OpenXR backend requires a non-empty name.");
    return false;
  }
  g_state.visible = options.visible;
  g_state.size_meters = options.size_meters;
  g_state.curvature = options.curvature;
  g_state.placement = options.placement;
  if (!CreateInstance(error_message) || !CreateSession(error_message) ||
      !CreateSwapchain(options.width, options.height, error_message)) {
    ResetState();
    return false;
  }
  g_state.initialized = true;
  if (error_message != nullptr) error_message->clear();
  std::cout << "OpenXR initialized macOS Metal session mode: "
            << (g_state.overlay_session ? "overlay" : "standard") << std::endl;
  return true;
}

bool SubmitOpenXRFrameMac(const MacTextureInfo& texture_info, std::string* error_message) {
  if (!g_state.initialized) {
    SetError(error_message, "OpenXR backend is not initialized.");
    return false;
  }
  if (texture_info.io_surface == 0 || texture_info.width == 0 || texture_info.height == 0) {
    SetError(error_message, "OpenXR macOS submission requires a valid IOSurface and dimensions.");
    return false;
  }
  const bool result = SubmitFrame(texture_info, error_message);
  if (result && error_message != nullptr) error_message->clear();
  return result;
}

bool SubmitOpenXRFrameWindows(uint64_t, std::string* error_message) {
  SetError(error_message, "Windows frame submission is unavailable in a macOS build.");
  return false;
}

bool SubmitOpenXRFrameLinux(const LinuxTextureInfo&, std::string* error_message) {
  SetError(error_message, "Linux frame submission is unavailable in a macOS build.");
  return false;
}

bool SetOpenXRPlacement(const OverlayPlacement& placement, std::string* error_message) {
  if (!g_state.initialized) { SetError(error_message, "OpenXR backend is not initialized."); return false; }
  g_state.placement = placement;
  g_state.logged_first_frame = false;
  if (error_message != nullptr) error_message->clear();
  return true;
}

bool SetOpenXRVisible(bool visible, std::string* error_message) {
  if (!g_state.initialized) { SetError(error_message, "OpenXR backend is not initialized."); return false; }
  g_state.visible = visible;
  if (error_message != nullptr) error_message->clear();
  return true;
}

bool SetOpenXRSizeMeters(float size_meters, std::string* error_message) {
  if (!g_state.initialized || !std::isfinite(size_meters) || size_meters <= 0.0f) {
    SetError(error_message, "OpenXR size must be greater than zero.");
    return false;
  }
  g_state.size_meters = size_meters;
  if (error_message != nullptr) error_message->clear();
  return true;
}

bool SetOpenXRCurvature(float curvature, std::string* error_message) {
  if (!g_state.initialized || !std::isfinite(curvature) || curvature < 0.0f || curvature > 1.0f) {
    SetError(error_message, "OpenXR curvature must be between 0 and 1.");
    return false;
  }
  if (curvature > 0.0f && !g_state.cylinder_enabled) {
    SetError(error_message, "OpenXR runtime does not expose XR_KHR_composition_layer_cylinder.");
    return false;
  }
  g_state.curvature = curvature;
  if (error_message != nullptr) error_message->clear();
  return true;
}

void PopulateOpenXRRuntimeInfo(RuntimeInfo* runtime_info) {
  if (runtime_info == nullptr) return;
  runtime_info->openxr_session_state = SessionStateName(g_state.session_state);
  runtime_info->openxr_session_running = g_state.session_running;
}

void ShutdownOpenXRBackend() {
  ResetState();
}

}  // namespace vrbridge
