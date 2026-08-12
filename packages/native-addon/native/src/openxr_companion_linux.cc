#include "openxr_companion_linux.h"

#if defined(__linux__)
#include <libdrm/drm_fourcc.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
  LayerHello hello{};
  OverlaySnapshot snapshot{};
  std::string socket_path;
};

State g_state;

std::string RuntimeDirectory() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') return {};
  return std::string(runtime) + "/electron-vr";
}

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
    if (recv(client, &hello, sizeof(hello), MSG_WAITALL) != sizeof(hello) ||
        hello.header.magic != kProtocolMagic || hello.header.version != kProtocolVersion ||
        hello.header.type != MessageType::kHello) {
      close(client);
      continue;
    }
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.client >= 0) close(g_state.client);
    g_state.client = client;
    g_state.hello = hello;
    g_state.snapshot.revision++;
    SendSnapshotLocked(nullptr, 0);
  }
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

bool InitializeOpenXRCompanionLinux(const InitializeOptions& options, std::string* error) {
#if defined(__linux__)
  if (options.curvature != 0.0f) { SetError(error, "Linux API-layer curvature is not supported yet."); return false; }
  ShutdownOpenXRCompanionLinux();
  const std::string directory = RuntimeDirectory();
  if (directory.empty()) { SetError(error, "XDG_RUNTIME_DIR is required for Linux OpenXR API-layer IPC."); return false; }
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  chmod(directory.c_str(), 0700);
  g_state.socket_path = directory + "/" + kSocketName;
  unlink(g_state.socket_path.c_str());
  g_state.server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (g_state.server < 0) { SetError(error, "Failed to create Linux OpenXR companion socket."); return false; }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, g_state.socket_path.c_str(), sizeof(address.sun_path) - 1);
  if (bind(g_state.server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(g_state.server, 1) != 0) {
    SetError(error, "Failed to bind Linux OpenXR companion socket: " + std::string(std::strerror(errno)));
    ShutdownOpenXRCompanionLinux();
    return false;
  }
  chmod(g_state.socket_path.c_str(), 0600);
  g_state.snapshot = {};
  g_state.snapshot.visible = options.visible ? 1U : 0U;
  g_state.snapshot.size_meters = options.size_meters;
  ApplyPlacement(options.placement);
  g_state.initialized = true;
  g_state.stopping.store(false);
  g_state.thread = std::thread(ServerMain);
  if (error != nullptr) error->clear();
  return true;
#else
  (void)options; SetError(error, "Linux OpenXR companion is available only on Linux."); return false;
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
  g_state.snapshot.revision++;
  const int fd = texture.planes[0].fd;
  if (g_state.client >= 0 && !SendSnapshotLocked(&fd, 1)) { close(g_state.client); g_state.client = -1; }
  if (error != nullptr) error->clear();
  return true;
#else
  (void)texture; SetError(error, "Linux OpenXR companion is available only on Linux."); return false;
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
  if (curvature != 0.0f) { SetError(error, "Linux API-layer curvature is not supported yet."); return false; }
  if (error) error->clear(); return true;
}
void PopulateOpenXRCompanionRuntimeInfoLinux(RuntimeInfo* info) {
#if defined(__linux__)
  if (!info) return; std::lock_guard<std::mutex> lock(g_state.mutex);
  info->openxr_companion_connected = g_state.client >= 0;
  info->openxr_host_process_id = g_state.client >= 0 ? g_state.hello.process_id : 0;
  info->openxr_host_application_name = g_state.client >= 0 ? g_state.hello.application_name : "";
  info->openxr_host_graphics_api = g_state.client >= 0 ? "opengl-xlib" : "";
  info->openxr_protocol_version = kProtocolVersion;
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
  if (!g_state.socket_path.empty()) unlink(g_state.socket_path.c_str());
#endif
}

}  // namespace vrbridge
