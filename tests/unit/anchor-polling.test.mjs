import test from 'node:test';
import assert from 'node:assert/strict';
import { createAnchorPoll } from '../../packages/electron-vr/dist/anchorPolling.js';

test('seated updates continue without image paints and use edited offsets', () => {
  let placement = { mode: 'seated', position: { x: 1, y: 0, z: -1 }, rotation: { x: 0, y: 0, z: 0, w: 1 } };
  const applied = [];
  const tick = createAnchorPoll({ getPlacement: () => placement, isInitialized: () => true,
    apply: value => { applied.push(value); return true; }, onError: () => assert.fail('unexpected error') });
  tick(); tick();
  placement = { ...placement, position: { ...placement.position, x: 2 } };
  tick();
  assert.equal(applied.length, 3);
  assert.equal(applied.at(-1).position.x, 2);
  placement = { ...placement, mode: 'world' };
  tick();
  assert.equal(applied.length, 3);
});
test('uninitialized overlays are skipped; transient errors recover without log spam', () => {
  let initialized = false, success = false, errors = 0, calls = 0;
  const tick = createAnchorPoll({ getPlacement: () => ({ mode: 'seated' }), isInitialized: () => initialized,
    apply: () => { calls++; return success; }, onError: () => errors++ });
  tick(); assert.equal(calls, 0);
  initialized = true; tick(); tick(); assert.equal(errors, 1);
  success = true; tick(); success = false; tick(); assert.equal(errors, 2);
});
