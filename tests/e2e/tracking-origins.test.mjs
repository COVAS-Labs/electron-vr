import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

// Opt in with SteamVR running; creates only an invisible overlay and never resets tracking.
test('native seat placement and read-only origin conversion', {
  skip: process.env.ELECTRON_VR_TEST_ORIGINS !== '1'
}, () => {
  process.env.ELECTRON_VR_DISABLE_OPENXR = '1';
  const require = createRequire(import.meta.url);
  const native = require('../../packages/native-addon/build/Release/vr_bridge.node');
  assert.equal(native.getTrackingOriginTransform('seated', 'standing'), null);
  assert.throws(() => native.getTrackingOriginTransform('camera', 'standing'), RangeError);
  assert.throws(() => native.getTrackingOriginTransform('seated'), TypeError);
  const placement = { mode: 'seat', position: { x: 0.4, y: -0.2, z: -1.5 },
    rotation: { x: 0, y: 0, z: 0, w: 1 } };
  try {
    assert.equal(native.initializeVR({ name: `origin-test-${process.pid}`, width: 64, height: 64,
      sizeMeters: 1, curvature: 0.3, visible: false, placement }), true, native.getLastError());
    assert.equal(native.getRuntimeInfo().selectedBackend, 'openvr');
    const forward = native.getTrackingOriginTransform('seated', 'standing');
    const backward = native.getTrackingOriginTransform('standing', 'seated');
    assert.equal(forward.from, 'seated'); assert.equal(forward.to, 'standing');
    for (let r = 0; r < 3; r++) for (let c = 0; c < 4; c++) {
      const a = forward.matrix, b = backward.matrix;
      const result = a[r*4] * b[c] + a[r*4+1] * b[4+c] + a[r*4+2] * b[8+c] + (c === 3 ? a[r*4+3] : 0);
      assert.ok(Math.abs(result - Number(r === c)) < 0.0001);
    }
    assert.deepEqual(native.getTrackingOriginTransform('standing', 'standing').matrix,
      [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]);
    for (const mode of ['world', 'head', 'seat']) {
      assert.equal(native.setOverlayPlacement({ ...placement, mode }), true, native.getLastError());
    }
    assert.equal(native.setOverlayCurvature(0.5), true, native.getLastError());
  } finally {
    native.shutdownVR();
  }
  assert.equal(native.getTrackingOriginTransform('seated', 'standing'), null);
});
