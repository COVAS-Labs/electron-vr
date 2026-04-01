import assert from "node:assert/strict";
import test from "node:test";

import { assertSizeMeters, normalizePlacement } from "../../packages/electron-vr/dist/overlayOptions.js";

test("overlay placement defaults to a head-locked transform", () => {
  const placement = normalizePlacement();

  assert.deepEqual(placement, {
    mode: "head",
    position: { x: 0, y: 0, z: -1.2 },
    rotation: { x: 0, y: 0, z: 0, w: 1 }
  });
});

test("overlay size validation rejects invalid values", () => {
  assert.doesNotThrow(() => assertSizeMeters(1.25));
  assert.throws(() => assertSizeMeters(Number.NaN), /sizeMeters/);
  assert.throws(() => assertSizeMeters(-1), /sizeMeters/);
  assert.throws(() => assertSizeMeters(0), /sizeMeters/);
});

test("overlay placement validation rejects bad mode and coordinates", () => {
  assert.throws(
    () => normalizePlacement({ mode: "sideways", position: { x: 0, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0, w: 1 } }),
    /placement.mode/
  );

  assert.throws(
    () => normalizePlacement({ mode: "world", position: { x: Number.POSITIVE_INFINITY, y: 0, z: 0 }, rotation: { x: 0, y: 0, z: 0, w: 1 } }),
    /placement.position/
  );
  assert.throws(
    () => normalizePlacement({ mode: "world", position: { x: 0, y: 0, z: -2 }, rotation: { x: 0, y: 0, z: 0, w: Number.NaN } }),
    /placement.rotation/
  );
});
