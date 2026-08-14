# electron-vr

Electron VR overlay workspace built around one public package: `@covas-labs/electron-vr`.

## Installation

This package is currently published to GitHub Packages, not the public npm registry.

Add GitHub Packages auth for the `@covas-labs` scope in your app:

```ini
@covas-labs:registry=https://npm.pkg.github.com
//npm.pkg.github.com/:_authToken=${GITHUB_PACKAGES_TOKEN}
```

Then install the public package in your Electron app:

```bash
npm install @covas-labs/electron-vr
```

Also keep `electron` in your app dependencies or devDependencies.

## Usage

### Create and open a dedicated VR overlay window

```ts
import { app } from "electron";
import { VROverlay } from "@covas-labs/electron-vr";

let overlay: VROverlay | null = null;

app.on("ready", async () => {
  overlay = new VROverlay({
    name: "Status_HUD",
    width: 1280,
    height: 720,
    url: "file:///absolute/path/to/overlay.html"
  });

  console.log("Runtime probe:", overlay.getRuntimeInfo());

  const success = await overlay.init();
  if (!success) {
    console.error("Overlay init failed");
    app.quit();
    return;
  }

  console.log("Selected backend:", overlay.getSelectedBackend());
});

app.on("before-quit", () => {
  overlay?.destroy();
  overlay = null;
});
```

The package probes OpenXR capability and chooses backends based on the available platform graphics binding. Runtimes that expose `XR_EXTX_overlay` use a direct overlay session. On Windows, runtimes without that extension can use the separately installed implicit API layer, which appends the Electron quad to the primary application's existing OpenXR session.

- `openxr` on Linux when `XR_EXTX_overlay`, `XR_MNDX_egl_enable`, and `XR_KHR_opengl_enable` are available
- `openxr` on Windows when a D3D11 overlay session or the installed API-layer companion is available
- `openvr` when the OpenXR overlay path is unavailable or explicitly disabled and a Linux OpenVR runtime is installed
- `mock` otherwise

Linux OpenXR remains the preferred path when the overlay extension is available. Linux OpenVR uses public `TextureType_Vulkan` automatically on the GPU reported by SteamVR, independent of the game's graphics API, and falls back from direct DMA-BUF import to bitmap-to-Vulkan upload. Set `ELECTRON_VR_DISABLE_OPENXR=1` to force Linux onto the OpenVR-or-mock selection branch while debugging. `ELECTRON_VR_DISABLE_OPENVR_VULKAN=1` disables the default Vulkan path; `ELECTRON_VR_OPENVR_GL_UPLOAD=1` explicitly selects the CPU-readback OpenGL diagnostic path.

Linux x64 packages also include an explicit implicit-API-layer fallback for runtimes without `XR_EXTX_overlay`:

```bash
npx electron-vr-openxr-layer install
npx electron-vr-openxr-layer status
```

The Linux fallback supports Vulkan and desktop OpenGL Xlib host sessions with one single-plane linear RGB DMA-BUF quad. Vulkan hosts import the DMA-BUF on the host device, perform a fenced transfer into an OpenXR swapchain image, and acknowledge Electron only after completion; GLX hosts retain the immutable CPU snapshot and shared-context upload fallback. The Vulkan path is currently scoped to the validated Ubuntu 24.04 AMD RADV, Monado, and `hello_xr -g Vulkan` configuration. Xcb, deprecated OpenGL Wayland bindings, modifier-backed or multiplane buffers, and curvature are deferred and pass through unchanged. Use the same CLI with `enable`, `disable`, or `uninstall`; ordinary `npm install` never registers the layer.

Native Wayland sessions can use this path when the OpenXR host runs through XWayland and supplies `XrGraphicsBindingOpenGLXlibKHR`; Electron may remain native Wayland for DMA-BUF export. For controlled testing only, `ELECTRON_VR_FORCE_OPENXR_API_LAYER=1` selects an installed and enabled layer even when the runtime also exposes `XR_EXTX_overlay`; normal product launches should retain automatic direct-overlay priority.

On macOS, OpenXR uses `XR_KHR_metal_enable` and imports Electron shared textures from their `IOSurface` handles. The Khronos loader is built automatically from `.openxr-sdk`, copied beside the native addon, and loaded through the addon's `@loader_path` rpath.

### Meta OpenXR Simulator on macOS

Install and launch Meta XR Simulator, then run:

```bash
npm run start:openxr:meta
```

This selects Meta's runtime explicitly, builds the Khronos loader and Electron addon, and launches the demo. Meta XR Simulator 205 exposes Metal but not `XR_EXTX_overlay`, so the demo runs as a standard primary OpenXR session and submits the Electron panel as a quad or cylinder composition layer. It does not coexist above an unrelated primary OpenXR application.

For automated validation, keep the simulator application running and use:

```bash
npm run test:e2e:smoke:openxr
```

On macOS this test requires Meta XR Simulator to be installed and verifies runtime selection, standard-session initialization, Electron `IOSurface` import, Metal swapchain copying, and the first submitted composition layer. Set `XR_LOADER_DEBUG=all` for Khronos loader diagnostics. The active runtime can also be selected globally with Meta's `activate_simulator.sh`, but the npm commands use `XR_RUNTIME_JSON` and do not require changing the global runtime.

Linux OpenXR overlay submission now preserves the Electron window alpha channel by default, so transparent overlay UI should composite correctly on top of other XR content.

For Linux verification, `npm run test:e2e:smoke:openxr` forces the demo app onto the OpenXR backend and asserts that initialization plus placement, size, and visibility updates succeed.

`npm run test:e2e:smoke:openvr:linux` disables OpenXR and, when a Linux OpenVR runtime with overlay support is available, asserts initialization, placement, size, visibility, and Vulkan or explicitly requested OpenGL frame submission. The test skips cleanly on hosts without a usable Linux OpenVR overlay runtime.

For OpenVR translators that accept `TextureType_OpenGL` but do not implement SteamVR's private DMA-BUF resource manager, set `ELECTRON_VR_OPENVR_GL_UPLOAD=1`. This experimental Linux-only path reads back the Electron bitmap, uploads it into a persistent GLX texture, and bypasses DMA-BUF import. It is never selected automatically and is not intended as a native SteamVR fallback.

The packaged Windows and Linux builds bundle the OpenVR runtime library they need, so consumers do not need to set `OPENVR_SDK_DIR` just to load the addon.

### Windows OpenXR API layer

Windows x64 packages include an implicit OpenXR API layer for D3D11 and D3D12 applications. Installation is explicit; `npm install` never changes global OpenXR registration.

For a packaged consumer application:

```powershell
npx electron-vr-openxr-layer install
npx electron-vr-openxr-layer status
npx electron-vr-openxr-layer disable
npx electron-vr-openxr-layer enable
npx electron-vr-openxr-layer uninstall
```

From this repository, use `npm run openxr-layer -- <command>` after `npm run rebuild:electron`.

The layer is registered per user in `HKCU`, uses the host application's D3D11 device, and exchanges a three-slot keyed-mutex texture ring with Electron. It passes every OpenXR call through unchanged when no companion is connected or overlay submission fails. Set `ELECTRON_VR_DISABLE_OPENXR_API_LAYER=1` as an emergency process-level disable switch.

Electron applications can perform the same explicit per-user lifecycle through the package API:

```ts
const status = await VROverlay.installOpenXRApiLayer();
console.log(status.installed, status.enabled, status.manifestPath);

await VROverlay.getOpenXRApiLayerStatus();
await VROverlay.disableOpenXRApiLayer();
await VROverlay.enableOpenXRApiLayer();
await VROverlay.uninstallOpenXRApiLayer();
```

Each method returns `Promise<OpenXRApiLayerStatus>` and rejects if the platform is unsupported or registration fails. Ask for explicit user consent before installation; do not install silently during startup. Restart OpenXR games after changing layer installation or enablement. The API supports the packaged Linux x64 and Windows x64 layers and normally operates without administrator/root privileges.

Production applications should use the combined compatibility report for backend selection, setup prompts, and feature gating:

```ts
const report = await VROverlay.getCompatibilityReport();
console.log(report.launch.verdict, report.launch.requiredActions);
```

`report.launch.wouldWorkNow` is the authoritative answer to whether launching a browser overlay now is expected to produce real VR output. A false result is classified as either an actionable setup/runtime/host requirement or a fundamental incompatibility. See [`PRODUCT_INTEGRATION.md`](./PRODUCT_INTEGRATION.md) for the recommended end-user consent flow, main-process IPC boundary, waiting/connected states, issue copy, recovery controls, telemetry, and support diagnostics.

The API layer supports one flat quad in Windows x64 D3D11 and D3D12 OpenXR applications. D3D12 uses cross-API shared textures and shared fences on the application's OpenXR command queue. Positive curvature, cross-adapter copying, 32-bit applications, elevated applications, and anti-cheat-protected processes are not supported yet. Per-user API layers are not loaded into elevated OpenXR applications by the Khronos loader.

Runtime selection prefers a direct `XR_EXTX_overlay` session, then the installed and enabled API layer, then OpenVR, then mock. This path is compiled in Windows CI, but real runtime/game validation still requires a Windows VR machine.

### Position the overlay in VR space

```ts
const overlay = new VROverlay({
  name: "Status_HUD",
  url: "file:///absolute/path/to/overlay.html",
  sizeMeters: 0.9,
  placement: {
    mode: "head",
    position: { x: 0, y: 0, z: -1.2 },
    rotation: { x: 0, y: 0, z: 0, w: 1 }
  }
});

overlay.setPlacement({
  mode: "world",
  position: { x: 0, y: 1.4, z: -2 },
  rotation: { x: 0, y: 0, z: 0, w: 1 }
});

overlay.setSizeMeters(1.2);
overlay.setVisible(true);
```

`sizeMeters` must be greater than zero, and placement vectors/quaternions must use finite numeric values.

`getRuntimeInfo()` reports OpenXR loader and overlay capability details as well as whether an OpenVR runtime is installed by reading the OpenVR paths file, avoiding OpenVR initialization during simple availability checks. It reports `openxrMode`, API-layer installation and connection state, protocol version, and the connected host's process, application, graphics API, and adapter LUID. It also reports `openxrSessionState`, `openxrSessionRunning`, and active OpenVR scene app metadata. `probeMode` carries the backend-selection decision path.

### Reuse an existing BrowserWindow

```ts
import { app, BrowserWindow } from "electron";
import { VROverlay } from "@covas-labs/electron-vr";

let overlay: VROverlay | null = null;

app.on("ready", async () => {
  const runtimeInfo = VROverlay.getRuntimeInfo();
  if (!VROverlay.isAvailable(runtimeInfo)) {
    return;
  }

  const window = new BrowserWindow({
    width: 1280,
    height: 720,
    show: false,
    frame: false,
    transparent: true,
    backgroundColor: "#00000000",
    webPreferences: {
      offscreen: {
        useSharedTexture: true
      },
      preload: "/absolute/path/to/preload.js",
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false
    }
  });

  await window.loadURL("file:///absolute/path/to/overlay.html");

  overlay = await VROverlay.openWindow(window, {
    name: "Status_HUD",
    frameRate: 60
  });

  if (!overlay) {
    console.error("Failed to move existing window into VR");
    window.close();
  }
});
```

Use this path when you want to check VR availability first, create a compatible offscreen `BrowserWindow` yourself, fully control its options, and only then attach that window to the VR bridge.

## Development

### Layout

- `packages/native-addon`: native `node-gyp` addon source and build config
- `packages/electron-vr`: public Electron package and runtime loader
- `apps/demo-electron`: demo application that consumes the public package
- `tests/e2e`: repository-owned end-to-end tests
- `tools`: workspace build and publish helpers

### Local workflow

- `npm install`
- `npm run build`
- `npm run rebuild:electron`
- `npm run start`
- `npm run test:e2e`

### Package model

Consumers install only `@covas-labs/electron-vr`.

Platform-specific prebuilt binaries are published as internal implementation packages and loaded automatically by the public package at runtime.

The public package is the only consumer-facing install target. Apps should not import platform-specific package names directly.

### Publishing

`.github/workflows/publish-prebuilt-packages.yml` publishes:

- internal Electron prebuilt packages for Linux and Windows
- the public `@covas-labs/electron-vr` package that depends on those prebuilds

The same workflow also creates a temporary consumer app and verifies that the published package installs and boots under Electron.
