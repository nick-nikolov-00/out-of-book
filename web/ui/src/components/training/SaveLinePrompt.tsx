import type { Verdict } from '../../training';

interface SaveLinePromptProps {
  verdict: Verdict;
  onKeep: () => void;
}

/**
 * An offer, not a question that has to be answered: the move is held back but
 * the board stays live, so playing again from the same position is how you say
 * you wanted another go. Only the button does anything irreversible.
 *
 * The tree's pick is deliberately not named. The move was not played, so the
 * position is still in front of you to guess at, and keeping a line is a choice
 * about your own move rather than one that needs the answer shown first —
 * Reveal is there when you want it.
 */
export function SaveLinePrompt({ verdict, onKeep }: SaveLinePromptProps) {
  return (
    <div className="save-prompt">
      <p>
        <strong>{verdict.san}</strong> gives up {verdict.loss.toFixed(1)}% against the best move
        here.
      </p>

      <div className="save-prompt-actions">
        <button type="button" className="primary" onClick={onKeep}>
          Keep as my line
        </button>
        <span className="save-prompt-hint">or just play another move</span>
      </div>
    </div>
  );
}
