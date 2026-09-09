import type { Notice } from '../../hooks/useTraining';
import type { Verdict } from '../../training';

interface TrainingFeedbackProps {
  verdict: Verdict | null;
  notice: Notice;
}

/*
 * Only a committed move names the tree's pick. A move that fell short is not
 * played, so you are still standing in the same position with the same question
 * in front of you, and printing the answer would be answering it for you.
 */
const GRADE_TEXT: Record<Verdict['grade'], (v: Verdict) => string> = {
  best: () => '✓ Best move',
  excellent: (v) => `Good — ${v.loss.toFixed(1)}% behind ${v.bestSan}`,
  custom: (v) => `✓ Your line${v.bestSan ? ` — recommended ${v.bestSan}` : ''}`,
  suboptimal: (v) => `${v.loss.toFixed(1)}% behind the best move — try again`,
  unknown: () => 'Unknown move. Try another.',
};

/**
 * One line of feedback, in a fixed slot. A notice about the opponent's reply
 * outranks the grade of your own move because it is the thing you have to react
 * to; the grade has already been read by the time it appears.
 *
 * role="status" because it changes without anyone navigating to it, and on a
 * narrow screen CSS moves it above the board visually but not in the document.
 */
export function TrainingFeedback({ verdict, notice }: TrainingFeedbackProps) {
  if (notice?.kind === 'complete') {
    return <p className="feedback complete" role="status">Line complete — starting again</p>;
  }

  if (notice?.kind === 'sideline') {
    return (
      <p className="feedback sideline" role="status">
        ⚠ Sideline: {notice.san} is played in {(notice.share * 100).toFixed(1)}% of games here
      </p>
    );
  }

  if (!verdict) return <p className="feedback idle" role="status">Your move</p>;

  return (
    <p className={`feedback ${verdict.grade}`} role="status">
      {GRADE_TEXT[verdict.grade](verdict)}
    </p>
  );
}
