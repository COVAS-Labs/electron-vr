export type TrackingOrigin = "seated" | "standing";
export type Matrix34 = readonly [number, number, number, number, number, number, number, number, number, number, number, number];

export interface TrackingOriginTransform {
  from: TrackingOrigin;
  to: TrackingOrigin;
  /** Row-major rigid 3x4 matrix: targetPoint = matrix * sourcePoint. Units are metres. */
  matrix: Matrix34;
}

export function readTrackingOriginTransform(
  read: ((from: TrackingOrigin, to: TrackingOrigin) => unknown) | undefined,
  from: TrackingOrigin,
  to: TrackingOrigin
): TrackingOriginTransform | null {
  if (!["seated", "standing"].includes(from) || !["seated", "standing"].includes(to)) {
    throw new RangeError("Tracking origins must be 'seated' or 'standing'.");
  }
  if (!read) return null;
  const value = read(from, to) as TrackingOriginTransform | null;
  if (!value || value.from !== from || value.to !== to || !Array.isArray(value.matrix) ||
      value.matrix.length !== 12 || !value.matrix.every(Number.isFinite)) return null;
  return { from, to, matrix: [...value.matrix] as unknown as Matrix34 };
}
