#pragma once

#include "position.h"
#include "types.h"

#include <array>

namespace lineodds {

/*
 * Whether a position can still become the target within a given number of
 * moves for each side.
 *
 * The target FEN pins the placement of every piece on the board, which makes
 * the remaining moves of *both* sides nearly forced: each side has to spend
 * its moves walking its own pieces onto their target squares, and a side with
 * no move to spare cannot afford a single step that does not make progress.
 * That is what keeps the forward enumeration small enough to be exhaustive.
 *
 * Every bound here is a lower bound on the moves a side still needs, and every
 * predicate is a necessary condition, so a position that can reach the target
 * is never rejected. The bounds are deliberately loose where tightening them
 * would cost more than the states they would save; the exact answer comes from
 * the backward pass, which only has to sift what survives this test.
 */
class TargetShape {
 public:
  /* Throws when the target holds material the starting position cannot
   * produce without a promotion, which this bound does not model. */
  explicit TargetShape(const Stockfish::Position& target);

  bool reachable(const Stockfish::Position& pos, int whiteMovesLeft, int blackMovesLeft) const;

  /* A lower bound on the moves `c` still needs. UNREACHABLE when no sequence
   * of moves by `c` alone could place its pieces as the target has them. */
  int minMoves(const Stockfish::Position& pos, Stockfish::Color c) const;

  static constexpr int UNREACHABLE = 1'000;

 private:
  /* Pieces the given side must still see captured before the placement
   * matches, one move apiece for whoever does the capturing. */
  int capturesOwed(const Stockfish::Position& pos, Stockfish::Color victim) const;

  int count[Stockfish::COLOR_NB][Stockfish::PIECE_TYPE_NB]{};
  Stockfish::Bitboard placement[Stockfish::COLOR_NB][Stockfish::PIECE_TYPE_NB]{};
  Stockfish::Square kingSquare[Stockfish::COLOR_NB]{};
  bool castles[Stockfish::COLOR_NB]{};
  bool mayCastle[Stockfish::COLOR_NB][2]{}; // [colour][0 = kingside, 1 = queenside]
};

} // namespace lineodds
