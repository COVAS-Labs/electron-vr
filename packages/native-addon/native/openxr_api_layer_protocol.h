#pragma once

#include <cstddef>
#include <cstdint>

namespace electron_vr::openxr_layer {

constexpr uint32_t kProtocolMagic = 0x45565258;  // EVRX
constexpr uint32_t kProtocolVersion = 3;
constexpr uint32_t kTextureSlotCount = 3;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\electron-vr-openxr-overlay-v3";
constexpr wchar_t kHostPresenceMappingPrefix[] = L"Local\\electron-vr-openxr-host-v3-";

enum class GraphicsApi : uint32_t {
  kUnknown = 0,
  kD3D11 = 1,
  kD3D12 = 2,
  kVulkan = 3,
  kOpenGL = 4,
};

enum class PlacementMode : uint32_t {
  kHead = 0,
  kWorld = 1,
};

enum class TextureTransport : uint32_t {
  kCopiedRing = 0,
  kDirectElectronTexture = 1,
};

#pragma pack(push, 1)

struct AdapterLuid {
  uint32_t low_part = 0;
  int32_t high_part = 0;
};

struct LayerHello {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kProtocolVersion;
  uint32_t size = sizeof(LayerHello);
  uint32_t process_id = 0;
  GraphicsApi graphics_api = GraphicsApi::kUnknown;
  AdapterLuid adapter_luid;
  char application_name[128] = {};
};

struct TextureSlot {
  uint64_t handle = 0;
  uint64_t fence_handle = 0;
  uint64_t fence_value = 0;
  uint64_t sequence = 0;
};

struct OverlaySnapshot {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kProtocolVersion;
  uint32_t size = sizeof(OverlaySnapshot);
  TextureTransport texture_transport = TextureTransport::kCopiedRing;
  uint64_t revision = 0;
  uint64_t connection_id = 0;
  uint64_t texture_generation = 0;
  uint64_t latest_sequence = 0;
  uint32_t latest_slot = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dxgi_format = 0;
  uint32_t visible = 0;
  PlacementMode placement_mode = PlacementMode::kHead;
  float position[3] = {0.0f, 0.0f, -1.2f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float size_meters = 1.0f;
  float curvature = 0.0f;
  TextureSlot slots[kTextureSlotCount];
};

#pragma pack(pop)

struct alignas(8) HostPresence {
  LayerHello hello;
  uint32_t reserved = 0;
  volatile int64_t acknowledged_connection_id = 0;
  volatile int64_t consumed_sequence = 0;
  volatile int64_t opened_connection_id = 0;
  volatile int64_t opened_sequences[kTextureSlotCount] = {};
};

static_assert(sizeof(AdapterLuid) == 8, "Adapter LUID wire layout changed");
static_assert(sizeof(LayerHello) == 156, "Layer hello wire layout changed");
static_assert(offsetof(HostPresence, consumed_sequence) % 8 == 0, "Host acknowledgement must be 8-byte aligned");

}  // namespace electron_vr::openxr_layer
