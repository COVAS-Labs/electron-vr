# Seated Anchor: Bridge Maintainer Handoff

## Status

Windows prototype, built from upstream
`f683c68039efc8206d36141cce91941bfc8c0b7a` (v0.8.9). AI-assisted implementation;
maintainer review is expected. No upstream repository was modified.

Paired application branch:
[lag0matic/Elite-Dangerous-AI-Integration: codex/seated-vr-anchor](https://github.com/lag0matic/Elite-Dangerous-AI-Integration/tree/codex/seated-vr-anchor).
Its [handoff](https://github.com/lag0matic/Elite-Dangerous-AI-Integration/blob/codex/seated-vr-anchor/SEATED_ANCHOR_HANDOFF.md)
documents COVAS lifecycle changes, placement compatibility, and integration.

The tester confirmed native curvature and working recentering on Windows with
SteamVR and Elite Dangerous through EDVR v0.21.1. Offset edits exposed a later
failure; the final build separates pose polling from image paints and preserves
controllers during property-only edits. The tester confirmed recentering after
that update. Linux/Proton transport is still outstanding.

## Architecture

```text
OpenXR application's xrEndFrame
  -> locate its projection reference space relative to SteamVR STAGE
  -> publish a small versioned pose sample, independent of image submission
  -> Windows shared memory + named mutex
  -> Electron's independent OpenVR anchor timer
  -> recenter delta * configured legacy world-space overlay transform
  -> OpenVR absolute standing-space overlay, with native curvature
```

The application may use OpenXR while the overlay uses OpenVR. SteamVR supplies
both interfaces; this is not a second headset runtime. The observer only emits
poses for a SteamVR OpenXR instance with a valid STAGE space, so it does not
assume another runtime's coordinates match OpenVR standing coordinates.

This does not hook a recenter key, parse EDVR logs, identify a game by name,
modify the game binary, or reset the user's SteamVR origin. EDVR's matching tag
was read to establish whether its recenter actually replaces an observable
reference space. It was not copied into the implementation.

## Files and responsibilities

| File | Responsibility |
| --- | --- |
| `packages/electron-vr/src/overlayOptions.ts`, types and N-API mode handling | Accept the new `seated` placement mode without changing the default. |
| `packages/native-addon/native/anchor_transform.h` | Platform-independent rigid transforms and recenter-delta follower. |
| `packages/native-addon/native/app_anchor_windows.h` | Versioned pose IPC, validation, discovery, freshness, and lifecycle. |
| `packages/native-addon/native/openxr-api-layer/layer.cc` | Publish application-space observations before the texture-delivery checks. |
| `packages/native-addon/native/src/openvr_backend.cc` | Consume observations and apply seated deltas through the native OpenVR overlay API. |
| `packages/electron-vr/src/anchorPolling.ts`, `src/bridge.ts` | Poll placement independently of image paints; clear the timer on detach/shutdown. |
| `packages/native-addon/native/openxr-api-layer/swapchain_format.h` | Same-layout UNORM-to-sRGB negotiation for the separate OpenXR renderer. |
| `packages/native-addon/native/seated_anchor.h` | Earlier OpenXR seated-capture path, distinct from the validated OpenVR observer path. |

The initial mode addition also touches Linux/macOS backend mappings and public
type declarations. These compile-time mappings are not physical-headset proof
of equivalent recenter behavior on those platforms.

## Transform contract

The COVAS branch supplies the original World Locked transform `W`, including
its horizontal and floor-height adjustments. Initially, the output is exactly
`W`, rather than treating it as an eye-relative offset.

For a stable source:

```text
delta = currentApplicationToStage * inverse(baselineApplicationToStage) * prefix
output = delta * configuredWorldPose
```

At the first sample, `prefix` is identity and the baseline equals the current
sample, so the initial pose is unchanged. Recenter changes the application-space
pose and therefore `delta`; ordinary head motion does not.

On a source switch, keep the existing delta as `prefix` and establish a new
baseline. This preserves continuity instead of jumping when the observer first
connects or a game/session changes. The formula is recomputed from the baseline
rather than repeatedly multiplying frame-to-frame deltas, avoiding drift.
Offset edits change `configuredWorldPose`, not the baseline. Full overlay
destruction establishes a fresh baseline; it is not persisted across app exits.

Without an application observer, OpenVR seated-to-standing origin changes are
used instead. When a previously observed application is alive but its sample
is temporarily invalid/stale, hold the last transform rather than snapping back
to a different origin. That policy and source-discovery behavior deserve review
for multiple simultaneous OpenXR applications.

## Windows IPC

- Mapping: `Local\electron-vr-app-anchor-v1-<pid>`.
- Mutex: the mapping name plus `-lock`.
- Payload: magic/version/size, producer PID, session identity, monotonic timestamp,
  validity, and a rigid 3x4 application-to-STAGE transform.
- Producer and consumer use nonblocking mutex acquisition. No wait is introduced
  on the game render thread for a slow consumer.
- Samples must be version/size compatible, under two seconds old, finite, and
  approximately orthonormal with positive determinant.
- Discovery accepts one unambiguous fresh producer. It must not guess between
  multiple fresh applications. Once connected it prefers that producer.
- Session destruction invalidates the sample. Observer publication works even
  when no OpenXR image overlay is connected or visible.
- Local IPC exposes tracking poses, not credentials or conversational data. The
  current adapter uses the Windows user/session namespace and default ACLs;
  hardening and multi-user behavior remain review items.

The IPC is separate from the existing image-transport v3 protocol. The image
protocol layout/version was not changed for the new observer channel.

## Rendering and update lifecycle

Curvature remains `IVROverlay::SetOverlayCurvature`. No cylinder extension,
custom mesh, or segmented flat-panel approximation is used by the tested path.

Updating the pose only during image submission was insufficient: a static
overlay could stop following recentering. `anchorPolling.ts` is now scheduled
on the Electron main thread at the overlay frame interval, without forcing a
new texture paint. It applies only to initialized seated OpenVR overlays and
uses the current placement, so polling cannot restore an older offset value.
The timer is stopped on window detach and shutdown. Repeated transient failures
are logged once until recovery rather than every tick.

The companion COVAS changes update offset/size/curvature values in place and
serialize lifecycle operations. Those changes are part of the complete fix;
updating only this library does not preserve application-level controller state.

## Other fixes retained in this branch

The standalone OpenXR renderer initially failed on SteamVR because Chromium
supplied BGRA UNORM (DXGI 87), while the runtime advertised its matching sRGB
format (91). The layer now prefers an exact format or the matching BGRA/RGBA
sRGB counterpart; it never changes channel order. The source format remains the
resource-change key, avoiding per-frame swapchain recreation. Tests cover the
runtime's reported format list and rejected incompatible substitutions.

Opt-in layer diagnostics are enabled by `enable-layer-diagnostics.txt` beside
the installed layer DLL. They write at most 64 distinct messages to a PID-named
log there. No marker means no diagnostic file. This was added to distinguish
texture failures, pose failures, and compositor submission failures.

The native OpenXR companion still rejects nonzero curvature. Its flat renderer
is not the native-curvature route validated here.

## Build and tests

From the workspace root, with the documented SDK/toolchain dependencies:

```powershell
npm ci
npm run build --workspace @covas-labs/electron-vr
npm run build:addon
node --test tests/unit/anchor-polling.test.mjs tests/unit/compatibility.test.mjs tests/unit/openxr-api-layer.test.mjs tests/unit/vr-overlay.test.mjs
& packages/native-addon/build/Release/electron_vr_openxr_layer_test.exe
& packages/native-addon/build/Release/electron_vr_seated_anchor_test.exe
```

Local verification passed both native executables and 37 selected JS tests.
Native tests cover initial-position preservation, translation/rotation deltas,
no cumulative drift, source switches, invalid samples, observer publication
without image injection, and format negotiation. JS tests cover static-image
anchor polling, current offsets, initialized/mode checks, and failure recovery.

The local Windows build used Node 24 and VS 2026 Build Tools. The repository's
node-gyp 11 did not recognize that VS installation; node-gyp 13.1 was installed
locally without saving or changing the lockfile. Prefer a supported existing
toolchain or resolve that separately; no local node_modules/SDK/binaries are
included in this branch.

Do not run `openxr-registration-windows.test.mjs` casually: it changes real layer
registration. It was deliberately excluded from these tests. Integration setup
is explicit (`npm run openxr-layer -- install/status/enable/disable/uninstall`),
not an npm install side effect. Back up existing registration/files and close
the game before swapping a layer. Restore the original registration afterward.

For a controlled OpenVR presentation test, set `ELECTRON_VR_DISABLE_OPENXR=1`
only in the overlay process. Do not propagate it to the game. If using an isolated
LOCALAPPDATA, point `VR_PATHREG_OVERRIDE` at the existing OpenVR path-registry
file before isolating that directory. Keep the game and overlay on SteamVR for
the coordinate relationship tested here.

## Linux/Proton handoff and remaining review

The transform follower and polling behavior are portable, but
`app_anchor_windows.h` is explicitly Windows-only. **This branch does not finish
Linux application-space recentering.** Existing Linux OpenVR curvature and the
runtime-seated fallback are different from this new observer transport.

Native Linux observer/presenter communication could reuse the repository's
existing Unix-socket conventions. Proton-to-native IPC needs separate analysis:
Windows named mappings are not automatically native Linux shared memory, and
PID/network/filesystem namespaces can differ under Steam Runtime or Flatpak.
Loopback TCP is an option, not an implemented or tested solution. Any replacement
should retain bounded latest-pose delivery, versioning, local access control,
freshness checks, nonblocking publication, and predictable disconnect behavior.

Other review items:

- Define product backend-selection policy instead of requiring a debug override.
- Validate tracking-space alignment on each supported runtime; do not remove the
  SteamVR gate without establishing the OpenXR-to-OpenVR transform.
- Decide how to select among multiple simultaneous application observers.
- Games that change only private camera/view matrices expose no usable recenter
  signal through this observer. EDVR v0.21.1 replaces its seated reference space;
  newer source may not. No game-version compatibility guarantee is implied.
- Review the earlier direct/API-layer OpenXR seated-placement semantics separately
  from the now world-compatible OpenVR translation contract.
- Review/release matching JS and native packages before integrating the COVAS
  branch; publishing the app changes alone against old prebuilts is insufficient.

No personal profiles, API keys, generated media, game binaries, or local
standalone recovery snapshots are committed. This is source-only review work.
