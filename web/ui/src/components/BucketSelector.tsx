import { BUCKETS } from '../buckets';

interface BucketSelectorProps {
  bucket: number;
  onChange: (bucket: number) => void;
  /** Games recorded in each bucket for the position on the board. */
  totals: (number | null)[];
}

const compact = new Intl.NumberFormat('en', { notation: 'compact', maximumFractionDigits: 1 });

/**
 * The evaluations are computed independently per rating bucket, so exactly one
 * is active at a time — blending them would not correspond to anything
 * expectimax actually produced.
 */
export function BucketSelector({ bucket, onChange, totals }: BucketSelectorProps) {
  return (
    <div className="bucket-selector" role="radiogroup" aria-label="Rating bucket">
      {BUCKETS.map((entry, index) => {
        const games = totals[index] ?? 0;

        return (
          <button
            key={entry.label}
            type="button"
            role="radio"
            aria-checked={index === bucket}
            className={index === bucket ? 'bucket active' : 'bucket'}
            onClick={() => onChange(index)}
            title={`${entry.label} · ${games.toLocaleString()} games here`}
          >
            <span className="bucket-label">{entry.label}</span>
            <span className="bucket-games">{games > 0 ? compact.format(games) : '—'}</span>
          </button>
        );
      })}
    </div>
  );
}
