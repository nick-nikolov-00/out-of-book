#pragma once

#include "DubiousMoves.h"
#include "OtbStore.h"
#include "MoveRanking.h"

#include "position.h"
#include "types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

constexpr size_t NUM_BUCKETS = Stockfish::NUMBER_OF_BUCKETS;

struct BucketStat {
  uint64_t count = 0;
  uint64_t whiteWins = 0;
  uint64_t draws = 0;

  bool empty() const {
    return count == 0;
  }
};

/* One table row: a move (or a pseudo-move) with its per-bucket tallies. */
struct MoveRow {
  uint16_t raw = 0;
  std::array<BucketStat, NUM_BUCKETS> stats{};
};

enum class RowKind { Move, Termination, Aggregate };

RowKind kindOf(uint16_t raw);
const char* kindName(RowKind kind);

/* A playable row's child, read once: its evaluations and the per-bucket games
 * standing behind them. */
struct Child {
  OteEntry ote{};
  ranking::BucketSums sums{};
};

/*
 * One node's candidate moves, scored the way the evaluator scored them.
 *
 * A child's own evaluation is only the starting point of that score: it is
 * shrunk toward a prior — the same position seen at neighbouring rating buckets
 * — by an amount set by how thin the evidence behind it is. A move played a
 * hundred times keeps almost none of a spectacular eval. This is why the tree's
 * chosen move is routinely not the best-scoring child, and why a move list
 * ordered on the raw evaluations does not reproduce the search's own ordering.
 *
 * Scores are white scores like everything else here, so White maximises them
 * and Black minimises them.
 */
struct Analysis {
  /* `present` is a flag rather than a NaN score because this builds with
   * -ffast-math, under which a NaN test folds to false: an unset score would
   * look like a real one and would win comparisons against real ones. */
  struct Pick {
    bool present = false;
    ranking::Candidate candidate{};
    double evidence = 0;
    double prior = 0;
    /*
     * Where this move sits in the order the search itself would play the
     * position out: 0 is the move it picks, 1 the move it would pick if that
     * one were taken away, and so on. -1 for a row that is not a candidate.
     */
    int rank = -1;
  };

  std::vector<MoveRow> rows;
  std::vector<std::optional<Child>> children;
  std::vector<bool> playable;
  std::vector<std::array<Pick, NUM_BUCKETS>> picks;

  /*
   * Why a row is a move the book will not recommend, and empty for a row that
   * is not one.
   *
   * A flagged row is ranked last in every bucket, behind every move that is
   * not flagged, so it can hold rank 0 only when there is nothing else to
   * play. Nothing else about it changes: its score, its share and its win
   * rates are what they were, because they are measurements and the flag is a
   * judgement about what to advise.
   *
   * Borrowed from the list this was analysed against, which outlives any
   * analysis of it; an empty reason is refused when the list is loaded, so an
   * empty view here means unflagged and nothing else.
   */
  std::vector<std::string_view> dubious;

  /* The row holding rank 0 in a bucket, i.e. the move the rule picks; npos
   * when the bucket has no candidates. */
  size_t bestRow(size_t bucket) const {
    for (size_t i = 0; i < picks.size(); ++i)
      if (picks[i][bucket].rank == 0)
        return i;

    return std::string::npos;
  }
};

Analysis analyse(const OtbStore& store, Stockfish::Position& pos, const OtbStore::Node& node,
                 const DubiousMoves& dubious);

/*
 * The move the ranking chooses in one bucket, or 0 when nothing was played:
 * the head of the order, which is the evaluator's own pick by construction.
 */
uint16_t chosenMove(const Analysis& analysis, size_t bucket);
