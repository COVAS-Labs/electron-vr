#pragma once

#include <cstdint>
#include <string>

#include "bridge.h"

namespace vrbridge {

bool IsOpenXRApiLayerInstalled(bool* enabled, std::string* manifest_path);
bool InitializeOpenXRCompanion(const InitializeOptions& options, std::string* error_message);
bool SubmitOpenXRCompanionFrame(uint64_t shared_handle, std::string* error_message);
bool SetOpenXRCompanionPlacement(const OverlayPlacement& placement, std::string* error_message);
bool SetOpenXRCompanionVisible(bool visible, std::string* error_message);
bool SetOpenXRCompanionSizeMeters(float size_meters, std::string* error_message);
bool SetOpenXRCompanionCurvature(float curvature, std::string* error_message);
void PopulateOpenXRCompanionRuntimeInfo(RuntimeInfo* runtime_info);
void ShutdownOpenXRCompanion();

}  // namespace vrbridge
