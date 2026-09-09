import type { TrainingSide } from '../../trainingSide';

interface TrainingSideToggleProps {
  side: TrainingSide;
  onChange: (side: TrainingSide) => void;
  /** The colour actually being played, which "Random" alone does not say. */
  playing: 'w' | 'b';
}

const SIDES: { id: TrainingSide; label: string; title: string }[] = [
  { id: 'w', label: 'White', title: 'Train every line as White' },
  { id: 'b', label: 'Black', title: 'Train every line as Black' },
  { id: 'random', label: 'Random', title: 'Draw a colour afresh for every line' },
];

/**
 * Picks the side training runs as. It is the same three-way choice a game
 * against an opponent offers, so a session can rehearse both halves of an
 * opening without the colour being the thing you are anticipating.
 */
export function TrainingSideToggle({ side, onChange, playing }: TrainingSideToggleProps) {
  return (
    <div className="perspective-toggle" role="radiogroup" aria-label="Training as">
      <span className="perspective-caption">Training as</span>

      {SIDES.map((option) => (
        <button
          key={option.id}
          type="button"
          role="radio"
          aria-checked={side === option.id}
          className={side === option.id ? `side ${option.id} active` : `side ${option.id}`}
          onClick={() => onChange(option.id)}
          title={option.title}
        >
          {option.label}
        </button>
      ))}

      {/* Which colour the draw landed on is otherwise only readable off the
        * board's orientation, which is a slower thing to check mid-line. */}
      {side === 'random' && (
        <span className="perspective-drawn">{playing === 'w' ? 'White' : 'Black'}</span>
      )}
    </div>
  );
}
