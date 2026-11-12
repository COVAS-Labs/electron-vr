import type { OverlayPlacement } from './bridge.js';

export function createAnchorPoll(options: {
  getPlacement: () => OverlayPlacement | null;
  isInitialized: () => boolean;
  apply: (placement: OverlayPlacement) => boolean;
  onError: () => void;
}): () => void {
  let failed = false;
  return () => {
    const placement = options.getPlacement();
    if (!placement || placement.mode !== 'seated' || !options.isInitialized()) return;
    try {
      if (options.apply(placement)) { failed = false; return; }
    } catch { /* Report once, but retry on the next tick after transient errors. */ }
    if (!failed) options.onError();
    failed = true;
  };
}
