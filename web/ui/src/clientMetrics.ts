/*
 * What the browser knows and the server cannot see.
 *
 * The server measures its own work well: how long a lookup took, what it
 * resolved to, how many it refused. What it has no view of is the half that
 * happens out here — how long the bundle took to become a board on somebody's
 * phone, how much of the navigation never reached it because the cache answered
 * first, and the failures that leave no request behind at all.
 *
 * Nothing here opens a connection of its own on a schedule. Counts accumulate
 * in memory and ride out on a header attached to a request the app was making
 * anyway, at most once a minute. The one exception is a page that has just
 * thrown: it may never make another request, so that flush is pushed out rather
 * than waited for.
 *
 * What goes on the wire is a delta since the last flush, never a running total.
 * The server adds those into one counter per metric, which means a flush that
 * never arrives is an undercount rather than a series that steps backwards, and
 * costs the server no per-client state to difference against.
 */

/** How long a flush waits for a request to ride out on. */
const FLUSH_INTERVAL_MS = 60_000;

/** Above this, a wait for a position is one somebody noticed. */
const SLOW_WAIT_MS = 1000;

/**
 * The least time between two pushed flushes.
 *
 * A page that throws once tends to throw repeatedly — a render loop can do it
 * every frame — and the report is worth one request, not one per exception.
 */
const PUSH_INTERVAL_MS = 30_000;

/** Where the version check leaves word that it reloaded the page. */
const RELOADED_FOR = 'initium.reloadedForApiVersion';

/** Which of those reloads has already been counted, so it is counted once. */
const RELOAD_COUNTED = 'initium.reloadCounted';

/*
 * The wire names. Two characters because they ride on a request that is already
 * carrying a FEN, and fixed because the server knows them: a key it does not
 * recognise is skipped, so a name changed here without changing it there is a
 * metric that silently stops arriving.
 */
interface Pending {
  ch: number;
  cm: number;
  ee: number;
  ea: number;
  wn: number;
  ws: number;
  vr: number;
  vs: number;
}

const empty = (): Pending => ({ ch: 0, cm: 0, ee: 0, ea: 0, wn: 0, ws: 0, vr: 0, vs: 0 });

let pending = empty();

/** Measured once, and cleared onto the first flush that carries it. */
let pageLoadMs: number | null = null;

let lastFlush = 0;
let lastPush = 0;

/** Set by anything that should not wait out the interval to be heard. */
let urgent = false;

export function countCacheHit(): void {
  pending.ch += 1;
}

export function countCacheMiss(): void {
  pending.cm += 1;
}

/**
 * A request failure that reached the screen.
 *
 * Aborted requests are not among them: those are the app cancelling its own
 * work after a navigation, and nobody sees one.
 */
export function countApiError(): void {
  pending.ea += 1;
  urgent = true;
}

/** How long the app waited for a position, cache hits and retries included. */
export function countPositionWait(ms: number): void {
  pending.wn += 1;
  if (ms > SLOW_WAIT_MS) pending.ws += 1;
}

/**
 * A page that reloaded and met the same disagreement.
 *
 * The reload that worked is counted by the page that comes back, since the one
 * that decided to reload is gone before it could report anything.
 */
export function countVersionStuck(): void {
  pending.vs += 1;
  urgent = true;
}

function readStored(key: string): string | null {
  try {
    return window.sessionStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeStored(key: string, value: string): void {
  try {
    window.sessionStorage.setItem(key, value);
  } catch {
    /* The reload is then counted again after the next one. An overcount of one
     * per session is not worth a second mechanism. */
  }
}

/**
 * Count the reload the previous page decided on, if this is the page it decided
 * on it for.
 */
function countVersionReloadOnce(): void {
  const reloadedFor = readStored(RELOADED_FOR);

  if (reloadedFor === null || readStored(RELOAD_COUNTED) === reloadedFor) return;

  writeStored(RELOAD_COUNTED, reloadedFor);
  pending.vr += 1;
  urgent = true;
}

function hasSomethingToSay(): boolean {
  return pageLoadMs !== null || Object.values(pending).some((n) => n > 0);
}

/**
 * The header for the next request, or null to send it without one.
 *
 * Reading it is what clears it, so a flush is offered to exactly one request.
 * If that request fails the counts are lost, which is the same undercount as a
 * tab closing mid-interval and is not worth the bookkeeping to avoid.
 */
export function flushHeader(): string | null {
  const now = Date.now();

  if (!hasSomethingToSay()) return null;
  if (!urgent && now - lastFlush < FLUSH_INTERVAL_MS) return null;

  const parts: string[] = [];

  if (pageLoadMs !== null) parts.push(`pl=${pageLoadMs}`);

  for (const [key, value] of Object.entries(pending)) {
    if (value > 0) parts.push(`${key}=${value}`);
  }

  pageLoadMs = null;
  pending = empty();
  lastFlush = now;
  urgent = false;

  return parts.join(',');
}

/**
 * Send what is pending without waiting for the app to make a request.
 *
 * For the case the riding-along scheme cannot cover: a page that has thrown may
 * never fetch anything again, and its report is the one most worth having.
 * /api/health is the cheapest thing to hang it on, and the answer is discarded.
 */
function push(): void {
  const now = Date.now();

  if (now - lastPush < PUSH_INTERVAL_MS) return;

  urgent = true;

  const header = flushHeader();

  if (header === null) return;

  lastPush = now;

  /* Deliberately not the app's request(): this must not retry, must not reload
   * the page on a version mismatch, and must not count its own failure. */
  void fetch('/api/health', { headers: { 'X-Initium-Client': header } }).catch(() => {
    /* The counts are gone. This was the attempt to save them, not a guarantee. */
  });
}

/**
 * Console handle, since none of this belongs on screen:
 *
 *   initium.metrics()   // what is waiting to go out
 */
function installConsoleHandle(): void {
  const host = window as unknown as Record<string, unknown>;

  host.initium = {
    ...(host.initium as object),
    metrics: () => ({ ...pending, pageLoadMs, urgent, sinceFlushMs: Date.now() - lastFlush }),
  };
}

/**
 * Start collecting. Called once, from the entry point, after the app is handed
 * to React.
 */
export function installClientMetrics(): void {
  countVersionReloadOnce();
  installConsoleHandle();

  window.addEventListener('error', () => {
    pending.ee += 1;
    push();
  });

  window.addEventListener('unhandledrejection', () => {
    pending.ee += 1;
    push();
  });

  /*
   * Two frames, because one only proves the browser is about to paint. The
   * second runs after the first frame is on screen, which is the moment the
   * board exists for the person waiting for it.
   *
   * performance.now() is milliseconds since the navigation started, so it is
   * the whole arrival without any need to subtract a start.
   */
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      pageLoadMs = Math.round(performance.now());
      urgent = true;
    });
  });
}
