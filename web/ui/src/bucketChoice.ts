/*
 * The rating bucket the user last picked, shared by every view.
 *
 * Views are mounted one at a time, so each tab switch unmounts whichever hook
 * was holding the choice and the next view would start over at the default.
 * Picking a band is a statement about how you want the tree read, not about
 * one screen, so it is kept here instead — module state, alive for as long as
 * the page is.
 *
 * It starts on the band the visitor said is theirs, so the first screen of a
 * visit is already the right field rather than a site-wide default. Picks made
 * while browsing move it but are not written back: the stored band is a
 * statement about the player, and reading somebody else's band for a minute is
 * not one. A link with no `bucket` in it is therefore read against whoever
 * follows it, not against whoever sent it.
 */

import { BUCKET_COUNT, DEFAULT_BUCKET } from './buckets';
import { preferredBucket } from './storage/preferredBucket';

let chosen = preferredBucket() ?? DEFAULT_BUCKET;

/** The bucket a freshly mounted view should open on. */
export function currentBucket(): number {
  return chosen;
}

/** Records a pick, ignoring anything outside the bucket range. */
export function rememberBucket(bucket: number): void {
  if (Number.isInteger(bucket) && bucket >= 0 && bucket < BUCKET_COUNT) chosen = bucket;
}
