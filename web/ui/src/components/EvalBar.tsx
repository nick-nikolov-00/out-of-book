interface EvalBarProps {
  /** White score in [0, 1], or null when the position has no evaluation. */
  score: number | null;
  orientation: 'white' | 'black';
}

/**
 * A Lichess-style vertical bar, except the scale is a win-expectancy in
 * [0, 1] rather than centipawns, because that is what the tree stores.
 */
export function EvalBar({ score, orientation }: EvalBarProps) {
  const white = score ?? 0.5;

  const whitePercent = Math.round(white * 1000) / 10;
  const percent = orientation === 'black' ? (100 - whitePercent) : whitePercent;

  return (
    <div className={orientation === 'black' ? 'eval-bar flipped' : 'eval-bar'}>
      <div
        className="eval-bar-white"
        style={{ height: `${whitePercent}%` }}
        aria-hidden="true"
      />
      <span className="eval-bar-value" title="Expected score">
        {score === null ? '—' : percent.toFixed(1)}
      </span>
    </div>
  );
}
