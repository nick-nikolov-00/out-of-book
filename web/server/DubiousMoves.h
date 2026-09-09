#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * Moves the book must never recommend, whatever their record says.
 *
 * The tree measures what happened, and a handful of moves win games for a
 * reason that is not about the move. After 1.d4 g6, 2.Bh6 simply hangs the
 * bishop; it scores because a large share of the field has already premoved
 * ...Bg7 and answers by giving up the exchange. Told to maximise expected
 * score, the evaluator is right to like it and the book is wrong to suggest it,
 * because the person reading the book is about to play someone who is looking
 * at the board.
 *
 * So this is an editorial judgement laid over a measurement, and it is kept
 * apart from the ranking rule for exactly that reason: the rule stays a
 * statement about the data, and this stays a short, reviewable list of
 * exceptions to it. A flagged move keeps its true share and its true win rates
 * everywhere they are shown -- the opponent really does play it, and a
 * repertoire that pretended otherwise would leave you unprepared for it. What
 * it loses is the ability to be recommended.
 *
 * Positions are keyed by Zobrist hash, as everything else here is, so flagging
 * a move flags it in every move order that reaches the same board.
 */

struct DubiousMove {
  /* Stockfish's 16-bit encoding, so it compares directly against a node blob. */
  uint16_t move = 0;
  /* Why, in a few words. Reaches the browser and is shown on the move. */
  std::string reason;
};

/*
 * The list, loaded once at startup. Empty is a perfectly good value: it means
 * nothing is flagged, and every move is judged on its record alone.
 */
class DubiousMoves {
 public:
  /*
   * Reads the file described in dubious.txt.
   *
   * Throws std::runtime_error naming the line for anything it cannot read: a
   * malformed entry, a move that is not legal where it is written, a move that
   * is legal two ways, or the same move flagged twice. A hand-maintained list
   * of a dozen entries is small enough to be right, and a typo that quietly
   * flagged nothing would show up only as the book going on recommending a
   * move somebody had already decided it should not.
   */
  static DubiousMoves load(const std::string& path);

  /* The flagged moves in one position, in the order the file lists them. */
  std::span<const DubiousMove> at(uint64_t zobrist) const;

  /* Why this move is flagged here, or nullptr when it is not. */
  const std::string* reasonFor(uint64_t zobrist, uint16_t move) const;

  size_t size() const {
    return count_;
  }

  bool empty() const {
    return count_ == 0;
  }

 private:
  std::unordered_map<uint64_t, std::vector<DubiousMove>> byPosition_;
  size_t count_ = 0;
};
