import type { Chess } from 'chess.js';

import { blackWinsOf, type BucketStat, type Evals, type MoveEntry, type MoveKind, type PositionData } from './api';

/**
 * Which side the board is being analysed for. This is *not* the side to move:
 * it is the player whose repertoire we are building, and it stays fixed while
 * the turn alternates underneath it.
 */
export type Perspective = 'w' | 'b';

/** A single row of the move table, already resolved for one rating bucket. */
export interface MoveRow {
  key: string;
  kind: MoveKind;
  uci: string | null;
  /** SAN for real moves; a description for the two pseudo-moves. */
  label: string;
  count: number;
  /** Fraction of the bucket's games that continued with this row. */
  share: number;
  whiteRate: number;
  drawRate: number;
  blackRate: number;
  /**
   * The number the eval column shows, as the analysis side's expected return so
   * higher is always better.
   *
   * On our own move this is the shrunk ranking score, because that is what the
   * search values the move at; the raw eval of a move played twice is not a
   * number anyone should read. On the opponent's move nothing is being chosen,
   * so it is the raw eval.
   */
  expected: number | null;
  /**
   * Where the tree puts this move: 0 is the move it plays, 1 the move it would
   * play if that one were taken away, and so on. This is the order the table
   * shows, and not the order of `expected` — a move can score higher than the
   * one above it and still rank below it, when too little stands behind the
   * score for the tree to believe the gap. Null wherever nothing is being
   * chosen.
   */
  rank: number | null;
  /**
   * False when the score rests on too few games to mean much — the rows where
   * the order and the number disagree, so the table dims it rather than letting
   * it look like a verdict.
   */
  solid: boolean;
  isBest: boolean;
  /**
   * Why the book will not recommend this move, and null for the moves it is
   * willing to. A flagged move is ranked below every unflagged one, so on our
   * own turn it sits at the bottom whatever it scores; on the opponent's turn
   * it is left exactly where the games put it, because that is where it will
   * be played.
   */
  dubious: string | null;
  /** Real moves can be played on the board; pseudo-moves cannot. */
  playable: boolean;
}

export const PSEUDO_MOVE_LABELS: Record<MoveKind, string> = {
  move: '',
  termination: 'Game ended here',
  aggregate: 'Rare / pruned moves',
};

/**
 * Reads the evaluation belonging to the analysis side, on the white-win axis.
 * Which array to take is fixed by who we are analysing for, not by whose turn
 * it happens to be: the opponent is always the field.
 */
export function scoreFor(
  evals: Evals | null | undefined,
  bucket: number,
  perspective: Perspective,
) {
  if (!evals) return null;
  const value = perspective === 'w' ? evals.w[bucket] : evals.b[bucket];
  return value ?? null;
}

/**
 * Turns a white-axis score into the analysis side's expected return, so that
 * Black reads as a maximiser too. Black minimising a white score and Black
 * maximising `1 - score` are the same ordering, but only the second one can be
 * shown and sorted like White's.
 */
export function expectedReturn(whiteScore: number, perspective: Perspective): number {
  return perspective === 'w' ? whiteScore : 1 - whiteScore;
}

/** Maps every legal move of a position from UCI to SAN. */
export function sanByUci(chess: Chess): Map<string, string> {
  const map = new Map<string, string>();

  for (const move of chess.moves({ verbose: true })) {
    const uci = `${move.from}${move.to}${move.promotion ?? ''}`;
    map.set(uci, move.san);
  }

  return map;
}

function rates(stat: BucketStat) {
  if (stat.count === 0) return { whiteRate: 0, drawRate: 0, blackRate: 0 };

  return {
    whiteRate: stat.whiteWins / stat.count,
    drawRate: stat.draws / stat.count,
    blackRate: blackWinsOf(stat) / stat.count,
  };
}

export function buildRows(
  data: PositionData,
  bucket: number,
  sanLookup: Map<string, string>,
  perspective: Perspective,
  /** True when the side we analyse for is the one on move. */
  analysisToMove: boolean,
): MoveRow[] {
  const entries = data.moves ?? [];
  const total = data.totals?.[bucket]?.count ?? 0;
  const best = data.bestMove?.[bucket] ?? null;

  const rows: MoveRow[] = [];

  entries.forEach((entry: MoveEntry, index) => {
    const stat = entry.stats[bucket];
    if (!stat || stat.count === 0) return;

    const playable = entry.kind === 'move' && entry.uci !== null;
    const label = playable
      ? (sanLookup.get(entry.uci as string) ?? (entry.uci as string))
      : PSEUDO_MOVE_LABELS[entry.kind];

    const whiteScore = playable ? scoreFor(entry.child, bucket, perspective) : null;

    /* The pick describes the move from the side to move's point of view, but
     * how thinly it was played is a fact about it either way — so the dimming
     * survives switching sides while the ranking does not. */
    const pick = playable ? (entry.pick?.[bucket] ?? null) : null;

    /* The shrunk score and the order only exist for the side on move: it is how
     * that side picks, and the opposite side's eval of the same position is a
     * crowd average that nothing was shrunk into. */
    const ranked = pick && analysisToMove ? pick.score : whiteScore;

    rows.push({
      key: entry.uci ?? `${entry.kind}-${index}`,
      kind: entry.kind,
      uci: entry.uci,
      label,
      count: stat.count,
      share: total > 0 ? stat.count / total : 0,
      ...rates(stat),
      expected: ranked === null ? null : expectedReturn(ranked, perspective),
      rank: pick && analysisToMove && pick.rank >= 0 ? pick.rank : null,
      solid: pick ? pick.solid : true,
      isBest: playable && entry.uci === best,
      dubious: entry.dubious ?? null,
      playable,
    });
  });

  return rows;
}

export type SortColumn = 'move' | 'games' | 'white' | 'draw' | 'black' | 'score';

export interface Sort {
  column: SortColumn;
  /** True when the first row should be the "best" one for that column. */
  bestFirst: boolean;
}

/**
 * The two sorts the table switches between on its own. On our move the rows are
 * choices, so they are shown in the tree's own order; on the opponent's move
 * they are the field we have to face, so the most played move leads.
 */
export const EVAL_SORT: Sort = { column: 'score', bestFirst: true };
export const FIELD_SORT: Sort = { column: 'games', bestFirst: true };

export function autoSortFor(analysisToMove: boolean): Sort {
  return analysisToMove ? EVAL_SORT : FIELD_SORT;
}

export function sortRows(rows: MoveRow[], sort: Sort): MoveRow[] {
  const sorted = [...rows];

  /* On our own move the tree hands us a complete order, so the eval column is
   * that order rather than a sort of the numbers in it. With nothing to rank by
   * the column falls back to sorting on the numbers themselves. */
  const ranked = rows.some((row) => row.rank !== null);

  sorted.sort((a, b) => {
    /* Pseudo-move rows have no evaluation, so they sink to the bottom of an
     * eval sort in either direction rather than pretending to be a zero. */
    if (sort.column === 'score') {
      if (ranked) {
        if (a.rank === null && b.rank === null) return b.count - a.count;
        if (a.rank === null) return 1;
        if (b.rank === null) return -1;

        const byRank = a.rank - b.rank;
        return sort.bestFirst ? byRank : -byRank;
      }

      if (a.expected === null && b.expected === null) return b.count - a.count;
      if (a.expected === null) return 1;
      if (b.expected === null) return -1;

      /* `expected` is already stated from the analysis side, so the better move
       * is the larger number whichever side that is. */
      const diff = b.expected - a.expected;

      /* An exact tie goes to the recorded pick, the way the search resolves
       * one, so the best move never sits below an equal-scoring neighbour. */
      if (diff === 0) return Number(b.isBest) - Number(a.isBest) || b.count - a.count;
      return sort.bestFirst ? diff : -diff;
    }

    if (sort.column === 'move') {
      const cmp = a.label.localeCompare(b.label);
      return sort.bestFirst ? cmp : -cmp;
    }

    const value = (row: MoveRow) => {
      switch (sort.column) {
        case 'games':
          return row.count;
        case 'white':
          return row.whiteRate;
        case 'draw':
          return row.drawRate;
        case 'black':
          return row.blackRate;
        default:
          return 0;
      }
    };

    const diff = value(b) - value(a);
    if (diff !== 0) return sort.bestFirst ? diff : -diff;
    return b.count - a.count;
  });

  return sorted;
}
