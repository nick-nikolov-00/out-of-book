import type { Perspective } from './analysis';

/**
 * The side setting of a training session. Unlike the analysis board's
 * perspective this is a choice about how the next line starts, so it holds one
 * value the board itself can never be in: "random" draws a colour per line.
 */
export type TrainingSide = Perspective | 'random';

/** The colour a fresh line is trained as. */
export function drawSide(): Perspective {
  return Math.random() < 0.5 ? 'w' : 'b';
}

/** The colour to train as under a setting, drawing one where the setting asks. */
export function resolveSide(side: TrainingSide): Perspective {
  return side === 'random' ? drawSide() : side;
}
