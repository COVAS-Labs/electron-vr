# @covas-labs/electron-vr

Electron-facing VR overlay bridge package for OpenXR or OpenVR overlays, with native mock preview fallback when no real XR runtime is usable.

Published Windows and Linux packages bundle the OpenVR runtime library they need, so consumers do not need to configure `OPENVR_SDK_DIR` for normal usage.

## Install

This package is published on GitHub Packages.

```ini
@covas-labs:registry=https://npm.pkg.github.com
//npm.pkg.github.com/:_authToken=${GITHUB_PACKAGES_TOKEN}
```

```bash
npm install @covas-labs/electron-vr
```

## Example

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
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false
    }
  });

  await window.loadURL("file:///absolute/path/to/overlay.html");

  overlay = await VROverlay.openWindow(window, {
    name: "Status_HUD",
    sizeMeters: 1,
    placement: {
      mode: "head",
      position: { x: 0, y: 0, z: -1.2 },
      rotation: { x: 0, y: 0, z: 0, w: 1 }
    }
  });
});
```

You can also reposition the overlay later with `overlay.setPlacement(...)`, toggle it with `overlay.setVisible(...)`, resize it in meters with `overlay.setSizeMeters(...)`, and curve it with `overlay.setCurvature({ horizontal, vertical })`.

`sizeMeters` must be greater than zero, placement values should be finite numbers, and curvature values are optional radii in meters greater than zero.

On Linux, runtime selection prefers `openxr`, then falls back to `openvr`, then to `mock`. Linux OpenVR is treated as a best-effort alternate backend when a compatible OpenVR runtime is installed but the OpenXR overlay path is unavailable or disabled. It is not currently validated end to end on the main development machine or in CI.

On Windows, runtime probing reports OpenXR overlay and D3D11 graphics-binding capability, but the default backend remains `openvr` during rollout. Set `ELECTRON_VR_ENABLE_OPENXR=1` to opt into the Windows OpenXR path when a compatible runtime exposes `XR_EXTX_overlay` and `XR_KHR_D3D11_enable`. This path is not currently validated end to end on the main development machine or in CI.

`getRuntimeInfo()` also includes `openvrRuntimeInstalled`, `openvrRuntimePath`, and platform-specific OpenXR capability details such as `openxrWindowsD3D11BindingAvailable`, so runtime diagnostics do not need to initialize OpenVR just to check availability. `probeMode` includes the backend-selection decision as well, which makes fallback behavior easier to diagnose even when Linux OpenVR or Windows OpenXR cannot be validated on the current host.
