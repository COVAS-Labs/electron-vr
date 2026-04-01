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

The package now probes Linux OpenXR capability and chooses backends in this order:

- `openxr` when `XR_EXTX_overlay`, `XR_MNDX_egl_enable`, and `XR_KHR_opengl_enable` are available
- `openvr` when the OpenXR overlay path is unavailable or explicitly disabled and a Linux OpenVR runtime is installed
- `mock` otherwise

Linux OpenXR remains the preferred path. Linux OpenVR is kept as a best-effort alternate or fallback runtime path for compatible OpenVR overlay runtimes, but it is not end-to-end validated on the main development machine or in CI. Set `ELECTRON_VR_DISABLE_OPENXR=1` to force Linux onto the OpenVR-or-mock selection branch while debugging. `ELECTRON_VR_ENABLE_OPENXR=1` is still accepted, but OpenXR no longer requires it.

Linux OpenXR overlay submission now preserves the Electron window alpha channel by default, so transparent overlay UI should composite correctly on top of other XR content.

For Linux verification, `npm run test:e2e:smoke:openxr` forces the demo app onto the OpenXR backend and asserts that initialization plus placement, size, and visibility updates succeed.

`npm run test:e2e:smoke:openvr:linux` disables OpenXR and, when a Linux OpenVR runtime with overlay support is available, asserts that the demo app initializes the OpenVR backend and that placement, size, and visibility updates succeed. The test is runtime-gated so it skips cleanly on hosts without a usable Linux OpenVR overlay runtime, including the current development machine and CI.

The packaged Windows and Linux builds bundle the OpenVR runtime library they need, so consumers do not need to set `OPENVR_SDK_DIR` just to load the addon.

On Windows, runtime probing now also reports whether the current OpenXR runtime exposes `XR_EXTX_overlay` and `XR_KHR_D3D11_enable`. Windows still defaults to `openvr` when available, even if Windows OpenXR looks capable. Set `ELECTRON_VR_ENABLE_OPENXR=1` to opt into the Windows OpenXR path during bring-up. This path is not validated on the current development machine or in CI, so treat it as careful best-effort support rather than production-proven behavior.

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

`getRuntimeInfo()` reports OpenXR loader and overlay capability details as well as whether an OpenVR runtime is installed by reading the OpenVR paths file, avoiding OpenVR initialization during simple availability checks. `probeMode` now also carries the backend-selection decision path, which helps explain whether Linux fell back because OpenXR was disabled or because the required overlay or EGL extensions were missing, and on Windows whether OpenXR was available but intentionally not selected by default. For Linux OpenVR specifically, treat this as diagnostic support rather than proof that overlay initialization will succeed on a given runtime.

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
