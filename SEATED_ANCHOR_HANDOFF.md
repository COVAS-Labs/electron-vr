# Seat Coordinates: Bridge Maintainer Handoff

## Scope

Based on upstream `f683c68039efc8206d36141cce91941bfc8c0b7a` (v0.8.9).
AI-assisted implementation; maintainer review expected.

Paired [COVAS branch](https://github.com/lag0matic/Elite-Dangerous-AI-Integration/tree/codex/seated-vr-anchor)
and its [application handoff](https://github.com/lag0matic/Elite-Dangerous-AI-Integration/blob/codex/seated-vr-anchor/SEATED_ANCHOR_HANDOFF.md).

Luca approved a generic seat-relative coordinate mode alongside head and world.
The library implements no Elite-specific policy or cockpit anchor.

## Public API

`placement.mode` accepts `head`, `world`, or `seat`:

- `head`: existing tracked-device-relative placement, unchanged.
- `world`: existing absolute standing-origin placement, unchanged.
- `seat`: absolute OpenVR `TrackingUniverseSeated` placement, in metres.

`seat` is raw runtime-local positioning, not a world-preserving follower. Apps
that preserve a previous world position perform their own conversion. The old
experimental library spelling `seated` is no longer accepted. COVAS's saved
setting remains `seated`; its wrapper translates the app policy.

An initialized VROverlay instance exposes:

```ts
overlay.getTrackingOriginTransform('seated', 'standing');
// null, or { from: 'seated', to: 'standing', matrix: [12 numbers] }
```

The row-major rigid 3x4 matrix transforms source points into target coordinates;
translation is in metres. Both directions and matching-origin identity are
supported on initialized direct OpenVR. Returned arrays are copies. The getter
reads `GetSeatedZeroPoseToStandingAbsoluteTrackingPose`; it neither initializes
nor resets the runtime, tracks the headset, or moves an overlay.

## Native Path

```text
application supplies seat-local placement
  -> SetOverlayTransformAbsolute(TrackingUniverseSeated, matrix)
  -> SteamVR handles seated-origin changes independently of image paints
```

No polling timer, yaw-only follower, shared-memory observer, mutex, OpenXR layer
modification, protocol change, or EDVR dependency is part of the net proposal.
Native OpenVR curvature is unchanged. Old observer/diagnostic experiments remain
in branch history, but are removed from the diff against the upstream base.

`bridge.h`, `addon.cc`, `bridge.cc`, `openvr_backend.{h,cc}` implement the native
contract. `trackingOrigins.ts`, `bridge.ts`, `VROverlay.ts`, `overlayOptions.ts`
and `index.ts` expose it to TypeScript. Tests cover API validation and raw mode.

## Backend Boundaries

Supported: directly initialized OpenVR (Windows and native Linux code paths).
Not implemented: direct OpenXR, OpenXR API-layer rendering, macOS backends, and
Linux OpenVR host relay. Unsupported getters return null; seat placement fails
explicitly instead of silently using head/world coordinates. Linux/Proton has
not been physically tested. Native Linux OpenVR needs no new observer transport;
the host-relay path would need a future, separately reviewed extension.

Build and distribute matching JavaScript/native binaries. An older addon without
the getter returns null at the JS boundary; it cannot provide this feature.

## Stock Elite Evidence

Testing without EDVR verified the original OpenVR DLL and stock game path.
Elite's normal recenter changed SteamVR's seated-to-standing origin by about
0.0678 metres and -60.7388 degrees yaw. Head motion without recentering left all
twelve matrix elements unchanged. The tester then confirmed the prior native-only
prototype recentered in stock Elite, including after offset edits.

This isolates the necessary signal from EDVR: stock Elite exposes runtime seated
origin changes already. A game that only changes its private internal camera is
not guaranteed to expose that signal; this is not universal camera tracking.

The tester subsequently confirmed the simplified standalone recenters in stock
Elite. With EDVR restored, it does not follow recentering: a controlled test on
2026-11-13 showed EDVR moving its private OpenXR origin by 6.8 cm and 22.2 degrees,
while all twelve elements of SteamVR's seated-origin matrix stayed unchanged.
This generic mode follows the runtime origin it is given; it cannot observe a
different application's private space. EDVR compatibility requires a separate
optional application-origin signal/adapter, not a change to raw `seat` semantics.
Test static images, ordinary head motion, recenter, curvature, offset edits, mode
changes, overlay restart, and game/session restart.

This revision passed the TypeScript and Windows native builds, 38 selected JS
unit tests, the unchanged OpenXR negotiation/pass-through executable, and an
opt-in native SteamVR check using an invisible overlay. That check initialized
directly in seat mode, verified forward/inverse/identity origin transforms,
switched head/world/seat, and applied native curvature. It did not recenter the
headset or validate visible cockpit behavior.

## Build and Checks

```sh
npm run build --workspace @covas-labs/electron-vr
npm run build:addon
node --test tests/unit/tracking-origins.test.mjs tests/unit/compatibility.test.mjs tests/unit/openxr-api-layer.test.mjs tests/unit/vr-overlay.test.mjs
```

With SteamVR running, set `ELECTRON_VR_TEST_ORIGINS=1` for the test process and run
`node --test tests/e2e/tracking-origins.test.mjs` to repeat the invisible native
check. The test otherwise skips, and never resets tracking or registers a layer.

The Windows host used Node 24 and a local node-gyp 13 to recognize VS2026; no
dependency or lockfile churn is included. Windows registration tests alter real
OpenXR registration: do not run them as isolated unit checks. Reviewing this
branch requires no working-install, profile, layer-registration, or game changes.
