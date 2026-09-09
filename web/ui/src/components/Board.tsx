import { useEffect, useRef } from 'react';
import { Chessground } from '@lichess-org/chessground';
import type { Api } from '@lichess-org/chessground/api';
import type { Color, Key } from '@lichess-org/chessground/types';
import type { DrawShape } from '@lichess-org/chessground/draw';
import type { Chess } from 'chess.js';

/*
 * Dragging a pawn to the last rank auto-queens: Chessground ships no promotion
 * dialog, and an underpromotion is still reachable by clicking its own row in
 * the move table, which carries the full UCI.
 */
interface BoardProps {
  fen: string;
  chess: Chess;
  orientation: 'white' | 'black';
  /** Arrow to draw, as UCI; used to preview the hovered move-table row. */
  highlight: string | null;
  lastMove: string | null;
  onMove: (uci: string) => void;
  /**
   * Which side may be dragged. Analysis lets you push both sides around;
   * training restricts it to the trainee, and to 'none' while the opponent is
   * thinking or a decision is waiting, so a stray drag cannot skip a turn.
   */
  movable?: 'both' | 'white' | 'black' | 'none';
  /**
   * Bump to force a redraw when the position has not changed but the board has
   * drifted from it. Chessground moves the piece as soon as it is dropped, so a
   * refused move stays on screen until something re-sets the FEN — and a
   * refusal is exactly the case where nothing else changed.
   */
  syncKey?: number;
}

function dests(chess: Chess): Map<Key, Key[]> {
  const map = new Map<Key, Key[]>();

  for (const move of chess.moves({ verbose: true })) {
    const from = move.from as Key;
    const list = map.get(from);

    if (list) list.push(move.to as Key);
    else map.set(from, [move.to as Key]);
  }

  return map;
}

/* Chessground clears the highlight when given an empty pair. */
function squares(uci: string | null): Key[] {
  if (!uci || uci.length < 4) return [];
  return [uci.slice(0, 2) as Key, uci.slice(2, 4) as Key];
}

export function Board({
  fen,
  chess,
  orientation,
  highlight,
  lastMove,
  onMove,
  movable = 'both',
  syncKey = 0,
}: BoardProps) {
  const element = useRef<HTMLDivElement>(null);
  const ground = useRef<Api | null>(null);

  /* onMove is read through a ref so that re-creating the callback each render
   * never forces Chessground to be rebuilt. */
  const onMoveRef = useRef(onMove);
  onMoveRef.current = onMove;

  useEffect(() => {
    if (!element.current) return;

    ground.current = Chessground(element.current, {
      fen,
      orientation,
      coordinates: true,
      movable: {
        free: false,
        showDests: true,
        events: {
          after: (from, to) => onMoveRef.current(`${from}${to}`),
        },
      },
      draggable: { enabled: true, showGhost: true },
      animation: { enabled: true, duration: 180 },
    });

    return () => {
      ground.current?.destroy();
      ground.current = null;
    };
  }, []);

  useEffect(() => {
    const api = ground.current;
    if (!api) return;

    const turnColor: Color = chess.turn() === 'w' ? 'white' : 'black';

    api.set({
      fen,
      orientation,
      turnColor,
      lastMove: squares(lastMove),
      check: chess.inCheck() ? turnColor : false,
      /* Locking the board is done with an empty destination map rather than an
       * undefined colour: it blocks every drag just the same, and the config
       * type does not accept an explicit undefined here. */
      movable:
        movable === 'none'
          ? { color: 'both', dests: new Map<Key, Key[]>() }
          : { color: movable, dests: dests(chess) },
    });
  }, [fen, chess, orientation, lastMove, movable, syncKey]);

  /*
   * Chessground caches the board's bounding rect and only clears it on a
   * document scroll or a window resize, so anything that moves the board
   * without resizing it — a verdict line above it wrapping onto a second row, a
   * phone's address bar collapsing — leaves every tap offset by the shift while
   * the pieces stay drawn in the right place.
   *
   * Watching the position costs one rect per layout change and hands it a fresh
   * one only when it actually moved.
   */
  useEffect(() => {
    const el = element.current;
    if (!el) return;

    let last = el.getBoundingClientRect();

    const check = () => {
      const api = ground.current;
      if (!api) return;

      const now = el.getBoundingClientRect();

      /* Sub-pixel drift is what fractional layout produces on its own, and is
       * far too small to reach the next square. */
      if (Math.abs(now.top - last.top) < 0.5 && Math.abs(now.left - last.left) < 0.5) return;

      last = now;
      api.redrawAll();
    };

    const observer = new ResizeObserver(check);
    observer.observe(document.body);
    observer.observe(el);

    return () => observer.disconnect();
  }, []);

  useEffect(() => {
    const api = ground.current;
    if (!api) return;

    const [orig, dest] = squares(highlight);
    const shapes: DrawShape[] = orig && dest ? [{ orig, dest, brush: 'green' }] : [];
    api.setShapes(shapes);
  }, [highlight]);

  return <div className="board-container" ref={element} />;
}
