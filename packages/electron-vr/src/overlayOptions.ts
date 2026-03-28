import type { Quat, OverlayPlacement, Vec3 } from "./bridge.js";

export interface OverlayPlacementInput {
  mode: "head" | "world";
  position?: Vec3;
  rotation?: Quat;
}

export function isFiniteNumber(value: number): boolean {
  return Number.isFinite(value);
}

export function assertVec3(value: Vec3, context: string): void {
  if (!isFiniteNumber(value.x) || !isFiniteNumber(value.y) || !isFiniteNumber(value.z)) {
    throw new TypeError(`${context} must contain finite x, y, and z values.`);
  }
}

export function assertQuat(value: Quat, context: string): void {
  if (!isFiniteNumber(value.x) || !isFiniteNumber(value.y) || !isFiniteNumber(value.z) || !isFiniteNumber(value.w)) {
    throw new TypeError(`${context} must contain finite x, y, z, and w values.`);
  }
}

export function assertSizeMeters(sizeMeters: number): void {
  if (!isFiniteNumber(sizeMeters) || sizeMeters <= 0) {
    throw new RangeError("sizeMeters must be a finite number greater than zero.");
  }
}

export function normalizePlacement(placement?: OverlayPlacementInput): OverlayPlacement {
  const mode = placement?.mode ?? "head";
  if (mode !== "head" && mode !== "world") {
    throw new RangeError("placement.mode must be 'head' or 'world'.");
  }

  const position = {
    x: placement?.position?.x ?? 0,
    y: placement?.position?.y ?? 0,
    z: placement?.position?.z ?? -1.2
  };
  const rotation = {
    x: placement?.rotation?.x ?? 0,
    y: placement?.rotation?.y ?? 0,
    z: placement?.rotation?.z ?? 0,
    w: placement?.rotation?.w ?? 1
  };

  assertVec3(position, "placement.position");
  assertQuat(rotation, "placement.rotation");

  return {
    mode,
    position,
    rotation
  };
}
