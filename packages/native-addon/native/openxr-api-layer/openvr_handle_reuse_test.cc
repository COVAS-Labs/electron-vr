#include "../src/openvr_backend.h"
#include "openvr.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <vector>

template<class T> struct Com {
  T* value = nullptr;
  ~Com() { if (value) value->Release(); }
  T** out() { return &value; }
  T* operator->() const { return value; }
};
static void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) { std::printf("FAIL %s hr=%08lx\n", operation, static_cast<unsigned long>(result)); ExitProcess(2); }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  vrbridge::InitializeOptions options{};
  options.name = "electron-vr-recycled-NT-handle-regression";
  options.width = 128; options.height = 64; options.size_meters = 1; options.visible = true;
  options.placement.position.z = -1;
  std::string error;
  if (!vrbridge::InitializeOpenVRBackend(options, &error)) { std::printf("FAIL %s\n", error.c_str()); return 1; }
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
    device.out(), nullptr, context.out()), "producer device");
  D3D11_TEXTURE2D_DESC description{};
  description.Width = options.width; description.Height = options.height;
  description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
  Com<ID3D11Texture2D> textures[2];
  HANDLE handles[2]{};
  for (unsigned i = 0; i < 2; ++i) {
    Check(device->CreateTexture2D(&description, nullptr, textures[i].out()), "producer texture");
    Com<ID3D11RenderTargetView> view;
    Check(device->CreateRenderTargetView(textures[i].value, nullptr, view.out()), "producer view");
    const float color[] = {i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, 0, 1};
    context->ClearRenderTargetView(view.value, color);
    Com<IDXGIResource1> resource;
    Check(textures[i]->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(resource.out())), "DXGI resource");
    Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handles[i]), "shared handle");
  }
  Com<ID3D11Query> completion;
  D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
  Check(device->CreateQuery(&query, completion.out()), "producer completion");
  context->End(completion.value); context->Flush();
  HRESULT ready = S_FALSE;
  for (unsigned i = 0; i < 30000 && ready == S_FALSE; ++i) { ready = context->GetData(completion.value, nullptr, 0, 0); if (ready == S_FALSE) Sleep(1); }
  if (ready != S_OK) return 2;
  HANDLE first = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), handles[0], GetCurrentProcess(), &first, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 2;
  if (!vrbridge::SubmitOpenVRFrameWindows(reinterpret_cast<uintptr_t>(first), &error)) { std::printf("FAIL red: %s\n", error.c_str()); return 3; }
  Sleep(100);
  CloseHandle(first);
  HANDLE second = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), handles[1], GetCurrentProcess(), &second, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 2;
  std::printf("first NT value=%p recycled NT value=%p\n", first, second);
  if (second != first) { std::printf("FAIL: numeric handle reuse was not reproduced\n"); return 4; }
  if (!vrbridge::SubmitOpenVRFrameWindows(reinterpret_cast<uintptr_t>(second), &error)) { std::printf("FAIL green: %s\n", error.c_str()); return 3; }
  CloseHandle(second);
  vr::VROverlayHandle_t overlay = 0;
  for (uint64_t generation = 0; generation < 256 && !overlay; ++generation) for (uint64_t index = 0; index < 64; ++index) {
    const auto handle = (generation << 32) | index;
    char name[256]{}; vr::EVROverlayError result{};
    vr::VROverlay()->GetOverlayName(handle, name, sizeof(name), &result);
    if (!result && std::string(name) == options.name) { overlay = handle; break; }
  }
  Sleep(250);
  std::vector<uint8_t> pixels(options.width * options.height * 4);
  uint32_t width = 0, height = 0;
  const auto result = vr::VROverlay()->GetOverlayImageData(overlay, pixels.data(), static_cast<uint32_t>(pixels.size()), &width, &height);
  bool valid = result == vr::VROverlayError_None && width == options.width && height == options.height;
  for (size_t offset = 0; offset < pixels.size(); offset += 4) {
    if (pixels[offset] != 0 || pixels[offset + 1] != 255 || pixels[offset + 2] != 0 || pixels[offset + 3] != 255) valid = false;
  }
  std::printf("recycled NT handle uses new texture: %s error=%d first-pixel=%u,%u,%u,%u\n", valid ? "PASS" : "FAIL", result,
    pixels[0], pixels[1], pixels[2], pixels[3]);
  vrbridge::ShutdownOpenVRBackend();
  CloseHandle(handles[0]); CloseHandle(handles[1]);
  return valid ? 0 : 5;
}
