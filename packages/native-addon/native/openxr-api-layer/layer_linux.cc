#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_GRAPHICS_API_VULKAN

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../openxr_api_layer_protocol_linux.h"

namespace {
using namespace electron_vr::openxr_layer_linux;

constexpr char kLayerName[] = "XR_APILAYER_ELECTRON_VR_overlay";
constexpr auto kMinimumCopyInterval = std::chrono::milliseconds(33);
constexpr auto kMaximumFrameWait = std::chrono::milliseconds(500);

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
  PFN_xrCreateVulkanDeviceKHR CreateVulkanDeviceKHR = nullptr;
  PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR = nullptr;
};

struct InstanceState {
  XrInstance instance = XR_NULL_HANDLE;
  PFN_xrGetInstanceProcAddr next_gipa = nullptr;
  Dispatch dispatch;
  std::string application_name;
};

struct SessionState {
  enum class GraphicsApi { kNone, kOpenGL, kVulkan } graphics_api = GraphicsApi::kNone;
  XrSession session = XR_NULL_HANDLE;
  std::shared_ptr<InstanceState> instance;
  XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
  Display* display = nullptr;
  GLXFBConfig fb_config = nullptr;
  GLXContext host_context = nullptr;
  GLXContext context = nullptr;
  GLXPbuffer pbuffer = 0;
  PFNGLGENFRAMEBUFFERSPROC gen_framebuffers = nullptr;
  PFNGLDELETEFRAMEBUFFERSPROC delete_framebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC framebuffer_texture = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer = nullptr;
  PFNGLBLITFRAMEBUFFERPROC blit_framebuffer = nullptr;
  GLuint source_texture = 0;
  uint32_t source_width = 0;
  uint32_t source_height = 0;
  GLuint read_framebuffer = 0;
  GLuint draw_framebuffer = 0;
  XrSpace view_space = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace stage_space = XR_NULL_HANDLE;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLKHR> images;
  XrGraphicsBindingVulkanKHR vulkan_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  std::vector<XrSwapchainImageVulkanKHR> vulkan_images;
  VkQueue vulkan_queue = VK_NULL_HANDLE;
  VkCommandPool vulkan_command_pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties vulkan_memory_properties{};
  PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
  PFN_vkGetImageMemoryRequirements2KHR get_image_memory_requirements2 = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t format = 0;
  uint32_t max_layers = 0;
  bool compatible = false;
  bool image_acquired = false;
  bool image_waited = false;
  uint32_t image_index = 0;
  uint64_t rendered_generation = 0;
  bool graphics_initialized = false;
};

struct FrameState {
  OverlaySnapshot snapshot{};
  std::shared_ptr<std::vector<uint8_t>> pixels;
  int dmabuf_fd = -1;
  bool consumed = false;
};

std::mutex g_mutex;
std::unordered_map<XrInstance, std::shared_ptr<InstanceState>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::mutex g_frame_mutex;
std::condition_variable g_frame_condition;
FrameState g_frame;
std::atomic<bool> g_stop{true};
std::thread g_socket_thread;
std::mutex g_socket_mutex;
int g_socket = -1;

std::string SocketPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  return runtime == nullptr ? std::string() : std::string(runtime) + "/electron-vr/" + kSocketName;
}

bool SyncDmabuf(int fd, uint64_t flags) {
  dma_buf_sync sync{};
  sync.flags = flags;
  int result = -1;
  do {
    result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
  } while (result != 0 && errno == EINTR);
  // Software frames arrive in coherent memfds, which do not implement DMA-BUF synchronization.
  return result == 0 || errno == ENOTTY;
}

void SendFrameAck(int socket_fd, const OverlaySnapshot& snapshot, bool consumed) {
  FrameAck ack{};
  ack.header = {kProtocolMagic, kProtocolVersion, MessageType::kFrameAck, sizeof(ack), 0, snapshot.header.sequence};
  ack.generation = snapshot.generation;
  ack.consumed = consumed ? 1U : 0U;
  send(socket_fd, &ack, sizeof(ack), MSG_NOSIGNAL);
}

void CloseFrame() {
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_frame.dmabuf_fd >= 0) close(g_frame.dmabuf_fd);
  g_frame = {};
  g_frame_condition.notify_all();
}

void SocketMain(LayerHello hello) {
  const std::string path = SocketPath();
  std::chrono::steady_clock::time_point last_copy;
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
      if (fd_count == 0) {
        if (snapshot.generation != 0) {
          std::lock_guard<std::mutex> lock(g_frame_mutex);
          g_frame.snapshot = snapshot;
        }
        continue;
      }
      if (snapshot.generation == 0 || received_fd < 0) {
        for (int descriptor : received_fds) if (descriptor >= 0) close(descriptor);
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last_copy < kMinimumCopyInterval) {
        close(received_fd);
        SendFrameAck(socket_fd, snapshot, true);
        continue;
      }
      const uint64_t pixel_bytes = static_cast<uint64_t>(snapshot.planes[0].stride) * snapshot.height;
      const uint64_t required_size = static_cast<uint64_t>(snapshot.planes[0].offset) + pixel_bytes;
      if (snapshot.plane_count != 1 ||
          (snapshot.modifier != DRM_FORMAT_MOD_INVALID && snapshot.modifier != DRM_FORMAT_MOD_LINEAR) ||
          snapshot.width > UINT32_MAX / 4 || snapshot.planes[0].stride < snapshot.width * 4 ||
          required_size < pixel_bytes || required_size > snapshot.planes[0].size) {
        for (int descriptor : received_fds) if (descriptor >= 0) close(descriptor);
        SendFrameAck(socket_fd, snapshot, false);
        continue;
      }
      if (hello.graphics_binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        std::unique_lock<std::mutex> lock(g_frame_mutex);
        if (g_frame.dmabuf_fd >= 0) {
          lock.unlock();
          close(received_fd);
          SendFrameAck(socket_fd, snapshot, true);
          continue;
        }
        g_frame.snapshot = snapshot;
        g_frame.pixels.reset();
        g_frame.dmabuf_fd = received_fd;
        g_frame.consumed = false;
        last_copy = now;
        const bool resolved = g_frame_condition.wait_for(lock, kMaximumFrameWait, [&] {
          return g_stop.load() || g_frame.dmabuf_fd < 0 ||
            g_frame.snapshot.header.sequence != snapshot.header.sequence;
        });
        if (!resolved && g_frame.snapshot.header.sequence == snapshot.header.sequence && g_frame.dmabuf_fd >= 0) {
          close(g_frame.dmabuf_fd);
          g_frame.dmabuf_fd = -1;
          g_frame.consumed = false;
        }
        const bool consumed = !g_stop.load() &&
          g_frame.snapshot.header.sequence == snapshot.header.sequence && g_frame.consumed;
        lock.unlock();
        SendFrameAck(socket_fd, snapshot, consumed);
        continue;
      }
      if (!SyncDmabuf(received_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
        close(received_fd);
        SendFrameAck(socket_fd, snapshot, false);
        continue;
      }
      void* mapping = mmap(nullptr, snapshot.planes[0].size, PROT_READ, MAP_SHARED, received_fd, 0);
      if (mapping == MAP_FAILED) {
        SyncDmabuf(received_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        close(received_fd);
        SendFrameAck(socket_fd, snapshot, false);
        continue;
      }
      auto pixels = std::make_shared<std::vector<uint8_t>>(pixel_bytes);
      std::memcpy(pixels->data(), static_cast<const uint8_t*>(mapping) + snapshot.planes[0].offset, pixel_bytes);
      munmap(mapping, snapshot.planes[0].size);
      const bool synchronized = SyncDmabuf(received_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
      close(received_fd);
      if (!synchronized) {
        SendFrameAck(socket_fd, snapshot, false);
        continue;
      }
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      g_frame.snapshot = snapshot;
      g_frame.pixels = std::move(pixels);
      last_copy = now;
      SendFrameAck(socket_fd, snapshot, true);
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

const XrGraphicsBindingVulkanKHR* FindVulkanBinding(const XrSessionCreateInfo* info) {
  for (auto* node = static_cast<const XrBaseInStructure*>(info == nullptr ? nullptr : info->next); node != nullptr; node = node->next) {
    // Vulkan2 is an alias with the same structure type and layout.
    if (node->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
      return reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(node);
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

  return state.gen_framebuffers && state.delete_framebuffers && state.bind_framebuffer && state.framebuffer_texture &&
    state.check_framebuffer && state.blit_framebuffer;
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
  if (state.vulkan_command_pool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(state.vulkan_binding.device, state.vulkan_command_pool, nullptr);
    state.vulkan_command_pool = VK_NULL_HANDLE;
  }
}

bool InitializeVulkanGraphics(SessionState& state) {
  const auto& binding = state.vulkan_binding;
  if (binding.instance == VK_NULL_HANDLE || binding.physicalDevice == VK_NULL_HANDLE ||
      binding.device == VK_NULL_HANDLE) return false;
  vkGetDeviceQueue(binding.device, binding.queueFamilyIndex, binding.queueIndex, &state.vulkan_queue);
  vkGetPhysicalDeviceMemoryProperties(binding.physicalDevice, &state.vulkan_memory_properties);
  state.get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
    vkGetDeviceProcAddr(binding.device, "vkGetMemoryFdPropertiesKHR"));
  state.get_image_memory_requirements2 = reinterpret_cast<PFN_vkGetImageMemoryRequirements2KHR>(
    vkGetDeviceProcAddr(binding.device, "vkGetImageMemoryRequirements2KHR"));
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool.queueFamilyIndex = binding.queueFamilyIndex;
  return state.vulkan_queue != VK_NULL_HANDLE && state.get_memory_fd_properties != nullptr &&
    state.get_image_memory_requirements2 != nullptr &&
    vkCreateCommandPool(binding.device, &pool, nullptr, &state.vulkan_command_pool) == VK_SUCCESS;
}

bool FindMemoryType(const SessionState& state, uint32_t bits, uint32_t* index) {
  for (uint32_t i = 0; i < state.vulkan_memory_properties.memoryTypeCount; ++i) {
    if ((bits & (1U << i)) != 0) { *index = i; return true; }
  }
  return false;
}

VkFormat VulkanFormat(const OverlaySnapshot& snapshot) {
  if (snapshot.drm_format == DRM_FORMAT_ARGB8888) return VK_FORMAT_B8G8R8A8_UNORM;
  if (snapshot.drm_format == DRM_FORMAT_ABGR8888) return VK_FORMAT_R8G8B8A8_UNORM;
  return VK_FORMAT_UNDEFINED;
}

bool ValidateVulkanImport(SessionState& state, VkFormat format) {
  VkDrmFormatModifierPropertiesListEXT modifiers{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  properties.pNext = &modifiers;
  vkGetPhysicalDeviceFormatProperties2(state.vulkan_binding.physicalDevice, format, &properties);
  std::vector<VkDrmFormatModifierPropertiesEXT> values(modifiers.drmFormatModifierCount);
  modifiers.pDrmFormatModifierProperties = values.data();
  vkGetPhysicalDeviceFormatProperties2(state.vulkan_binding.physicalDevice, format, &properties);
  const auto linear = std::find_if(values.begin(), values.end(), [](const auto& value) {
    return value.drmFormatModifier == DRM_FORMAT_MOD_LINEAR && value.drmFormatModifierPlaneCount == 1 &&
      (value.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
  });
  if (linear == values.end()) return false;

  VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
  modifier.pNext = &external;
  modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
  modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceImageFormatInfo2 image{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  image.pNext = &modifier;
  image.format = format;
  image.type = VK_IMAGE_TYPE_2D;
  image.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 image_properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  image_properties.pNext = &external_properties;
  return vkGetPhysicalDeviceImageFormatProperties2(state.vulkan_binding.physicalDevice, &image, &image_properties) == VK_SUCCESS &&
    (external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0 &&
    (external_properties.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
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
  state.vulkan_images.clear();
  if (state.swapchain) state.instance->dispatch.DestroySwapchain(state.swapchain);
  state.swapchain = XR_NULL_HANDLE;
  state.width = state.height = 0;
  state.format = 0;
  state.rendered_generation = 0;
}

bool EnsureSwapchain(SessionState& state, const OverlaySnapshot& snapshot) {
  const VkFormat source_format = VulkanFormat(snapshot);
  const int64_t source_srgb = source_format == VK_FORMAT_B8G8R8A8_UNORM
    ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB;
  if (state.swapchain != XR_NULL_HANDLE && state.width == snapshot.width && state.height == snapshot.height &&
      (state.graphics_api != SessionState::GraphicsApi::kVulkan ||
        state.format == source_format || state.format == source_srgb)) return true;
  DestroyOverlay(state);
  uint32_t count = 0;
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainFormats(state.session, 0, &count, nullptr)) || count == 0) return false;
  std::vector<int64_t> formats(count);
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainFormats(state.session, count, &count, formats.data()))) return false;
  const int64_t vulkan_preferred[] = {source_format, source_format == VK_FORMAT_B8G8R8A8_UNORM
    ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_SRGB};
  const int64_t gl_preferred[] = {GL_SRGB8_ALPHA8, GL_RGBA8};
  const int64_t* preferred = state.graphics_api == SessionState::GraphicsApi::kVulkan ? vulkan_preferred : gl_preferred;
  for (size_t i = 0; i < 2; ++i) {
    if (std::find(formats.begin(), formats.end(), preferred[i]) != formats.end()) { state.format = preferred[i]; break; }
  }
  if (state.format == 0) return false;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
    (state.graphics_api == SessionState::GraphicsApi::kVulkan ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
  info.format = state.format; info.sampleCount = 1; info.width = snapshot.width; info.height = snapshot.height;
  info.faceCount = 1; info.arraySize = 1; info.mipCount = 1;
  if (XR_FAILED(state.instance->dispatch.CreateSwapchain(state.session, &info, &state.swapchain))) return false;
  uint32_t images = 0;
  if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainImages(state.swapchain, 0, &images, nullptr)) || images == 0) return false;
  if (state.graphics_api == SessionState::GraphicsApi::kVulkan) {
    state.vulkan_images.resize(images);
    for (auto& image : state.vulkan_images) image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainImages(state.swapchain, images, &images,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(state.vulkan_images.data())))) return false;
  } else {
    state.images.resize(images);
    for (auto& image : state.images) image = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR};
    if (XR_FAILED(state.instance->dispatch.EnumerateSwapchainImages(state.swapchain, images, &images,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(state.images.data())))) return false;
  }
  state.width = snapshot.width; state.height = snapshot.height;
  return true;
}

bool RenderVulkanFrame(SessionState& state, const OverlaySnapshot& snapshot, int received_fd) {
  const uint64_t bytes = static_cast<uint64_t>(snapshot.planes[0].stride) * snapshot.height;
  const VkFormat source_format = VulkanFormat(snapshot);
  if (received_fd < 0 || snapshot.width == 0 || snapshot.height == 0 || snapshot.plane_count != 1 ||
       (snapshot.modifier != DRM_FORMAT_MOD_INVALID && snapshot.modifier != DRM_FORMAT_MOD_LINEAR) ||
       snapshot.planes[0].stride < snapshot.width * 4ULL ||
      snapshot.planes[0].offset > snapshot.planes[0].size || bytes > snapshot.planes[0].size - snapshot.planes[0].offset ||
      source_format == VK_FORMAT_UNDEFINED || !ValidateVulkanImport(state, source_format) || !EnsureSwapchain(state, snapshot)) return false;

  VkImage source = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int import_fd = -1;
  const VkDevice device = state.vulkan_binding.device;
  const auto cleanup = [&] {
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (command) vkFreeCommandBuffers(device, state.vulkan_command_pool, 1, &command);
    if (source) vkDestroyImage(device, source, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    if (import_fd >= 0) close(import_fd);
  };
  bool success = false;
  do {
    VkSubresourceLayout plane{};
    plane.offset = snapshot.planes[0].offset;
    plane.rowPitch = snapshot.planes[0].stride;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    modifier.pNext = &external;
    modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &plane;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.pNext = &modifier;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = source_format;
    image.extent = {snapshot.width, snapshot.height, 1};
    image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &image, nullptr, &source) != VK_SUCCESS) break;
    VkMemoryDedicatedRequirements dedicated_requirements{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    requirements.pNext = &dedicated_requirements;
    VkImageMemoryRequirementsInfo2 requirements_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    requirements_info.image = source;
    state.get_image_memory_requirements2(device, &requirements_info, &requirements);
    import_fd = dup(received_fd);
    if (import_fd < 0) break;
    VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (state.get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        import_fd, &fd_properties) != VK_SUCCESS) break;
    uint32_t memory_type = 0;
    if (!FindMemoryType(state, requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits, &memory_type)) break;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = source;
    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = import_fd;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &import;
    allocation.allocationSize = requirements.memoryRequirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS) break;
    import_fd = -1;  // Vulkan owns the duplicated descriptor after a successful import.
    if (vkBindImageMemory(device, source, memory, 0) != VK_SUCCESS) break;

    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(state.instance->dispatch.AcquireSwapchainImage(state.swapchain, &acquire, &state.image_index))) break;
    state.image_acquired = true;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(state.instance->dispatch.WaitSwapchainImage(state.swapchain, &wait)) ||
        state.image_index >= state.vulkan_images.size()) break;
    state.image_waited = true;

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = state.vulkan_command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS) break;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) break;
    VkImageMemoryBarrier barriers[2]{};
    for (auto& barrier : barriers) {
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barriers[0].dstQueueFamilyIndex = state.vulkan_binding.queueFamilyIndex;
    barriers[0].image = source;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = state.vulkan_images[state.image_index].image;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      0, nullptr, 0, nullptr, 2, barriers);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {snapshot.width, snapshot.height, 1};
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      state.vulkan_images[state.image_index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = 0;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = state.vulkan_binding.queueFamilyIndex;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
      0, nullptr, 0, nullptr, 2, barriers);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) break;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device, &fence_info, nullptr, &fence) != VK_SUCCESS) break;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    if (vkQueueSubmit(state.vulkan_queue, 1, &submit, fence) != VK_SUCCESS ||
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) break;
    success = true;
  } while (false);
  if (state.image_acquired) {
    if (!state.image_waited) {
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = XR_INFINITE_DURATION;
      state.instance->dispatch.WaitSwapchainImage(state.swapchain, &wait);
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    state.instance->dispatch.ReleaseSwapchainImage(state.swapchain, &release);
    state.image_acquired = state.image_waited = false;
  }
  cleanup();
  return success;
}

bool RenderFrame(SessionState& state, const OverlaySnapshot& snapshot, const std::vector<uint8_t>& pixels) {
  if (pixels.size() < static_cast<uint64_t>(snapshot.planes[0].stride) * snapshot.height ||
      !EnsureSwapchain(state, snapshot)) return false;
  Display* previous_display = glXGetCurrentDisplay();
  GLXContext previous_context = glXGetCurrentContext();
  GLXDrawable previous_draw = glXGetCurrentDrawable();
  GLXDrawable previous_read = glXGetCurrentReadDrawable();
  if (!glXMakeContextCurrent(state.display, state.pbuffer, state.pbuffer, state.context)) return false;
  bool copied = false;
  glBindTexture(GL_TEXTURE_2D, state.source_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, snapshot.planes[0].stride / 4);
  const GLenum format = snapshot.drm_format == DRM_FORMAT_ARGB8888 ? GL_BGRA : GL_RGBA;
  if (state.source_width != snapshot.width || state.source_height != snapshot.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, snapshot.width, snapshot.height, 0, format, GL_UNSIGNED_BYTE, nullptr);
    state.source_width = snapshot.width;
    state.source_height = snapshot.height;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, snapshot.width, snapshot.height, format, GL_UNSIGNED_BYTE, pixels.data());
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
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
      glFlush();
      copied = true;
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    state.instance->dispatch.ReleaseSwapchainImage(state.swapchain, &release);
    state.image_acquired = state.image_waited = false;
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

bool HasVulkanExtension(VkPhysicalDevice physical_device, const char* name) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
  return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

XrResult XRAPI_CALL LayerCreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* info,
                                               VkDevice* device, VkResult* vulkan_result) {
  try {
    auto state = Instance(instance);
    if (!state || !state->dispatch.CreateVulkanDeviceKHR || !info || !info->vulkanCreateInfo) {
      return XR_ERROR_VALIDATION_FAILURE;
    }
    const char* required[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    std::vector<const char*> extensions;
    extensions.reserve(info->vulkanCreateInfo->enabledExtensionCount + 8);
    for (uint32_t i = 0; i < info->vulkanCreateInfo->enabledExtensionCount; ++i) {
      extensions.push_back(info->vulkanCreateInfo->ppEnabledExtensionNames[i]);
    }
    for (const char* extension : required) {
      const bool already_enabled = std::any_of(extensions.begin(), extensions.end(), [extension](const char* enabled) {
        return std::strcmp(enabled, extension) == 0;
      });
      if (!already_enabled && HasVulkanExtension(info->vulkanPhysicalDevice, extension)) extensions.push_back(extension);
    }
    VkDeviceCreateInfo device_info = *info->vulkanCreateInfo;
    device_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    device_info.ppEnabledExtensionNames = extensions.data();
    XrVulkanDeviceCreateInfoKHR create_info = *info;
    create_info.vulkanCreateInfo = &device_info;
    return state->dispatch.CreateVulkanDeviceKHR(instance, &create_info, device, vulkan_result);
  } catch (...) { return XR_ERROR_OUT_OF_MEMORY; }
}

XrResult XRAPI_CALL LayerGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId system_id,
                                                       uint32_t capacity, uint32_t* count, char* buffer) {
  try {
    auto state = Instance(instance);
    if (!state || !state->dispatch.GetVulkanDeviceExtensionsKHR || !count) return XR_ERROR_VALIDATION_FAILURE;
    // The legacy API does not identify the physical device here, so adding
    // unverified extensions can make the host's vkCreateDevice fail.
    return state->dispatch.GetVulkanDeviceExtensionsKHR(instance, system_id, capacity, count, buffer);
  } catch (...) { return XR_ERROR_OUT_OF_MEMORY; }
}

XrResult XRAPI_CALL LayerCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* session) {
  try {
    auto owner = Instance(instance);
    if (!owner || !info || !session) return XR_ERROR_VALIDATION_FAILURE;
    const auto* binding = FindXlibBinding(info);
    const auto* vulkan_binding = FindVulkanBinding(info);
    const XrResult result = owner->dispatch.CreateSession(instance, info, session);
    if (XR_FAILED(result)) return result;
    auto state = std::make_shared<SessionState>(); state->session = *session; state->instance = owner;
    if (binding != nullptr) {
      state->binding = *binding;
      state->graphics_api = SessionState::GraphicsApi::kOpenGL;
      state->compatible = true;
    } else if (vulkan_binding != nullptr) {
      state->vulkan_binding = *vulkan_binding;
      state->graphics_api = SessionState::GraphicsApi::kVulkan;
      state->compatible = true;
    }
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(owner->dispatch.GetSystemProperties(instance, info->systemId, &properties))) state->max_layers = properties.graphicsProperties.maxLayerCount;
    { std::lock_guard<std::mutex> lock(g_mutex); g_sessions[*session] = state; }
    if (state->compatible && g_stop.exchange(false)) {
      LayerHello hello{}; hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHello, sizeof(hello), 0, 0};
      hello.process_id = getpid();
      hello.graphics_binding = state->graphics_api == SessionState::GraphicsApi::kVulkan
        ? XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR : XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
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
    CloseFrame();
    if (g_socket_thread.joinable()) g_socket_thread.join();
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
    FrameState frame;
    {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      frame = g_frame;
      if (g_frame.dmabuf_fd >= 0) frame.dmabuf_fd = dup(g_frame.dmabuf_fd);
    }
    const OverlaySnapshot& snapshot = frame.snapshot;
    if ((frame.pixels || frame.dmabuf_fd >= 0) && snapshot.generation != 0 && !state->graphics_initialized) {
      state->graphics_initialized = state->graphics_api == SessionState::GraphicsApi::kVulkan
        ? InitializeVulkanGraphics(*state) : InitializeGraphics(*state, state->binding);
      if (state->graphics_initialized) CreateSpaces(*state);
    }
    if (state->graphics_api == SessionState::GraphicsApi::kVulkan && frame.dmabuf_fd >= 0) {
      bool consumed = !snapshot.visible || snapshot.generation == state->rendered_generation;
      if (state->graphics_initialized && snapshot.visible && snapshot.generation != 0 &&
          snapshot.generation != state->rendered_generation) {
        const bool synchronized = SyncDmabuf(frame.dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        consumed = synchronized && RenderVulkanFrame(*state, snapshot, frame.dmabuf_fd);
        if (synchronized) {
          consumed = SyncDmabuf(frame.dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) && consumed;
        }
        if (consumed) state->rendered_generation = snapshot.generation;
      }
      close(frame.dmabuf_fd);
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      if (g_frame.snapshot.header.sequence == snapshot.header.sequence && g_frame.dmabuf_fd >= 0) {
        close(g_frame.dmabuf_fd);
        g_frame.dmabuf_fd = -1;
        g_frame.consumed = consumed;
        g_frame_condition.notify_all();
      }
    } else if (state->graphics_initialized && snapshot.visible && snapshot.generation != 0 &&
               snapshot.generation != state->rendered_generation && frame.pixels && RenderFrame(*state, snapshot, *frame.pixels)) {
        state->rendered_generation = snapshot.generation;
    }
    const XrSpace space = snapshot.placement_mode == PlacementMode::kHead ? state->view_space :
      (state->stage_space ? state->stage_space : state->local_space);
    if (!snapshot.visible || state->rendered_generation == 0 || state->swapchain == XR_NULL_HANDLE ||
        space == XR_NULL_HANDLE) return forward();
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
  else if (!std::strcmp(name, "xrCreateVulkanDeviceKHR") && state->dispatch.CreateVulkanDeviceKHR)
    *function = reinterpret_cast<PFN_xrVoidFunction>(LayerCreateVulkanDeviceKHR);
  else if (!std::strcmp(name, "xrGetVulkanDeviceExtensionsKHR") && state->dispatch.GetVulkanDeviceExtensionsKHR)
    *function = reinterpret_cast<PFN_xrVoidFunction>(LayerGetVulkanDeviceExtensionsKHR);
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
    Load(state, "xrCreateVulkanDeviceKHR", &state->dispatch.CreateVulkanDeviceKHR);
    Load(state, "xrGetVulkanDeviceExtensionsKHR", &state->dispatch.GetVulkanDeviceExtensionsKHR);
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
