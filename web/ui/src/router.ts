/*
 * The client router: flat routes, no nesting, no route-level data loading. That
 * is small enough that a library would be more configuration than code, so this
 * is the whole of it — read the URL, write the URL, tell React when it changed.
 *
 * The contract with the server is the usual single-page one: every path that is
 * not a real file and not an API call serves index.html, and this decides what
 * to render.
 */

import { useMemo, useSyncExternalStore } from 'react';

const listeners = new Set<() => void>();

function emit(): void {
  for (const listener of listeners) listener();
}

/* The browser announces the back and forward buttons and nothing else: a
 * pushState of our own fires no event, so `navigate` emits for itself. */
window.addEventListener('popstate', emit);

function subscribe(listener: () => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/*
 * Path and query are subscribed to separately, and both snapshots are strings.
 * Both parts matter: useSyncExternalStore compares snapshots with Object.is, so
 * returning a freshly parsed object would re-render forever — and keeping them
 * apart means the shell, which only cares about the path, does not re-render
 * every time a board writes a FEN into the query.
 */
const readPath = () => window.location.pathname;
const readSearch = () => window.location.search;

/** The current path, e.g. `/analysis`. */
export function usePath(): string {
  return useSyncExternalStore(subscribe, readPath);
}

/** The current query, e.g. `?fen=…&bucket=6`. */
export function useQuery(): URLSearchParams {
  const search = useSyncExternalStore(subscribe, readSearch);
  return useMemo(() => new URLSearchParams(search), [search]);
}

export interface NavigateOptions {
  /**
   * Overwrite the current history entry rather than adding one. Use it for a
   * URL that is tracking state the user did not navigate to — the FEN of the
   * board they are already looking at — so that Back still leaves the view
   * instead of walking a move at a time through everything they tried.
   */
  replace?: boolean;
}

export function navigate(to: string, { replace = false }: NavigateOptions = {}): void {
  if (to === window.location.pathname + window.location.search) return;

  window.history[replace ? 'replaceState' : 'pushState'](null, '', to);
  emit();
}

/** `/analysis` plus a query string, or bare when there is nothing to say. */
export function href(path: string, query: URLSearchParams): string {
  const search = query.toString();
  return search ? `${path}?${search}` : path;
}

/**
 * Whether a click on a link is ours to handle. A middle click or a click with
 * a modifier held is the user asking for a new tab or window, and swallowing
 * it is the commonest way a hand-rolled router quietly breaks a link.
 */
export function isPlainLeftClick(event: {
  button: number;
  metaKey: boolean;
  ctrlKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
}): boolean {
  return (
    event.button === 0 && !event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey
  );
}
