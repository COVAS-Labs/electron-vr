# @covas-labs/electron-vr

Electron-facing VR overlay bridge package for OpenVR with native mock preview fallback.

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

You can also reposition the overlay later with `overlay.setPlacement(...)`, toggle it with `overlay.setVisible(...)`, and resize it in meters with `overlay.setSizeMeters(...)`.

`sizeMeters` must be greater than zero, and placement values should be finite numbers.

`getRuntimeInfo()` also includes `openvrRuntimeInstalled` and `openvrRuntimePath` based on the OpenVR paths file, so runtime diagnostics do not need to initialize OpenVR just to check availability.
