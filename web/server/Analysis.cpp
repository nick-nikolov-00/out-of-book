#include "Analysis.h"

#include "movegen.h"

#include <algorithm>
#include <span>

using namespace Stockfish;

namespace {

/*
 * Collapses the node blob's (move, bucket) rows into one row per move while
 * preserving the order the moves appear in the blob.
 */
std::vector<MoveRow> collectRows(const OtbStore::Node& node) {
  std::vector<MoveRow> rows;
  rows.reserve(node.nEntries);

  for (uint16_t i = 0; i < node.nEntries; ++i) {
    const NodeBlobEntry& e = node.entries[i];

    if (e.bucket >= NUM_BUCKETS)
      continue;

    /* Blobs keep a move's buckets adjacent, so the last row is almost always
     * the right one; the scan keeps us correct if that ever stops holding. */
    MoveRow* row = nullptr;

    if (!rows.empty() && rows.back().raw == e.move) {
      row = &rows.back();
    } else {
      const auto it = std::find_if(rows.begin(), rows.end(), [&](const MoveRow& r) {
        return r.raw == e.move;
      });

      if (it != rows.end()) {
        row = &*it;
      } else {
        rows.push_back(MoveRow{e.move, {}});
        row = &rows.back();
      }
    }

    BucketStat& bucket = row->stats[e.bucket];
    bucket.count += e.count;
    bucket.whiteWins += e.white_wins;
    bucket.draws += e.draws;
  }

  return rows;
}

/*
 * Ranks every bucket's candidates into the shrink rule's total order, stamping
 * each pick with its place in it.
 *
 * A move list is worth showing in exactly this order and no other. The scores
 * are shown beside the moves and will not always agree with it: a thin move can
 * out-score the move above it and still sit lower, because the rule ranks on
 * the score discounted by its own uncertainty rather than on the score.
 *
 * Flagged moves are then moved to the end, keeping the order they had among
 * themselves. That is a separate pass on purpose: the rule stays a statement
 * about the data, and the flag stays an editorial exception laid over the
 * answer it gave, which is the only arrangement in which either can be read on
 * its own.
 */
void rankBuckets(Analysis& analysis, bool whiteToMove) {
  std::vector<ranking::Candidate> candidates;
  std::vector<size_t> rowOf;

  for (size_t b = 0; b < NUM_BUCKETS; ++b) {
    candidates.clear();
    rowOf.clear();

    for (size_t i = 0; i < analysis.picks.size(); ++i) {
      if (!analysis.picks[i][b].present)
        continue;

      candidates.push_back(analysis.picks[i][b].candidate);
      rowOf.push_back(i);
    }

    std::vector<size_t> order = ranking::rankAll(candidates, whiteToMove);

    std::stable_partition(order.begin(), order.end(), [&](size_t candidate) {
      return analysis.dubious[rowOf[candidate]].empty();
    });

    for (size_t rank = 0; rank < order.size(); ++rank)
      analysis.picks[rowOf[order[rank]]][b].rank = static_cast<int>(rank);
  }
}

} // namespace

RowKind kindOf(uint16_t raw) {
  if (raw == Move::termination().raw())
    return RowKind::Termination;

  if (raw == Move::forcedAggregation().raw())
    return RowKind::Aggregate;

  return RowKind::Move;
}

const char* kindName(RowKind kind) {
  switch (kind) {
  case RowKind::Termination:
    return "termination";
  case RowKind::Aggregate:
    return "aggregate";
  default:
    return "move";
  }
}

Analysis analyse(const OtbStore& store, Position& pos, const OtbStore::Node& node,
                 const DubiousMoves& dubious) {
  Analysis out;
  out.rows = collectRows(node);

  const size_t n = out.rows.size();
  out.children.resize(n);
  out.playable.assign(n, false);
  out.picks.resize(n);
  out.dubious.assign(n, {});

  /* One lookup for the position, then a walk of however few moves it flags,
   * rather than a hash of every row against a list that is almost always
   * empty. */
  for (const DubiousMove& flagged : dubious.at(pos.key()))
    for (size_t i = 0; i < n; ++i)
      if (out.rows[i].raw == flagged.move)
        out.dubious[i] = flagged.reason;

  const bool whiteToMove = pos.side_to_move() == WHITE;
  const MoveList<LEGAL> legal(pos);

  /* Children are resolved up front rather than while the rows are streamed out:
   * the scores below need this position's chance value, which is itself an
   * average over all of the children, so one pass would not be enough. */
  for (size_t i = 0; i < n; ++i) {
    const Move move(out.rows[i].raw);

    if (kindOf(out.rows[i].raw) != RowKind::Move || !legal.contains(move))
      continue;

    out.playable[i] = true;

    if (!store.hasEvals())
      continue;

    StateInfo childState;
    pos.do_move(move, childState);
    const auto childIndex = store.find(pos.key());
    pos.undo_move(move);

    if (!childIndex)
      continue;

    Child child;
    store.readEvals(*childIndex, child.ote);

    OtbStore::Node childNode;
    store.readNode(*childIndex, childNode);
    child.sums = ranking::bucketSumsOf({childNode.entries, childNode.nEntries});

    out.children[i] = child;
  }

  if (!store.hasEvals())
    return out;

  /* A child counts as resolved in a bucket when its own position was reached in
   * that bucket at all; below that there is nothing to read but the edge. */
  const auto resolvedIn = [&](size_t i, size_t b) {
    return out.children[i] && out.children[i]->sums.cnt[b] > 0;
  };

  const auto edgeOf = [](const BucketStat& st) {
    return ranking::edgeScore(static_cast<double>(st.count), static_cast<double>(st.whiteWins),
                             static_cast<double>(st.draws));
  };

  /*
   * This position's chance value: what it is worth when the side to move
   * follows the crowd instead of choosing. It is the fallback prior for a child
   * that no neighbouring bucket has seen, and the evaluator averages it over
   * every row, pseudo-moves included, so this does too.
   */
  std::array<double, NUM_BUCKETS> chanceW{}, chanceB{}, sumW{}, sumB{}, games{};

  for (size_t i = 0; i < n; ++i) {
    for (size_t b = 0; b < NUM_BUCKETS; ++b) {
      const BucketStat& st = out.rows[i].stats[b];

      if (st.empty())
        continue;

      const bool res = resolvedIn(i, b);
      const double edge = edgeOf(st);

      sumW[b] += st.count * (res ? out.children[i]->ote.evalW[b] : edge);
      sumB[b] += st.count * (res ? out.children[i]->ote.evalB[b] : edge);
      games[b] += st.count;
    }
  }

  const std::span<const NodeBlobEntry> entries(node.entries, node.nEntries);

  for (size_t b = 0; b < NUM_BUCKETS; ++b) {
    const double fallback = ranking::emptyBucketPrior(entries, static_cast<int>(b));

    chanceW[b] = games[b] > 0 ? sumW[b] / games[b] : fallback;
    chanceB[b] = games[b] > 0 ? sumB[b] / games[b] : fallback;
  }

  for (size_t i = 0; i < n; ++i) {
    if (!out.playable[i])
      continue;

    for (size_t b = 0; b < NUM_BUCKETS; ++b) {
      const BucketStat& st = out.rows[i].stats[b];

      if (st.empty())
        continue;

      const bool res = resolvedIn(i, b);
      const double edge = edgeOf(st);

      const double value = !res         ? edge
                           : whiteToMove ? out.children[i]->ote.evalW[b]
                                         : out.children[i]->ote.evalB[b];
      double chance = whiteToMove ? chanceW[b] : chanceB[b];
      const double evidence =
          res ? static_cast<double>(out.children[i]->sums.cnt[b]) : static_cast<double>(st.count);

      /* A playable move whose child is not in the store at all has nothing to
       * build a neighbour prior from; empty sums give Prior{0.5, 0}, which
       * carries zero weight, leaving the edge and the chance value. */
      static constexpr ranking::BucketSums NO_SUMS{};
      const ranking::BucketSums& sums = out.children[i] ? out.children[i]->sums : NO_SUMS;

      const ranking::Prior prior = ranking::nbrPrior(sums, b);
      const double sPrior = ranking::S_PRIOR[b];

      const ranking::Candidate candidate{
          out.rows[i].raw, ranking::candidateScore(value, evidence, prior, chance, sPrior),
          ranking::candidateScoreSd(evidence, prior, sPrior), ranking::supportOf(evidence, prior)};

      out.picks[i][b] = {true, candidate, evidence, prior.value};
    }
  }

  rankBuckets(out, whiteToMove);

  return out;
}

uint16_t chosenMove(const Analysis& analysis, size_t bucket) {
  const size_t row = analysis.bestRow(bucket);

  return row == std::string::npos ? 0 : analysis.rows[row].raw;
}
