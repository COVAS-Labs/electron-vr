#include <cstdint>
#include <string>
#include <vector>

#include <napi.h>

#include "bridge.h"

namespace vrbridge {

namespace {

Vector3 ReadVector3(const Napi::Value& value, const char* context) {
  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), std::string(context) + " must be an object.");
  }

  const Napi::Object object = value.As<Napi::Object>();
  Vector3 vector;
  vector.x = object.Get("x").As<Napi::Number>().FloatValue();
  vector.y = object.Get("y").As<Napi::Number>().FloatValue();
  vector.z = object.Get("z").As<Napi::Number>().FloatValue();
  return vector;
}

Quaternion ReadQuaternion(const Napi::Value& value, const char* context) {
  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), std::string(context) + " must be an object.");
  }

  const Napi::Object object = value.As<Napi::Object>();
  Quaternion quaternion;
  quaternion.x = object.Get("x").As<Napi::Number>().FloatValue();
  quaternion.y = object.Get("y").As<Napi::Number>().FloatValue();
  quaternion.z = object.Get("z").As<Napi::Number>().FloatValue();
  quaternion.w = object.Get("w").As<Napi::Number>().FloatValue();
  return quaternion;
}

OverlayPlacement ReadOverlayPlacement(const Napi::Value& value) {
  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), "placement must be an object.");
  }

  const Napi::Object object = value.As<Napi::Object>();
  const Napi::Value mode_value = object.Get("mode");
  const std::string mode = mode_value.As<Napi::String>().Utf8Value();

  OverlayPlacement placement;
  if (mode == "head") {
    placement.mode = OverlayPlacementMode::kHead;
  } else if (mode == "world") {
    placement.mode = OverlayPlacementMode::kWorld;
  } else {
    throw Napi::RangeError::New(value.Env(), "placement.mode must be 'head' or 'world'.");
  }

  placement.position = ReadVector3(object.Get("position"), "placement.position");
  placement.rotation = ReadQuaternion(object.Get("rotation"), "placement.rotation");
  return placement;
}

#if defined(_WIN32)
uint64_t ReadWindowsHandle(const Napi::Value& value) {
  if (value.IsBigInt()) {
    bool lossless = false;
    const uint64_t handle = value.As<Napi::BigInt>().Uint64Value(&lossless);
    if (!lossless) {
      throw Napi::Error::New(value.Env(), "BigInt handle could not be represented losslessly.");
    }

    return handle;
  }

  if (!value.IsBuffer()) {
    throw Napi::TypeError::New(value.Env(), "submitSharedTexture expects a Buffer or BigInt on Windows.");
  }

  const Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
  if (buffer.Length() == 0 || buffer.Length() > sizeof(uint64_t)) {
    throw Napi::RangeError::New(value.Env(), "Windows shared handle buffer has an unsupported length.");
  }

  uint64_t result = 0;
  for (size_t index = 0; index < buffer.Length(); ++index) {
    result |= static_cast<uint64_t>(buffer[index]) << (index * 8U);
  }

  return result;
}
#endif

#if defined(__linux__)
LinuxPlaneInfo ReadLinuxPlaneInfo(const Napi::Value& value) {
  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), "Linux texture plane must be an object.");
  }

  const Napi::Object plane_object = value.As<Napi::Object>();
  LinuxPlaneInfo plane_info;
  plane_info.fd = plane_object.Get("fd").As<Napi::Number>().Int32Value();
  plane_info.stride = plane_object.Get("stride").As<Napi::Number>().Uint32Value();
  plane_info.offset = plane_object.Get("offset").As<Napi::Number>().Uint32Value();

  const Napi::Value size_value = plane_object.Get("size");
  if (size_value.IsBigInt()) {
    bool lossless = false;
    plane_info.size = size_value.As<Napi::BigInt>().Uint64Value(&lossless);
    if (!lossless) {
      throw Napi::RangeError::New(value.Env(), "Plane size could not be represented losslessly.");
    }
  } else if (size_value.IsNumber()) {
    plane_info.size = size_value.As<Napi::Number>().Int64Value();
  }

  return plane_info;
}

LinuxTextureInfo ReadLinuxTextureInfo(const Napi::Value& value) {
  if (value.IsNumber()) {
    LinuxTextureInfo fallback_info;
    LinuxPlaneInfo plane_info;
    plane_info.fd = value.As<Napi::Number>().Int32Value();
    fallback_info.planes.push_back(plane_info);
    return fallback_info;
  }

  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), "submitSharedTexture expects a texture info object or numeric file descriptor on Linux.");
  }

  const Napi::Object texture_object = value.As<Napi::Object>();
  LinuxTextureInfo texture_info;

  const Napi::Value coded_size_value = texture_object.Get("codedSize");
  if (coded_size_value.IsObject()) {
    const Napi::Object coded_size_object = coded_size_value.As<Napi::Object>();
    texture_info.width = coded_size_object.Get("width").As<Napi::Number>().Uint32Value();
    texture_info.height = coded_size_object.Get("height").As<Napi::Number>().Uint32Value();
  }

  const Napi::Value pixel_format_value = texture_object.Get("pixelFormat");
  if (pixel_format_value.IsString()) {
    texture_info.pixel_format = pixel_format_value.As<Napi::String>().Utf8Value();
  }

  const Napi::Value modifier_value = texture_object.Get("modifier");
  if (modifier_value.IsString()) {
    texture_info.modifier = modifier_value.As<Napi::String>().Utf8Value();
  }

  const Napi::Value planes_value = texture_object.Get("planes");
  if (planes_value.IsArray()) {
    const Napi::Array planes_array = planes_value.As<Napi::Array>();
    const uint32_t plane_count = planes_array.Length();
    texture_info.planes.reserve(plane_count);
    for (uint32_t index = 0; index < plane_count; ++index) {
      texture_info.planes.push_back(ReadLinuxPlaneInfo(planes_array.Get(index)));
    }
  }

  if (texture_info.planes.empty()) {
    const Napi::Value legacy_handle_value = texture_object.Get("sharedTextureHandle");
    if (legacy_handle_value.IsObject()) {
      const Napi::Object legacy_handle_object = legacy_handle_value.As<Napi::Object>();
      const Napi::Value native_pixmap_value = legacy_handle_object.Get("nativePixmap");
      if (native_pixmap_value.IsObject()) {
        const Napi::Object native_pixmap_object = native_pixmap_value.As<Napi::Object>();
        const Napi::Value legacy_planes_value = native_pixmap_object.Get("planes");
        if (legacy_planes_value.IsArray()) {
          const Napi::Array legacy_planes_array = legacy_planes_value.As<Napi::Array>();
          const uint32_t plane_count = legacy_planes_array.Length();
          texture_info.planes.reserve(plane_count);
          for (uint32_t index = 0; index < plane_count; ++index) {
            texture_info.planes.push_back(ReadLinuxPlaneInfo(legacy_planes_array.Get(index)));
          }
        }
      }
    }
  }

  return texture_info;
}
#endif

SharedTextureSubmission ReadSharedTextureSubmission(const Napi::Value& value) {
  SharedTextureSubmission submission;
#if defined(_WIN32)
  submission.windows_handle = ReadWindowsHandle(value);
#elif defined(__linux__)
  submission.linux_texture = ReadLinuxTextureInfo(value);
#else
  throw Napi::Error::New(value.Env(), "submitSharedTexture is not supported on this platform.");
#endif
  return submission;
}

SoftwareFrameInfo ReadSoftwareFrameInfo(const Napi::Value& value, const char* method_name) {
  if (!value.IsObject()) {
    throw Napi::TypeError::New(value.Env(), std::string(method_name) + " expects a frame object.");
  }

  const Napi::Object frame_object = value.As<Napi::Object>();
  const Napi::Value width_value = frame_object.Get("width");
  const Napi::Value height_value = frame_object.Get("height");
  const Napi::Value pixels_value = frame_object.Get("rgbaPixels");

  if (!width_value.IsNumber() || !height_value.IsNumber() || !pixels_value.IsBuffer()) {
    throw Napi::TypeError::New(value.Env(), std::string(method_name) + " expects width, height, and rgbaPixels.");
  }

  SoftwareFrameInfo frame_info;
  frame_info.width = width_value.As<Napi::Number>().Uint32Value();
  frame_info.height = height_value.As<Napi::Number>().Uint32Value();

  const Napi::Buffer<uint8_t> pixel_buffer = pixels_value.As<Napi::Buffer<uint8_t>>();
  frame_info.rgba_pixels.assign(pixel_buffer.Data(), pixel_buffer.Data() + pixel_buffer.Length());
  return frame_info;
}

Napi::Object RuntimeInfoToObject(Napi::Env env, const RuntimeInfo& info) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("platform", info.platform);
  result.Set("probeMode", info.probe_mode);
  result.Set("openxrAvailable", info.openxr_available);
  result.Set("openxrOverlayExtensionAvailable", info.openxr_overlay_extension_available);
  result.Set("openxrLinuxEglBindingAvailable", info.openxr_linux_egl_binding_available);
  result.Set("openxrWindowsD3D11BindingAvailable", info.openxr_windows_d3d11_binding_available);
  result.Set("openvrAvailable", info.openvr_available);
  result.Set("openvrRuntimeInstalled", info.openvr_runtime_installed);
  result.Set("openvrRuntimePath", info.openvr_runtime_path);
  result.Set("selectedBackend", BackendKindToString(info.selected_backend));
  return result;
}

}  // namespace

Napi::Value GetRuntimeInfoWrapped(const Napi::CallbackInfo& info) {
  return RuntimeInfoToObject(info.Env(), GetBridgeState().GetRuntimeInfo());
}

Napi::Value InitializeVRWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(info.Env(), "initializeVR expects an options object.");
  }

  const Napi::Object options = info[0].As<Napi::Object>();
  InitializeOptions native_options;

  const Napi::Value name_value = options.Get("name");
  const Napi::Value width_value = options.Get("width");
  const Napi::Value height_value = options.Get("height");
  const Napi::Value size_meters_value = options.Get("sizeMeters");
  const Napi::Value visible_value = options.Get("visible");
  const Napi::Value placement_value = options.Get("placement");

  if (!name_value.IsString() || !width_value.IsNumber() || !height_value.IsNumber() ||
      !size_meters_value.IsNumber() || !visible_value.IsBoolean() || !placement_value.IsObject()) {
    throw Napi::TypeError::New(info.Env(), "initializeVR options must include name, width, height, sizeMeters, visible, and placement.");
  }

  native_options.name = name_value.As<Napi::String>().Utf8Value();
  native_options.width = width_value.As<Napi::Number>().Uint32Value();
  native_options.height = height_value.As<Napi::Number>().Uint32Value();
  native_options.size_meters = size_meters_value.As<Napi::Number>().FloatValue();
  native_options.visible = visible_value.As<Napi::Boolean>().Value();
  native_options.placement = ReadOverlayPlacement(placement_value);

  return Napi::Boolean::New(info.Env(), GetBridgeState().Initialize(native_options));
}

Napi::Value SubmitSharedTextureWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1) {
    throw Napi::TypeError::New(info.Env(), "submitSharedTexture expects one argument.");
  }

  return Napi::Boolean::New(info.Env(), GetBridgeState().SubmitSharedTexture(ReadSharedTextureSubmission(info[0])));
}

Napi::Value SubmitSoftwareFrameWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1) {
    throw Napi::TypeError::New(info.Env(), "submitSoftwareFrame expects one argument.");
  }

  const SoftwareFrameInfo frame_info = ReadSoftwareFrameInfo(info[0], "submitSoftwareFrame");
  return Napi::Boolean::New(info.Env(), GetBridgeState().SubmitSoftwareFrame(frame_info));
}

Napi::Value ShutdownVRWrapped(const Napi::CallbackInfo& info) {
  GetBridgeState().Shutdown();
  return info.Env().Undefined();
}

Napi::Value SetOverlayPlacementWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1) {
    throw Napi::TypeError::New(info.Env(), "setOverlayPlacement expects one argument.");
  }

  return Napi::Boolean::New(info.Env(), GetBridgeState().SetOverlayPlacement(ReadOverlayPlacement(info[0])));
}

Napi::Value SetOverlayVisibleWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1 || !info[0].IsBoolean()) {
    throw Napi::TypeError::New(info.Env(), "setOverlayVisible expects a boolean.");
  }

  return Napi::Boolean::New(info.Env(), GetBridgeState().SetOverlayVisible(info[0].As<Napi::Boolean>().Value()));
}

Napi::Value SetOverlaySizeMetersWrapped(const Napi::CallbackInfo& info) {
  if (info.Length() != 1 || !info[0].IsNumber()) {
    throw Napi::TypeError::New(info.Env(), "setOverlaySizeMeters expects a number.");
  }

  return Napi::Boolean::New(info.Env(), GetBridgeState().SetOverlaySizeMeters(info[0].As<Napi::Number>().FloatValue()));
}

Napi::Value IsInitializedWrapped(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), GetBridgeState().IsInitialized());
}

Napi::Value GetLastErrorWrapped(const Napi::CallbackInfo& info) {
  const std::string last_error = GetBridgeState().GetLastError();
  if (last_error.empty()) {
    return info.Env().Null();
  }

  return Napi::String::New(info.Env(), last_error);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("getRuntimeInfo", Napi::Function::New(env, GetRuntimeInfoWrapped));
  exports.Set("initializeVR", Napi::Function::New(env, InitializeVRWrapped));
  exports.Set("submitSharedTexture", Napi::Function::New(env, SubmitSharedTextureWrapped));
  exports.Set("submitSoftwareFrame", Napi::Function::New(env, SubmitSoftwareFrameWrapped));
  exports.Set("setOverlayPlacement", Napi::Function::New(env, SetOverlayPlacementWrapped));
  exports.Set("setOverlayVisible", Napi::Function::New(env, SetOverlayVisibleWrapped));
  exports.Set("setOverlaySizeMeters", Napi::Function::New(env, SetOverlaySizeMetersWrapped));
  exports.Set("shutdownVR", Napi::Function::New(env, ShutdownVRWrapped));
  exports.Set("isInitialized", Napi::Function::New(env, IsInitializedWrapped));
  exports.Set("getLastError", Napi::Function::New(env, GetLastErrorWrapped));
  return exports;
}

NODE_API_MODULE(vr_bridge, Init)

}  // namespace vrbridge
