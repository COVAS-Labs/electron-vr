#define XR_NO_PROTOTYPES

#include <dlfcn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstring>
#include <iostream>
#include <string>

namespace {
constexpr char kLayerName[] = "XR_APILAYER_ELECTRON_VR_overlay";
XrInstance g_instance = reinterpret_cast<XrInstance>(0x1001);
XrSession g_session = reinterpret_cast<XrSession>(0x2001);
const XrFrameEndInfo* g_frame = nullptr;

#define STUB(name, signature) XrResult XRAPI_CALL name signature { return XR_SUCCESS; }
STUB(DestroyInstance, (XrInstance))
STUB(DestroySession, (XrSession))
STUB(DestroySpace, (XrSpace))
STUB(DestroySwapchain, (XrSwapchain))
STUB(CreateReferenceSpace, (XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*))
STUB(CreateSwapchain, (XrSession, const XrSwapchainCreateInfo*, XrSwapchain*))
STUB(AcquireImage, (XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*))
STUB(WaitImage, (XrSwapchain, const XrSwapchainImageWaitInfo*))
STUB(ReleaseImage, (XrSwapchain, const XrSwapchainImageReleaseInfo*))

XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) { *session = g_session; return XR_SUCCESS; }
XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) { properties->graphicsProperties.maxLayerCount = 16; return XR_SUCCESS; }
XrResult XRAPI_CALL EnumerateSpaces(XrSession, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* types) { *count = 0; (void)capacity; (void)types; return XR_SUCCESS; }
XrResult XRAPI_CALL EnumerateFormats(XrSession, uint32_t, uint32_t* count, int64_t*) { *count = 0; return XR_SUCCESS; }
XrResult XRAPI_CALL EnumerateImages(XrSwapchain, uint32_t, uint32_t* count, XrSwapchainImageBaseHeader*) { *count = 0; return XR_SUCCESS; }
XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* frame) { g_frame = frame; return XR_SUCCESS; }

XrResult XRAPI_CALL Gipa(XrInstance, const char* name, PFN_xrVoidFunction* output) {
  struct Entry { const char* name; PFN_xrVoidFunction function; };
  const Entry entries[] = {
    {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(DestroyInstance)}, {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(GetSystemProperties)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(CreateSession)}, {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(DestroySession)},
    {"xrEnumerateReferenceSpaces", reinterpret_cast<PFN_xrVoidFunction>(EnumerateSpaces)}, {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(CreateReferenceSpace)},
    {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(DestroySpace)}, {"xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction>(EnumerateFormats)},
    {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(CreateSwapchain)}, {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(DestroySwapchain)},
    {"xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction>(EnumerateImages)}, {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(AcquireImage)},
    {"xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(WaitImage)}, {"xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(ReleaseImage)},
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(EndFrame)}};
  for (const auto& entry : entries) if (!std::strcmp(name, entry.name)) { *output = entry.function; return XR_SUCCESS; }
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo* layer, XrInstance* instance) {
  if (layer->nextInfo != nullptr) return XR_ERROR_VALIDATION_FAILURE; *instance = g_instance; return XR_SUCCESS;
}
bool Expect(bool value, const char* message) { if (!value) std::cerr << "FAILED: " << message << '\n'; return value; }
}  // namespace

int main() {
  bool passed = true;
  void* library = dlopen("./libelectron_vr_openxr_layer.so", RTLD_NOW | RTLD_LOCAL);
  const char* load_error = library == nullptr ? dlerror() : nullptr;
  passed &= Expect(library != nullptr, load_error ? load_error : "loads Linux API layer"); if (!library) return 1;
  auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(dlsym(library, "xrNegotiateLoaderApiLayerInterface"));
  XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, 1, sizeof(loader), 1, 1, XR_MAKE_VERSION(1,0,0), XR_CURRENT_API_VERSION};
  XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, 1, sizeof(request)};
  passed &= Expect(negotiate && negotiate(&loader, kLayerName, &request) == XR_SUCCESS, "negotiates Linux layer");
  XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, 1, sizeof(next)};
  std::strncpy(next.layerName, kLayerName, sizeof(next.layerName)-1); next.nextGetInstanceProcAddr = Gipa; next.nextCreateApiLayerInstance = CreateInstance;
  XrApiLayerCreateInfo layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO, 1, sizeof(layer)}; layer.nextInfo = &next;
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO}; std::strncpy(create.applicationInfo.applicationName, "linux-ci", XR_MAX_APPLICATION_NAME_SIZE-1);
  XrInstance instance = XR_NULL_HANDLE; passed &= Expect(request.createApiLayerInstance(&create, &layer, &instance) == XR_SUCCESS, "chains instance");
  PFN_xrVoidFunction function = nullptr; request.getInstanceProcAddr(instance, "xrCreateSession", &function);
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO}; session_info.systemId = 1; XrSession session = XR_NULL_HANDLE;
  passed &= Expect(reinterpret_cast<PFN_xrCreateSession>(function)(instance, &session_info, &session) == XR_SUCCESS, "unsupported binding passes through");
  request.getInstanceProcAddr(instance, "xrEndFrame", &function); XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
  passed &= Expect(reinterpret_cast<PFN_xrEndFrame>(function)(session, &frame) == XR_SUCCESS && g_frame == &frame, "no companion forwards frame unchanged");
  request.getInstanceProcAddr(instance, "xrDestroySession", &function); reinterpret_cast<PFN_xrDestroySession>(function)(session);
  request.getInstanceProcAddr(instance, "xrDestroyInstance", &function); reinterpret_cast<PFN_xrDestroyInstance>(function)(instance);
  dlclose(library); if (passed) std::cout << "Linux OpenXR API-layer tests passed.\n"; return passed ? 0 : 1;
}
