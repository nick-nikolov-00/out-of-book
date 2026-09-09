import { LRUCache } from 'lru-cache';

import { checkApiVersion } from './apiVersion';
import { countApiError, countCacheHit, countCacheMiss, countPositionWait, flushHeader } from './clientMetrics';
import { positionKey } from './position';

/*
 * Client for the position server. One request returns everything a position
 * needs: per-bucket totals, the node's own evaluations, and every child move
 * with its own tallies and evaluations.
 */

/**
 * Black's wins are not on the wire: the three outcomes partition the games, so
 * they are exactly `count - whiteWins - draws`.
 */
export interface BucketStat {
  count: number;
  whiteWins: number;
  draws: number;
}

export const blackWinsOf = (stat: BucketStat): number =>
  stat.count - stat.whiteWins - stat.draws;

/**
 * Evaluations are per bucket, both expressed as a *white* score in [0, 1]
 * (0 = Black wins, 1 = White wins).
 *
 *   w — White plays the expectimax-optimal move, Black follows the empirical
 *       move distribution.
 *   b — the mirror: Black plays optimally, White plays the distribution.
 */
export interface Evals {
  w: number[];
  b: number[];
}

/** A row of the node blob: a real move, or one of the two pseudo-moves. */
export type MoveKind = 'move' | 'termination' | 'aggregate';

/**
 * Where the search puts a move, and what it scored it, in one bucket.
 *
 * `rank` is the order to show the moves in: 0 is the move the tree plays, 1 the
 * move it would play if that one were taken away, and so on. It is *not* the
 * order of `score`.
 *
 * `score` is the child's own eval shrunk toward what neighbouring rating
 * buckets say about the same position, by an amount set by how thinly it was
 * played. Still a white score, like every other number here.
 *
 * `solid` is false when too few games stand behind the score to trust it —
 * exactly the moves whose place in the order their score will not explain, so
 * it is what the table dims.
 */
export interface Pick {
  rank: number;
  score: number;
  solid: boolean;
}

export interface MoveEntry {
  kind: MoveKind;
  /** UCI (e.g. "e2e4", "e7e8q", "e1g1"); null for pseudo-moves. */
  uci: string | null;
  /** Per-bucket tallies; null where the move was never played in that bucket. */
  stats: (BucketStat | null)[];
  /** Evaluations of the resulting position, or null if it is not in the tree. */
  child: Evals | null;
  /**
   * Per bucket; null where the move was never played in that bucket. Ranked and
   * scored for the side to move, which is not necessarily the side being
   * analysed for — `solid` describes the move either way, `rank` and `score`
   * only mean something on the side-to-move's own turn.
   */
  pick: (Pick | null)[];
  /**
   * Why the book will not recommend this move, and null for the moves it is
   * willing to.
   *
   * A flagged move is one whose record is real but misleading — it wins games
   * for a reason that will not be there when you play it. The server ranks it
   * below every unflagged move in every bucket, so it can never be the pick,
   * and changes nothing else about it: the share and the win rates are what
   * the games say, because the opponent does still play it.
   */
  dubious: string | null;
}

export interface PositionData {
  fen: string;
  sideToMove: 'w' | 'b';
  hasEvals: boolean;
  found: boolean;
  totals?: BucketStat[];
  eval?: Evals | null;
  /** Expectimax-preferred move per bucket, as UCI. */
  bestMove?: (string | null)[];
  moves?: MoveEntry[];
}

export class ApiError extends Error {}

/* Every ApiError is one somebody sees: the view has nothing else to draw when
 * a lookup fails. Counting them here rather than at each throw site is what
 * keeps that true as throw sites are added. */
function apiError(message: string): ApiError {
  countApiError();
  return new ApiError(message);
}

/** What the server says about the tree it has open. */
export interface Health {
  /** The JSON contract the server speaks, as on every /api/ response. */
  apiVersion: number;
  /** Positions in the tree. */
  nodes: number;
  /** False when the server was started without an .ote, so every eval is null. */
  hasEvals: boolean;
  buckets: number;
}

/**
 * How many times a 429 is waited out before the error reaches the view.
 *
 * A refusal means a burst of navigation briefly outran the server's allowance,
 * and a few seconds of patience is the whole fix. The cap stops a tab from
 * retrying out of sight forever if the cause turns out not to be temporary.
 */
const RETRY_ATTEMPTS = 4;

/** Ceiling on any single backoff wait, in milliseconds. */
const RETRY_MAX_MS = 4000;

/**
 * A sleep the caller's AbortSignal can cut short, so a retry cannot outlive the
 * position that asked for it and land on top of the one since navigated to.
 */
function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(signal.reason);
      return;
    }

    let timer: ReturnType<typeof setTimeout>;

    const onAbort = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };

    timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);

    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/**
 * How long to wait before trying again. The server says how long it needs, so
 * the first choice is to believe it; doubling is the fallback for a 429 from
 * anything else in front of it.
 *
 * The jitter matters: every tab a person has open is refused at the same
 * instant, so retrying at precisely the reported time would send them all back
 * together and have all but one refused again.
 */
function retryDelayMs(retryAfter: unknown, attempt: number): number {
  const advertised =
    typeof retryAfter === 'number' && Number.isFinite(retryAfter) && retryAfter >= 0
      ? retryAfter * 1000
      : null;

  const backoff = 250 * 2 ** attempt;

  return Math.min(RETRY_MAX_MS, advertised ?? backoff) + Math.random() * 250;
}

/**
 * One request, with rate limiting waited out rather than shown to the user.
 *
 * Only 429 is retried. Every other status is the server's real answer and is
 * handed back untouched — a bad FEN is not going to become a good one.
 *
 * Every request in the app comes through here, which is why the contract
 * version is checked here: one place, and no request made for the sake of
 * asking.
 */
async function request(url: string, signal?: AbortSignal): Promise<Response> {
  for (let attempt = 0; ; attempt += 1) {
    let response: Response;

    /* Offered per attempt rather than per call: a retried request is still a
     * request, and the header is only produced when a flush is actually due. */
    const flush = flushHeader();

    const init: RequestInit = {};

    if (signal) init.signal = signal;
    if (flush !== null) init.headers = { 'X-Initium-Client': flush };

    try {
      response = await fetch(url, init);
    } catch (cause) {
      if (cause instanceof DOMException && cause.name === 'AbortError') throw cause;
      throw apiError('Cannot reach the position server. Is it running?');
    }

    checkApiVersion(response);

    if (response.status !== 429) return response;

    const detail = (await response.json().catch(() => null)) as { retryAfter?: number } | null;

    if (attempt >= RETRY_ATTEMPTS) {
      throw apiError(
        'The server is limiting how fast this browser can ask for positions. Give it a moment.',
      );
    }

    await sleep(retryDelayMs(detail?.retryAfter, attempt), signal);
  }
}

export async function fetchHealth(signal?: AbortSignal): Promise<Health> {
  const response = await request('/api/health', signal);

  if (!response.ok) throw apiError(`Position server returned ${response.status}`);

  return (await response.json()) as Health;
}

/*
 * Stepping back and forth through a line revisits the same positions
 * constantly, so caching them is what keeps navigation instant rather than a
 * round trip per ply.
 *
 * Bounded by bytes rather than by count: the payloads span three orders of
 * magnitude — a position outside the tree is a couple of hundred bytes and a
 * busy opening node is tens of kilobytes — and one of those should not evict
 * the other one for one. The size charged is the length of the response text,
 * which measured close enough to the parsed heap cost.
 *
 * Keyed by position rather than by FEN so that a transposition is a hit,
 * instead of fetching and holding the same node twice.
 */
const MAX_BYTES = 8 * 1024 * 1024;

/** Rough count of what the cache has saved, for `initium.cache.stats()`. */
const counters = { hits: 0, misses: 0 };

const cache = new LRUCache<string, PositionData>({ maxSize: MAX_BYTES });

/**
 * The payload for a FEN if it has already been fetched, with no promise in the
 * way: awaiting even a resolved one costs a paint, and that is a paint in which
 * the board has moved but the panel has not.
 *
 * Peeks rather than reads, because it runs during render and the position being
 * rendered is about to be marked as used anyway.
 */
export function peekPosition(fen: string): PositionData | undefined {
  return cache.peek(positionKey(fen));
}

/**
 * The cache from the console, since nothing on screen should be about it:
 *
 *   initium.cache.stats()   // entries, bytes held, hit rate
 *   initium.cache.clear()   // after restarting the server on a different .ote
 */
export function installCacheTools(): void {
  const api = {
    stats: () => ({
      positions: cache.size,
      bytes: cache.calculatedSize,
      megabytes: Number(((cache.calculatedSize ?? 0) / 1048576).toFixed(2)),
      limit: `${(MAX_BYTES / 1048576).toFixed(2)} MB`,
      hits: counters.hits,
      misses: counters.misses,
      hitRate: counters.hits + counters.misses
        ? `${((counters.hits / (counters.hits + counters.misses)) * 100).toFixed(1)}%`
        : 'n/a',
    }),
    clear: () => {
      cache.clear();
      counters.hits = 0;
      counters.misses = 0;
      return 'cleared';
    },
  };

  const host = window as unknown as Record<string, unknown>;
  host.initium = { ...(host.initium as object), cache: api };
}

export async function fetchPosition(fen: string, signal?: AbortSignal): Promise<PositionData> {
  const key = positionKey(fen);
  const cached = cache.get(key);

  if (cached) {
    counters.hits += 1;
    countCacheHit();
    /* Counted rather than skipped: a hit is a wait of about nothing, and
     * leaving those out would make the measured waits describe a slower app
     * than the one people are using. */
    countPositionWait(0);
    return cached;
  }

  counters.misses += 1;
  countCacheMiss();

  /* The whole wait as the person experiences it — retries and rate-limit
   * backoff included — which is what the server's own timer cannot see. */
  const started = performance.now();

  const url = `/api/position?fen=${encodeURIComponent(fen)}`;
  const response = await request(url, signal);

  countPositionWait(performance.now() - started);

  if (!response.ok) {
    const detail = await response.json().catch(() => null);
    throw apiError(detail?.error ?? `Position server returned ${response.status}`);
  }

  /* Read as text so the entry can be charged its real weight; parsing it
   * ourselves costs nothing that response.json() was not already doing. */
  const text = await response.text();
  const data = JSON.parse(text) as PositionData;

  cache.set(key, data, { size: text.length });
  return data;
}
