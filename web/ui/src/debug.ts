/*
 * Whether the maintenance surfaces are showing.
 *
 * A build-time flag is the usual answer and the wrong one here: the deployment
 * these are actually wanted in serves a production build, so gating on it would
 * put the debug UI only in the dev server. The opt-in is a runtime one instead.
 *
 * It is sticky on purpose: a switch you have to re-flick every reload is one
 * nobody uses.
 */

const STORAGE_KEY = 'initium.debug';

function readStored(): boolean {
  try {
    return localStorage.getItem(STORAGE_KEY) === '1';
  } catch {
    /* Private window or site data disabled; off is the right default. */
    return false;
  }
}

function store(on: boolean): void {
  try {
    if (on) localStorage.setItem(STORAGE_KEY, '1');
    else localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* The flag simply will not survive the reload. */
  }
}

/* Read once at module load rather than per call, so every consumer in a render
 * pass agrees and the URL can be cleaned up straight afterwards. */
let enabled = (() => {
  let value = readStored();

  try {
    const param = new URLSearchParams(window.location.search).get('debug');

    if (param !== null) {
      value = param !== '0' && param !== 'false';
      store(value);

      /* Take the parameter back out of the address bar, or every link copied
       * out of the app turns the panels on for wherever it is pasted. */
      const url = new URL(window.location.href);
      url.searchParams.delete('debug');
      window.history.replaceState(null, '', url.toString());
    }
  } catch {
    /* No URL to read, or history is blocked; the stored value stands. */
  }

  return value;
})();

/** True while the maintenance surfaces should be rendered. */
export function isDebug(): boolean {
  return enabled;
}

/**
 * Console switch, so the panels can be reached without editing a URL:
 *
 *   initium.debug()       // what it is now
 *   initium.debug(true)   // on, remembered across reloads
 *   initium.debug(false)  // off
 */
export function installDebugSwitch(onChange: () => void): void {
  const toggle = (on?: boolean): boolean => {
    if (on === undefined) return enabled;

    enabled = on;
    store(on);
    onChange();

    return enabled;
  };

  const host = window as unknown as Record<string, unknown>;
  host.initium = { ...(host.initium as object), debug: toggle };
}
