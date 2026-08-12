#pragma once

#include <string>

#include "bridge.h"

namespace vrbridge {

bool IsOpenXRApiLayerInstalledLinux(bool* enabled, std::string* manifest_path);
bool InitializeOpenXRCompanionLinux(const InitializeOptions& options, std::string* error_message);
bool SubmitOpenXRCompanionFrameLinux(const LinuxTextureInfo& texture, std::string* error_message);
bool SetOpenXRCompanionPlacementLinux(const OverlayPlacement& placement, std::string* error_message);
bool SetOpenXRCompanionVisibleLinux(bool visible, std::string* error_message);
bool SetOpenXRCompanionSizeMetersLinux(float size_meters, std::string* error_message);
bool SetOpenXRCompanionCurvatureLinux(float curvature, std::string* error_message);
void PopulateOpenXRCompanionRuntimeInfoLinux(RuntimeInfo* runtime_info);
void ShutdownOpenXRCompanionLinux();

}  // namespace vrbridge
