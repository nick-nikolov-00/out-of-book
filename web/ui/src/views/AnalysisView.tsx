import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { Board } from '../components/Board';
import { EvalBar } from '../components/EvalBar';
import { LineNavigator } from '../components/analysis/LineNavigator';
import { MoveTable } from '../components/analysis/MoveTable';
import { PositionSummary } from '../components/analysis/PositionSummary';
import { PerspectiveToggle } from '../components/PerspectiveToggle';
import { ViewControls } from '../components/ViewControls';
import {
  autoSortFor,
  buildRows,
  expectedReturn,
  scoreFor,
  sortRows,
  type Perspective,
  type Sort,
  type SortColumn,
} from '../analysis';
import { BUCKET_COUNT } from '../buckets';
import { useAnalysis } from '../hooks/useAnalysis';

export function AnalysisView() {
  const analysis = useAnalysis();
  const { data, bucket, sanLookup, perspective } = analysis;

  const [sort, setSort] = useState<Sort>(autoSortFor(true));
  const [hovered, setHovered] = useState<string | null>(null);

  const sideToMove = data?.sideToMove ?? (analysis.chess.turn() as 'w' | 'b');
  const analysisToMove = sideToMove === perspective;

  const rows = useMemo(
    () => (data?.found ? buildRows(data, bucket, sanLookup, perspective, analysisToMove) : []),
    [data, bucket, sanLookup, perspective, analysisToMove],
  );

  const sortedRows = useMemo(() => sortRows(rows, sort), [rows, sort]);

  const bucketTotals = useMemo(
    () =>
      Array.from({ length: BUCKET_COUNT }, (_, index) => data?.totals?.[index]?.count ?? null),
    [data],
  );

  const bestMove = data?.bestMove?.[bucket] ?? null;
  const positionScore = scoreFor(data?.eval, bucket, perspective);
  const positionExpected =
    positionScore === null ? null : expectedReturn(positionScore, perspective);

  /* Our own move is a decision, so the best expected score leads; the
   * opponent's move is the field we have to be ready for, so the most played
   * move leads. Sorting by Games explicitly pins that choice and sorting by the
   * eval column hands control back; any other column leaves the table alone. */
  const pinnedField = useRef(false);

  useEffect(() => {
    if (pinnedField.current) return;

    setSort((current) =>
      current.column === 'score' || current.column === 'games'
        ? autoSortFor(analysisToMove)
        : current,
    );
  }, [analysisToMove]);

  const autoSort = autoSortFor(analysisToMove);
  const autoSorted = sort.column === autoSort.column && sort.bestFirst === autoSort.bestFirst;

  const onSort = useCallback((column: SortColumn) => {
    if (column === 'games') pinnedField.current = true;
    if (column === 'score') pinnedField.current = false;

    setSort((current) =>
      current.column === column
        ? { column, bestFirst: !current.bestFirst }
        : { column, bestFirst: true },
    );
  }, []);

  const { setPerspective } = analysis;

  const onPerspective = useCallback(
    (side: Perspective) => {
      /* Switching sides is a fresh start for the table as well, so a pinned
       * Games sort from the other side does not carry over. */
      pinnedField.current = false;
      setPerspective(side);
    },
    [setPerspective],
  );

  const { back, forward, first, last, flip } = analysis;

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.target instanceof HTMLInputElement) return;

      switch (event.key) {
        case 'ArrowLeft':
          back();
          break;
        case 'ArrowRight':
          forward();
          break;
        case 'ArrowUp':
          first();
          break;
        case 'ArrowDown':
          last();
          break;
        case 'f':
        case 'F':
          flip();
          break;
        default:
          return;
      }

      event.preventDefault();
    };

    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [back, forward, first, last, flip]);

  const lastMove = analysis.cursor > 0 ? (analysis.line[analysis.cursor - 1]?.uci ?? null) : null;

  return (
    <>
      <ViewControls
        sideControl={
          <PerspectiveToggle
            perspective={perspective}
            onChange={onPerspective}
            label="Analysing for"
          />
        }
        bucket={bucket}
        onBucket={analysis.setBucket}
        totals={bucketTotals}
      />

      <main className="layout">
        <section className="board-column">
          <div className="board-row">
            <EvalBar score={positionScore} orientation={analysis.orientation} />

            <div className="board-frame">
              <Board
                fen={analysis.fen}
                chess={analysis.chess}
                orientation={analysis.orientation}
                highlight={hovered}
                lastMove={lastMove}
                onMove={analysis.playUci}
              />
            </div>
          </div>

          {/* Doubled triangles for the jumps rather than ⏮/⏭: those two code
            * points are emoji by default, so a phone draws them from its colour
            * emoji font while the single triangles beside them stay text. */}
          <div className="controls">
            <button type="button" className="jump" onClick={first} title="Start (↑)">
              ◀◀
            </button>
            <button type="button" onClick={back} disabled={analysis.cursor === 0} title="Back (←)">
              ◀
            </button>
            <button
              type="button"
              onClick={forward}
              disabled={analysis.cursor >= analysis.line.length}
              title="Forward (→)"
            >
              ▶
            </button>
            <button type="button" className="jump" onClick={last} title="End (↓)">
              ▶▶
            </button>
            <button type="button" onClick={flip} title="Flip board (f)">
              Flip
            </button>
            <button type="button" onClick={analysis.reset} title="Back to the starting position">
              Reset
            </button>
          </div>

          <LineNavigator line={analysis.line} cursor={analysis.cursor} onSelect={analysis.goto} />

          <FenBox fen={analysis.fen} onLoad={analysis.loadFen} />
        </section>

        <section className="panel">
          {analysis.error && <p className="banner error">{analysis.error}</p>}

          {!analysis.error && data && !data.found && (
            <p className="banner">This position is not in the tree.</p>
          )}

          {data?.found && (
            <>
              <PositionSummary
                bucket={bucket}
                totals={data.totals?.[bucket] ?? null}
                expected={positionExpected}
                perspective={perspective}
                analysisToMove={analysisToMove}
                autoSorted={autoSorted}
                bestMove={bestMove}
                bestSan={bestMove ? (sanLookup.get(bestMove) ?? bestMove) : null}
              />

              <MoveTable
                rows={sortedRows}
                sort={sort}
                perspective={perspective}
                analysisToMove={analysisToMove}
                onSort={onSort}
                onPlay={analysis.playUci}
                onHover={setHovered}
              />
            </>
          )}

          {analysis.loading && <p className="loading">Loading…</p>}
        </section>
      </main>
    </>
  );
}

function FenBox({ fen, onLoad }: { fen: string; onLoad: (fen: string) => boolean }) {
  const [draft, setDraft] = useState(fen);
  const [invalid, setInvalid] = useState(false);

  useEffect(() => {
    setDraft(fen);
    setInvalid(false);
  }, [fen]);

  return (
    <form
      className={invalid ? 'fen-box invalid' : 'fen-box'}
      onSubmit={(event) => {
        event.preventDefault();
        setInvalid(!onLoad(draft.trim()));
      }}
    >
      <input
        value={draft}
        spellCheck={false}
        aria-label="FEN"
        onChange={(event) => setDraft(event.target.value)}
      />
      <button type="submit">Analyse</button>
    </form>
  );
}
