import { useCallback, useSyncExternalStore } from 'react';

/**
 * Whether a media query matches right now, as state.
 *
 * Layout stays CSS's job. This exists for the one thing a stylesheet cannot
 * express: below the narrow breakpoint the rating bands are not laid out
 * differently, they are a different control — so one of the two must not be in
 * the document at all.
 */
export function useMediaQuery(query: string): boolean {
  const subscribe = useCallback(
    (listener: () => void) => {
      const list = window.matchMedia(query);
      list.addEventListener('change', listener);
      return () => list.removeEventListener('change', listener);
    },
    [query],
  );

  return useSyncExternalStore(subscribe, () => window.matchMedia(query).matches);
}
