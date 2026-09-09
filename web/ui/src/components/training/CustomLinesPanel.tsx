import { BUCKETS } from '../../buckets';
import { clearLines, removeLine, type CustomLine } from '../../storage/customLines';

interface CustomLinesPanelProps {
  lines: CustomLine[];
  /** The saved deviation for the position on the board, if there is one. */
  current: CustomLine | null;
  onChange: () => void;
}

/**
 * Every saved deviation at once, and the way to get rid of them in bulk.
 *
 * A maintenance surface rather than part of training, so it is rendered only
 * behind the debug opt-in; the line for the position actually on the board is
 * shown by the training panel itself, with its own way to take it back.
 */
export function CustomLinesPanel({ lines, current, onChange }: CustomLinesPanelProps) {
  const clearAll = () => {
    clearLines();
    onChange();
  };

  const forget = (fen: string) => {
    removeLine(fen);
    onChange();
  };

  return (
    <section className="debug-panel custom-lines">
      <header>
        <span className="summary-label">Saved lines · {lines.length}</span>

        {lines.length > 0 && (
          <button type="button" className="link" onClick={clearAll} title="Forget every saved line">
            Clear all
          </button>
        )}
      </header>

      {lines.length === 0 ? (
        <p className="empty-note">
          None yet. Play a move well below the tree's pick and you will be offered the chance to
          keep it.
        </p>
      ) : (
        <ul>
          {lines.slice(0, 12).map((line) => (
            <li key={line.fen} className={current?.fen === line.fen ? 'here' : undefined}>
              <span className="line-san">{line.san}</span>
              <span className="line-meta">
                {line.color === 'w' ? 'White' : 'Black'} · {BUCKETS[line.bucket]?.label}
                {line.bestSan ? ` · tree plays ${line.bestSan}` : ''}
              </span>
              <button type="button" className="link" onClick={() => forget(line.fen)} title="Forget this line">
                ✕
              </button>
            </li>
          ))}
        </ul>
      )}

      {lines.length > 12 && <p className="empty-note">…and {lines.length - 12} more</p>}
    </section>
  );
}
