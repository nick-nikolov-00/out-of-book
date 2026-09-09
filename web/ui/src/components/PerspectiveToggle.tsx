import type { Perspective } from '../analysis';

interface PerspectiveToggleProps {
  perspective: Perspective;
  onChange: (perspective: Perspective) => void;
  /** What picking a side means here — "Analysing for", "Training as". */
  label: string;
}

const SIDES: { id: Perspective; label: string; title: string }[] = [
  {
    id: 'w',
    label: 'White',
    title: 'Evaluate every position with White playing best and Black playing the field',
  },
  {
    id: 'b',
    label: 'Black',
    title: 'Evaluate every position with Black playing best and White playing the field',
  },
];

/**
 * Picks the side the whole board is analysed for. It is not the side to move:
 * it fixes which expectimax eval is read, which way the percentages point, and
 * which turns count as our choice rather than the field.
 */
export function PerspectiveToggle({ perspective, onChange, label }: PerspectiveToggleProps) {
  return (
    <div className="perspective-toggle" role="radiogroup" aria-label={label}>
      <span className="perspective-caption">{label}</span>

      {SIDES.map((side) => (
        <button
          key={side.id}
          type="button"
          role="radio"
          aria-checked={perspective === side.id}
          className={perspective === side.id ? `side ${side.id} active` : `side ${side.id}`}
          onClick={() => onChange(side.id)}
          title={side.title}
        >
          {side.label}
        </button>
      ))}
    </div>
  );
}
