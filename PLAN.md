# OpenXR API Layer Plan

## Goal

Provide a floating Electron overlay that coexists with an existing OpenXR application even when the runtime does not implement `XR_EXTX_overlay`.

The API layer will run inside the primary application's process, use its existing OpenXR session, and append an Electron-backed composition layer during `xrEndFrame`.

The first usable milestone targets Windows D3D11 applications and one overlay. D3D12 support follows after the D3D11 path is validated end to end.

## Implementation Status

The Windows D3D11 single-overlay vertical slice is implemented in the workspace:

- Separate implicit API-layer DLL target and loader negotiation
- Per-instance and per-session downstream dispatch
- Host D3D11 device capture, reference spaces, swapchain, and quad injection
- Versioned named-pipe protocol with a three-slot keyed-mutex texture ring
- Native Electron companion backend and runtime diagnostics
- Explicit per-user install, enable, disable, status, and uninstall utility
- Windows prebuilt packaging and CI artifact checks
- Pass-through behavior when the companion, graphics API, texture, or overlay resources are unavailable

Local builds, TypeScript tests, and runtime-info tests pass. The D3D11 API-layer build and headless Windows harness pass in CI. D3D12 host support is now implemented with shared fences and is pending the same Windows CI and real runtime/game validation. Later Linux/macOS API-layer follow-ups remain intentionally unimplemented.

## Target Architecture

```text
Electron window
    | shared texture
    v
Native addon / producer
    | named pipe + shared D3D texture
    v
API layer inside game process
    | append quad during xrEndFrame
    v
Existing game OpenXR session
    |
    v
Compatible OpenXR runtime
```

The runtime sees one application session containing the game's projection layers and the additional Electron quad. `XR_EXTX_overlay` is not required.

## Decisions

- The first milestone supports Windows D3D11 hosts.
- The first milestone supports one overlay.
- D3D12 follows after the D3D11 vertical slice.
- `XR_EXTX_overlay` remains the preferred direct path where available.
- Linux keeps its extension-based path initially.
- OpenVR remains available for SteamVR.
- Standard primary sessions will not be presented as a coexistence fallback.
- The macOS standard session remains a rendering and simulator test harness.
- The public `selectedBackend` remains `openxr`; a separate mode identifies how OpenXR is implemented.

## 1. Separate OpenXR Modes

Introduce an internal mode independent of the public backend:

```ts
type OpenXRMode =
  | "overlay-session"
  | "api-layer"
  | "standard-test-session"
  | "none";
```

Expose additional runtime diagnostics:

```text
openxrMode
openxrApiLayerInstalled
openxrApiLayerEnabled
openxrCompanionConnected
openxrHostProcessId
openxrHostApplicationName
openxrHostGraphicsApi
openxrHostAdapterLuid
openxrProtocolVersion
```

The intended Windows selection order is:

```text
XR_EXTX_overlay
-> installed API-layer companion
-> OpenVR
-> mock
```

Environment options will allow explicitly selecting or disabling each path.

## 2. Restore Direct-Backend Semantics

The existing in-process Windows backend will create a session only when `XR_EXTX_overlay` is available.

When the extension is absent:

- Electron will not create another primary OpenXR session.
- Runtime selection will choose the API-layer transport.
- Linux will continue preferring the direct extension path.
- macOS standard sessions will require an explicit development or test mode.

This prevents a standard session from being mistaken for coexistence support.

## 3. Create the Windows API-Layer DLL

Add a separately built x64 DLL rather than embedding API-layer code in the Node addon.

The initial layer will intercept:

```text
xrNegotiateLoaderApiLayerInterface
xrCreateApiLayerInstance
xrGetInstanceProcAddr
xrCreateSession
xrDestroySession
xrDestroyInstance
xrBeginSession
xrEndFrame
```

The layer will maintain per-instance and per-session dispatch tables and always call the next layer or runtime.

When no Electron producer is connected, every call will pass through unchanged.

## 4. Track the Game's D3D11 Session

During `xrCreateSession`, inspect the `XrGraphicsBindingD3D11KHR` chain and retain the game's `ID3D11Device`.

Track:

- Host process ID
- OpenXR application name
- D3D11 device and immediate context
- DXGI adapter LUID
- Session lifecycle
- Supported swapchain formats
- Cylinder extension availability

Unsupported D3D12, Vulkan, or OpenGL hosts will pass through normally and report an explicit incompatibility rather than disrupting the application.

## 5. Create Layer-Owned OpenXR Resources

Inside the game's session, the API layer will create:

- A VIEW reference space for head-locked placement
- A LOCAL reference space for world placement
- A STAGE reference space when supported
- One D3D11 OpenXR swapchain for the Electron texture

The first milestone will append a core `XrCompositionLayerQuad`. This avoids requiring an optional composition-layer extension.

Curvature will be added conditionally through `XR_KHR_composition_layer_cylinder`. The API layer may add that extension while creating the downstream instance when the runtime exposes it.

## 6. Append the Overlay in `xrEndFrame`

The API layer will perform OpenXR swapchain operations on the game's frame thread:

```text
Read latest producer state
Acquire overlay swapchain image
Wait for image
Acquire shared-texture synchronization
Copy Electron frame into swapchain image
Release shared-texture synchronization
Release swapchain image
Append XrCompositionLayerQuad
Forward xrEndFrame
```

The original `XrFrameEndInfo` values will be preserved:

- `displayTime`
- `environmentBlendMode`
- Existing composition layers
- Existing layer order

The Electron layer will be appended last so it appears above the application's layers where the runtime honors submitted order.

## 7. Define a Versioned IPC Protocol

Use a per-user authenticated Windows named pipe.

The API layer will connect to the Electron addon and send:

```text
Protocol version
Host PID
OpenXR application name
Graphics API
Adapter LUID
Session state
Supported composition-layer capabilities
```

Electron will send:

```text
Overlay configuration
Placement
Visibility
Physical size
Texture generation
Shared texture handles
Frame sequence
Shutdown
```

The IPC worker may run asynchronously, but it will not call OpenXR. It will only update state consumed by `xrEndFrame`.

The protocol will use generation numbers so resize, reconnect, and device recreation cannot consume stale handles.

## 8. Implement Cross-Process D3D11 Texture Transport

Electron's original shared texture is valid only during the paint callback. The addon will copy it into addon-owned textures before calling `texture.release()`.

Use a three-slot texture ring:

```text
Electron texture
-> producer-owned shareable D3D11 texture
-> duplicated NT handle
-> game process API layer
-> game OpenXR swapchain
```

Each slot will contain:

- Sequence number
- Width and height
- DXGI format
- Shared NT handle
- Keyed mutex state
- Generation number

The API-layer handshake provides the target PID and adapter LUID. The producer creates its D3D11 device on that adapter and duplicates handles into the target process.

Synchronization for the first milestone will use `IDXGIKeyedMutex`.

If Electron and the game use different GPUs, initialization will report an adapter mismatch. A software-copy fallback can be added after the GPU path works.

## 9. Add the Native Companion Backend

Add a Windows companion backend behind the existing `BridgeState` dispatch.

Responsibilities:

- Host the named-pipe server.
- Accept and validate the API-layer connection.
- Open Electron's shared texture immediately.
- Maintain the shareable texture ring.
- Publish configuration changes.
- Monitor host disconnect and restart.
- Preserve synchronous setter behavior after initialization.
- Report asynchronous connection state through runtime diagnostics.

Existing public calls remain unchanged:

```ts
overlay.setPlacement(...);
overlay.setSizeMeters(...);
overlay.setVisible(...);
overlay.setCurvature(...);
```

## 10. Install the Implicit API Layer Explicitly

Package:

```text
electron_vr_openxr_layer.dll
electron_vr_openxr_layer.json
registration/status utility
protocol metadata
```

Register the layer per user under the Khronos implicit API-layer registry location.

Provide explicit commands to:

- Install the layer
- Enable the layer
- Disable the layer
- Show registration and connection status
- Uninstall the layer

Ordinary `npm install` will not silently register a global API layer.

The manifest will include an environment-variable disable switch so users can recover from compatibility problems without uninstalling.

## 11. Handle Security and Failure Cases

The API layer must never prevent the game from starting.

Required behavior:

- Missing Electron process: pass through.
- Protocol mismatch: pass through.
- Unsupported graphics API: pass through.
- Pipe failure: stop submitting the overlay.
- Invalid shared handle: skip that overlay frame.
- Session recreation: destroy and recreate owned resources.
- Electron restart: reconnect without restarting the game where feasible.
- Game restart: reconnect and rebuild the texture ring.
- Runtime error: omit the overlay and forward the game's original frame.
- Layer disabled: no meaningful overhead beyond function forwarding.

Named-pipe access will be restricted to the current user and session. Elevated games may require an elevated companion or broker.

## 12. Automated Testing

Add a fake Windows OpenXR runtime and host harness for CI.

Test coverage will include:

- Loader negotiation and dispatch chaining
- API-layer pass-through with no companion
- D3D11 session detection
- Swapchain creation and destruction
- Existing application layers preserved
- Overlay quad appended last
- Original frame metadata preserved
- Resize and texture-generation changes
- Named-pipe reconnect
- Invalid and stale handles
- Game restart
- Electron restart
- Unsupported D3D12 or Vulkan host pass-through
- Direct `XR_EXTX_overlay` path remains selected
- OpenVR fallback remains functional

## 13. Real Runtime Validation

The Windows D3D11 validation matrix should include:

- Meta Quest Link OpenXR
- SteamVR OpenXR
- Virtual Desktop VDXR
- Windows Mixed Reality where available
- At least one Unity OpenXR title
- At least one Unreal OpenXR title

Acceptance criteria:

- The game owns the only primary OpenXR session.
- The overlay is visible while the game is rendering.
- Head and world placement work.
- Alpha is preserved.
- Overlay updates continuously.
- Game frame submission remains valid when Electron exits.
- The API layer can be disabled cleanly.
- Resource usage remains stable during a prolonged run.

## 14. D3D12 Follow-Up

After D3D11 coexistence is proven:

- Detect `XrGraphicsBindingD3D12KHR`.
- Retain the game's `ID3D12Device` and command queue.
- Open shared NT texture handles on D3D12.
- Replace keyed mutex synchronization with shared fences.
- Record command lists on the game's frame path.
- Add D3D12-specific swapchain image handling.
- Repeat the runtime and game validation matrix.

D3D12 support is required before describing the Windows API-layer backend as broadly compatible.

## 15. Linux and macOS Follow-Up

Linux will retain `XR_EXTX_overlay` as its primary path.

A Linux API-layer fallback can later support OpenGL/EGL and Vulkan hosts, but it is not part of the Windows-first milestone.

The Meta macOS simulator remains useful for testing generic API-layer negotiation and layer-list injection. Metal cross-process texture transport is a later platform-specific implementation.

## First Milestone Definition

The first milestone is complete when:

```text
A D3D11 OpenXR game runs through a runtime without XR_EXTX_overlay,
the implicit API layer loads into that game,
Electron publishes one shared overlay texture,
the layer appends a quad during the game's xrEndFrame,
and both game and overlay remain visible concurrently.
```
