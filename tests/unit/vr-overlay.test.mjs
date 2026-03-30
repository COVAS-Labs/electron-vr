import assert from "node:assert/strict";
import test from "node:test";

import { assertCurvatureRadius, assertSizeMeters, normalizeCurvature, normalizePlacement } from "../../packages/electron-vr/dist/overlayOptions.js";

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

test("overlay curvature validation rejects invalid values", () => {
  assert.doesNotThrow(() => assertCurvatureRadius(2.0, "curvature.horizontal"));
  assert.throws(() => assertCurvatureRadius(Number.NaN, "curvature.horizontal"), /curvature.horizontal/);
  assert.throws(() => assertCurvatureRadius(0, "curvature.horizontal"), /curvature.horizontal/);
  assert.throws(() => assertCurvatureRadius(-1, "curvature.vertical"), /curvature.vertical/);
});

test("overlay curvature defaults to flat and normalizes provided axes", () => {
  assert.deepEqual(normalizeCurvature(), {});
  assert.deepEqual(normalizeCurvature({ horizontal: 2.5 }), { horizontal: 2.5 });
  assert.deepEqual(normalizeCurvature({ vertical: 4.0 }), { vertical: 4.0 });
  assert.deepEqual(normalizeCurvature({ horizontal: 2.5, vertical: 4.0 }), { horizontal: 2.5, vertical: 4.0 });
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
