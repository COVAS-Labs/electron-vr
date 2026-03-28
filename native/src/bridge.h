#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime_probe.h"

namespace vrbridge {

struct InitializeOptions {
  std::string name;
  uint32_t width = 0;
  uint32_t height = 0;
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
  bool SubmitFrameWindows(uint64_t shared_handle);
  bool SubmitFrameLinux(const LinuxTextureInfo& texture_info);
  bool SubmitSoftwareFrame(const SoftwareFrameInfo& frame_info);
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
