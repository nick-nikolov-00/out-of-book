import { useEffect } from 'react';

import { Board } from '../components/Board';
import { EvalBar } from '../components/EvalBar';
import { ViewControls } from '../components/ViewControls';
import { RatingBandPanel } from '../components/debug/RatingBandPanel';
import { CustomLinesPanel } from '../components/training/CustomLinesPanel';
import { TrainingSideToggle } from '../components/training/TrainingSideToggle';
import { SaveLinePrompt } from '../components/training/SaveLinePrompt';
import { TrainingFeedback } from '../components/training/TrainingFeedback';
import { scoreFor } from '../analysis';
import { isDebug } from '../debug';
import { BUCKET_COUNT } from '../buckets';
import { analysisHref } from '../hooks/useAnalysis';
import { useTraining } from '../hooks/useTraining';
import { isPlainLeftClick, navigate } from '../router';

/**
 * Play a line out against the field: the tree replies as the crowd does,
 * weighted by how often each move was actually played, and every move of yours
 * is graded against the move the tree itself picks in the chosen bucket.
 */
export function TrainingView() {
  const training = useTraining();
  const { data, bucket, color, verdict, proposal, undo, redo, restart, reveal } = training;

  const bucketTotals = Array.from(
    { length: BUCKET_COUNT },
    (_, index) => data?.totals?.[index]?.count ?? null,
  );

  /* The bar stays on the white axis like the analysis board's, but reads the
   * eval belonging to the side being trained. */
  const whiteScore = scoreFor(data?.eval, bucket, color);

  const lastMove = training.line.at(-1)?.uci ?? null;

  /* Carries the whole setting over, not just the position: the board opens on
   * the same band and the same side you were training. */
  const analysisLink = analysisHref(training.fen, bucket, color);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.target instanceof HTMLInputElement) return;

      switch (event.key) {
        case 'ArrowLeft':
          undo();
          break;
        case 'ArrowRight':
          redo();
          break;
        case 'r':
        case 'R':
          restart();
          break;
        case ' ':
          reveal();
          break;
        default:
          return;
      }

      event.preventDefault();
    };

    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [undo, redo, restart, reveal]);

  return (
    <>
      <ViewControls
        sideControl={
          <TrainingSideToggle side={training.side} onChange={training.setSide} playing={color} />
        }
        bucket={bucket}
        onBucket={training.setBucket}
        totals={bucketTotals}
      />

      <main className="layout">
        <section className="board-column">
          <div className="board-row">
            <EvalBar score={whiteScore} orientation={color === 'w' ? 'white' : 'black'} />

            <div className="board-frame">
              <Board
                fen={training.fen}
                chess={training.chess}
                orientation={color === 'w' ? 'white' : 'black'}
                highlight={training.revealed}
                lastMove={lastMove}
                onMove={training.tryMove}
                movable={training.accepting ? (color === 'w' ? 'white' : 'black') : 'none'}
                syncKey={training.syncKey}
              />
            </div>
          </div>

          <div className="controls">
            <button type="button" onClick={undo} disabled={!training.canUndo} title="Back (←)">
              ◀
            </button>
            <button type="button" onClick={redo} disabled={!training.canRedo} title="Forward (→)">
              ▶
            </button>
            <button type="button" onClick={reveal} title="Show the tree's move (space)">
              Reveal
            </button>
            <button type="button" onClick={restart} title="New line (r)">
              Restart
            </button>

            {/* A real link, so ⌘/ctrl-click opens the board in a new tab and
              * the session survives the trip. A plain click is a navigation you
              * chose, so it pushes and Back comes home. */}
            <a
              className="control-link"
              href={analysisLink}
              title="Open this position on the analysis board"
              onClick={(event) => {
                if (!isPlainLeftClick(event)) return;

                event.preventDefault();
                navigate(analysisLink);
              }}
            >
              Analyse
            </a>
          </div>

          <TrainingFeedback verdict={verdict} notice={training.notice} />
        </section>

        <section className="panel">
          {training.error && <p className="banner error">{training.error}</p>}

          {proposal && verdict?.grade === 'suboptimal' && (
            <SaveLinePrompt verdict={verdict} onKeep={training.keepProposal} />
          )}

          {/* The one saved line worth showing unprompted is the one for the
            * position in front of you: it is the reason this move is not being
            * asked about, and here is where you would take it back. */}
          {training.customHere && (
            <section className="summary">
              <p className="summary-note">
                Saved here: <strong>{training.customHere.san}</strong>
                <button
                  type="button"
                  className="link"
                  onClick={training.forgetHere}
                  title="Forget this line and be asked about the position again"
                >
                  forget
                </button>
              </p>
            </section>
          )}

          <MoveList line={training.line} />

          {isDebug() && (
            <>
              <CustomLinesPanel
                lines={training.saved}
                current={training.customHere}
                onChange={training.refreshSaved}
              />

              <RatingBandPanel />
            </>
          )}
        </section>
      </main>
    </>
  );
}

/** The line so far, read-only — stepping through it is what ← and → are for. */
function MoveList({ line }: { line: { san: string; uci: string }[] }) {
  if (line.length === 0) return <p className="empty-note">No moves yet.</p>;

  return (
    <div className="line-navigator">
      {line.map((ply, index) => (
        <span key={`${index}-${ply.uci}`} className="ply-group">
          {index % 2 === 0 && <span className="move-number">{index / 2 + 1}.</span>}
          <span className="ply">{ply.san}</span>
        </span>
      ))}
    </div>
  );
}
