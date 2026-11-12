#pragma once

#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace electron_vr {

inline XrSpace ProjectionSpace(const XrFrameEndInfo& frame) {
  if (frame.layerCount && !frame.layers) return XR_NULL_HANDLE;
  for (uint32_t i = 0; i < frame.layerCount; ++i) {
    const auto* layer = frame.layers[i];
    if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && layer->space) {
      return layer->space;
    }
  }
  return XR_NULL_HANDLE;
}

// The anchor is captured once, not followed each frame. Application-space
// replacement is a recenter signal; it is never used as a head-locked base.
class SeatedAnchor {
 public:
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    valid_ = false;
    changes_.clear();
  }

  void ReferenceChanged(XrTime time) {
    std::lock_guard<std::mutex> lock(mutex_);
    changes_.push_back(time);
  }

  bool Apply(XrSpace view, XrSpace base, XrSpace application_space, XrTime time,
             PFN_xrLocateSpace locate, XrPosef* placement) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!view || !base || !application_space || !locate || !placement || time <= 0) return false;
    const auto due = [time](XrTime change) { return change <= time; };
    if (std::any_of(changes_.begin(), changes_.end(), due)) valid_ = false;
    changes_.erase(std::remove_if(changes_.begin(), changes_.end(), due), changes_.end());
    if (base_ != base || application_space_ != application_space) valid_ = false;
    if (!valid_) {
      XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
      if (XR_FAILED(locate(view, base, time, &head))) return false;
      const auto required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
      if ((head.locationFlags & required) != required) return false;
      const auto& q = head.pose.orientation;
      const auto& p = head.pose.position;
      const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
      if (!std::isfinite(norm) || norm < 0.000001f ||
          !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
      const float forward_x = 2.f * (q.x*q.z + q.w*q.y) / norm;
      const float forward_z = 1.f - 2.f * (q.x*q.x + q.y*q.y) / norm;
      if (forward_x*forward_x + forward_z*forward_z < 0.000001f) return false;
      yaw_ = std::atan2(forward_x, forward_z);
      position_ = p;
      base_ = base;
      application_space_ = application_space;
      valid_ = true;
    }
    const float s = std::sin(yaw_), c = std::cos(yaw_);
    const auto p = placement->position;
    placement->position = {position_.x + c*p.x + s*p.z, position_.y + p.y,
                           position_.z - s*p.x + c*p.z};
    const float sy = std::sin(yaw_ / 2.f), cy = std::cos(yaw_ / 2.f);
    const auto q = placement->orientation;
    placement->orientation = {cy*q.x + sy*q.z, cy*q.y + sy*q.w,
                              cy*q.z - sy*q.x, cy*q.w - sy*q.y};
    return true;
  }

 private:
  std::mutex mutex_;
  bool valid_ = false;
  XrSpace base_ = XR_NULL_HANDLE;
  XrSpace application_space_ = XR_NULL_HANDLE;
  XrVector3f position_{};
  float yaw_ = 0;
  std::vector<XrTime> changes_;
};
}  // namespace electron_vr
