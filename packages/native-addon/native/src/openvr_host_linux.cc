#include "openvr_backend.h"
#include "../openxr_api_layer_protocol_linux.h"
#include "openvr.h"

#include <poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>
#include <sstream>
#include <thread>

namespace {
using namespace electron_vr::openxr_layer_linux;
using namespace vrbridge;
std::atomic<bool> stopping{false};
void Stop(int) { stopping.store(true); }

bool SteamVRRunning() {
  static std::filesystem::path server_executable;
  static std::chrono::steady_clock::time_point last_scan;
  std::error_code error;
  bool server_process = !server_executable.empty() &&
    std::filesystem::read_symlink(server_executable, error).filename() == "vrserver" && !error;
  if (!server_process && std::chrono::steady_clock::now() - last_scan < std::chrono::seconds(1)) return false;
  if (!server_process) {
    last_scan = std::chrono::steady_clock::now();
    error.clear();
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
      const std::string pid = entry.path().filename().string();
      if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
      struct stat details{};
      if (stat(entry.path().c_str(), &details) != 0 || details.st_uid != geteuid()) continue;
      const auto executable = std::filesystem::read_symlink(entry.path() / "exe", error);
      if (!error && executable.filename() == "vrserver") {
        server_process = true;
        server_executable = entry.path() / "exe";
        break;
      }
      error.clear();
    }
  }
  if (!server_process) return false;
  std::ifstream sockets("/proc/net/unix");
  std::string line;
  while (std::getline(sockets, line)) {
    std::istringstream fields(line);
    std::string number, refs, protocol, flags, type, state, inode, name;
    fields >> number >> refs >> protocol >> flags >> type >> state >> inode >> name;
    // Disconnected accepted sockets can retain the same name after vrserver
    // exits. Only a listening endpoint proves that SteamVR is still running.
    if (flags == "00010000" && state == "01" && name.find("SteamVR_Namespace") != std::string::npos) return true;
  }
  return false;
}

bool SameUser(int socket) {
  ucred peer{};
  socklen_t size = sizeof(peer);
  return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == geteuid();
}

bool ReceiveSnapshot(int socket, OverlaySnapshot* snapshot, int* fd) {
  char control[CMSG_SPACE(sizeof(int) * kMaxPlanes)]{};
  iovec data{snapshot, sizeof(*snapshot)};
  msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t size = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
  unsigned count = 0;
  for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    const size_t descriptors = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    const auto* values = reinterpret_cast<const int*>(CMSG_DATA(c));
    for (size_t i = 0; i < descriptors; ++i) {
      if (count++ == 0) *fd = values[i]; else close(values[i]);
    }
  }
  return size == sizeof(*snapshot) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
    snapshot->header.magic == kProtocolMagic && snapshot->header.version == kProtocolVersion &&
    snapshot->header.type == MessageType::kSnapshot && snapshot->header.byte_size == sizeof(*snapshot) &&
    snapshot->header.fd_count == count && count <= 1;
}

bool ApplySnapshot(const OverlaySnapshot& s, int fd, OverlaySnapshot* previous, bool* controls_applied, std::string* error) {
  OverlayPlacement placement;
  placement.mode = s.placement_mode == PlacementMode::kHead ? OverlayPlacementMode::kHead : OverlayPlacementMode::kWorld;
  placement.position = {s.position[0], s.position[1], s.position[2]};
  placement.rotation = {s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]};
  if ((!*controls_applied || s.placement_mode != previous->placement_mode ||
       std::memcmp(s.position, previous->position, sizeof(s.position)) ||
       std::memcmp(s.rotation, previous->rotation, sizeof(s.rotation))) && !SetOpenVRPlacement(placement, error)) return false;
  if ((!*controls_applied || s.visible != previous->visible) && !SetOpenVRVisible(s.visible != 0, error)) return false;
  if ((!*controls_applied || s.size_meters != previous->size_meters) && !SetOpenVRSizeMeters(s.size_meters, error)) return false;
  if ((!*controls_applied || s.curvature != previous->curvature) && !SetOpenVRCurvature(s.curvature, error)) return false;
  *previous = s;
  *controls_applied = true;
  if (fd < 0) return true;
  if (!s.width || !s.height || s.width > 16384 || s.height > 16384 || s.plane_count != 1 ||
      s.planes[0].stride < static_cast<uint64_t>(s.width) * 4 ||
      static_cast<uint64_t>(s.planes[0].offset) + static_cast<uint64_t>(s.planes[0].stride) * s.height > s.planes[0].size) return false;
  if (s.software_frame) {
    const uint64_t bytes = static_cast<uint64_t>(s.width) * s.height * 4;
    if (s.planes[0].offset || s.planes[0].stride != s.width * 4 || bytes != s.planes[0].size) return false;
    void* pixels = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) return false;
    SoftwareFrameInfo frame;
    frame.width = s.width;
    frame.height = s.height;
    const auto* begin = static_cast<const uint8_t*>(pixels);
    frame.rgba_pixels.assign(begin, begin + bytes);
    munmap(pixels, bytes);
    return SubmitOpenVRSoftwareFrame(frame, error);
  }
  LinuxTextureInfo texture;
  texture.width = s.width;
  texture.height = s.height;
  // DRM_FORMAT_ARGB8888 is little-endian BGRA; all other accepted frames are RGBA.
  texture.pixel_format = s.drm_format == 0x34325241 ? "bgra" : "rgba";
  texture.modifier = std::to_string(s.modifier);
  texture.planes.push_back({fd, s.planes[0].stride, s.planes[0].offset, s.planes[0].size});
  return SubmitOpenVRFrameLinux(texture, error);
}
}  // namespace

int main() {
  // SDK-launched SteamVR components must not keep the app's log pipes alive.
  fcntl(STDOUT_FILENO, F_SETFD, FD_CLOEXEC);
  fcntl(STDERR_FILENO, F_SETFD, FD_CLOEXEC);
  std::signal(SIGTERM, Stop);
  std::signal(SIGINT, Stop);
  std::signal(SIGPIPE, SIG_IGN);
  const int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  sockaddr_un address{};
  const auto length = MakeOpenVRRelayAddress(&address, true);
  if (server < 0) return 1;
  if (bind(server, reinterpret_cast<sockaddr*>(&address), length) != 0 || listen(server, 8) != 0) {
    close(server);
    return 0; // Another COVAS launch already owns the per-user helper.
  }
  // SteamVR's closed-source IPC can block during runtime teardown. Bound helper
  // shutdown even when its client cleanup cannot finish after the app exits.
  std::atomic<bool> finished{false};
  std::thread shutdown_watchdog([&] {
    while (!finished.load()) {
      if (stopping.load()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!finished.load()) std::_Exit(0);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  bool initialized = false;
  bool controls_applied = false;
  OverlaySnapshot previous{};
  int producer = -1;
  std::string error;
  auto next_init = std::chrono::steady_clock::now();
  while (!stopping.load()) {
    const auto now = std::chrono::steady_clock::now();
    const bool runtime_running = SteamVRRunning();
    if (initialized && !runtime_running) {
      // Do not call the SDK on a dead runtime: some client interfaces attempt
      // to reconnect and auto-launch vrserver during cleanup. OS process exit
      // releases our sockets/GPU resources; the app starts a fresh helper.
      std::_Exit(0);
    }
    if (initialized) {
      vr::VREvent_t event{};
      while (vr::VRSystem() && vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
        if (event.eventType == vr::VREvent_Quit) {
          vr::VRSystem()->AcknowledgeQuit_Exiting();
          std::_Exit(0);
        }
      }
    }
    if (!initialized && now >= next_init) {
      next_init = now + std::chrono::seconds(2);
      // Do not start SteamVR just because COVAS starts.
      if (SteamVRRunning() && vr::VR_IsHmdPresent()) {
        InitializeOptions options;
        options.name = "COVAS_NEXT_Host_Overlay";
        options.width = 1280; options.height = 720; options.size_meters = 0.5f;
        options.visible = false;
        // Use a background client for host-side overlay presentation, avoiding
        // the foreground application transition of an overlay-app launch.
        initialized = InitializeOpenVRBackend(options, &error, true);
        if (!initialized) ShutdownOpenVRBackend();
      }
    }
    pollfd descriptors[] = {{server, POLLIN, 0}, {producer, POLLIN, 0}};
    poll(descriptors, 2, 50);
    if (descriptors[0].revents & POLLIN) {
      const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (client >= 0) {
        if (SameUser(client) && initialized && SteamVRRunning()) {
          RuntimeInfo info;
          PopulateOpenVRRuntimeInfo(&info);
          HostPresence presence{};
          presence.hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHostPresence, sizeof(presence), 0, 0};
          presence.hello.header.sequence = info.openvr_texture_submission_count;
          presence.hello.process_id = info.openvr_scene_process_id;
          presence.hello.graphics_binding = kGraphicsBindingOpenVRRelay;
          std::strncpy(presence.hello.application_name, info.openvr_scene_application_name.c_str(), sizeof(presence.hello.application_name) - 1);
          std::strcpy(presence.runtime_name, "SteamVR");
          send(client, &presence, sizeof(presence), MSG_NOSIGNAL);
        }
        close(client);
      }
    }
    if (initialized && producer < 0) {
      const int client = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      sockaddr_un target{};
      const auto size = MakeOpenVRRelayAddress(&target, false);
      if (client >= 0 && connect(client, reinterpret_cast<sockaddr*>(&target), size) == 0 && SameUser(client)) {
        LayerHello hello{};
        hello.header = {kProtocolMagic, kProtocolVersion, MessageType::kHello, sizeof(hello), 0, 0};
        hello.process_id = getpid();
        hello.graphics_binding = kGraphicsBindingOpenVRRelay;
        std::strcpy(hello.application_name, "SteamVR host companion");
        if (send(client, &hello, sizeof(hello), MSG_NOSIGNAL) == sizeof(hello)) producer = client;
      }
      if (client >= 0 && producer != client) close(client);
    }
    if (producer >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      OverlaySnapshot snapshot{};
      int fd = -1;
      const bool received = ReceiveSnapshot(producer, &snapshot, &fd);
      if (!received) {
        close(producer); producer = -1;
        controls_applied = false;
        SetOpenVRVisible(false, &error);
      } else {
        const bool accepted = ApplySnapshot(snapshot, fd, &previous, &controls_applied, &error);
        if (fd >= 0) {
          FrameAck ack{};
          ack.header = {kProtocolMagic, kProtocolVersion, MessageType::kFrameAck, sizeof(ack), 0, snapshot.header.sequence};
          ack.generation = snapshot.generation;
          ack.consumed = accepted ? 1 : 0;
          send(producer, &ack, sizeof(ack), MSG_NOSIGNAL);
        }
        if (!accepted) std::cerr << "OpenVR host companion: " << error << '\n';
      }
      if (fd >= 0) close(fd);
    }
  }
  if (producer >= 0) close(producer);
  close(server);
  if (initialized) ShutdownOpenVRBackend();
  finished.store(true);
  shutdown_watchdog.join();
  return 0;
}
