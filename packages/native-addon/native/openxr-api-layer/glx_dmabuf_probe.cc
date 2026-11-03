// Exercise the production GLX upload/copy code with real Electron DMA-BUFs.
// Only the OpenXR swapchain acquisition callbacks are replaced by a GL texture;
// import, synchronization, GPU upload and the swapchain blit are unchanged.
#include "layer_linux.cc"

#include <cstdio>
#include <iomanip>
#include <set>

namespace {
XrResult XRAPI_CALL AcquireProbeImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
  *index = 0;
  return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitProbeImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL ReleaseProbeImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

int ConnectProbe() {
  const std::string path = SocketPath();
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0 || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  const timeval timeout{10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  LayerHello hello{};
  hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHello, sizeof(hello), 0, 0};
  hello.process_id = getpid();
  hello.graphics_binding = kGraphicsBindingOpenGLXlib;
  std::strncpy(hello.application_name, "GLX shared-texture GPU probe", sizeof(hello.application_name) - 1);
  if (send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) {
    close(fd);
    return -1;
  }
  return fd;
}

bool ReceiveProbeFrame(int socket_fd, OverlaySnapshot* snapshot, int* frame_fd) {
  *frame_fd = -1;
  iovec data{snapshot, sizeof(*snapshot)};
  char control[CMSG_SPACE(sizeof(int) * kMaxPlanes)]{};
  msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t count = recvmsg(socket_fd, &message, MSG_CMSG_CLOEXEC);
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
    const size_t fds = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    const auto* values = reinterpret_cast<const int*>(CMSG_DATA(header));
    for (size_t i = 0; i < fds; ++i) {
      if (*frame_fd < 0) *frame_fd = values[i]; else close(values[i]);
    }
  }
  return count == sizeof(*snapshot) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
    snapshot->header.magic == kProtocolMagic && snapshot->header.version == kProtocolVersion;
}
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const unsigned target = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 120;
  if (!target) return 2;
  Display* display = XOpenDisplay(nullptr);
  if (!display) { std::cerr << "No X11 display\n"; return 2; }
  const int attributes[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
    GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
  int config_count = 0;
  GLXFBConfig* configs = glXChooseFBConfig(display, DefaultScreen(display), attributes, &config_count);
  if (!configs || !config_count) return 2;
  const GLXFBConfig config = configs[0];
  XFree(configs);
  GLXContext host = glXCreateNewContext(display, config, GLX_RGBA_TYPE, nullptr, True);
  const int pbuffer_attributes[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
  GLXPbuffer pbuffer = glXCreatePbuffer(display, config, pbuffer_attributes);
  if (!host || !pbuffer || !glXMakeContextCurrent(display, pbuffer, pbuffer, host)) return 2;
  std::cout << "GL renderer: " << glGetString(GL_RENDERER) << '\n';
  SessionState state;
  state.graphics_api = SessionState::GraphicsApi::kOpenGL;
  state.instance = std::make_shared<InstanceState>();
  state.instance->dispatch.AcquireSwapchainImage = AcquireProbeImage;
  state.instance->dispatch.WaitSwapchainImage = WaitProbeImage;
  state.instance->dispatch.ReleaseSwapchainImage = ReleaseProbeImage;
  XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
  binding.xDisplay = display; binding.glxFBConfig = config; binding.glxContext = host;
  if (!InitializeGraphics(state, binding) || !state.dmabuf_buffer_import) {
    std::cerr << "GLX GPU DMA-BUF import is unavailable\n";
    return 2;
  }
  const int socket_fd = ConnectProbe();
  if (socket_fd < 0) { std::perror("connect companion"); return 2; }
  GLuint output = 0;
  glGenTextures(1, &output);
  std::set<uint64_t> checksums;
  unsigned completed = 0;
  const auto started = std::chrono::steady_clock::now();
  while (completed < target) {
    OverlaySnapshot snapshot{};
    int frame_fd = -1;
    if (!ReceiveProbeFrame(socket_fd, &snapshot, &frame_fd)) return 3;
    if (frame_fd < 0) continue;
    if (snapshot.frame_transport != FrameTransport::kDmaBuf) {
      std::cerr << "FAIL: producer used a software snapshot\n";
      close(frame_fd);
      return 3;
    }
    if (state.width != snapshot.width || state.height != snapshot.height) {
      glBindTexture(GL_TEXTURE_2D, output);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, snapshot.width, snapshot.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      state.width = snapshot.width; state.height = snapshot.height;
      state.swapchain = reinterpret_cast<XrSwapchain>(1);
      state.images = {{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR, nullptr, output}};
    }
    const bool copied = WaitForDmabufProducer(frame_fd) && RenderFrame(state, snapshot, nullptr, frame_fd);
    close(frame_fd);
    SendFrameAck(socket_fd, snapshot, copied);
    if (!copied) { std::cerr << "FAIL: production GLX GPU copy\n"; return 4; }
    ++completed;
    if (completed != 1 && completed % 60 != 0 && completed != target) continue;
    std::vector<uint8_t> pixels(static_cast<size_t>(snapshot.width) * snapshot.height * 4);
    glBindTexture(GL_TEXTURE_2D, output);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR) return 4;
    if (std::getenv("ELECTRON_VR_PROBE_PATTERN")) {
      if (snapshot.width < 18 || snapshot.height < 18) return 4;
      // glGetTexImage starts at the bottom row; the production swapchain blit
      // flips Electron's top-down image into OpenGL's bottom-up coordinates.
      const uint32_t x[] = {8, snapshot.width - 9, 8, snapshot.width - 9};
      const uint32_t y[] = {8, 8, snapshot.height - 9, snapshot.height - 9};
      const uint8_t expected[][4] = {{0,0,255,255}, {255,255,0,255}, {255,0,0,255}, {0,255,0,255}};
      for (unsigned corner = 0; corner < 4; ++corner) {
        const size_t offset = (static_cast<size_t>(y[corner]) * snapshot.width + x[corner]) * 4;
        if (std::memcmp(pixels.data() + offset, expected[corner], 4) != 0) {
          std::cerr << "FAIL: GLX GPU copy quadrant/orientation mismatch corner=" << corner << '\n';
          return 4;
        }
      }
    }
    uint64_t checksum = 1469598103934665603ULL;
    bool nonzero = false;
    for (uint8_t value : pixels) { checksum = (checksum ^ value) * 1099511628211ULL; nonzero |= value != 0; }
    if (!nonzero) { std::cerr << "FAIL: copied image is empty\n"; return 4; }
    checksums.insert(checksum);
    if (completed == 1 || completed % 60 == 0) {
      std::cout << "GPU GLX copy frames=" << completed << " distinct=" << checksums.size()
        << " checksum=0x" << std::hex << checksum << std::dec << '\n';
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "GPU GLX copy result: frames=" << completed << " distinct=" << checksums.size()
    << " seconds=" << seconds << '\n';
  close(socket_fd);
  state.swapchain = XR_NULL_HANDLE;
  DestroyGraphics(state);
  glDeleteTextures(1, &output);
  glXMakeContextCurrent(display, None, None, nullptr);
  glXDestroyPbuffer(display, pbuffer);
  glXDestroyContext(display, host);
  XCloseDisplay(display);
  return checksums.size() >= 2 ? 0 : 5;
}
