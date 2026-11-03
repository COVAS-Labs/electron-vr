#include "../src/openvr_backend.h"
#include "openvr.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  vrbridge::InitializeOptions options{};
  options.name = "electron-vr-raw-upload-regression";
  options.width = 128; options.height = 64; options.size_meters = 1.0f;
  options.visible = true;
  options.placement.position.z = -1.0f;
  options.placement.rotation.w = 1.0f;
  std::string error;
  if (!vrbridge::InitializeOpenVRBackend(options, &error)) {
    std::printf("FAIL initialize: %s\n", error.c_str());
    return 1;
  }
  vrbridge::SoftwareFrameInfo frame{};
  frame.width = options.width; frame.height = options.height;
  frame.rgba_pixels.resize(frame.width * frame.height * 4);
  for (unsigned i = 0; i < 1600; ++i) {
    for (size_t offset = 0; offset < frame.rgba_pixels.size(); offset += 4) {
      frame.rgba_pixels[offset] = i == 1599 ? 0 : static_cast<uint8_t>(i);
      frame.rgba_pixels[offset + 1] = 255;
      frame.rgba_pixels[offset + 2] = 0;
      frame.rgba_pixels[offset + 3] = 255;
    }
    if (!vrbridge::SubmitOpenVRSoftwareFrame(frame, &error)) {
      std::printf("FAIL frame=%u: %s\n", i, error.c_str());
      vrbridge::ShutdownOpenVRBackend();
      return 2;
    }
    if ((i + 1) % 200 == 0) std::printf("raw uploads completed=%u\n", i + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
  for (uint64_t generation = 0; generation < 256 && !handle; ++generation) {
    for (uint64_t index = 0; index < 64; ++index) {
      const auto candidate = (generation << 32) | index;
      char name[256]{};
      vr::EVROverlayError result{};
      vr::VROverlay()->GetOverlayName(candidate, name, sizeof(name), &result);
      if (!result && std::string(name) == options.name) { handle = candidate; break; }
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  std::vector<uint8_t> actual(frame.rgba_pixels.size());
  uint32_t width = 0, height = 0;
  const auto result = vr::VROverlay()->GetOverlayImageData(handle, actual.data(), static_cast<uint32_t>(actual.size()), &width, &height);
  const bool valid = result == vr::VROverlayError_None && width == frame.width && height == frame.height && actual == frame.rgba_pixels;
  std::printf("1600 raw uploads + runtime image readback: %s error=%d size=%ux%u\n", valid ? "PASS" : "FAIL", result, width, height);
  vrbridge::ShutdownOpenVRBackend();
  return valid ? 0 : 3;
}
