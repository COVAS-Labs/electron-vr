# Production Integration and End-User Flow

This document describes the recommended product experience for detecting VR
backends, explaining compatibility, installing OpenXR application integration,
and recovering from problems. It is intended for Electron applications that
consume `@covas-labs/electron-vr`.

## Product Principles

- Treat VR support as a state machine, not a one-time boolean capability check.
- Do not expose raw OpenXR extension names, registry paths, DMA-BUF details, or
  graphics API terminology in the default user interface.
- Do not call API-layer installation without explicit user consent.
- Do not present “waiting for a game” as an error.
- Do not imply that successful installation guarantees compatibility with every
  game. Graphics API, elevation, anti-cheat, GPU, runtime, and driver constraints
  still apply.
- Always provide Disable and Uninstall recovery actions where Install is offered.
- Keep detailed diagnostics available behind a support/details affordance.

## Public Product API

Use the high-level report for normal product decisions:

```ts
import { VROverlay } from "@covas-labs/electron-vr";

const report = await VROverlay.getCompatibilityReport();
```

The report combines runtime probing, selected backend, API-layer installation,
host connection, supported host graphics APIs, feature compatibility, and
actionable issues:

```ts
interface VRCompatibilityReport {
  launch: {
    verdict: "works-now" | "action-required" | "incompatible";
    wouldWorkNow: boolean;
    fundamentalIncompatibility: boolean;
    message: string;
    requiredActions: VRSetupAction[];
  };
  readiness:
    | "ready"
    | "setup-required"
    | "waiting-for-host"
    | "fallback-only"
    | "unavailable"
    | "development-only";
  backend: "openxr" | "openvr" | "mock" | "none";
  openxrMode: "overlay-session" | "api-layer" | "standard-test-session" | "none";
  backendLabel: string;
  summary: string;
  canRenderOverlay: boolean;
  isRealVrBackend: boolean;
  requiresApiLayer: boolean;
  requiresOpenXRAppRestart: boolean;
  apiLayer: OpenXRApiLayerStatus | null;
  compatibleHostGraphicsApis: string[];
  features: VRFeatureCompatibility;
  issues: VRCompatibilityIssue[];
  recommendedAction: VRSetupAction;
  diagnostics: RuntimeInfo;
}
```

For API-layer mode, `diagnostics.openxrHostDetected` becomes true when a
compatible OpenXR session has loaded the layer, before the Electron companion
connects. `diagnostics.openxrCompanionConnected` becomes true after the overlay
is initialized and its frame transport is connected.

For the primary product decision, read `report.launch` first:

- `works-now`: launching an Electron browser overlay now is expected to produce
  real VR output.
- `action-required`: it will not produce real VR output yet, but setup, runtime
  installation, or starting/restarting a compatible host can make it work.
- `incompatible`: the current platform or mode has no production coexistence
  path in this implementation.

`canRenderOverlay` is broader and may include a Linux desktop preview. Do not use
it as the “will this work in VR now?” answer.

The raw `RuntimeInfo` API remains available for diagnostics. Product UI should
prefer `VRCompatibilityReport` because `selectedBackend !== "none"` alone can
mean a Linux desktop mock preview rather than real VR output.

Installation lifecycle methods are asynchronous and per-user:

```ts
await VROverlay.installOpenXRApiLayer();
await VROverlay.getOpenXRApiLayerStatus();
await VROverlay.enableOpenXRApiLayer();
await VROverlay.disableOpenXRApiLayer();
await VROverlay.uninstallOpenXRApiLayer();
```

`OpenXRApiLayerStatus.requiresUpdate` is true when the registered layer assets
do not match the package managing them. Treat this like installation: ask for
confirmation, call `installOpenXRApiLayer()` again, and restart OpenXR hosts.

After every lifecycle call, request a new compatibility report. Do not mutate a
previous report in application state.

## Readiness States

| State | Product meaning | Primary UI |
| --- | --- | --- |
| `ready` | A real backend is available, or an API-layer host is detected | “VR overlay ready” or detected application name |
| `setup-required` | A supported OpenXR runtime exists but per-user integration must be installed or enabled | Consent card with Enable action |
| `waiting-for-host` | Integration is installed and enabled, but no compatible OpenXR application is connected | Calm waiting state with restart guidance |
| `fallback-only` | No real backend is ready; Linux may still show desktop preview | “Desktop preview only” |
| `unavailable` | No usable backend or preview is available | Runtime installation/configuration guidance |
| `development-only` | A test session exists but is not production coexistence support | Developer-only warning; hide in consumer builds |

## Recommended First-Run Flow

### 1. Detect silently

Call `VROverlay.getCompatibilityReport()` after `app.whenReady()`. Detection is
read-only and should not trigger a permission dialog.

Do not block the rest of the application while detection runs. Show a compact
“Checking VR support…” state only in the VR settings or overlay onboarding UI.

### 2. Ready without setup

If `readiness === "ready"`:

- Show the `backendLabel` in a secondary status line.
- Enable “Start overlay.”
- Display feature controls according to `features`.
- Hide installation prompts.
- If API-layer mode is connected, optionally show the host application name.

Recommended copy:

```text
VR overlay ready
Connected through OpenXR.
```

or:

```text
VR overlay ready
SteamVR overlay support is available.
```

### 3. Ask before enabling OpenXR integration

If `recommendedAction === "install-openxr-api-layer"`, show a dedicated consent
dialog. This is not an administrator prompt; it explains that the application
will register a per-user component loaded by subsequently started compatible
OpenXR applications.

Recommended copy:

```text
Enable overlays in OpenXR applications?

To display this panel in OpenXR applications that do not support overlays
directly, [Product] can install a per-user OpenXR integration component.

It does not require administrator access. OpenXR applications must be restarted
after enabling it. Some elevated or anti-cheat-protected applications may not
load third-party integration components.

[Not now] [Enable integration]
```

Do not preselect consent, hide the action in terms and conditions, or install on
ordinary application startup.

After consent:

1. Disable the action and show progress.
2. Call `installOpenXRApiLayer()` in the Electron main process.
3. Request a fresh compatibility report.
4. If successful, show restart/start guidance.
5. If unsuccessful, keep the application usable and show a retry plus details.

### 4. Installed but disabled

If `recommendedAction === "enable-openxr-api-layer"`, use concise copy:

```text
OpenXR integration is disabled
Enable it to display overlays in compatible OpenXR applications. Restart those
applications afterward.

[Keep disabled] [Enable]
```

The user already installed the component, but enabling still changes loader
behavior and should remain an explicit action.

### 5. Wait for a compatible host

If `readiness === "waiting-for-host"`, do not show an error icon. The expected
state before a game starts is no companion connection.

Recommended copy:

```text
Ready for an OpenXR application
Start a compatible OpenXR application. If it is already running, restart it so
the integration can load.
```

Show supported host categories in an expandable detail:

- Windows: D3D11 or D3D12 OpenXR applications.
- Linux: desktop OpenGL Xlib/GLX OpenXR applications.

Poll a fresh compatibility report every 1-2 seconds while this screen is
visible. Stop aggressive polling after a host connects or the window closes.

### 6. Connected

When the report transitions from `waiting-for-host` to `ready` and
`openxrMode === "api-layer"`:

- Show “Connected” without interrupting the user.
- Display `diagnostics.openxrHostApplicationName` only if non-empty.
- Start or initialize the overlay if the user previously enabled it.
- Do not display process IDs or adapter identifiers in primary UI.

### 7. Fallback or unavailable

For Linux `fallback-only`:

```text
Desktop preview only
No compatible VR runtime is currently ready. You can preview the overlay on
this desktop while configuring OpenXR or SteamVR.
```

For Windows/macOS without visible mock output:

```text
VR overlay unavailable
No compatible OpenXR or SteamVR runtime was detected.
```

Offer “Check again” and “View setup help.” Do not offer API-layer installation
unless the report specifically recommends it; installing a layer cannot fix a
missing OpenXR runtime.

## Feature Compatibility UI

Feature support has three states:

- `supported`: enable the control normally.
- `unsupported`: hide the control or disable it with a direct explanation.
- `runtime-dependent`: allow the control only after runtime/session capability
  is confirmed, or show it as conditional.

Recommended control behavior:

| Feature | Product behavior |
| --- | --- |
| Flat overlay | Required for real backend readiness |
| Head/world placement | Enable for direct OpenXR, API layer, and OpenVR |
| Curvature | Enable for OpenVR; conditional for direct OpenXR; disable in API-layer mode |
| Alpha transparency | Enable for real paths and Linux desktop preview |
| Multiple overlays | Do not advertise as supported |
| Software fallback | Internal resilience detail; do not expose as a user setting |

For unsupported curvature in API-layer mode, use:

```text
Curved overlays are not available with this OpenXR application. The overlay
will remain flat.
```

Do not call unsupported setters and then rely on their return value to define
the product experience. Gate controls using the compatibility report first.

## Production Electron Architecture

All detection, lifecycle, and overlay methods belong in the Electron main
process. The renderer should receive a deliberately reduced view through a
preload bridge.

### Main process service

```ts
// vr-service.ts
import { VROverlay, type VRCompatibilityReport } from "@covas-labs/electron-vr";

export type ProductVRState = Pick<
  VRCompatibilityReport,
  | "launch"
  | "readiness"
  | "backendLabel"
  | "summary"
  | "canRenderOverlay"
  | "isRealVrBackend"
  | "requiresOpenXRAppRestart"
  | "compatibleHostGraphicsApis"
  | "features"
  | "recommendedAction"
> & {
  connectedApplication: string;
  issues: Array<Pick<VRCompatibilityReport["issues"][number], "code" | "severity" | "title" | "message" | "action">>;
};

export async function getProductVRState(): Promise<ProductVRState> {
  const report = await VROverlay.getCompatibilityReport();
  return {
    launch: report.launch,
    readiness: report.readiness,
    backendLabel: report.backendLabel,
    summary: report.summary,
    canRenderOverlay: report.canRenderOverlay,
    isRealVrBackend: report.isRealVrBackend,
    requiresOpenXRAppRestart: report.requiresOpenXRAppRestart,
    compatibleHostGraphicsApis: report.compatibleHostGraphicsApis,
    features: report.features,
    issues: report.issues.map(({ code, severity, title, message, action }) => ({
      code,
      severity,
      title,
      message,
      action
    })),
    recommendedAction: report.recommendedAction,
    connectedApplication: report.diagnostics.openxrHostApplicationName
  };
}
```

### Validated IPC handlers

```ts
// main.ts
import { ipcMain } from "electron";
import { VROverlay } from "@covas-labs/electron-vr";
import { getProductVRState } from "./vr-service.js";

const allowedActions = new Set(["install", "enable", "disable", "uninstall"]);

ipcMain.handle("vr:get-state", () => getProductVRState());

ipcMain.handle("vr:integration-action", async (_event, action: unknown) => {
  if (typeof action !== "string" || !allowedActions.has(action)) {
    throw new TypeError("Invalid VR integration action.");
  }

  if (action === "install") await VROverlay.installOpenXRApiLayer();
  if (action === "enable") await VROverlay.enableOpenXRApiLayer();
  if (action === "disable") await VROverlay.disableOpenXRApiLayer();
  if (action === "uninstall") await VROverlay.uninstallOpenXRApiLayer();
  return getProductVRState();
});
```

Do not accept a command, executable path, manifest path, or arbitrary argument
array from the renderer. Expose an allowlisted action enum only.

### Preload bridge

```ts
// preload.ts
import { contextBridge, ipcRenderer } from "electron";

contextBridge.exposeInMainWorld("vrSetup", {
  getState: () => ipcRenderer.invoke("vr:get-state"),
  installIntegration: () => ipcRenderer.invoke("vr:integration-action", "install"),
  enableIntegration: () => ipcRenderer.invoke("vr:integration-action", "enable"),
  disableIntegration: () => ipcRenderer.invoke("vr:integration-action", "disable"),
  uninstallIntegration: () => ipcRenderer.invoke("vr:integration-action", "uninstall")
});
```

Keep `contextIsolation: true` and `nodeIntegration: false`. Do not expose the
raw `ipcRenderer` object.

### Renderer state machine

```ts
async function refreshVRState() {
  const state = await window.vrSetup.getState();
  renderVRState(state);

  if (state.readiness === "waiting-for-host") {
    scheduleRefresh(1500);
  }
}

async function onInstallConfirmed() {
  setBusy(true);
  try {
    const state = await window.vrSetup.installIntegration();
    renderVRState(state);
    showRestartNotice();
  } catch (error) {
    showIntegrationFailure(toUserSafeMessage(error));
  } finally {
    setBusy(false);
  }
}
```

The overlay launch button should be enabled for real VR only when
`state.launch.wouldWorkNow` is true. If the verdict is `action-required`, render
the actions in `requiredActions`. If it is `incompatible`, do not offer retrying
the same launch as though it were a transient failure.

Cancel polling when the settings view is hidden. Ensure only one lifecycle
operation can run at a time.

## Communicating Issues

Use three levels:

### Informational

Expected states that do not require repair:

- Waiting for a compatible application.
- OpenXR integration is optional because OpenVR is already ready.
- A game restart is required.

Use neutral colors and no error icon.

### Warning

The user can fix or work around the issue:

- Integration is not installed.
- Integration is disabled.
- Integration status could not be inspected.
- Development-only session.

Offer one clear primary action and a “Details” disclosure.

### Error

No useful real backend is available or an explicit operation failed:

- No compatible runtime.
- Installation/enablement failed.
- Overlay initialization failed after readiness was reported.

Keep the rest of the app usable. Include Retry, Disable integration where
applicable, and Copy diagnostics.

## User-Safe Error Mapping

Do not place raw child-process output directly in a modal. Map operation errors
to stable product copy and retain the original error for diagnostics.

| Failure | User message | Recovery |
| --- | --- | --- |
| Unsupported platform | “OpenXR application integration is not available on this device.” | Use supported Windows/Linux x64 host |
| Utility unavailable | “The VR integration component is missing from this installation.” | Repair/reinstall application |
| Install failed | “OpenXR integration could not be installed for this user.” | Retry, show details, verify security software/filesystem |
| Enable failed | “OpenXR integration could not be enabled.” | Retry or reinstall integration |
| Status failed | “Integration status could not be checked.” | Continue with detected fallback and retry |
| No companion | Not an error | Start/restart compatible OpenXR application |
| Unsupported host graphics API | “This OpenXR application uses a graphics mode that is not supported yet.” | Windows D3D11/D3D12; Linux OpenGL Xlib |
| Elevated Windows host | “The VR application is running as administrator and cannot load per-user integration.” | Run both normally; do not suggest elevating Electron by default |
| Anti-cheat restriction | “This application may block third-party OpenXR integration.” | Disable integration for that title; follow vendor policy |
| Cross-GPU mismatch | “The overlay and VR application are using different GPUs.” | Assign both applications to the same GPU |

Never suggest disabling anti-cheat, security software, or operating-system
protections as a default recovery step.

## Diagnostics and Support Bundles

Primary UI may include:

- Readiness.
- Backend label.
- Runtime name.
- Connected application name.
- Supported host graphics APIs.
- Issue codes.

The support bundle may additionally include:

- Full `report.diagnostics`.
- API-layer manifest path and scope.
- Protocol version.
- Host process ID and graphics API.
- Adapter identifier.
- Application version, Electron version, OS, architecture, GPU, and driver.
- Original lifecycle and initialization errors.

Review paths and application names for privacy before uploading. Ask permission
before sending diagnostics. Do not include authentication tokens, environment
variables wholesale, home-directory contents, or unrelated process lists.

Use stable issue codes such as `openxr-api-layer-not-installed` for telemetry.
Do not use localized titles or raw exception messages as event identifiers.

Recommended events:

```text
vr_compatibility_checked
vr_integration_consent_shown
vr_integration_install_started
vr_integration_install_succeeded
vr_integration_install_failed
vr_host_connected
vr_overlay_initialized
vr_overlay_initialization_failed
vr_integration_disabled
vr_integration_uninstalled
```

Record backend, OpenXR mode, issue code, OS, architecture, and application
version. Avoid recording game names unless the user opted into diagnostics.

## Settings and Recovery UI

Provide a persistent VR settings section with:

- Current readiness and backend.
- “Check again.”
- Install/Enable action only when recommended.
- Disable action whenever integration is installed and enabled.
- Uninstall action whenever integration is installed.
- Supported feature list.
- Troubleshooting details.
- Copy diagnostics.

Disabling should be prominent enough to serve as a recovery path if a game has
compatibility problems. Uninstall should require confirmation but not be hidden.

Recommended emergency guidance:

```text
If an OpenXR application stops launching after integration was enabled, disable
OpenXR integration here and restart the application.
```

The manifest also supports `ELECTRON_VR_DISABLE_OPENXR_API_LAYER=1` as an
advanced process-level recovery switch. Keep this in troubleshooting details,
not first-run UI.

`ELECTRON_VR_DISABLE_OPENVR=1` prevents the OpenVR/SteamVR fallback for process-level OpenXR diagnostics. Do not present it as a normal product setting.

## Startup and Refresh Policy

- Probe once after Electron is ready.
- Refresh when the VR settings page opens.
- Refresh after install, enable, disable, or uninstall.
- Poll every 1-2 seconds only while waiting for an API-layer host.
- Refresh after overlay initialization failure before choosing recovery copy.
- Refresh after runtime switching or when the user clicks Check again.
- Do not install or enable integration merely because a probe changed.

An already created `VROverlay` should be destroyed and recreated if backend
selection changes. Do not assume a bridge initialized for mock/OpenVR can switch
in place to API-layer OpenXR after installation.

## Production Checklist

- Use `getCompatibilityReport()` for product state.
- Use `report.launch.wouldWorkNow` for the real-VR launch decision.
- Keep lifecycle calls in the main process.
- Require explicit installation consent.
- Validate and allowlist IPC actions.
- Re-probe after every lifecycle operation.
- Restart or recreate the overlay after backend changes.
- Poll waiting state without presenting it as failure.
- Gate feature controls using `features`.
- Offer Disable and Uninstall recovery actions.
- Preserve raw diagnostics separately from user-safe messages.
- Obtain consent before uploading support data.
- Validate on physical Windows and Linux hosts using `VALIDATION.md`.
