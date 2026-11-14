import test from 'node:test';
import assert from 'node:assert/strict';
import { readTrackingOriginTransform } from '../../packages/electron-vr/dist/trackingOrigins.js';

const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
test('returns a defensive copy of a transform in the requested direction', () => {
  const matrix = [...identity];
  const result = readTrackingOriginTransform((from, to) => ({ from, to, matrix }), 'seated', 'standing');
  assert.deepEqual(result, { from: 'seated', to: 'standing', matrix: identity });
  matrix[3] = 5;
  assert.equal(result.matrix[3], 0);
});
test('unsupported or older native backends return null, not a fake identity', () => {
  assert.equal(readTrackingOriginTransform(undefined, 'seated', 'standing'), null);
  assert.equal(readTrackingOriginTransform(() => null, 'standing', 'seated'), null);
});
test('rejects invalid origins and malformed native results', () => {
  assert.throws(() => readTrackingOriginTransform(undefined, 'camera', 'standing'), RangeError);
  for (const value of [false, {}, { from: 'standing', to: 'seated', matrix: identity },
    { from: 'seated', to: 'standing', matrix: [] },
    { from: 'seated', to: 'standing', matrix: [...identity.slice(0, 11), NaN] }]) {
    assert.equal(readTrackingOriginTransform(() => value, 'seated', 'standing'), null);
  }
});
test('seat is a raw runtime coordinate mode, not a world-preserving follower', async () => {
  const { normalizePlacement } = await import('../../packages/electron-vr/dist/overlayOptions.js');
  assert.equal(normalizePlacement({ mode: 'seat', position: { x: 1, y: 2, z: -3 } }).position.y, 2);
  assert.throws(() => normalizePlacement({ mode: 'seated' }), RangeError);
});
