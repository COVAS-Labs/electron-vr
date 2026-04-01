#pragma once

#include <cstdint>
#include <string>

#include "bridge.h"

namespace vrbridge {

bool InitializeOpenVRBackend(const InitializeOptions& options, std::string* error_message);
bool SubmitOpenVRFrameWindows(uint64_t shared_handle, std::string* error_message);
bool SubmitOpenVRFrameLinux(const LinuxTextureInfo& texture_info, std::string* error_message);
bool SubmitOpenVRSoftwareFrame(const SoftwareFrameInfo& frame_info, std::string* error_message);
bool SetOpenVRPlacement(const OverlayPlacement& placement, std::string* error_message);
bool SetOpenVRVisible(bool visible, std::string* error_message);
bool SetOpenVRSizeMeters(float size_meters, std::string* error_message);
void PopulateOpenVRRuntimeInfo(RuntimeInfo* runtime_info);
void ShutdownOpenVRBackend();

}  // namespace vrbridge
