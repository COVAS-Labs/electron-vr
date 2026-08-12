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
    curvature: 0,
    placement: {
      mode: "head",
      position: { x: 0, y: 0, z: -1.2 },
      rotation: { x: 0, y: 0, z: 0, w: 1 }
    }
  });
});
```

You can also reposition the overlay later with `overlay.setPlacement(...)`, toggle it with `overlay.setVisible(...)`, resize it in meters with `overlay.setSizeMeters(...)`, and curve it with `overlay.setCurvature(...)`.

`sizeMeters` must be greater than zero, `curvature` must be between `0` and `1`, and placement values should be finite numbers. `curvature: 0` keeps the overlay flat. Positive curvature uses OpenVR overlay curvature on OpenVR and `XR_KHR_composition_layer_cylinder` cylinder layers on OpenXR.

On Linux, runtime selection prefers `openxr`, then falls back to `openvr`, then to `mock`. Linux OpenVR is treated as a best-effort alternate backend when a compatible OpenVR runtime is installed but the OpenXR overlay path is unavailable or disabled. It is not currently validated end to end on the main development machine or in CI.

When `XR_EXTX_overlay` is unavailable, Linux x64 can use the explicitly installed API layer before falling back to OpenVR:

```bash
npx electron-vr-openxr-layer install
```

The initial Linux layer supports desktop OpenGL Xlib sessions and one single-plane linear RGB DMA-BUF overlay. Xcb, native Wayland host bindings, Vulkan, modifier-backed or multiplane buffers, explicit synchronization, and curvature are not yet supported.

On Windows x64, selection prefers a direct `XR_EXTX_overlay` session, then an installed implicit API layer for D3D11 or D3D12 hosts, then OpenVR, then mock. API-layer installation is explicit:

```powershell
npx electron-vr-openxr-layer install
npx electron-vr-openxr-layer status
```

Use the same command with `enable`, `disable`, or `uninstall`. `npm install` does not register the layer. `ELECTRON_VR_DISABLE_OPENXR_API_LAYER=1` disables it for a process. The API layer supports one flat quad for D3D11 and D3D12 hosts; elevated applications and positive curvature are not yet supported.

Applications can manage the same per-user installation through the public API:

```ts
const installed = await VROverlay.installOpenXRApiLayer();
console.log(installed.installed, installed.enabled, installed.manifestPath);

const status = await VROverlay.getOpenXRApiLayerStatus();
await VROverlay.disableOpenXRApiLayer();
await VROverlay.enableOpenXRApiLayer();
await VROverlay.uninstallOpenXRApiLayer();
```

The methods return `Promise<OpenXRApiLayerStatus>` and reject when the platform is unsupported or the utility fails. Installation should require explicit user consent. It affects subsequently started OpenXR applications, so restart an already running game after installing, enabling, disabling, or uninstalling the layer. These methods support Linux x64 and Windows x64 and do not require administrator/root access for the normal per-user installation.

For product UI, prefer the combined readiness report instead of interpreting raw probe fields:

```ts
const report = await VROverlay.getCompatibilityReport();
console.log(report.launch.verdict, report.launch.requiredActions);
```

`report.launch.wouldWorkNow` answers whether launching a browser overlay now is expected to produce real VR output. Otherwise, the verdict distinguishes an actionable setup/runtime/host requirement from a fundamental incompatibility. The report also exposes detailed readiness states, actionable issues, and per-feature support. See `PRODUCT_INTEGRATION.md` in the repository for the recommended consent, IPC, restart, recovery, diagnostics, and production UI flow.

`getRuntimeInfo()` also includes `openvrRuntimeInstalled`, `openvrRuntimePath`, `openxrMode`, API-layer installation and connection state, protocol version, and connected OpenXR host metadata. `probeMode` includes the backend-selection decision.
