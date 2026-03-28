#pragma once

#include <cstdint>
#include <string>

#include "bridge.h"

namespace vrbridge {

bool InitializeOpenXRBackend(const InitializeOptions& options, std::string* error_message);
bool SubmitOpenXRFrameWindows(uint64_t shared_handle, std::string* error_message);
bool SubmitOpenXRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message);
void ShutdownOpenXRBackend();

}  // namespace vrbridge
