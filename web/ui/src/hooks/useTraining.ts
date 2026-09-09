import { useCallback, useEffect, useMemo, useState } from 'react';
import { Chess } from 'chess.js';

import { ApiError, fetchPosition, type PositionData } from '../api';
import { currentBucket, rememberBucket } from '../bucketChoice';
import { buildRows, sanByUci, type MoveRow, type Perspective } from '../analysis';
import { positionKey } from '../position';
import { drawSide, resolveSide, type TrainingSide } from '../trainingSide';
import {
  chooseOpponentMove,
  grade,
  isLineComplete,
  bestRow,
  type Verdict,
} from '../training';
import {
  allLines,
  isCustom,
  onLinesChanged,
  lineFor,
  removeLine,
  saveLine,
  type CustomLine,
} from '../storage/customLines';

export const START_FEN = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

/* Long enough to read the feedback on your own move, short enough that a line
 * still plays at the speed you would rehearse it at. */
const OPPONENT_DELAY = 550;
const RESTART_DELAY = 1800;

export interface Ply {
  uci: string;
  san: string;
  /** Position reached after this ply. */
  fen: string;
}

export type Notice =
  | { kind: 'sideline'; san: string; share: number }
  | { kind: 'complete' }
  | null;

/**
 * One training session: a colour, a rating bucket, and a line being played out
 * against the field. Everything the screen shows is derived here, so the view
 * stays a rendering of this state rather than a second copy of the rules.
 */
export function useTraining() {
  const [side, setSideState] = useState<TrainingSide>('w');
  /* The colour of the line on the board, which under "random" is drawn afresh
   * for each line rather than being the setting itself. */
  const [color, setColor] = useState<Perspective>('w');
  /* Opens on the band last picked, here or on the analysis board, so crossing
   * between the two tabs does not quietly move you to a different field. */
  const [bucket, setBucketState] = useState(currentBucket);

  const [past, setPast] = useState<Ply[]>([]);
  const [future, setFuture] = useState<Ply[]>([]);

  const [data, setData] = useState<PositionData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  const [verdict, setVerdict] = useState<Verdict | null>(null);
  const [notice, setNotice] = useState<Notice>(null);
  const [revealed, setRevealed] = useState<string | null>(null);
  const [saved, setSaved] = useState<CustomLine[]>(() => allLines());

  const fen = past.at(-1)?.fen ?? START_FEN;

  const chess = useMemo(() => new Chess(fen), [fen]);

  /* From the position the data describes, not the board's: while a fetch is in
   * flight those differ, and labelling one position's moves with another's SAN
   * degrades every row to raw UCI. */
  const sanLookup = useMemo(() => sanByUci(new Chess(data?.fen ?? fen)), [data?.fen, fen]);

  const sideToMove = data?.sideToMove ?? (chess.turn() as Perspective);
  const traineeToMove = sideToMove === color;

  const refreshSaved = useCallback(() => setSaved(allLines()), []);

  /* Asks to be told when the console handle changes a saved line, so the list
   * on screen keeps up. */
  useEffect(() => onLinesChanged(refreshSaved), [refreshSaved]);


  useEffect(() => {
    const controller = new AbortController();
    let cancelled = false;

    setLoading(true);

    fetchPosition(fen, controller.signal)
      .then((result) => {
        if (cancelled) return;
        setData(result);
        setError(null);
      })
      .catch((cause: unknown) => {
        if (cancelled || (cause instanceof DOMException && cause.name === 'AbortError')) return;
        setData(null);
        setError(cause instanceof ApiError ? cause.message : 'Unexpected error loading position');
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });

    return () => {
      cancelled = true;
      controller.abort();
    };
  }, [fen]);

  const rows: MoveRow[] = useMemo(
    () => (data?.found ? buildRows(data, bucket, sanLookup, color, traineeToMove) : []),
    [data, bucket, sanLookup, color, traineeToMove],
  );

  /* Ready means the rows on screen describe the position on the board; until
   * then nothing may be graded or replied to. By position rather than by FEN
   * string, because a cache hit on a transposition carries the FEN of whichever
   * move order fetched it first. */
  const ready = !loading && data !== null && positionKey(data.fen) === positionKey(fen);
  const complete = ready && (!data.found || isLineComplete(rows, traineeToMove));

  const [proposal, setProposal] = useState<Ply | null>(null);

  /* Counts moves the board played but the session did not accept. The board
   * needs it to snap the piece back: neither the FEN nor anything else about
   * the position changes when a move is refused. */
  const [rejections, setRejections] = useState(0);

  const play = useCallback((ply: Ply) => {
    setPast((current) => [...current, ply]);
    setFuture([]);
    setProposal(null);
    setRevealed(null);
  }, []);

  /** Resolves a UCI against the current position, auto-queening a bare drag. */
  const toPly = useCallback(
    (uci: string): Ply | null => {
      const from = uci.slice(0, 2);
      const to = uci.slice(2, 4);
      const promotion = uci.slice(4, 5);

      const board = new Chess(fen);
      const attempts = promotion ? [promotion] : [undefined, 'q'];

      for (const piece of attempts) {
        try {
          const move = board.move(piece ? { from, to, promotion: piece } : { from, to });
          return {
            uci: `${move.from}${move.to}${move.promotion ?? ''}`,
            san: move.san,
            fen: board.fen(),
          };
        } catch {
          /* try the next shape */
        }
      }

      return null;
    },
    [fen],
  );

  /** Your move: graded, then committed unless it needs a decision from you. */
  const tryMove = useCallback(
    (uci: string) => {
      /* Every path that does not end in a committed move has to bump the
       * counter, or the piece the board has already moved stays where it was
       * dropped — the guards below included. */
      const refuse = () => setRejections((count) => count + 1);

      if (!ready || !traineeToMove || complete) {
        refuse();
        return;
      }

      const ply = toPly(uci);

      if (!ply) {
        refuse();
        return;
      }

      const result = grade(rows, ply.uci, ply.san, isCustom(fen, ply.uci));

      setVerdict(result);
      setNotice(null);

      if (!result.pending) {
        play(ply);
        return;
      }

      /* Only a gradeable move is worth holding on to: an unknown one has no
       * pick to weigh it against, so there is nothing to offer saving. */
      setProposal(result.grade === 'suboptimal' ? ply : null);
      refuse();
    },
    [ready, traineeToMove, complete, toPly, rows, fen, play],
  );

  /** Keeps the held-back move as a deliberate deviation and plays it. */
  const keepProposal = useCallback(() => {
    if (!proposal || !verdict) return;

    saveLine({
      fen,
      uci: proposal.uci,
      san: proposal.san,
      color,
      bucket,
      bestSan: verdict.bestSan,
      loss: verdict.loss,
    });

    refreshSaved();
    setVerdict({ ...verdict, grade: 'custom', pending: false });
    play(proposal);
  }, [proposal, verdict, fen, color, bucket, refreshSaved, play]);

  /* Cleared together on every jump: feedback describes one move from one
   * position, and none of it survives leaving it. */
  const clearFeedback = useCallback(() => {
    setProposal(null);
    setVerdict(null);
    setNotice(null);
    setRevealed(null);
  }, []);

  const reveal = useCallback(() => {
    const best = bestRow(rows);
    setRevealed(best?.uci ?? null);
  }, [rows]);

  const clearBoard = useCallback(() => {
    setPast([]);
    setFuture([]);
    clearFeedback();
  }, [clearFeedback]);

  /* Every new line redraws the colour, so "random" means random per line and
   * not one coin toss held for the session. */
  const restart = useCallback(() => {
    clearBoard();
    if (side === 'random') setColor(drawSide());
  }, [clearBoard, side]);

  /* Both stacks are rebuilt from the current values rather than from inside a
   * setState updater: those are re-run under StrictMode, and driving the other
   * stack from within one would push the same ply twice. */
  const undo = useCallback(() => {
    const last = past.at(-1);
    if (!last) return;

    setPast(past.slice(0, -1));
    setFuture([last, ...future]);
    clearFeedback();
  }, [past, future, clearFeedback]);

  const redo = useCallback(() => {
    const [next, ...rest] = future;
    if (!next) return;

    setPast([...past, next]);
    setFuture(rest);
    clearFeedback();
  }, [past, future, clearFeedback]);

  /* Draws here rather than leaning on restart: the setting has only just
   * changed, so restart is still closed over the previous one. */
  const setSide = useCallback(
    (next: TrainingSide) => {
      setSideState(next);
      setColor(resolveSide(next));
      clearBoard();
    },
    [clearBoard],
  );

  const setBucket = useCallback(
    (next: number) => {
      setBucketState(next);
      rememberBucket(next);
      restart();
    },
    [restart],
  );

  /*
   * The opponent's reply. Held in an effect rather than fired from tryMove
   * because it must also run at the start of a session you are training Black
   * in, and after an undo that lands back on their move — the trigger is the
   * position, not the click.
   */
  useEffect(() => {
    if (!ready || traineeToMove || complete) return;

    let cancelled = false;

    const timer = setTimeout(() => {
      if (cancelled) return;

      const choice = chooseOpponentMove(rows);

      if (!choice) return;

      const ply = toPly(choice.uci);
      if (!ply) return;

      setVerdict(null);
      setNotice(choice.isSideline ? { kind: 'sideline', san: choice.san, share: choice.share } : null);
      play(ply);
    }, OPPONENT_DELAY);

    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [ready, traineeToMove, complete, rows, toPly, play]);

  /* A finished line restarts on its own, so a session keeps going without
   * asking; undoing during the pause cancels it through the cleanup. */
  useEffect(() => {
    if (!complete) return;

    setNotice({ kind: 'complete' });
    const timer = setTimeout(restart, RESTART_DELAY);

    return () => clearTimeout(timer);
  }, [complete, restart]);

  const customHere = lineFor(fen);

  /* Taking back the saved line for the position on the board. The verdict goes
   * with it: the note saying the move was yours describes a decision you have
   * just withdrawn. */
  const forgetHere = useCallback(() => {
    removeLine(fen);
    setVerdict(null);
    refreshSaved();
  }, [fen, refreshSaved]);

  return {
    side,
    color,
    bucket,
    fen,
    chess,
    rows,
    data,
    error,
    loading,
    ready,
    complete,
    sideToMove,
    traineeToMove,
    /*
     * A held-back move deliberately does not lock the board: playing again from
     * the same position is how you say you want another go. Only the opponent
     * being to move, the line being over, or the rows not having landed stop a
     * drag.
     */
    accepting: ready && traineeToMove && !complete,
    line: past,
    canUndo: past.length > 0,
    canRedo: future.length > 0,
    verdict,
    notice,
    proposal,
    revealed,
    syncKey: rejections,
    saved,
    customHere,
    forgetHere,
    setSide,
    setBucket,
    tryMove,
    keepProposal,
    reveal,
    restart,
    undo,
    redo,
    refreshSaved,
  };
}
