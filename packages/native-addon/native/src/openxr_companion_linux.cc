#include "openxr_companion_linux.h"

#if defined(__linux__)
#include <libdrm/drm_fourcc.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "../openxr_api_layer_protocol_linux.h"
#endif

namespace vrbridge {
namespace {

void SetError(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
}

#if defined(__linux__)
using namespace electron_vr::openxr_layer_linux;

struct State {
  std::mutex mutex;
  std::thread thread;
  std::atomic<bool> stopping{false};
  int server = -1;
  int client = -1;
  bool initialized = false;
  bool openvr_relay = false;
  uint64_t consumed_sequence = 0;
  LayerHello hello{};
  OverlaySnapshot snapshot{};
};

State g_state;

uint32_t ParseFormat(const std::string& format) {
  if (format == "rgba" || format == "ABGR8888") return DRM_FORMAT_ABGR8888;
  return DRM_FORMAT_ARGB8888;
}

uint64_t ParseModifier(const std::string& value) {
  if (value.empty()) return DRM_FORMAT_MOD_INVALID;
  try { return std::stoull(value, nullptr, 0); } catch (...) { return DRM_FORMAT_MOD_INVALID; }
}

bool SendSnapshotLocked(const int* fds, uint32_t fd_count) {
  if (g_state.client < 0) return false;
  g_state.snapshot.header.magic = kProtocolMagic;
  g_state.snapshot.header.version = kProtocolVersion;
  g_state.snapshot.header.type = MessageType::kSnapshot;
  g_state.snapshot.header.byte_size = sizeof(OverlaySnapshot);
  g_state.snapshot.header.fd_count = fd_count;

  iovec data{&g_state.snapshot, sizeof(g_state.snapshot)};
  char control[CMSG_SPACE(sizeof(int) * kMaxPlanes)] = {};
  msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1;
  if (fd_count > 0) {
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    std::memcpy(CMSG_DATA(header), fds, sizeof(int) * fd_count);
  }
  return sendmsg(g_state.client, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(g_state.snapshot));
}

bool ReceiveFrameAckLocked() {
  pollfd descriptor{g_state.client, POLLIN, 0};
  if (poll(&descriptor, 1, 1000) <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    return false;
  }
  FrameAck ack{};
  const ssize_t received = recv(g_state.client, &ack, sizeof(ack), MSG_WAITALL);
  return received == sizeof(ack) && ack.header.magic == kProtocolMagic &&
    ack.header.version == kProtocolVersion && ack.header.type == MessageType::kFrameAck &&
    ack.header.byte_size == sizeof(ack) && ack.header.fd_count == 0 &&
    ack.header.sequence == g_state.snapshot.header.sequence && ack.generation == g_state.snapshot.generation &&
    ack.consumed != 0;
}

bool SendFrameLocked(int fd, std::string* error) {
  if (g_state.client < 0) return true;
  if (SendSnapshotLocked(&fd, 1) && ReceiveFrameAckLocked()) {
    g_state.consumed_sequence = g_state.snapshot.header.sequence;
    return true;
  }
  close(g_state.client);
  g_state.client = -1;
  SetError(error, "Linux API-layer frame acknowledgment failed.");
  return false;
}

void ServerMain() {
  int server = -1;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    server = g_state.server;
  }
  while (!g_state.stopping.load()) {
    const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (!g_state.stopping.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || credentials.uid != geteuid()) {
      close(client);
      continue;
    }
    LayerHello hello{};
    const timeval timeout{0, 200000};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (recv(client, &hello, sizeof(hello), MSG_WAITALL) != sizeof(hello) ||
        hello.header.magic != kProtocolMagic || hello.header.version != kProtocolVersion ||
        hello.header.type != MessageType::kHello || hello.header.byte_size != sizeof(hello) ||
        hello.header.fd_count != 0 || !std::memchr(hello.application_name, '\0', sizeof(hello.application_name))) {
      close(client);
      continue;
    }
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.client >= 0) close(g_state.client);
    g_state.client = client;
    g_state.hello = hello;
    g_state.consumed_sequence = 0;
    g_state.snapshot.header.sequence = 0;
    g_state.snapshot.revision++;
    SendSnapshotLocked(nullptr, 0);
  }
}

bool ReadHostPresence(const std::string& name, HostPresence* presence, pid_t* peer_pid = nullptr) {
  if (name.size() + 1 > sizeof(sockaddr_un::sun_path)) return false;
  const int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (client < 0) return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  const int connected = connect(client, reinterpret_cast<sockaddr*>(&address), length);
  bool valid = connected == 0;
  if (connected < 0 && errno == EINPROGRESS) {
    pollfd descriptor{client, POLLOUT, 0};
    int error = 0;
    socklen_t size = sizeof(error);
    valid = poll(&descriptor, 1, 50) > 0 &&
      getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
  }
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  valid = valid && getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 && credentials.uid == geteuid();
  if (valid && peer_pid) *peer_pid = credentials.pid;
  pollfd descriptor{client, POLLIN, 0};
  valid = valid && poll(&descriptor, 1, 50) > 0 &&
    recv(client, presence, sizeof(*presence), MSG_TRUNC) == sizeof(*presence);
  close(client);
  return valid && presence->hello.header.magic == kProtocolMagic &&
    presence->hello.header.version == kProtocolVersion && presence->hello.header.type == MessageType::kHostPresence &&
    presence->hello.header.byte_size == sizeof(*presence) && presence->hello.header.fd_count == 0 &&
    std::memchr(presence->hello.application_name, '\0', sizeof(presence->hello.application_name)) &&
    std::memchr(presence->runtime_name, '\0', sizeof(presence->runtime_name));
}

void ApplyPlacement(const OverlayPlacement& placement) {
  g_state.snapshot.placement_mode = placement.mode == OverlayPlacementMode::kHead
    ? PlacementMode::kHead : PlacementMode::kWorld;
  g_state.snapshot.position[0] = placement.position.x;
  g_state.snapshot.position[1] = placement.position.y;
  g_state.snapshot.position[2] = placement.position.z;
  g_state.snapshot.rotation[0] = placement.rotation.x;
  g_state.snapshot.rotation[1] = placement.rotation.y;
  g_state.snapshot.rotation[2] = placement.rotation.z;
  g_state.snapshot.rotation[3] = placement.rotation.w;
}
#endif
}  // namespace

void PopulateOpenXRHostPresenceLinux(RuntimeInfo* info) {
#if defined(__linux__)
  if (!info) return;
  const uint32_t preferred_pid = info->openxr_host_process_id;
  info->openxr_host_detected = false;
  info->openxr_host_process_id = 0;
  info->openxr_host_application_name.clear();
  info->openxr_host_graphics_api.clear();
  info->openxr_host_runtime_name.clear();
  info->openxr_host_runtime_version.clear();
  info->openxr_host_session_started_at_ms = 0;
  std::ifstream sockets("/proc/net/unix");
  const std::string prefix = "@" + HostSocketPrefix();
  std::string line;
  HostPresence selected{};
  bool found = false;
  unsigned attempts = 0;
  while (std::getline(sockets, line) && attempts < 32) {
    const size_t start = line.find(prefix);
    if (start == std::string::npos) continue;
    ++attempts;
    HostPresence presence{};
    pid_t peer_pid = -1;
    if (!ReadHostPresence(line.substr(start + 1), &presence, &peer_pid)) continue;
    // SO_PEERCRED translates PID namespaces; the game's reported local PID
    // may numerically equal ours even when it is a different process.
    if (peer_pid == getpid()) continue;
    const auto binding = presence.hello.graphics_binding;
    if (binding != kGraphicsBindingVulkan && binding != kGraphicsBindingOpenGLXlib) continue;
    if (!found || presence.hello.process_id == preferred_pid) selected = presence;
    found = true;
    if (presence.hello.process_id == preferred_pid) break;
  }
  if (!found) return;
  info->openxr_host_detected = true;
  info->openxr_host_process_id = selected.hello.process_id;
  info->openxr_host_application_name = selected.hello.application_name;
  info->openxr_host_graphics_api = selected.hello.graphics_binding == kGraphicsBindingVulkan ? "vulkan" : "opengl-xlib";
  info->openxr_host_runtime_name = selected.runtime_name;
  const uint64_t version = selected.runtime_version;
  info->openxr_host_runtime_version = std::to_string(version >> 48) + "." +
    std::to_string((version >> 32) & 0xffff) + "." + std::to_string(version & 0xffffffff);
  info->openxr_host_session_started_at_ms = selected.session_started_at_ms;
  info->openxr_protocol_version = kProtocolVersion;
#else
  (void)info;
#endif
}

void PopulateOpenVRRelayPresenceLinux(RuntimeInfo* info) {
#if defined(__linux__)
  if (!info) return;
  static std::mutex cache_mutex;
  static std::chrono::steady_clock::time_point checked;
  static HostPresence presence{};
  static bool available = false;
  std::lock_guard<std::mutex> lock(cache_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - checked >= std::chrono::milliseconds(250)) {
    available = ReadHostPresence(OpenVRRelayName(true), &presence) &&
      presence.hello.graphics_binding == kGraphicsBindingOpenVRRelay;
    checked = std::chrono::steady_clock::now();
  }
  info->openvr_scene_process_id = 0;
  info->openvr_scene_application_name.clear();
  info->openvr_scene_application_key.clear();
  info->openvr_texture_submission_count = 0;
  info->openvr_host_relay_available = available;
  if (info->openvr_host_relay_available) {
    info->openvr_scene_process_id = presence.hello.process_id;
    info->openvr_scene_application_name = presence.hello.application_name;
    info->openvr_texture_submission_count = presence.hello.header.sequence;
  }
#else
  (void)info;
#endif
}

bool IsOpenXRApiLayerInstalledLinux(bool* enabled, std::string* path) {
#if defined(__linux__)
  const char* data_home = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  const std::filesystem::path manifest = data_home != nullptr
    ? std::filesystem::path(data_home) / "openxr/1/api_layers/implicit.d/electron_vr_openxr_layer.json"
    : std::filesystem::path(home == nullptr ? "" : home) / ".local/share/openxr/1/api_layers/implicit.d/electron_vr_openxr_layer.json";
  const std::filesystem::path disabled = manifest.string() + ".disabled";
  const bool library = std::filesystem::exists(manifest.parent_path() / "libelectron_vr_openxr_layer.so");
  const bool installed = library && (std::filesystem::exists(manifest) || std::filesystem::exists(disabled));
  if (enabled != nullptr) *enabled = installed && std::filesystem::exists(manifest) && std::getenv("ELECTRON_VR_DISABLE_OPENXR_API_LAYER") == nullptr;
  if (path != nullptr) *path = installed ? manifest.string() : "";
  return installed;
#else
  if (enabled != nullptr) *enabled = false;
  if (path != nullptr) path->clear();
  return false;
#endif
}

bool InitializeOpenXRCompanionLinux(const InitializeOptions& options, std::string* error, bool openvr_relay) {
#if defined(__linux__)
  if (!openvr_relay && options.curvature != 0.0f) { SetError(error, "Linux API-layer curvature is not supported yet."); return false; }
  ShutdownOpenXRCompanionLinux();
  g_state.openvr_relay = openvr_relay;
  g_state.server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (g_state.server < 0) { SetError(error, "Failed to create Linux OpenXR companion socket."); return false; }
  sockaddr_un address{};
  const socklen_t address_length = openvr_relay ? MakeOpenVRRelayAddress(&address, false) : MakeSocketAddress(&address);
  if (bind(g_state.server, reinterpret_cast<sockaddr*>(&address), address_length) != 0 || listen(g_state.server, 1) != 0) {
    SetError(error, "Failed to bind Linux OpenXR companion socket: " + std::string(std::strerror(errno)));
    ShutdownOpenXRCompanionLinux();
    return false;
  }
  g_state.snapshot = {};
  g_state.snapshot.visible = options.visible ? 1U : 0U;
  g_state.snapshot.size_meters = options.size_meters;
  g_state.snapshot.curvature = options.curvature;
  ApplyPlacement(options.placement);
  g_state.initialized = true;
  g_state.stopping.store(false);
  g_state.thread = std::thread(ServerMain);
  if (error != nullptr) error->clear();
  return true;
#else
  (void)options; (void)openvr_relay; SetError(error, "Linux OpenXR companion is available only on Linux."); return false;
#endif
}

bool SubmitOpenXRCompanionFrameLinux(const LinuxTextureInfo& texture, std::string* error) {
#if defined(__linux__)
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.initialized) { SetError(error, "Linux OpenXR companion is not initialized."); return false; }
  if (texture.width == 0 || texture.height == 0 || texture.planes.size() != 1 || texture.planes[0].fd < 0) {
    SetError(error, "Linux API-layer transport currently requires one DMA-BUF plane and non-zero dimensions."); return false;
  }
  g_state.snapshot.header.sequence++;
  g_state.snapshot.generation++;
  g_state.snapshot.width = texture.width;
  g_state.snapshot.height = texture.height;
  g_state.snapshot.drm_format = ParseFormat(texture.pixel_format);
  g_state.snapshot.plane_count = 1;
  g_state.snapshot.modifier = ParseModifier(texture.modifier);
  g_state.snapshot.planes[0] = {texture.planes[0].stride, texture.planes[0].offset, texture.planes[0].size};
  g_state.snapshot.software_frame = 0;
  g_state.snapshot.revision++;
  const int fd = texture.planes[0].fd;
  if (!SendFrameLocked(fd, error)) return false;
  if (error != nullptr) error->clear();
  return true;
#else
  (void)texture; SetError(error, "Linux OpenXR companion is available only on Linux."); return false;
#endif
}

bool SubmitOpenXRCompanionSoftwareFrameLinux(const SoftwareFrameInfo& frame_info, std::string* error) {
#if defined(__linux__)
  const uint64_t stride = static_cast<uint64_t>(frame_info.width) * 4;
  const uint64_t byte_size = stride * frame_info.height;
  if (frame_info.width == 0 || frame_info.height == 0 || byte_size != frame_info.rgba_pixels.size()) {
    SetError(error, "Linux API-layer software transport requires tightly packed RGBA pixels.");
    return false;
  }

  const int fd = memfd_create("electron-vr-openxr-frame", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(byte_size)) != 0) {
    if (fd >= 0) close(fd);
    SetError(error, "Failed to allocate Linux API-layer software frame: " + std::string(std::strerror(errno)));
    return false;
  }
  void* mapping = mmap(nullptr, byte_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    SetError(error, "Failed to map Linux API-layer software frame: " + std::string(std::strerror(errno)));
    return false;
  }
  std::memcpy(mapping, frame_info.rgba_pixels.data(), byte_size);
  munmap(mapping, byte_size);

  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.initialized) {
    close(fd);
    SetError(error, "Linux OpenXR companion is not initialized.");
    return false;
  }
  g_state.snapshot.header.sequence++;
  g_state.snapshot.generation++;
  g_state.snapshot.width = frame_info.width;
  g_state.snapshot.height = frame_info.height;
  g_state.snapshot.drm_format = DRM_FORMAT_ABGR8888;
  g_state.snapshot.plane_count = 1;
  g_state.snapshot.modifier = DRM_FORMAT_MOD_INVALID;
  g_state.snapshot.planes[0] = {static_cast<uint32_t>(stride), 0, byte_size};
  g_state.snapshot.software_frame = 1;
  g_state.snapshot.revision++;
  const bool submitted = SendFrameLocked(fd, error);
  close(fd);
  if (submitted && error != nullptr) error->clear();
  return submitted;
#else
  (void)frame_info; SetError(error, "Linux OpenXR companion is available only on Linux."); return false;
#endif
}

bool SetOpenXRCompanionPlacementLinux(const OverlayPlacement& placement, std::string* error) {
#if defined(__linux__)
  std::lock_guard<std::mutex> lock(g_state.mutex); ApplyPlacement(placement); g_state.snapshot.revision++; SendSnapshotLocked(nullptr, 0); if (error) error->clear(); return true;
#else
  (void)placement; SetError(error, "Unsupported platform."); return false;
#endif
}
bool SetOpenXRCompanionVisibleLinux(bool visible, std::string* error) {
#if defined(__linux__)
  std::lock_guard<std::mutex> lock(g_state.mutex); g_state.snapshot.visible = visible; g_state.snapshot.revision++; SendSnapshotLocked(nullptr, 0); if (error) error->clear(); return true;
#else
  (void)visible; SetError(error, "Unsupported platform."); return false;
#endif
}
bool SetOpenXRCompanionSizeMetersLinux(float size, std::string* error) {
#if defined(__linux__)
  if (!std::isfinite(size) || size <= 0) { SetError(error, "Overlay size must be positive."); return false; }
  std::lock_guard<std::mutex> lock(g_state.mutex); g_state.snapshot.size_meters = size; g_state.snapshot.revision++; SendSnapshotLocked(nullptr, 0); if (error) error->clear(); return true;
#else
  (void)size; SetError(error, "Unsupported platform."); return false;
#endif
}
bool SetOpenXRCompanionCurvatureLinux(float curvature, std::string* error) {
#if defined(__linux__)
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (!g_state.openvr_relay && curvature != 0.0f) { SetError(error, "Linux API-layer curvature is not supported yet."); return false; }
  g_state.snapshot.curvature = curvature;
  g_state.snapshot.revision++;
  SendSnapshotLocked(nullptr, 0);
#else
  if (curvature != 0.0f) return false;
#endif
  if (error) error->clear(); return true;
}
void PopulateOpenXRCompanionRuntimeInfoLinux(RuntimeInfo* info) {
#if defined(__linux__)
  if (!info) return; std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.client >= 0) {
    pollfd descriptor{g_state.client, 0, 0};
    if (poll(&descriptor, 1, 0) > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))) {
      close(g_state.client);
      g_state.client = -1;
    }
  }
  info->openxr_companion_connected = g_state.client >= 0;
  if (g_state.client >= 0) {
    info->openxr_host_detected = true;
    info->openxr_host_process_id = g_state.hello.process_id;
    info->openxr_host_application_name = g_state.hello.application_name;
    info->openxr_host_graphics_api =
      (g_state.hello.graphics_binding == kGraphicsBindingVulkan ? "vulkan" :
        (g_state.hello.graphics_binding == kGraphicsBindingOpenGLXlib ? "opengl-xlib" : ""));
  }
  info->openxr_submitted_frame_sequence = g_state.client >= 0 ? g_state.snapshot.header.sequence : 0;
  info->openxr_consumed_frame_sequence = g_state.client >= 0 ? g_state.consumed_sequence : 0;
  info->openxr_protocol_version = kProtocolVersion;
#else
  (void)info;
#endif
}

void PopulateOpenVRRelayRuntimeInfoLinux(RuntimeInfo* info) {
#if defined(__linux__)
  if (!info) return;
  RuntimeInfo transport;
  info->openvr_host_relay_active = true;
  PopulateOpenXRCompanionRuntimeInfoLinux(&transport);
  info->openvr_host_relay_connected = transport.openxr_companion_connected;
  info->openvr_submitted_frame_sequence = transport.openxr_submitted_frame_sequence;
  info->openvr_consumed_frame_sequence = transport.openxr_consumed_frame_sequence;
  PopulateOpenVRRelayPresenceLinux(info);
#else
  (void)info;
#endif
}
void ShutdownOpenXRCompanionLinux() {
#if defined(__linux__)
  g_state.stopping.store(true);
  int server = -1;
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    server = g_state.server;
    g_state.server = -1;
  }
  if (server >= 0) { shutdown(server, SHUT_RDWR); close(server); }
  if (g_state.thread.joinable()) g_state.thread.join();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  if (g_state.client >= 0) close(g_state.client);
  g_state.client = -1; g_state.initialized = false;
  g_state.hello = {};
  g_state.consumed_sequence = 0;
#endif
}

}  // namespace vrbridge
