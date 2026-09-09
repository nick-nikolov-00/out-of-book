/*
 * The rating band a visitor says is theirs, kept between visits.
 *
 * Which band you read is the setting that most changes what this site tells
 * you — every score and every recommendation is measured inside one band — and
 * for a given player the answer is the same on every visit. So it is asked once
 * and stored, and from then on it is the band both boards open on.
 *
 * localStorage rather than a cookie: the server has no use for it, and a cookie
 * would ride along on every request for nothing.
 */

import { BUCKET_COUNT } from '../buckets';

const STORAGE_KEY = 'initium.preferredBucket.v1';

function inRange(bucket: number): boolean {
  return Number.isInteger(bucket) && bucket >= 0 && bucket < BUCKET_COUNT;
}

/**
 * The stored band, or null when there is none — which is what puts the
 * first-visit question on screen.
 *
 * Anything unreadable counts as none: a private window, site data turned off
 * and a value edited by hand all land here, and asking again costs less than
 * opening on a band nobody chose.
 */
export function preferredBucket(): number | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw === null) return null;

    const bucket = Number(raw);

    return inRange(bucket) ? bucket : null;
  } catch {
    return null;
  }
}

export function savePreferredBucket(bucket: number): void {
  if (!inRange(bucket)) return;

  try {
    localStorage.setItem(STORAGE_KEY, String(bucket));
  } catch {
    /* The choice still stands for this session; it just will not survive the
     * reload, and the question comes back on the next visit. */
  }
}

export function clearPreferredBucket(): void {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* Nothing managed to store it in the first place. */
  }

  notify();
}

/*
 * Forgetting the band has to reach the shell, which is what decides whether the
 * first-visit question is on screen. A subscription rather than a callback
 * handed to an installer, because the two have different lifetimes: the console
 * handle below is installed once, and the shell subscribes as it mounts.
 *
 * Only forgetting is signalled. Storing a band happens on the question's own
 * screen, which already knows it has been answered — and a store that silently
 * fails, as it does in a private window, must not be what the shell reads to
 * decide the question is done with.
 */
const listeners = new Set<() => void>();

/** Subscribe to the stored band being dropped; returns the unsubscribe. */
export function onBandForgotten(listener: () => void): () => void {
  listeners.add(listener);

  return () => {
    listeners.delete(listener);
  };
}

function notify(): void {
  for (const listener of listeners) listener();
}

/**
 * Console handle for the stored band:
 *
 *   initium.ratingBand()      // the stored band index, or null
 *   initium.ratingBand(6)     // set it; boards already mounted keep theirs
 *   initium.ratingBand(null)  // forget it, which asks the question again at once
 */
export function installBandTools(): void {
  const band = (bucket?: number | null): number | null => {
    if (bucket === undefined) return preferredBucket();

    if (bucket === null) clearPreferredBucket();
    else savePreferredBucket(bucket);

    return preferredBucket();
  };

  const host = window as unknown as Record<string, unknown>;
  host.initium = { ...(host.initium as object), ratingBand: band };
}
