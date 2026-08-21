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

On Linux, runtime selection prefers `openxr`, then falls back to `openvr`, then to `mock`. Linux OpenVR automatically uses public `TextureType_Vulkan`, regardless of the game's graphics API, with bitmap-to-Vulkan fallback when direct DMA-BUF import is unavailable. `ELECTRON_VR_DISABLE_OPENVR_VULKAN=1` disables it; `ELECTRON_VR_OPENVR_GL_UPLOAD=1` explicitly selects the slower OpenGL diagnostic path.

When `XR_EXTX_overlay` is unavailable, Linux x64 can use the explicitly installed API layer before falling back to OpenVR:

```bash
npx electron-vr-openxr-layer install
```

The Linux layer supports Vulkan and desktop OpenGL Xlib sessions with one single-plane linear RGB DMA-BUF overlay. Vulkan DMA-BUF import is currently validated for Ubuntu 24.04 AMD RADV with Monado and `hello_xr -g Vulkan`; GLX retains its software snapshot fallback. Xcb, native Wayland host bindings, modifier-backed or multiplane buffers, and curvature are not yet supported.

On Windows x64, selection prefers a direct `XR_EXTX_overlay` session, then an installed implicit API layer for D3D11 or D3D12 hosts, then OpenVR, then mock. API-layer installation is explicit:

```powershell
npx electron-vr-openxr-layer install
npx electron-vr-openxr-layer status
```

Use the same command with `enable`, `disable`, or `uninstall`. `npm install` does not register the layer. `ELECTRON_VR_DISABLE_OPENXR_API_LAYER=1` disables it for a process. The API layer supports one flat quad for D3D11 and D3D12 hosts; elevated applications and positive curvature are not yet supported.

Set `ELECTRON_VR_DISABLE_OPENVR=1` to prevent the OpenVR/SteamVR fallback while diagnosing OpenXR. When no usable OpenXR overlay path is available, the bridge selects `mock` and includes `openvr-disabled-by-env` in `probeMode`.

When Virtual Desktop VDXR is the active Windows OpenXR runtime, the bridge automatically avoids the OpenVR fallback and does not initialize OpenVR during probing, because that can start SteamVR and disrupt an OpenXR game. Install and enable the OpenXR API layer to use the OpenXR application-integration path, then restart the game.

OpenComposite does not implement the OpenVR overlay API required by this package. When OpenComposite is the registered OpenVR runtime, the bridge reports it in diagnostics but does not select the OpenVR backend; use a compatible OpenXR runtime with the API layer instead.

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

`getOpenXRApiLayerStatus()` reports `requiresUpdate: true` when the installed layer assets differ from those bundled with the package. Re-run `installOpenXRApiLayer()` after confirmation to update them.

For product UI, prefer the combined readiness report instead of interpreting raw probe fields:

```ts
const report = await VROverlay.getCompatibilityReport();
console.log(report.launch.verdict, report.launch.requiredActions);
```

`report.launch.wouldWorkNow` answers whether launching a browser overlay now is expected to produce real VR output. Otherwise, the verdict distinguishes an actionable setup/runtime/host requirement from a fundamental incompatibility. The report also exposes detailed readiness states, actionable issues, and per-feature support. See `PRODUCT_INTEGRATION.md` in the repository for the recommended consent, IPC, restart, recovery, diagnostics, and production UI flow.

`getRuntimeInfo()` also includes `openvrRuntimeInstalled`, `openvrRuntimePath`, `openxrMode`, API-layer installation state, `openxrHostDetected`, companion connection state, protocol version, and OpenXR host metadata. `openxrHostDetected` can become true before the overlay companion connects. `probeMode` includes the backend-selection decision.
