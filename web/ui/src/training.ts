/*
 * The rules of a training session, with no React and no storage in them: given
 * the rows already built for a position, decide what the opponent plays and how
 * good the move you played was. Kept apart from the view so the grading can be
 * reasoned about, and tested, on its own.
 */

import type { MoveRow } from './analysis';

/**
 * How far below the best move you may land and still be told you are fine, in
 * expected-score percentage points. Anything worse is offered for saving as a
 * line of your own rather than simply marked wrong.
 */
export const EXCELLENT_MARGIN = 2;

/**
 * A reply played this rarely is worth calling out: you are being asked to
 * handle something you will hardly ever meet, and knowing that is the point of
 * the warning rather than a reason to hide the move.
 *
 * The bar is low because the tree is the whole of Lichess rather than a curated
 * repertoire: at 15% the warning fires on the French and the Caro-Kann.
 */
export const SIDELINE_SHARE = 0.05;

export type Grade =
  /** The move the tree itself picks in this bucket. */
  | 'best'
  /** Not the pick, but within EXCELLENT_MARGIN of it. */
  | 'excellent'
  /** Yours: saved earlier as a deliberate deviation. */
  | 'custom'
  /** More than EXCELLENT_MARGIN below the pick. */
  | 'suboptimal'
  /** Legal, but the tree has never seen it in this bucket. */
  | 'unknown';

export interface Verdict {
  grade: Grade;
  uci: string;
  san: string;
  /** The best row's move, for the arrow and the feedback line. */
  bestUci: string | null;
  bestSan: string | null;
  /** Expected-score percentage points below the best move; 0 when unknown. */
  loss: number;
  /** True while the move is only a proposal — suboptimal and not yet saved. */
  pending: boolean;
}

/**
 * The row the tree picks here: the head of its own order, else its recorded
 * best move, else the top row as the table happens to be sorted.
 *
 * Rank leads because it is the order the tree would actually play the position
 * out, and it is defined in places the stored pick is not. The fallback order
 * matters: grading and the move table must not name different moves.
 */
export function bestRow(rows: MoveRow[]): MoveRow | null {
  const playable = rows.filter((row) => row.playable && row.expected !== null);

  return (
    playable.find((row) => row.rank === 0) ?? playable.find((row) => row.isBest) ?? playable[0] ?? null
  );
}

/**
 * Grades one move. `rows` must already be the analysis rows for the position
 * being moved from, built from the trainee's perspective, so `expected` reads
 * as "higher is better for whoever is training".
 */
export function grade(rows: MoveRow[], uci: string, san: string, isCustom: boolean): Verdict {
  const best = bestRow(rows);
  const played = rows.find((row) => row.uci === uci) ?? null;

  const shared = {
    uci,
    san,
    bestUci: best?.uci ?? null,
    bestSan: best?.label ?? null,
  };

  /* A move the tree has no evaluation for cannot be scored against the pick,
   * so it is not wrong so much as off the map — and committing to it would
   * train a line the rest of the session cannot follow. */
  if (!played || played.expected === null || best === null || best.expected === null)
    return { ...shared, grade: 'unknown', loss: 0, pending: true };

  /*
   * Clamped at zero because the column and the order do not have to agree: a
   * thinly played move can score above the tree's pick and still rank below it,
   * and reporting that as a negative loss would tell the trainee they beat the
   * tree on a number the tree itself does not believe.
   */
  const loss = Math.max(0, (best.expected - played.expected) * 100);

  if (isCustom) return { ...shared, grade: 'custom', loss, pending: false };
  if (played.uci === best.uci) return { ...shared, grade: 'best', loss: 0, pending: false };
  if (loss <= EXCELLENT_MARGIN) return { ...shared, grade: 'excellent', loss, pending: false };

  return { ...shared, grade: 'suboptimal', loss, pending: true };
}

export interface OpponentMove {
  uci: string;
  san: string;
  /** Share of this bucket's games in the position that played it. */
  share: number;
  /** True when that share is below SIDELINE_SHARE. */
  isSideline: boolean;
}

/**
 * Picks the opponent's reply the way the field would: weighted by how often
 * each move was actually played, so common lines come up most and the rare
 * ones still show up at their real frequency.
 *
 * `random` is injectable so a session can be replayed deterministically.
 */
export function chooseOpponentMove(
  rows: MoveRow[],
  random: () => number = Math.random,
): OpponentMove | null {
  /* Pseudo-move rows carry real games but cannot be played, so they are out of
   * the draw — and out of its denominator, or the weights would not sum to 1. */
  const playable = rows.filter((row) => row.playable && row.uci !== null && row.count > 0);
  const total = playable.reduce((sum, row) => sum + row.count, 0);

  if (total === 0) return null;

  /* The last row is the fallback for a ticket that lands exactly on the end of
   * the range; `total > 0` guarantees there is one. */
  let chosen = playable.at(-1);
  if (!chosen) return null;

  let ticket = random() * total;

  for (const row of playable) {
    ticket -= row.count;

    if (ticket <= 0) {
      chosen = row;
      break;
    }
  }

  const share = chosen.count / total;

  return {
    uci: chosen.uci as string,
    san: chosen.label,
    share,
    isSideline: share < SIDELINE_SHARE,
  };
}

/**
 * A line ends when there is nothing left to train: no playable continuation, or
 * — on your own turn — nothing the tree can grade you against.
 */
export function isLineComplete(rows: MoveRow[], traineeToMove: boolean): boolean {
  const playable = rows.filter((row) => row.playable && row.uci !== null && row.count > 0);

  if (playable.length === 0) return true;
  if (!traineeToMove) return false;

  return playable.every((row) => row.expected === null);
}
