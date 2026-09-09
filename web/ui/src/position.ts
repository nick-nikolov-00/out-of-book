/**
 * The identity of a position, for anything that wants to treat two ways of
 * reaching the same board as the same thing.
 *
 * A FEN's last two fields are the halfmove clock and the move number, which are
 * facts about the game so far rather than about the position: 1.e4 e5 2.Nf3 and
 * 1.Nf3 e5 2.e4 reach an identical board and differ only in the clock. The tree
 * does not distinguish them either, being keyed by a hash of the board alone.
 *
 * The en passant field is kept because both sides agree on what belongs there:
 * each records the square only when the capture is actually legal, so two FENs
 * that mean the same position to the server produce the same key here.
 */
export function positionKey(fen: string): string {
  return fen.split(' ').slice(0, 4).join(' ');
}
