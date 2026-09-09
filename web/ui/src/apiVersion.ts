/*
 * Keeping an open page in step with the server it is talking to.
 *
 * The server stamps every /api/ response with the version of the JSON contract
 * it speaks, and this bundle was compiled with the number that was current when
 * it was built. Equal is the normal case and costs a header lookup. Different
 * means a deploy has landed underneath a page that is still running, and the
 * only way that page becomes correct again is to fetch the bundle that matches.
 *
 * Nothing polls: the check rides on requests the app was making anyway, so a
 * stale page finds out on its first click after the deploy and an idle one
 * finds out when it is next used, which is the moment it starts to matter.
 */

/* Only the stuck case is counted here. The reload that works is counted by the
 * page that comes back, which reads the marker written below. */
import { countVersionStuck } from './clientMetrics';

/** Substituted at build time from the server's declaration of the contract. */
declare const __API_VERSION__: number;

const HEADER = 'X-Api-Version';

/**
 * Which advertised version this tab has already reloaded for.
 *
 * Session-scoped, and the guard against the one way this mechanism can turn
 * into a spin: if a bundle somehow expects a version the server does not serve,
 * reloading returns the same bundle to the same disagreement. Refusing a second
 * reload for the same number turns that from an unusable site into a page that
 * works as well as it can and says why in the console.
 */
const RELOADED_FOR = 'initium.reloadedForApiVersion';

/** So a page stuck in disagreement complains once rather than per request. */
let reported = false;

function alreadyReloadedFor(version: number): boolean {
  try {
    return window.sessionStorage.getItem(RELOADED_FOR) === String(version);
  } catch {
    /* Storage can be refused outright — a private window, or a browser set to
     * block site data. Without somewhere to record the attempt there is no way
     * to stop at one, so the reload is skipped rather than risked. */
    return true;
  }
}

function rememberReloadFor(version: number): void {
  try {
    window.sessionStorage.setItem(RELOADED_FOR, String(version));
  } catch {
    /* A store that refuses a write almost always refused the read above too,
     * and that already declined the reload. Over quota is the exception, and
     * one unrecorded reload is not worth handling separately. */
  }
}

/**
 * Compare one response against the contract this bundle was built for, and
 * reload the page if a newer server has taken over behind it.
 *
 * Silent for any response without the header, which is every static file and
 * anything that answered before reaching the server.
 */
export function checkApiVersion(response: Response): void {
  const stamp = response.headers.get(HEADER);

  /* Absent rather than different: a static file, or something in front of the
   * server that answered on its own. Nothing has been said about the contract,
   * so nothing is concluded about it — an empty header must not read as
   * version zero and reload the page. */
  if (stamp === null) return;

  const advertised = Number(stamp);

  if (!Number.isInteger(advertised) || advertised === __API_VERSION__) return;

  if (alreadyReloadedFor(advertised)) {
    if (!reported) {
      reported = true;
      countVersionStuck();
      console.error(
        `Out of Book: this page expects API version ${__API_VERSION__} but the server ` +
          `serves ${advertised}, and reloading did not resolve it. Some of what you ` +
          `see may be wrong.`,
      );
    }
    return;
  }

  rememberReloadFor(advertised);
  window.location.reload();
}
