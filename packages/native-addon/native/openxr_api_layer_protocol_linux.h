#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace electron_vr::openxr_layer_linux {

constexpr uint32_t kProtocolMagic = 0x4556524c;  // EVRL
constexpr uint16_t kProtocolVersion = 4;
constexpr uint32_t kMaxPlanes = 4;
constexpr char kSocketName[] = "electron-vr-openxr-overlay-v4";
constexpr uint32_t kGraphicsBindingOpenGLXlib = 1000023001;
constexpr uint32_t kGraphicsBindingVulkan = 1000025000;  // Vulkan and Vulkan2 alias this value.

// Linux abstract Unix sockets are visible across mount namespaces when the
// processes share a network namespace (as Steam and Flatpak normally do).
inline socklen_t MakeSocketAddress(sockaddr_un* address) {
  *address = {};
  address->sun_family = AF_UNIX;
  static_assert(sizeof(kSocketName) + 21 < sizeof(address->sun_path));
  const int length = std::snprintf(address->sun_path + 1, sizeof(address->sun_path) - 1,
    "%s-%lu", kSocketName, static_cast<unsigned long>(geteuid()));
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length);
}

enum class MessageType : uint16_t {
  kHello = 1,
  kSnapshot = 2,
  kFrameAck = 3,
};

enum class PlacementMode : uint32_t {
  kHead = 0,
  kWorld = 1,
};

struct MessageHeader {
  uint32_t magic = kProtocolMagic;
  uint16_t version = kProtocolVersion;
  MessageType type = MessageType::kHello;
  uint32_t byte_size = 0;
  uint32_t fd_count = 0;
  uint64_t sequence = 0;
};

struct LayerHello {
  MessageHeader header;
  uint32_t process_id = 0;
  uint32_t graphics_binding = 0;
  char application_name[128] = {};
};

struct Plane {
  uint32_t stride = 0;
  uint32_t offset = 0;
  uint64_t size = 0;
};

struct OverlaySnapshot {
  MessageHeader header;
  uint64_t revision = 0;
  uint64_t generation = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t drm_format = 0;
  uint32_t plane_count = 0;
  uint64_t modifier = 0;
  uint32_t visible = 0;
  PlacementMode placement_mode = PlacementMode::kHead;
  float position[3] = {0.0f, 0.0f, -1.2f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float size_meters = 1.0f;
  float curvature = 0.0f;
  Plane planes[kMaxPlanes];
  uint32_t software_frame = 0;
};

struct FrameAck {
  MessageHeader header;
  uint64_t generation = 0;
  uint32_t consumed = 0;
  uint32_t reserved = 0;
};

static_assert(sizeof(MessageHeader) == 24, "Linux protocol header layout changed");

}  // namespace electron_vr::openxr_layer_linux
