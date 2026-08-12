#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../openxr_api_layer_protocol_linux.h"

namespace {
using namespace electron_vr::openxr_layer_linux;

constexpr char kLayerName[] = "XR_APILAYER_ELECTRON_VR_overlay";

struct Dispatch {
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
  PFN_xrGetInstanceProcAddr next_gipa = nullptr;
  Dispatch dispatch;
  std::string application_name;
};

struct SessionState {
  XrSession session = XR_NULL_HANDLE;
  std::shared_ptr<InstanceState> instance;
  Display* display = nullptr;
  GLXFBConfig fb_config = nullptr;
  GLXContext host_context = nullptr;
  GLXContext context = nullptr;
  GLXPbuffer pbuffer = 0;
  EGLDisplay egl_display = EGL_NO_DISPLAY;
  PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;
  PFNGLGENFRAMEBUFFERSPROC gen_framebuffers = nullptr;
  PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer = nullptr;
  PFNGLBLITFRAMEBUFFERPROC blit_framebuffer = nullptr;
  GLuint source_texture = 0;
  GLuint read_framebuffer = 0;
  GLuint draw_framebuffer = 0;
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace stage_space = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLKHR> images;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t format = 0;
  uint32_t max_layers = 0;
  bool compatible = false;
  bool image_acquired = false;
  bool image_waited = false;
  uint32_t image_index = 0;
  uint64_t rendered_generation = 0;
};

struct FrameState {
  OverlaySnapshot snapshot{};
  int fd = -1;
};

std::mutex g_mutex;
std::unordered_map<XrInstance, std::shared_ptr<InstanceState>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::mutex g_frame_mutex;
FrameState g_frame;
std::atomic<bool> g_stop{true};
std::thread g_socket_thread;
std::mutex g_socket_mutex;
int g_socket = -1;

std::string SocketPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  return runtime == nullptr ? std::string() : std::string(runtime) + "/electron-vr/" + kSocketName;
}

void CloseFrame() {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame.fd >= 0) close(g_frame.fd);
  g_frame = {};
  g_frame.fd = -1;
}

void SocketMain(LayerHello hello) {
  const std::string path = SocketPath();
  while (!g_stop.load() && !path.empty()) {
    const int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(socket_fd);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 || credentials.uid != geteuid()) {
      close(socket_fd);
      continue;
    }
    { std::lock_guard<std::mutex> lock(g_socket_mutex); g_socket = socket_fd; }
    if (send(socket_fd, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) {
      close(socket_fd);
      { std::lock_guard<std::mutex> lock(g_socket_mutex); g_socket = -1; }
      continue;
    }
    while (!g_stop.load()) {
      OverlaySnapshot snapshot{};
      iovec data{&snapshot, sizeof(snapshot)};
      char control[CMSG_SPACE(sizeof(int) * kMaxPlanes)] = {};
      msghdr message{};
      message.msg_iov = &data;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      const ssize_t received = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
      if (received != sizeof(snapshot) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
          snapshot.header.magic != kProtocolMagic || snapshot.header.version != kProtocolVersion ||
          snapshot.header.type != MessageType::kSnapshot || snapshot.header.byte_size != sizeof(snapshot)) break;
      int received_fd = -1;
      int received_fds[kMaxPlanes] = {-1, -1, -1, -1};
      uint32_t fd_count = 0;
      for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
          fd_count = static_cast<uint32_t>((header->cmsg_len - CMSG_LEN(0)) / sizeof(int));
          const uint32_t copy_count = std::min(fd_count, kMaxPlanes);
          if (copy_count > 0) std::memcpy(received_fds, CMSG_DATA(header), sizeof(int) * copy_count);
          received_fd = received_fds[0];
        }
      }
      if (fd_count != snapshot.header.fd_count || fd_count > 1) {
        for (int descriptor : received_fds) if (descriptor >= 0) close(descriptor);
        continue;
      }
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      if (received_fd >= 0) {
        if (g_frame.fd >= 0) close(g_frame.fd);
        g_frame.fd = received_fd;
      }
      g_frame.snapshot = snapshot;
    }
    close(socket_fd);
    { std::lock_guard<std::mutex> lock(g_socket_mutex); g_socket = -1; }
    CloseFrame();
  }
}

template <typename T>
bool Load(const std::shared_ptr<InstanceState>& state, const char* name, T* output) {
  PFN_xrVoidFunction function = nullptr;
  if (XR_FAILED(state->next_gipa(state->instance, name, &function)) || function == nullptr) return false;
  *output = reinterpret_cast<T>(function);
  return true;
}

bool LoadDispatch(const std::shared_ptr<InstanceState>& state) {
  return Load(state, "xrDestroyInstance", &state->dispatch.DestroyInstance) &&
    Load(state, "xrGetSystemProperties", &state->dispatch.GetSystemProperties) &&
    Load(state, "xrCreateSession", &state->dispatch.CreateSession) &&
    Load(state, "xrDestroySession", &state->dispatch.DestroySession) &&
    Load(state, "xrEnumerateReferenceSpaces", &state->dispatch.EnumerateReferenceSpaces) &&
    Load(state, "xrCreateReferenceSpace", &state->dispatch.CreateReferenceSpace) &&
    Load(state, "xrDestroySpace", &state->dispatch.DestroySpace) &&
    Load(state, "xrEnumerateSwapchainFormats", &state->dispatch.EnumerateSwapchainFormats) &&
    Load(state, "xrCreateSwapchain", &state->dispatch.CreateSwapchain) &&
    Load(state, "xrDestroySwapchain", &state->dispatch.DestroySwapchain) &&
    Load(state, "xrEnumerateSwapchainImages", &state->dispatch.EnumerateSwapchainImages) &&
    Load(state, "xrAcquireSwapchainImage", &state->dispatch.AcquireSwapchainImage) &&
    Load(state, "xrWaitSwapchainImage", &state->dispatch.WaitSwapchainImage) &&
    Load(state, "xrReleaseSwapchainImage", &state->dispatch.ReleaseSwapchainImage) &&
    Load(state, "xrEndFrame", &state->dispatch.EndFrame);
}

std::shared_ptr<InstanceState> Instance(XrInstance handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_instances.find(handle);
  return found == g_instances.end() ? nullptr : found->second;
}
std::shared_ptr<SessionState> Session(XrSession handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_sessions.find(handle);
  return found == g_sessions.end() ? nullptr : found->second;
}

const XrGraphicsBindingOpenGLXlibKHR* FindXlibBinding(const XrSessionCreateInfo* info) {
  for (auto* node = static_cast<const XrBaseInStructure*>(info == nullptr ? nullptr : info->next); node != nullptr; node = node->next) {
    if (node->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR) {
      return reinterpret_cast<const XrGraphicsBindingOpenGLXlibKHR*>(node);
    }
  }
  return nullptr;
}

void CreateSpaces(SessionState& state) {
  uint32_t count = 0;
  std::vector<XrReferenceSpaceType> types;
  if (XR_SUCCEEDED(state.instance->dispatch.EnumerateReferenceSpaces(state.session, 0, &count, nullptr))) {
    types.resize(count);
    state.instance->dispatch.EnumerateReferenceSpaces(state.session, count, &count, types.data());
  }
  const auto create = [&](XrReferenceSpaceType type, XrSpace* output) {
    if (std::find(types.begin(), types.end(), type) == types.end()) return;
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType = type;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(state.instance->dispatch.CreateReferenceSpace(state.session, &info, output))) *output = XR_NULL_HANDLE;
  };
  create(XR_REFERENCE_SPACE_TYPE_VIEW, &state.view_space);
  create(XR_REFERENCE_SPACE_TYPE_LOCAL, &state.local_space);
  create(XR_REFERENCE_SPACE_TYPE_STAGE, &state.stage_space);
}

template <typename T>
T GlProc(const char* name) { return reinterpret_cast<T>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name))); }

bool InitializeGraphics(SessionState& state, const XrGraphicsBindingOpenGLXlibKHR& binding) {
  state.display = binding.xDisplay;
  state.fb_config = binding.glxFBConfig;
  state.host_context = binding.glxContext;
  if (state.display == nullptr || state.fb_config == nullptr || state.host_context == nullptr) return false;
  state.context = glXCreateNewContext(state.display, state.fb_config, GLX_RGBA_TYPE, state.host_context, True);
  const int attributes[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
  state.pbuffer = glXCreatePbuffer(state.display, state.fb_config, attributes);
  if (state.context == nullptr || state.pbuffer == 0) return false;

  Display* previous_display = glXGetCurrentDisplay();
  GLXContext previous_context = glXGetCurrentContext();
  GLXDrawable previous_draw = glXGetCurrentDrawable();
  GLXDrawable previous_read = glXGetCurrentReadDrawable();
  if (!glXMakeContextCurrent(state.display, state.pbuffer, state.pbuffer, state.context)) return false;
  state.gen_framebuffers = GlProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers");
  state.delete_framebuffers = GlProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers");
  state.bind_framebuffer = GlProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
  state.framebuffer_texture = GlProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D");
  state.check_framebuffer = GlProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus");
  state.blit_framebuffer = GlProc<PFNGLBLITFRAMEBUFFERPROC>("glBlitFramebuffer");
  state.image_target_texture = GlProc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>("glEGLImageTargetTexture2DOES");
  glGenTextures(1, &state.source_texture);
  if (state.gen_framebuffers != nullptr) {
    state.gen_framebuffers(1, &state.read_framebuffer);
    state.gen_framebuffers(1, &state.draw_framebuffer);
  }
  if (previous_display != nullptr && previous_context != nullptr) {
    glXMakeContextCurrent(previous_display, previous_draw, previous_read, previous_context);
  } else {
    glXMakeContextCurrent(state.display, None, None, nullptr);
  }

  state.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(state.display));
  EGLint major = 0, minor = 0;
  if (state.egl_display == EGL_NO_DISPLAY || eglInitialize(state.egl_display, &major, &minor) != EGL_TRUE) return false;
  state.egl_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  state.egl_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  return state.gen_framebuffers && state.delete_framebuffers && state.bind_framebuffer && state.framebuffer_texture &&
    state.check_framebuffer && state.blit_framebuffer && state.image_target_texture && state.egl_create_image && state.egl_destroy_image;
}

void DestroyGraphics(SessionState& state) {
  if (state.context != nullptr && state.display != nullptr && state.pbuffer != 0) {
    Display* previous_display = glXGetCurrentDisplay();
    GLXContext previous_context = glXGetCurrentContext();
    GLXDrawable previous_draw = glXGetCurrentDrawable();
    GLXDrawable previous_read = glXGetCurrentReadDrawable();
    if (glXMakeContextCurrent(state.display, state.pbuffer, state.pbuffer, state.context)) {
      if (state.source_texture) glDeleteTextures(1, &state.source_texture);
      if (state.read_framebuffer) state.delete_framebuffers(1, &state.read_framebuffer);
      if (state.draw_framebuffer) state.delete_framebuffers(1, &state.draw_framebuffer);
      glFinish();
    }
    if (previous_display != nullptr && previous_context != nullptr) {
      glXMakeContextCurrent(previous_display, previous_draw, previous_read, previous_context);
    } else {
      glXMakeContextCurrent(state.display, None, None, nullptr);
    }
  }
  if (state.context) glXDestroyContext(state.display, state.context);
  if (state.pbuffer) glXDestroyPbuffer(state.display, state.pbuffer);
  if (state.egl_display != EGL_NO_DISPLAY) eglTerminate(state.egl_display);
}

void DestroyOverlay(SessionState& state) {
  if (state.image_acquired) {
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(state.instance->dispatch.WaitSwapchainImage(state.swapchain, &wait))) {
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      state.instance->dispatch.ReleaseSwapchainImage(state.swapchain, &release);
    }
  }
  state.image_acquired = state.image_waited = false;
  state.images.clear();
  if (state.swapchain) state.instance->dispatch.DestroySwapchain(state.swapchain);
  state.swapchain = XR_NULL_HANDLE;
  state.width = state.height = 0;
}

bool EnsureSwapchain(SessionState& state, const OverlaySnapshot& snapshot) {
  if (state.swapchain != XR_NULL_HANDLE && state.width == snapshot.width && state.height == snapshot.height) return true;
  DestroyOverlay(state);
  uint32_t count = 0;
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainFormats(state.session, 0, &count, nullptr)) || count == 0) return false;
  std::vector<int64_t> formats(count);
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainFormats(state.session, count, &count, formats.data()))) return false;
  const int64_t preferred[] = {GL_SRGB8_ALPHA8, GL_RGBA8};
  for (int64_t candidate : preferred) if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) { state.format = candidate; break; }
  if (state.format == 0) return false;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = state.format; info.sampleCount = 1; info.width = snapshot.width; info.height = snapshot.height;
  info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
  if (XR_FAILED(state.instance->dispatch.CreateSwapchain(state.session, &info, &state.swapchain))) return false;
  uint32_t images = 0;
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainImages(state.swapchain, 0, &images, nullptr)) || images == 0) return false;
  state.images.resize(images);
  for (auto& image : state.images) image = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR};
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainImages(state.swapchain, images, &images,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(state.images.data())))) return false;
  state.width = snapshot.width; state.height = snapshot.height;
  return true;
}

bool RenderFrame(SessionState& state, const OverlaySnapshot& snapshot, int fd) {
  if (fd < 0 || snapshot.plane_count != 1 || !EnsureSwapchain(state, snapshot)) return false;
  Display* previous_display = glXGetCurrentDisplay();
  GLXContext previous_context = glXGetCurrentContext();
  GLXDrawable previous_draw = glXGetCurrentDrawable();
  GLXDrawable previous_read = glXGetCurrentReadDrawable();
  if (!glXMakeContextCurrent(state.display, state.pbuffer, state.pbuffer, state.context)) return false;
  std::vector<EGLint> attributes = {
    EGL_WIDTH, static_cast<EGLint>(snapshot.width), EGL_HEIGHT, static_cast<EGLint>(snapshot.height),
    EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(snapshot.drm_format),
    EGL_DMA_BUF_PLANE0_FD_EXT, fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(snapshot.planes[0].offset),
    EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(snapshot.planes[0].stride)};
  const char* egl_extensions = eglQueryString(state.egl_display, EGL_EXTENSIONS);
  if (snapshot.modifier != UINT64_MAX && egl_extensions != nullptr &&
      std::strstr(egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr) {
    attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
    attributes.push_back(static_cast<EGLint>(snapshot.modifier & 0xffffffffu));
    attributes.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
    attributes.push_back(static_cast<EGLint>(snapshot.modifier >> 32));
  }
  attributes.push_back(EGL_NONE);
  EGLImageKHR image = state.egl_create_image(state.egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  bool copied = false;
  if (image != EGL_NO_IMAGE_KHR) {
    glBindTexture(GL_TEXTURE_2D, state.source_texture);
    state.image_target_texture(GL_TEXTURE_2D, image);
    if (!state.image_acquired) {
      XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      if (XR_SUCCEEDED(state.instance->dispatch.AcquireSwapchainImage(state.swapchain, &acquire, &state.image_index))) state.image_acquired = true;
    }
    if (state.image_acquired && !state.image_waited) {
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = 0;
      const XrResult result = state.instance->dispatch.WaitSwapchainImage(state.swapchain, &wait);
      if (result != XR_TIMEOUT_EXPIRED && XR_SUCCEEDED(result)) state.image_waited = true;
    }
    if (state.image_waited && state.image_index < state.images.size()) {
      state.bind_framebuffer(GL_READ_FRAMEBUFFER, state.read_framebuffer);
      state.framebuffer_texture(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state.source_texture, 0);
      state.bind_framebuffer(GL_DRAW_FRAMEBUFFER, state.draw_framebuffer);
      state.framebuffer_texture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state.images[state.image_index].image, 0);
      if (state.check_framebuffer(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
          state.check_framebuffer(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        state.blit_framebuffer(0, 0, snapshot.width, snapshot.height, 0, snapshot.height, snapshot.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glFinish();
        copied = true;
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      state.instance->dispatch.ReleaseSwapchainImage(state.swapchain, &release);
      state.image_acquired = state.image_waited = false;
    }
    state.egl_destroy_image(state.egl_display, image);
  }
  state.bind_framebuffer(GL_FRAMEBUFFER, 0);
  if (previous_display != nullptr && previous_context != nullptr) {
    glXMakeContextCurrent(previous_display, previous_draw, previous_read, previous_context);
  } else {
    glXMakeContextCurrent(state.display, None, None, nullptr);
  }
  return copied;
}

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction*);
XrResult XRAPI_CALL LayerCreateApiLayerInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance*);

XrResult XRAPI_CALL LayerCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
  try {
    auto owner = Instance(instance);
    if (!owner || !info || !session) return XR_ERROR_VALIDATION_FAILURE;
    const auto* binding = FindXlibBinding(info);
    const XrResult result = owner->dispatch.CreateSession(instance, info, session);
    if (XR_FAILED(result)) return result;
    auto state = std::make_shared<SessionState>(); state->session = *session; state->instance = owner;
    if (binding != nullptr) state->compatible = InitializeGraphics(*state, *binding);
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(owner->dispatch.GetSystemProperties(instance, info->systemId, &properties))) state->max_layers = properties.graphicsProperties.maxLayerCount;
    if (state->compatible) CreateSpaces(*state);
    { std::lock_guard<std::mutex> lock(g_mutex); g_sessions[*session] = state; }
    if (state->compatible && g_stop.exchange(false)) {
      LayerHello hello{}; hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHello, sizeof(hello), 0, 0};
      hello.process_id = getpid(); hello.graphics_binding = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
      std::strncpy(hello.application_name, owner->application_name.c_str(), sizeof(hello.application_name) - 1);
      g_socket_thread = std::thread(SocketMain, hello);
    }
    return result;
  } catch (...) { return XR_ERROR_RUNTIME_FAILURE; }
}

XrResult XRAPI_CALL LayerDestroySession(XrSession session) {
  try {
    auto state = Session(session); if (!state) return XR_ERROR_HANDLE_INVALID;
    { std::lock_guard<std::mutex> lock(g_mutex); g_sessions.erase(session); }
    g_stop.store(true);
    { std::lock_guard<std::mutex> lock(g_socket_mutex); if (g_socket >= 0) shutdown(g_socket, SHUT_RDWR); }
    if (g_socket_thread.joinable()) g_socket_thread.join(); CloseFrame();
    DestroyOverlay(*state);
    if (state->stage_space) state->instance->dispatch.DestroySpace(state->stage_space);
    if (state->local_space) state->instance->dispatch.DestroySpace(state->local_space);
    if (state->view_space) state->instance->dispatch.DestroySpace(state->view_space);
    DestroyGraphics(*state);
    return state->instance->dispatch.DestroySession(session);
  } catch (...) { return XR_ERROR_RUNTIME_FAILURE; }
}

XrResult XRAPI_CALL LayerEndFrame(XrSession session, const XrFrameEndInfo* info) {
  try {
    auto state = Session(session); if (!state) return XR_ERROR_HANDLE_INVALID;
    const auto forward = [&] { return state->instance->dispatch.EndFrame(session, info); };
    if (!state->compatible || !info || (info->layerCount && !info->layers) ||
        (state->max_layers && info->layerCount >= state->max_layers)) return forward();
    OverlaySnapshot snapshot{}; int fd = -1;
    { std::lock_guard<std::mutex> lock(g_frame_mutex); snapshot = g_frame.snapshot; if (g_frame.fd >= 0) fd = dup(g_frame.fd); }
    const bool copied = snapshot.visible && snapshot.generation != 0 && RenderFrame(*state, snapshot, fd);
    if (fd >= 0) close(fd);
    const XrSpace space = snapshot.placement_mode == PlacementMode::kHead ? state->view_space :
      (state->stage_space ? state->stage_space : state->local_space);
    if (!copied || space == XR_NULL_HANDLE) return forward();
    float length = 0; for (float value : snapshot.rotation) length += value * value; length = std::sqrt(length);
    if (!std::isfinite(length) || length < 0.0001f || snapshot.width == 0) return forward();
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; quad.space = space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = state->swapchain; quad.subImage.imageRect.extent = {static_cast<int32_t>(snapshot.width), static_cast<int32_t>(snapshot.height)};
    quad.pose.orientation = {snapshot.rotation[0]/length, snapshot.rotation[1]/length, snapshot.rotation[2]/length, snapshot.rotation[3]/length};
    quad.pose.position = {snapshot.position[0], snapshot.position[1], snapshot.position[2]};
    quad.size = {snapshot.size_meters, snapshot.size_meters * snapshot.height / snapshot.width};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    if (info->layerCount > 0) layers.assign(info->layers, info->layers + info->layerCount);
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    XrFrameEndInfo modified = *info; modified.layerCount = layers.size(); modified.layers = layers.data();
    return state->instance->dispatch.EndFrame(session, &modified);
  } catch (...) { auto state = Session(session); return state ? state->instance->dispatch.EndFrame(session, info) : XR_ERROR_RUNTIME_FAILURE; }
}

XrResult XRAPI_CALL LayerDestroyInstance(XrInstance instance) {
  auto state = Instance(instance); if (!state) return XR_ERROR_HANDLE_INVALID;
  { std::lock_guard<std::mutex> lock(g_mutex); g_instances.erase(instance); }
  return state->dispatch.DestroyInstance(instance);
}

XrResult XRAPI_CALL LayerGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
  if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
  if (!std::strcmp(name, "xrGetInstanceProcAddr")) { *function = reinterpret_cast<PFN_xrVoidFunction>(LayerGetInstanceProcAddr); return XR_SUCCESS; }
  auto state = Instance(instance); if (!state) return XR_ERROR_HANDLE_INVALID;
  if (!std::strcmp(name, "xrCreateSession")) *function = reinterpret_cast<PFN_xrVoidFunction>(LayerCreateSession);
  else if (!std::strcmp(name, "xrDestroySession")) *function = reinterpret_cast<PFN_xrVoidFunction>(LayerDestroySession);
  else if (!std::strcmp(name, "xrDestroyInstance")) *function = reinterpret_cast<PFN_xrVoidFunction>(LayerDestroyInstance);
  else if (!std::strcmp(name, "xrEndFrame")) *function = reinterpret_cast<PFN_xrVoidFunction>(LayerEndFrame);
  else return state->next_gipa(instance, name, function);
  return XR_SUCCESS;
}

XrResult XRAPI_CALL LayerCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer, XrInstance* instance) {
  try {
    if (!info || !layer || !instance || !layer->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    const auto* next = layer->nextInfo;
    if (layer->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        next->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO || std::strcmp(next->layerName, kLayerName) ||
        !next->nextGetInstanceProcAddr || !next->nextCreateApiLayerInstance) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo downstream = *layer; downstream.nextInfo = next->next;
    const XrResult result = next->nextCreateApiLayerInstance(info, &downstream, instance); if (XR_FAILED(result)) return result;
    auto state = std::make_shared<InstanceState>(); state->instance = *instance; state->next_gipa = next->nextGetInstanceProcAddr;
    state->application_name = info->applicationInfo.applicationName;
    if (!LoadDispatch(state)) { PFN_xrVoidFunction destroy = nullptr; state->next_gipa(*instance, "xrDestroyInstance", &destroy); if (destroy) reinterpret_cast<PFN_xrDestroyInstance>(destroy)(*instance); return XR_ERROR_INITIALIZATION_FAILED; }
    { std::lock_guard<std::mutex> lock(g_mutex); g_instances[*instance] = state; }
    return XR_SUCCESS;
  } catch (...) { return XR_ERROR_OUT_OF_MEMORY; }
}
}  // namespace

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* name, XrNegotiateApiLayerRequest* request) {
  if (!loader || !name || !request || std::strcmp(name, kLayerName) ||
      loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
      loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1) return XR_ERROR_INITIALIZATION_FAILED;
  request->layerInterfaceVersion = 1; request->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
  request->getInstanceProcAddr = LayerGetInstanceProcAddr; request->createApiLayerInstance = LayerCreateApiLayerInstance;
  return XR_SUCCESS;
}
