#include "curved_geometry.h"

#include <algorithm>
#include <cmath>

namespace vrbridge {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxErrorMeters = 0.004f;
constexpr uint32_t kMinCurvedAxisSegments = 4;
constexpr uint32_t kMaxAxisSegments = 96;

float ClampFloat(float value, float min_value, float max_value) {
  return std::max(min_value, std::min(max_value, value));
}

Quaternion NormalizeQuaternion(const Quaternion& value) {
  const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
  if (!std::isfinite(length) || length <= 0.0f) {
    return Quaternion{};
  }

  Quaternion result;
  result.x = value.x / length;
  result.y = value.y / length;
  result.z = value.z / length;
  result.w = value.w / length;
  return result;
}

Vector3 NormalizeVector(const Vector3& value) {
  const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (!std::isfinite(length) || length <= 0.0f) {
    return Vector3{};
  }

  Vector3 result;
  result.x = value.x / length;
  result.y = value.y / length;
  result.z = value.z / length;
  return result;
}

Vector3 Cross(const Vector3& left, const Vector3& right) {
  Vector3 result;
  result.x = left.y * right.z - left.z * right.y;
  result.y = left.z * right.x - left.x * right.z;
  result.z = left.x * right.y - left.y * right.x;
  return result;
}

float Dot(const Vector3& left, const Vector3& right) {
  return left.x * right.x + left.y * right.y + left.z * right.z;
}

Quaternion QuaternionFromBasis(const Vector3& right, const Vector3& up, const Vector3& forward) {
  const float m00 = right.x;
  const float m01 = up.x;
  const float m02 = forward.x;
  const float m10 = right.y;
  const float m11 = up.y;
  const float m12 = forward.y;
  const float m20 = right.z;
  const float m21 = up.z;
  const float m22 = forward.z;

  Quaternion result;
  const float trace = m00 + m11 + m22;
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    result.w = 0.25f * s;
    result.x = (m21 - m12) / s;
    result.y = (m02 - m20) / s;
    result.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    result.w = (m21 - m12) / s;
    result.x = 0.25f * s;
    result.y = (m01 + m10) / s;
    result.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    result.w = (m02 - m20) / s;
    result.x = (m01 + m10) / s;
    result.y = 0.25f * s;
    result.z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    result.w = (m10 - m01) / s;
    result.x = (m02 + m20) / s;
    result.y = (m12 + m21) / s;
    result.z = 0.25f * s;
  }

  return NormalizeQuaternion(result);
}

uint32_t ComputeSegmentsForAxis(float span_meters, bool has_radius, float radius_meters) {
  if (!has_radius || !std::isfinite(radius_meters) || radius_meters <= 0.0f || !std::isfinite(span_meters) || span_meters <= 0.0f) {
    return 1;
  }

  const float error_ratio = ClampFloat(1.0f - (kMaxErrorMeters / radius_meters), -1.0f, 1.0f);
  const float max_delta = 2.0f * std::acos(error_ratio);
  if (!std::isfinite(max_delta) || max_delta <= 0.0f) {
    return kMaxAxisSegments;
  }

  const float total_angle = std::abs(span_meters / radius_meters);
  const uint32_t segments = static_cast<uint32_t>(std::ceil(total_angle / max_delta));
  return std::clamp(std::max(segments, kMinCurvedAxisSegments), kMinCurvedAxisSegments, kMaxAxisSegments);
}

struct SurfaceSample {
  Vector3 position;
  Vector3 tangent_x;
  Vector3 tangent_y;
};

SurfaceSample SampleSurface(
  float x,
  float y,
  const OverlayCurvature& curvature,
  float width_meters,
  float height_meters) {
  (void)width_meters;
  (void)height_meters;

  SurfaceSample sample;
  sample.position.x = x;
  sample.position.y = y;
  sample.position.z = 0.0f;
  sample.tangent_x = {1.0f, 0.0f, 0.0f};
  sample.tangent_y = {0.0f, 1.0f, 0.0f};

  if (curvature.has_horizontal) {
    const float theta = x / curvature.horizontal;
    sample.position.x = curvature.horizontal * std::sin(theta);
    sample.position.z += curvature.horizontal * (1.0f - std::cos(theta));
    sample.tangent_x = {std::cos(theta), 0.0f, std::sin(theta)};
  }

  if (curvature.has_vertical) {
    const float phi = y / curvature.vertical;
    sample.position.y = curvature.vertical * std::sin(phi);
    sample.position.z += curvature.vertical * (1.0f - std::cos(phi));
    sample.tangent_y = {0.0f, std::cos(phi), std::sin(phi)};
  }

  return sample;
}

CurvedQuadSegment BuildSegment(
  float x0,
  float x1,
  float y0,
  float y1,
  float u0,
  float u1,
  float v0,
  float v1,
  float width_meters,
  float height_meters,
  const OverlayCurvature& curvature) {
  const float xc = 0.5f * (x0 + x1);
  const float yc = 0.5f * (y0 + y1);

  const SurfaceSample center = SampleSurface(xc, yc, curvature, width_meters, height_meters);
  const SurfaceSample left = SampleSurface(x0, yc, curvature, width_meters, height_meters);
  const SurfaceSample right = SampleSurface(x1, yc, curvature, width_meters, height_meters);
  const SurfaceSample bottom = SampleSurface(xc, y0, curvature, width_meters, height_meters);
  const SurfaceSample top = SampleSurface(xc, y1, curvature, width_meters, height_meters);

  Vector3 right_axis = NormalizeVector(center.tangent_x);
  Vector3 up_axis = NormalizeVector(center.tangent_y);
  Vector3 forward_axis = NormalizeVector(Cross(right_axis, up_axis));
  if (Dot(forward_axis, forward_axis) <= 0.0f) {
    forward_axis = {0.0f, 0.0f, 1.0f};
  }
  up_axis = NormalizeVector(Cross(forward_axis, right_axis));

  CurvedQuadSegment segment;
  segment.position = center.position;
  segment.rotation = QuaternionFromBasis(right_axis, up_axis, forward_axis);
  segment.width_meters = std::sqrt(
    (right.position.x - left.position.x) * (right.position.x - left.position.x) +
    (right.position.y - left.position.y) * (right.position.y - left.position.y) +
    (right.position.z - left.position.z) * (right.position.z - left.position.z));
  segment.height_meters = std::sqrt(
    (top.position.x - bottom.position.x) * (top.position.x - bottom.position.x) +
    (top.position.y - bottom.position.y) * (top.position.y - bottom.position.y) +
    (top.position.z - bottom.position.z) * (top.position.z - bottom.position.z));
  segment.u_min = u0;
  segment.u_max = u1;
  segment.v_min = v0;
  segment.v_max = v1;
  return segment;
}

}  // namespace

float ComputeOverlayHeightMeters(uint32_t width, uint32_t height, float width_meters) {
  if (width == 0 || height == 0 || !std::isfinite(width_meters) || width_meters <= 0.0f) {
    return width_meters;
  }

  return width_meters * (static_cast<float>(height) / static_cast<float>(width));
}

CurvedQuadLayout BuildCurvedQuadLayout(
  uint32_t frame_width,
  uint32_t frame_height,
  float width_meters,
  const OverlayCurvature& curvature) {
  CurvedQuadLayout layout;
  layout.width_meters = width_meters;
  layout.height_meters = ComputeOverlayHeightMeters(frame_width, frame_height, width_meters);
  layout.horizontal_segments = ComputeSegmentsForAxis(layout.width_meters, curvature.has_horizontal, curvature.horizontal);
  layout.vertical_segments = ComputeSegmentsForAxis(layout.height_meters, curvature.has_vertical, curvature.vertical);

  const float x_step = layout.width_meters / static_cast<float>(layout.horizontal_segments);
  const float y_step = layout.height_meters / static_cast<float>(layout.vertical_segments);
  const float u_step = 1.0f / static_cast<float>(layout.horizontal_segments);
  const float v_step = 1.0f / static_cast<float>(layout.vertical_segments);
  const float x_min = -0.5f * layout.width_meters;
  const float y_min = -0.5f * layout.height_meters;

  layout.segments.reserve(static_cast<size_t>(layout.horizontal_segments) * static_cast<size_t>(layout.vertical_segments));
  for (uint32_t y_index = 0; y_index < layout.vertical_segments; ++y_index) {
    const float y0 = y_min + static_cast<float>(y_index) * y_step;
    const float y1 = y0 + y_step;
    const float v0 = static_cast<float>(y_index) * v_step;
    const float v1 = v0 + v_step;
    for (uint32_t x_index = 0; x_index < layout.horizontal_segments; ++x_index) {
      const float x0 = x_min + static_cast<float>(x_index) * x_step;
      const float x1 = x0 + x_step;
      const float u0 = static_cast<float>(x_index) * u_step;
      const float u1 = u0 + u_step;
      layout.segments.push_back(BuildSegment(x0, x1, y0, y1, u0, u1, v0, v1, layout.width_meters, layout.height_meters, curvature));
    }
  }

  return layout;
}

}  // namespace vrbridge
