/*
 * Prints the move table for one position and bucket exactly as the UI builds
 * it, so its numbers can be diffed against apps/fen_stats.
 *
 *   node --experimental-strip-types scripts/inspect.mjs "<fen>" <bucket> [w|b] [column] [worst]
 *
 * The third argument is the side being analysed for, exactly like the UI's
 * toggle: it picks which eval array is read (evalW or evalB) and states the
 * `exp` column as that side's expected return. It defaults to the side to move,
 * and the column defaults to what the UI would auto-select for that turn.
 *
 * Requires the position server to be reachable at INITIUM_SERVER (default
 * http://127.0.0.1:8080).
 */
import { Chess } from 'chess.js';
import { autoSortFor, buildRows, sortRows, sanByUci, scoreFor, expectedReturn } from '../src/analysis.ts';

const FEN = process.argv[2];
const BUCKET = Number(process.argv[3]);

const server = process.env.INITIUM_SERVER ?? 'http://127.0.0.1:8080';
const res = await fetch(`${server}/api/position?fen=${encodeURIComponent(FEN)}`);
const data = await res.json();

const perspective = process.argv[4] === 'b' || process.argv[4] === 'w' ? process.argv[4] : data.sideToMove;
const auto = autoSortFor(data.sideToMove === perspective);
const column = process.argv[5] ?? auto.column;

const chess = new Chess(FEN);
const lookup = sanByUci(chess);
const analysisToMove = data.sideToMove === perspective;
const rows = sortRows(buildRows(data, BUCKET, lookup, perspective, analysisToMove), {
  column,
  bestFirst: process.argv[6] !== 'worst',
});

const nodeScore = scoreFor(data.eval, BUCKET, perspective);

console.log(
  `side=${data.sideToMove} for=${perspective} bucket=${BUCKET} totals=${data.totals[BUCKET].count}` +
    ` nodeEval=${nodeScore.toFixed(3)} exp=${expectedReturn(nodeScore, perspective).toFixed(3)}` +
    ` best=${data.bestMove[BUCKET]} sort=${column}`,
);
/* `rank` is the tree's own order and `exp` is the score; they disagree exactly
 * on the rows marked thin, which is the thing this script is for checking. */
console.log('move     share    games        white   draw    black    raw    exp   evidence rank');
for (const r of rows) {
  console.log(
    r.label.padEnd(8),
    (r.share * 100).toFixed(2).padStart(6) + '%',
    String(r.count).padStart(11),
    (r.whiteRate * 100).toFixed(2).padStart(7) + '%',
    (r.drawRate * 100).toFixed(2).padStart(6) + '%',
    (r.blackRate * 100).toFixed(2).padStart(6) + '%',
    (r.rawExpected === null ? '   N/A' : r.rawExpected.toFixed(3).padStart(6)),
    (r.expected === null ? '   N/A' : r.expected.toFixed(3).padStart(6)),
    String(r.pick ? r.pick.evidence : '-').padStart(10),
    String(r.rank ?? '-').padStart(4),
    r.solid ? '     ' : ' thin',
    r.isBest ? ' <-best' : '',
  );
}
