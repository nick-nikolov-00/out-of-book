/*
 * Deviations you chose to keep: positions where you played something the tree
 * does not pick, and said you meant it. Training treats a saved move as correct
 * from then on instead of asking again.
 *
 * localStorage rather than cookies, which are a fraction of the room and would
 * be sent to the server on every request for nothing.
 */

export interface CustomLine {
  /** Position the deviation applies to, without the move counters. */
  fen: string;
  uci: string;
  san: string;
  /** Side that was to move, so the list can be read back per colour. */
  color: 'w' | 'b';
  /** Rating bucket it was saved under; deviations are still honoured in all. */
  bucket: number;
  /** The move the tree picked instead, for reviewing the choice later. */
  bestSan: string | null;
  /** Expected-score points given up against that pick, when it was saved. */
  loss: number;
  savedAt: number;
}

import { positionKey } from '../position';

const STORAGE_KEY = 'initium.customLines.v1';

/*
 * Every read and write is wrapped: a private window, disabled site data or a
 * corrupted value all throw here, and none of them are worth losing a session
 * over. It simply runs without memory.
 */
function read(): Record<string, CustomLine> {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return {};

    const parsed = JSON.parse(raw) as unknown;
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return {};

    return parsed as Record<string, CustomLine>;
  } catch {
    return {};
  }
}

function write(lines: Record<string, CustomLine>): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(lines));
  } catch {
    /* Nothing to do: the session carries on, it just will not be remembered. */
  }
}

export function allLines(): CustomLine[] {
  return Object.values(read()).sort((a, b) => b.savedAt - a.savedAt);
}

export function lineFor(fen: string): CustomLine | null {
  return read()[positionKey(fen)] ?? null;
}

export function isCustom(fen: string, uci: string): boolean {
  return lineFor(fen)?.uci === uci;
}

export function saveLine(line: Omit<CustomLine, 'fen' | 'savedAt'> & { fen: string }): CustomLine {
  const stored: CustomLine = {
    ...line,
    fen: positionKey(line.fen),
    savedAt: Date.now(),
  };

  const lines = read();
  lines[stored.fen] = stored;
  write(lines);

  return stored;
}

export function removeLine(fen: string): void {
  const lines = read();
  delete lines[positionKey(fen)];
  write(lines);
}

export function clearLines(): void {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* Same as write: nothing useful to do about it. */
  }
}

/*
 * Anything on screen that lists these has to be told when the console handle
 * below changes them. A subscription rather than a callback handed to the
 * installer, because the two have different lifetimes: the handle is installed
 * once, and what needs redrawing is a view that may not be mounted at the time.
 */
const listeners = new Set<() => void>();

/** Subscribe to console-driven changes; returns the unsubscribe. */
export function onLinesChanged(listener: () => void): () => void {
  listeners.add(listener);

  return () => {
    listeners.delete(listener);
  };
}

function notify(): void {
  for (const listener of listeners) listener();
}

/**
 * Console handle for working on these without a UI. It is the primary way to
 * manage saved lines: the list of them is a debug surface, not part of the
 * normal layout, so this is what is left when that is off.
 *
 *   initium.customLines.list()
 *   initium.customLines.get(fen)
 *   initium.customLines.remove(fen)
 *   initium.customLines.clear()
 *   initium.customLines.export()      // JSON string, for keeping a copy
 *   initium.customLines.import(json)  // replaces everything
 */
export function installDevTools(): void {
  const api = {
    list: () => allLines(),
    get: (fen: string) => lineFor(fen),
    remove: (fen: string) => {
      removeLine(fen);
      notify();
      return allLines().length;
    },
    clear: () => {
      clearLines();
      notify();
      return 0;
    },
    export: () => JSON.stringify(read(), null, 2),
    import: (json: string) => {
      write(JSON.parse(json) as Record<string, CustomLine>);
      notify();
      return allLines().length;
    },
  };

  const host = window as unknown as Record<string, unknown>;
  host.initium = { ...(host.initium as object), customLines: api };
}
