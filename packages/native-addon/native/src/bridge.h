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

struct OverlayCurvature {
  bool has_horizontal = false;
  float horizontal = 0.0f;
  bool has_vertical = false;
  float vertical = 0.0f;
};

struct InitializeOptions {
  std::string name;
  uint32_t width = 0;
  uint32_t height = 0;
  float size_meters = 1.0f;
  bool visible = true;
  OverlayPlacement placement;
  OverlayCurvature curvature;
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

struct SharedTextureSubmission {
  uint64_t windows_handle = 0;
  LinuxTextureInfo linux_texture;
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
  bool SubmitSharedTexture(const SharedTextureSubmission& texture_submission);
  bool SubmitSoftwareFrame(const SoftwareFrameInfo& frame_info);
  bool SetOverlayPlacement(const OverlayPlacement& placement);
  bool SetOverlayCurvature(const OverlayCurvature& curvature);
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
