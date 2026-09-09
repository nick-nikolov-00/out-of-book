/*
 * The nine Lichess rating buckets, in the order the .otb/.ote files store them.
 * Every per-bucket array coming back from the server is indexed by it.
 */
export const BUCKETS = [
  { label: '<1000', short: '<1k' },
  { label: '1000-1199', short: '1000' },
  { label: '1200-1399', short: '1200' },
  { label: '1400-1599', short: '1400' },
  { label: '1600-1799', short: '1600' },
  { label: '1800-1999', short: '1800' },
  { label: '2000-2199', short: '2000' },
  { label: '2200-2399', short: '2200' },
  { label: '2400+', short: '2400+' },
] as const;

export const BUCKET_COUNT = BUCKETS.length;

/** Bucket shown before the user picks one; the most populated band overall. */
export const DEFAULT_BUCKET = 4;

export type BucketIndex = number;
