#pragma once

#include <string>

namespace vrbridge {

struct OpenXRApiLayerStatus {
  bool installed = false;
  bool enabled = false;
  bool registered = false;
  bool requires_update = false;
  std::string manifest_path;
  std::string scope = "current-user (elevated OpenXR applications do not load HKCU layers)";
};

OpenXRApiLayerStatus GetOpenXRApiLayerStatus(const std::string& source_directory);
bool InstallOpenXRApiLayer(const std::string& source_directory, std::string* error);
bool SetOpenXRApiLayerEnabled(bool enabled, std::string* error);
bool UninstallOpenXRApiLayer(std::string* error);

}  // namespace vrbridge
