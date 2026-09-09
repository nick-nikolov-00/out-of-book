import { useCallback, useEffect, useMemo, useState } from 'react';
import { Chess } from 'chess.js';

import { ApiError, fetchPosition, peekPosition, type PositionData } from '../api';
import { currentBucket, rememberBucket } from '../bucketChoice';
import { BUCKET_COUNT, DEFAULT_BUCKET } from '../buckets';
import { positionKey } from '../position';
import { href, navigate } from '../router';
import { sanByUci, type Perspective } from '../analysis';

export const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

export interface Ply {
  san: string;
  uci: string;
  /** Position reached after this ply. */
  fen: string;
}

export type Orientation = 'white' | 'black';

const ANALYSIS_PATH = '/analysis';

/**
 * The part of the board's state that belongs in the address bar: the position,
 * the rating band and the side being analysed for — what somebody would want to
 * send to someone else.
 *
 * The move list is deliberately not among them. A shared link opens its position
 * as a fresh root with an empty line, which is what typing a FEN into the box
 * already does, and it keeps the URL to a readable length.
 */
interface UrlState {
  fen: string;
  bucket: number;
  perspective: Perspective;
}

function readUrlState(): UrlState {
  const query = new URLSearchParams(window.location.search);

  let fen = START_FEN;
  const candidate = query.get('fen');

  if (candidate) {
    try {
      /* Normalised so that what is written back matches what was read, or the
       * URL is rewritten on arrival and the link somebody pasted is not the
       * link they end up on. */
      fen = new Chess(candidate).fen();
    } catch {
      /* A truncated or mangled link opens the starting position rather than
       * an error: there is nothing here worth failing over. */
    }
  }

  const rawBucket = query.get('bucket');
  const bucket = rawBucket === null ? NaN : Number(rawBucket);

  return {
    fen,
    /* A band named in the link wins; without one the view opens on whichever
     * band was last picked, so crossing over from training keeps it. */
    bucket:
      Number.isInteger(bucket) && bucket >= 0 && bucket < BUCKET_COUNT ? bucket : currentBucket(),
    perspective: query.get('as') === 'b' ? 'b' : 'w',
  };
}

/**
 * The link to a position on the analysis board. Exported because the training
 * view hands positions over, so there is one description of what such a link
 * looks like rather than two that drift apart. Defaults are left out, which
 * keeps the ordinary URL /analysis.
 */
export function analysisHref(fen: string, bucket: number, perspective: Perspective): string {
  const query = new URLSearchParams();

  if (fen !== START_FEN) query.set('fen', fen);
  if (bucket !== DEFAULT_BUCKET) query.set('bucket', String(bucket));
  if (perspective !== 'w') query.set('as', perspective);

  return href(ANALYSIS_PATH, query);
}

/**
 * Holds a single mainline plus a cursor into it, the way an analysis board
 * behaves: stepping back and then playing something new replaces the tail.
 */
export function useAnalysis() {
  /* Read once. Afterwards the board owns the URL rather than the other way
   * round, and arriving at a different one means a fresh mount. */
  const [initial] = useState(readUrlState);

  const [rootFen, setRootFen] = useState(initial.fen);
  const [line, setLine] = useState<Ply[]>([]);
  const [cursor, setCursor] = useState(0);
  const [bucket, setBucket] = useState(initial.bucket);
  const [orientation, setOrientation] = useState<Orientation>(
    initial.perspective === 'w' ? 'white' : 'black',
  );

  /* Which side we are building a repertoire for. It drives which eval array is
   * read and how the move table sorts, and is deliberately separate from the
   * board orientation so the board can still be flipped freely. */
  const [perspective, setPerspectiveState] = useState<Perspective>(initial.perspective);

  const [fetched, setFetched] = useState<PositionData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState(true);

  const fen = cursor === 0 ? rootFen : (line[cursor - 1]?.fen ?? rootFen);

  /*
   * The address bar follows the board, so the link in it is always the position
   * on screen and copying it needs no button.
   *
   * Replacing rather than pushing is the point: a history entry per move would
   * turn Back into an undo that fights the ← key, and would bury whatever the
   * user was looking at before they opened the board.
   */
  useEffect(() => {
    navigate(analysisHref(fen, bucket, perspective), { replace: true });
  }, [fen, bucket, perspective]);

  /* Handing the pick to the other views. Covering the band that arrived in the
   * URL as well as one chosen here is what lets a shared link set the band the
   * training screen opens on. */
  useEffect(() => rememberBucket(bucket), [bucket]);

  /*
   * Playing a move moves the board at once, while the panel can only follow when
   * the response lands. A position visited before is already in the cache, so it
   * is read here rather than awaited: resolving even an immediate promise costs
   * a paint in which the panel still describes the position we just left.
   *
   * Compared by position rather than by FEN string, because a cache hit on a
   * transposition carries the FEN of whichever move order fetched it first.
   */
  const here = positionKey(fen);
  const describesHere = (payload: PositionData | null | undefined) =>
    payload !== null && payload !== undefined && positionKey(payload.fen) === here;

  const data = describesHere(fetched) ? fetched : (peekPosition(fen) ?? fetched);

  /* True while the panel is still showing the previous position. */
  const stale = !describesHere(data);

  const chess = useMemo(() => new Chess(fen), [fen]);

  /*
   * Labels for the move table, so taken from the position the *data* describes
   * rather than from the board's. The two agree except while a fetch is in
   * flight, and there they must not be mixed: one position's moves looked up in
   * another's legal moves miss on nearly all of them, and every row falls back
   * to raw UCI for a frame.
   */
  const sanLookup = useMemo(() => sanByUci(new Chess(data?.fen ?? fen)), [data?.fen, fen]);

  useEffect(() => {
    const controller = new AbortController();
    let cancelled = false;

    setPending(true);

    fetchPosition(fen, controller.signal)
      .then((result) => {
        if (cancelled) return;
        setFetched(result);
        setError(null);
      })
      .catch((cause: unknown) => {
        if (cancelled || (cause instanceof DOMException && cause.name === 'AbortError')) return;
        setFetched(null);
        setError(cause instanceof ApiError ? cause.message : 'Unexpected error loading position');
      })
      .finally(() => {
        if (!cancelled) setPending(false);
      });

    return () => {
      cancelled = true;
      controller.abort();
    };
  }, [fen]);

  const playUci = useCallback(
    (uci: string) => {
      const from = uci.slice(0, 2);
      const to = uci.slice(2, 4);
      const promotion = uci.slice(4, 5);

      const board = new Chess(fen);

      /* A drag from the board carries no promotion piece, so a pawn reaching the
       * last rank is queened here; underpromotions arrive as full UCI. */
      const attempts = promotion ? [promotion] : [undefined, 'q'];
      let move = null;

      for (const piece of attempts) {
        try {
          move = board.move(piece ? { from, to, promotion: piece } : { from, to });
          break;
        } catch {
          /* try the next shape */
        }
      }

      if (!move) return;

      const ply: Ply = {
        san: move.san,
        uci: `${move.from}${move.to}${move.promotion ?? ''}`,
        fen: board.fen(),
      };

      setLine((current) => [...current.slice(0, cursor), ply]);
      setCursor((current) => current + 1);
    },
    [fen, cursor],
  );

  const goto = useCallback(
    (index: number) => setCursor(Math.max(0, Math.min(index, line.length))),
    [line.length],
  );

  const back = useCallback(() => setCursor((c) => Math.max(0, c - 1)), []);
  const forward = useCallback(() => setCursor((c) => Math.min(line.length, c + 1)), [line.length]);
  const first = useCallback(() => setCursor(0), []);
  const last = useCallback(() => setCursor(line.length), [line.length]);
  const flip = useCallback(
    () => setOrientation((o) => (o === 'white' ? 'black' : 'white')),
    [],
  );

  /* Switching sides all but always means you want to look from over there, so
   * the board turns with it; `flip` afterwards still overrides that freely. */
  const setPerspective = useCallback((side: Perspective) => {
    setPerspectiveState(side);
    setOrientation(side === 'w' ? 'white' : 'black');
  }, []);

  const reset = useCallback(() => {
    setRootFen(START_FEN);
    setLine([]);
    setCursor(0);
  }, []);

  /* Analysing an arbitrary FEN makes it the new root, so the move list stays
   * a description of what was played from wherever the analysis started. */
  const loadFen = useCallback((candidate: string) => {
    let board;

    try {
      board = new Chess(candidate);
    } catch {
      return false;
    }

    setRootFen(board.fen());
    setLine([]);
    setCursor(0);
    return true;
  }, []);

  return {
    fen,
    rootFen,
    chess,
    sanLookup,
    line,
    cursor,
    bucket,
    orientation,
    perspective,
    data,
    error,
    /* A cache hit is still "pending" for a moment, but there is nothing to wait
     * for once the panel already holds this position. */
    loading: pending && stale,
    setBucket,
    setPerspective,
    playUci,
    goto,
    back,
    forward,
    first,
    last,
    flip,
    reset,
    loadFen,
  };
}
