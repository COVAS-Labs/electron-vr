#pragma once

#include <cstdint>
#include <vector>

#include "bridge.h"

namespace vrbridge {

struct CurvedQuadSegment {
  Vector3 position;
  Quaternion rotation;
  float width_meters = 0.0f;
  float height_meters = 0.0f;
  float u_min = 0.0f;
  float u_max = 1.0f;
  float v_min = 0.0f;
  float v_max = 1.0f;
};

struct CurvedQuadLayout {
  std::vector<CurvedQuadSegment> segments;
  uint32_t horizontal_segments = 1;
  uint32_t vertical_segments = 1;
  float width_meters = 0.0f;
  float height_meters = 0.0f;
};

float ComputeOverlayHeightMeters(uint32_t width, uint32_t height, float width_meters);
CurvedQuadLayout BuildCurvedQuadLayout(
  uint32_t frame_width,
  uint32_t frame_height,
  float width_meters,
  const OverlayCurvature& curvature);

}  // namespace vrbridge
