#include "Reachability.h"

#include "bitboard.h"

#include <algorithm>
#include <vector>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace Stockfish;

namespace lineodds {

namespace {

constexpr int UNREACHABLE = TargetShape::UNREACHABLE;

/* Moves for a knight between two squares on an empty board. Blockers can only
 * lengthen a route, so the empty-board figure is the lower bound wanted here.
 *
 * The hops are spelled out rather than read from the engine's attack tables,
 * because this table is built during static initialisation, before anything
 * has had the chance to call Bitboards::init(). */
struct KnightDistance {
  uint8_t d[SQUARE_NB][SQUARE_NB]{};

  KnightDistance() {
    constexpr int HOPS[8][2] = {{1, 2},  {2, 1},  {2, -1},  {1, -2},
                                {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};

    for (int from = 0; from < SQUARE_NB; ++from) {
      std::fill(std::begin(d[from]), std::end(d[from]), uint8_t(UINT8_MAX));
      d[from][from] = 0;

      std::vector<int> frontier{from};

      for (uint8_t step = 1; !frontier.empty(); ++step) {
        std::vector<int> nextFrontier;

        for (int s : frontier)
          for (const auto& [df, dr] : HOPS) {
            const int file = (s & 7) + df;
            const int rank = (s >> 3) + dr;

            if (file < 0 || file > 7 || rank < 0 || rank > 7)
              continue;

            const int to = rank * 8 + file;
            if (d[from][to] != UINT8_MAX)
              continue;

            d[from][to] = step;
            nextFrontier.push_back(to);
          }

        frontier = std::move(nextFrontier);
      }
    }
  }
};

const KnightDistance knightDistance;

int slidingMoves(PieceType pt, Square from, Square to) {
  if (from == to)
    return 0;

  /* An empty board is the whole point: a piece that needs two moves here can
   * never need fewer, however the pieces around it happen to stand. */
  if (attacks_bb(pt, from, 0) & to)
    return 1;

  /* A bishop never leaves the colour it stands on. */
  if (pt == BISHOP) {
    const auto colourOf = [](Square s) { return (int(file_of(s)) + int(rank_of(s))) & 1; };
    return colourOf(from) == colourOf(to) ? 2 : UNREACHABLE;
  }

  return 2;
}

int pieceMoves(Color c, PieceType pt, Square from, Square to) {
  if (from == to)
    return 0;

  switch (pt) {
  case KNIGHT: {
    const uint8_t d = knightDistance.d[from][to];
    return d == UINT8_MAX ? UNREACHABLE : d;
  }

  case BISHOP:
  case ROOK:
  case QUEEN:
    return slidingMoves(pt, from, to);

  case PAWN: {
    /* A pawn gains exactly one rank per move and changes file only by
     * capturing, which costs it a rank too. The double push is granted
     * whenever the pawn still stands at home, which understates the cost of a
     * route that has to capture on the way -- an understatement is safe. */
    const int df = std::abs(int(file_of(to)) - int(file_of(from)));
    const int dr = c == WHITE ? int(rank_of(to)) - int(rank_of(from))
                              : int(rank_of(from)) - int(rank_of(to));

    if (dr <= 0 || df > dr)
      return UNREACHABLE;

    const Rank home = c == WHITE ? RANK_2 : RANK_7;
    return dr - (rank_of(from) == home && dr >= 2 ? 1 : 0);
  }

  default:
    return UNREACHABLE;
  }
}

int kingMoves(Color c, Square from, Square to) {
  if (from == to)
    return 0;

  /* Castling crosses two files in a single move, so the plain king walk is
   * not the shortest route out of the home square. */
  if (from == relative_square(c, SQ_E1) &&
      (to == relative_square(c, SQ_G1) || to == relative_square(c, SQ_C1)))
    return 1;

  return distance(from, to);
}

constexpr PieceType TRACKED[] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN};

} // namespace

TargetShape::TargetShape(const Position& target) {
  Position start;
  StateInfo st;
  start.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", false, &st);

  for (Color c : {WHITE, BLACK}) {
    for (PieceType pt : TRACKED) {
      placement[c][pt] = target.pieces(c, pt);
      count[c][pt] = popcount(placement[c][pt]);

      if (count[c][pt] > popcount(start.pieces(c, pt)))
        throw std::runtime_error(
            "the target holds more material than the starting position, which means a promotion; "
            "line_odds cannot bound the moves to such a target");
    }

    kingSquare[c] = target.square<KING>(c);
    mayCastle[c][0] = target.can_castle(c == WHITE ? WHITE_OO : BLACK_OO);
    mayCastle[c][1] = target.can_castle(c == WHITE ? WHITE_OOO : BLACK_OOO);

    castles[c] = kingSquare[c] == relative_square(c, SQ_G1) ||
                 kingSquare[c] == relative_square(c, SQ_C1);
  }

  /* A side standing in the target has nothing left to do, so a bound that says
   * otherwise is measuring the wrong thing and would silently reject every
   * route to it. */
  for (Color c : {WHITE, BLACK})
    if (minMoves(target, c) != 0)
      throw std::runtime_error(
          "the move bound does not see the target as already reached (" +
          std::to_string(minMoves(target, c)) + " moves still owed); this is a bug in line_odds");
}

int TargetShape::minMoves(const Position& pos, Color c) const {
  int total = 0;

  for (PieceType pt : TRACKED) {
    const Bitboard have = pos.pieces(c, pt);

    /* Each target square is charged the cheapest piece that could fill it,
     * without insisting the pieces be told apart. Letting one piece answer for
     * two squares only lowers the bound, which is the safe direction. */
    for (Bitboard want = placement[c][pt]; want;) {
      const Square to = pop_lsb(want);

      int best = UNREACHABLE;
      for (Bitboard b = have; b;)
        best = std::min(best, pieceMoves(c, pt, pop_lsb(b), to));

      if (best >= UNREACHABLE)
        return UNREACHABLE;

      total += best;
    }
  }

  total += kingMoves(c, pos.square<KING>(c), kingSquare[c]);

  /* Castling walks the king and a rook in one move, and both of them were
   * charged above. */
  if (castles[c] && pos.square<KING>(c) == relative_square(c, SQ_E1))
    --total;

  return std::max(total, 0);
}

int TargetShape::capturesOwed(const Position& pos, Color victim) const {
  int owed = 0;

  for (PieceType pt : TRACKED)
    owed += popcount(pos.pieces(victim, pt)) - count[victim][pt];

  return owed;
}

bool TargetShape::reachable(const Position& pos, int whiteMovesLeft, int blackMovesLeft) const {
  const int movesLeft[COLOR_NB] = {whiteMovesLeft, blackMovesLeft};

  for (Color c : {WHITE, BLACK}) {
    /* Captures only ever take material away, so a side already short of what
     * the target asks for can never make it up. */
    for (PieceType pt : TRACKED)
      if (popcount(pos.pieces(c, pt)) < count[c][pt])
        return false;

    /* Castling rights are only ever lost. */
    if (mayCastle[c][0] && !pos.can_castle(c == WHITE ? WHITE_OO : BLACK_OO))
      return false;
    if (mayCastle[c][1] && !pos.can_castle(c == WHITE ? WHITE_OOO : BLACK_OOO))
      return false;

    if (minMoves(pos, c) > movesLeft[c])
      return false;

    /* Every piece the target no longer holds has to be taken, and a move
     * takes at most one. */
    if (capturesOwed(pos, ~c) > movesLeft[c])
      return false;
  }

  return true;
}

} // namespace lineodds
