#pragma once

#include <cstdint>
#include <string>

#include "bridge.h"

namespace vrbridge {

bool InitializeMockBackend(const InitializeOptions& options, std::string* error_message);
bool SubmitMockFrameWindows(uint64_t shared_handle, std::string* error_message);
bool SubmitMockFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message);
bool SubmitMockSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message);
bool SetMockPlacement(const OverlayPlacement& placement, std::string* error_message);
bool SetMockVisible(bool visible, std::string* error_message);
bool SetMockSizeMeters(float size_meters, std::string* error_message);
void ShutdownMockBackend();

}  // namespace vrbridge
