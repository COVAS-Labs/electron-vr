#pragma once

#include <dxgiformat.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace electron_vr {

inline int64_t SelectOverlaySwapchainFormat(int64_t source, const std::vector<int64_t>& supported) {
  const auto has = [&](int64_t format) {
    return std::find(supported.begin(), supported.end(), format) != supported.end();
  };
  if (has(source)) return source;
  // Chromium's encoded UI pixels can be copied unchanged into the matching
  // sRGB format. Never substitute RGBA for BGRA or an unrelated pixel layout.
  const int64_t srgb = source == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB :
                       source == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB :
                       DXGI_FORMAT_UNKNOWN;
  return srgb != DXGI_FORMAT_UNKNOWN && has(srgb) ? srgb : DXGI_FORMAT_UNKNOWN;
}

}  // namespace electron_vr
