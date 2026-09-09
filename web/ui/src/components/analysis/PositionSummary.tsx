import { BUCKETS } from '../../buckets';
import type { Perspective } from '../../analysis';
import { blackWinsOf, type BucketStat } from '../../api';

interface PositionSummaryProps {
  bucket: number;
  totals: BucketStat | null;
  /** This position's value as the analysis side's expected return. */
  expected: number | null;
  perspective: Perspective;
  /** True when the side we analyse for is the one on move. */
  analysisToMove: boolean;
  /** True while the move table still follows the turn-driven default sort. */
  autoSorted: boolean;
  bestMove: string | null;
  bestSan: string | null;
}

const percent = (value: number, of: number) => (of === 0 ? '0.0%' : `${((value / of) * 100).toFixed(1)}%`);

export function PositionSummary({
  bucket,
  totals,
  expected,
  perspective,
  analysisToMove,
  autoSorted,
  bestMove,
  bestSan,
}: PositionSummaryProps) {
  const games = totals?.count ?? 0;
  const us = perspective === 'w' ? 'White' : 'Black';
  const them = perspective === 'w' ? 'Black' : 'White';

  return (
    <section className="summary">
      <header className="summary-head">
        <div>
          <span className="summary-label">Games in {BUCKETS[bucket]?.label}</span>
          <span className="summary-value">{games.toLocaleString()}</span>
        </div>

        <div>
          <span className="summary-label">Expected · {us}</span>
          <span
            className="summary-value"
            title={`What ${us} scores from here playing best against the field`}
          >
            {expected === null ? '—' : `${(expected * 100).toFixed(1)}%`}
          </span>
        </div>

        <div>
          <span className="summary-label">Best move{analysisToMove ? '' : ` · ${them}`}</span>
          <span className="summary-value">{bestSan ?? bestMove ?? '—'}</span>
        </div>
      </header>

      {/* Explains why the move table reorders itself as the turn alternates.
        * The second half is dropped once a hand-picked sort is in force, so the
        * note never describes an order the table is not in. */}
      <p className="summary-note">
        {analysisToMove
          ? `${us} to move — your choice.`
          : `${them} to move — the field.`}
        {autoSorted &&
          (analysisToMove
            ? ' Moves are in the order the tree plays them.'
            : ' Moves are ranked by how often they are played.')}
      </p>

      {totals && games > 0 && (
        <div className="wdl-bar" title={`White ${totals.whiteWins.toLocaleString()} · Draws ${totals.draws.toLocaleString()} · Black ${blackWinsOf(totals).toLocaleString()}`}>
          <span className="wdl white" style={{ width: percent(totals.whiteWins, games) }}>
            {percent(totals.whiteWins, games)}
          </span>
          <span className="wdl draw" style={{ width: percent(totals.draws, games) }}>
            {percent(totals.draws, games)}
          </span>
          <span className="wdl black" style={{ width: percent(blackWinsOf(totals), games) }}>
            {percent(blackWinsOf(totals), games)}
          </span>
        </div>
      )}
    </section>
  );
}
