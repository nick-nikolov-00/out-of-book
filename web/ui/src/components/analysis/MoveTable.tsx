import type { ReactNode } from 'react';

import type { MoveRow, Perspective, Sort, SortColumn } from '../../analysis';

interface MoveTableProps {
  rows: MoveRow[];
  sort: Sort;
  perspective: Perspective;
  /** True when the side we analyse for is the one on move. */
  analysisToMove: boolean;
  onSort: (column: SortColumn) => void;
  onPlay: (uci: string) => void;
  onHover: (uci: string | null) => void;
}

const percent = (value: number) => `${(value * 100).toFixed(1)}%`;

export function MoveTable({
  rows,
  sort,
  perspective,
  analysisToMove,
  onSort,
  onPlay,
  onHover,
}: MoveTableProps) {
  if (rows.length === 0) {
    return <p className="empty-note">No moves recorded in this bucket.</p>;
  }

  const us = perspective === 'w' ? 'White' : 'Black';

  /* The eval header names the side, because the table is read as "expected for
   * whom" — but that word is also the widest thing in the header row, so on a
   * panel too narrow for six columns the span is dropped and "Exp." carries it
   * with the summary above saying which side. */
  const columns: { id: SortColumn; label: ReactNode; title: string }[] = [
    { id: 'move', label: 'Move', title: 'Sort alphabetically' },
    {
      id: 'games',
      label: 'Games',
      title: analysisToMove
        ? 'Sort by number of games'
        : `Sort by number of games — the field ${us === 'White' ? 'Black' : 'White'} plays`,
    },
    { id: 'white', label: 'White', title: 'Sort by White win rate' },
    { id: 'draw', label: 'Draw', title: 'Sort by draw rate' },
    { id: 'black', label: 'Black', title: 'Sort by Black win rate' },
    {
      id: 'score',
      label: (
        <>
          Exp.<span className="col-side"> {us}</span>
        </>
      ),
      title: analysisToMove
        ? `Order the moves the way the tree does for ${us}, its own pick first`
        : `Sort by expected score for ${us}, best first`,
    },
  ];

  /* Only the dimmed scores get a tooltip: they are the ones whose look needs
   * explaining. A solid score reads as what the column header already says. */
  const thinTitle = 'Too few games played — not enough confidence in this score';

  /* The badge on the row says why that move is flagged; this is what earns a
   * line under the table saying what being flagged does to the order. */
  const anyDubious = rows.some((row) => row.dubious !== null);

  /* Shown once under the table rather than per row, so the dimming is legible
   * without repeating an explanation on every thin move. */
  const anyThin = rows.some((row) => row.expected !== null && !row.solid);

  /* Wrapped so that a width no amount of narrowing saves scrolls inside its own
   * box rather than making the whole page scroll sideways. */
  const table = (
    <div className="table-scroll">
      <table className="move-table" onMouseLeave={() => onHover(null)}>
        <thead>
          <tr>
            {columns.map((column) => (
              <th
                key={column.id}
                className={sort.column === column.id ? 'sorted' : ''}
                title={column.title}
                onClick={() => onSort(column.id)}
              >
                {column.label}
                {sort.column === column.id && (
                  <span className="sort-arrow">{sort.bestFirst ? '▾' : '▴'}</span>
                )}
              </th>
            ))}
          </tr>
        </thead>

        <tbody>
          {rows.map((row) => (
            <tr
              key={row.key}
              className={[
                row.playable ? 'playable' : 'pseudo',
                row.isBest ? 'best' : '',
              ]
                .filter(Boolean)
                .join(' ')}
              onClick={() => row.uci && onPlay(row.uci)}
              onMouseEnter={() => onHover(row.uci)}
            >
              <td className="move-cell">
                <span className="move-san">{row.label}</span>
                {row.dubious !== null && (
                  <span
                    className="dubious-badge"
                    title={`Flagged as dubious: ${row.dubious}.`}
                  >
                    dubious
                  </span>
                )}
                {row.isBest && (
                  <span
                    className={analysisToMove ? 'best-badge' : 'best-badge theirs'}
                    title={
                      analysisToMove
                        ? `Expectimax pick for ${us} in this bucket`
                        : 'Expectimax pick for the opponent — the field rarely finds it'
                    }
                  >
                    best
                  </span>
                )}
              </td>

              <td className="games-cell">
                <span className="games-count">{row.count.toLocaleString()}</span>
                <span className="games-share">{percent(row.share)}</span>
                <span className="share-bar" style={{ width: `${row.share * 100}%` }} />
              </td>

              <td>{percent(row.whiteRate)}</td>
              <td>{percent(row.drawRate)}</td>
              <td>{percent(row.blackRate)}</td>

              <td className="eval-cell">
                {row.expected === null ? (
                  <span className="no-eval" title="Resulting position is not in the tree">
                    —
                  </span>
                ) : (
                  <span
                    className={row.solid ? undefined : 'eval-thin'}
                    title={row.solid ? undefined : thinTitle}
                  >
                    {percent(row.expected)}
                  </span>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );

  if (!anyThin && !anyDubious) return table;

  return (
    <>
      {table}
      {anyThin && (
        <p className="table-note">
          {analysisToMove
            ? 'Rows are in the order the tree plays them. A dimmed score rests on too few games to be ranked on, so it can sit above a move the tree still prefers.'
            : 'A dimmed score rests on too few games to mean much.'}
        </p>
      )}
      {anyDubious && (
        <p className="table-note">
          {analysisToMove
            ? 'A move marked dubious wins games for a reason that will not be there when you play it, so it is ranked last however it scores.'
            : 'A move marked dubious is never recommended, but the field still plays it as often as the games say — be ready for it.'}
        </p>
      )}
    </>
  );
}
