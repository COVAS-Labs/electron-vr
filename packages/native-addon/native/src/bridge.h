#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime_probe.h"

namespace vrbridge {

enum class OverlayPlacementMode {
  kHead = 0,
  kWorld
};

struct Vector3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Quaternion {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct OverlayPlacement {
  OverlayPlacementMode mode = OverlayPlacementMode::kHead;
  Vector3 position;
  Quaternion rotation;
};

struct InitializeOptions {
  std::string name;
  uint32_t width = 0;
  uint32_t height = 0;
  float size_meters = 1.0f;
  bool visible = true;
  OverlayPlacement placement;
};

struct LinuxPlaneInfo {
  int fd = -1;
  uint32_t stride = 0;
  uint32_t offset = 0;
  uint64_t size = 0;
};

struct LinuxTextureInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  std::string pixel_format;
  std::string modifier;
  std::vector<LinuxPlaneInfo> planes;
};

struct SoftwareFrameInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba_pixels;
};

class BridgeState {
 public:
  RuntimeInfo GetRuntimeInfo() const;
  bool Initialize(const InitializeOptions& options);
  bool SubmitSharedTextureWindows(uint64_t shared_handle);
  bool SubmitSharedTextureLinux(const LinuxTextureInfo& texture_info);
  bool SubmitSoftwareFrameWindows(const SoftwareFrameInfo& frame_info);
  bool SubmitSoftwareFrameLinux(const SoftwareFrameInfo& frame_info);
  bool SetOverlayPlacement(const OverlayPlacement& placement);
  bool SetOverlayVisible(bool visible);
  bool SetOverlaySizeMeters(float size_meters);
  void Shutdown();
  bool IsInitialized() const;
  std::string GetLastError() const;

 private:
  void SetLastError(std::string message);

  RuntimeInfo runtime_info_ = ProbeRuntime();
  InitializeOptions options_;
  std::string last_error_;
  bool initialized_ = false;
  uint64_t frame_count_ = 0;
};

BridgeState& GetBridgeState();

}  // namespace vrbridge
