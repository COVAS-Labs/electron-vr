#pragma once

#include <cstdint>
#include <string>

#include "bridge.h"

namespace vrbridge {

bool InitializeOpenXRBackend(const InitializeOptions& options, std::string* error_message);
bool SubmitOpenXRFrameWindows(uint64_t shared_handle, std::string* error_message);
bool SubmitOpenXRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message);
bool SetOpenXRPlacement(const OverlayPlacement& placement, std::string* error_message);
bool SetOpenXRVisible(bool visible, std::string* error_message);
bool SetOpenXRSizeMeters(float size_meters, std::string* error_message);
void ShutdownOpenXRBackend();

}  // namespace vrbridge
