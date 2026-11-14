# Seat Coordinates

Adds generic seated-origin placement alongside head/world coordinates.
The paired [COVAS branch](https://github.com/lag0matic/Elite-Dangerous-AI-Integration/tree/codex/seated-vr-anchor)
handles offset conversion and EDVR compatibility.

## Changes

| Files | Change | Reason |
| --- | --- | --- |
| `packages/native-addon/native/src/bridge.h`, `addon.cc`, `bridge.cc` | Add `seat` placement and a read-only origin-transform getter. | Expose the runtime's seated coordinate system to applications. |
| `packages/native-addon/native/src/openvr_backend.h`, `openvr_backend.cc` | Use `TrackingUniverseSeated` for seat placement; read the seated-to-standing transform. | Let OpenVR follow recentering independently of image updates. |
| `packages/electron-vr/src/overlayOptions.ts`, `bridge.ts`, `VROverlay.ts`, `index.ts`, `trackingOrigins.ts` | Expose and validate the placement mode, getter and types. | Provide a generic JavaScript/TypeScript API. |

## API

```ts
overlay.setPlacement({
  mode: 'seat',
  position: { x: 0.4, y: -0.2, z: -1.5 },
  rotation: { x: 0, y: 0, z: 0, w: 1 }
});
const transform = overlay.getTrackingOriginTransform('seated', 'standing');
```

`seat` positions are raw seated-origin coordinates in metres. `head` and `world`
retain their existing behavior. Native OpenVR curvature is unchanged.

The getter returns `{ from, to, matrix }` or `null` when unavailable. The matrix
is a copied, row-major rigid 3x4 transform from source to target coordinates.
Both directions and same-origin identity are supported on initialized direct
OpenVR. Unsupported backends reject seat placement rather than substituting
another coordinate mode. JavaScript and native binaries must be updated together.

No recenter policy, polling timer, EDVR dependency, observer or OpenXR-layer
change is included. COVAS performs its own world-to-seat conversion and reads
EDVR's private origin through a separate logfile adapter.

## Verification

TypeScript/native builds, 38 selected unit tests and the hidden native SteamVR
check pass. Visible stock-Elite and EDVR recenter checks passed through COVAS.

Tests: `tests/unit/tracking-origins.test.mjs` and
`tests/e2e/tracking-origins.test.mjs`. The native check is enabled with
`ELECTRON_VR_TEST_ORIGINS=1`; it creates an invisible overlay without resetting tracking.
