#pragma once

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

/*
 * How a position's candidate moves are scored and ordered. Shared by the
 * evaluator that writes an .ote and everything that reads one back, so the two
 * cannot drift apart: a move list explained by a different rule than the file
 * was built with is wrong in a way nothing else would report.
 *
 * A child's raw evaluation is not the number the search maximises. A move
 * played twenty times can carry a spectacular eval that is mostly noise, so
 * every candidate is shrunk toward a prior in proportion to how much evidence
 * stands behind it, and ranked on what its own uncertainty still allows. The
 * consequence worth remembering when reading a move list: the best-scoring
 * child is frequently *not* the chosen move.
 */
namespace ranking {

/*
 * Pseudo-observations behind every candidate's prior. A move keeps its own
 * value only once its evidence is large next to this; below that it is read as
 * the prior plus a nudge.
 */
inline constexpr double K0 = 30.0;

/*
 * The pick rule's one knob. Every candidate is judged not by its score but by
 * the worst its own noise allows — score minus Z_LCB standard deviations of
 * that score, on the side to move's own axis — and the best of those bounds
 * wins. A well supported move barely moves under this, since its sd is tiny; a
 * move with four games behind it is pushed a long way down, which is the point.
 */
inline constexpr double Z_LCB = 2.0;

/*
 * Support below which a score rests on too little to mean much. The ranking
 * expresses that continuously through the bound above and does not consult
 * this; it is what the UI greys out.
 */
inline constexpr double SOLID_SUPPORT = 150.0;

/*
 * The per-bucket prior budget and the weight given to neighbouring rating
 * bands. Both are measured off the data by apps/calibration and then scaled by
 * what scored best in the evaluation harness, with the scalings folded in here
 * rather than left as separate multipliers.
 *
 * The top bucket is the exception, and gets a tenth of its measured budget
 * rather than a half. Pooling across bands is actively harmful there rather
 * than merely over-trusted: 2400+ has no neighbours above it, so every band it
 * borrows from is weaker players, and where a 2400 field genuinely diverges
 * from a 2200 field — the case the whole bucketing exists to capture — the
 * prior drags the estimate back toward what weaker players do. Cutting it is
 * not paid for anywhere else.
 *
 * That reduces the problem without fixing it: no shrinkage at all still scores
 * better in that one bucket than any setting of these constants, which says the
 * remaining fault is structural — the top bucket wants a prior that does not
 * borrow downward. These are the best setting found by a sweep scored on months
 * the tree was not built from.
 */
inline constexpr double S_PRIOR[Stockfish::NUMBER_OF_BUCKETS] = {179.5, 635,   755.5, 733, 642.5,
                                                                 530.5, 392.5, 292,   37};
inline constexpr double NEIGHBOUR_WEIGHT[Stockfish::NUMBER_OF_BUCKETS] = {
    0.45, 0.45, 0.6, 0.75, 0.9, 0.75, 0.6, 0.225, 0.225};

/* White's points per game across the edge, in [0, 1]. Taken as doubles so a
 * caller holding wider accumulated tallies does not have to narrow them. */
inline double edgeScore(double count, double whiteWins, double draws) {
  return (whiteWins + 0.5 * draws) / count;
}

inline double edgeScore(const NodeBlobEntry& e) {
  return edgeScore(e.count, e.white_wins, e.draws);
}

/*
 * One pass over a node's blob, keeping both halves of a per-bucket average:
 * the games seen and the points White took in them.
 */
struct BucketSums {
  uint32_t cnt[Stockfish::NUMBER_OF_BUCKETS]{};
  double sw[Stockfish::NUMBER_OF_BUCKETS]{};
};

inline BucketSums bucketSumsOf(std::span<const NodeBlobEntry> entries) {
  BucketSums s{};

  for (const auto& e : entries) {
    s.cnt[e.bucket] += e.count;
    s.sw[e.bucket] += e.count * edgeScore(e);
  }

  return s;
}

/*
 * What the *other* rating buckets say about this position, distance weighted.
 *
 * This is what a candidate is shrunk toward, and it has to be about the move
 * rather than about the position: the crowd average of the parent says only
 * "what happens here on average", so a thin move would shrink toward a number
 * that has nothing to do with it and one freak game would be enough to drag it
 * over the best supported alternative. The move's own position scored at
 * neighbouring ratings is a real estimate of that move.
 *
 * `kish` is zero when no other bucket has seen the position at all, which is
 * the caller's signal to fall back to the node's chance value. A zero-weight
 * prior rather than a NaN sentinel on purpose: this project builds with
 * -ffast-math, under which a NaN is not merely missed by std::isnan but
 * silently wins the comparisons downstream.
 */
struct Prior {
  double value;
  double kish;
};

inline Prior nbrPrior(const BucketSums& stats, int bkt) {
  double num{}, den{}, den2{};

  for (int a = 0; a < Stockfish::NUMBER_OF_BUCKETS; ++a) {
    if (a == bkt)
      continue;

    double wt = std::pow(NEIGHBOUR_WEIGHT[bkt], std::abs(a - bkt));
    num += wt * stats.sw[a];
    den += wt * stats.cnt[a];
    den2 += wt * wt * stats.cnt[a];
  }

  if (den <= 0)
    return Prior{0.5, 0};

  return Prior{num / den, den * den / den2};
}

/* The same distance weighting applied to a node's own edges, for a bucket in
 * which the node itself was never played. */
inline double emptyBucketPrior(std::span<const NodeBlobEntry> entries, int bkt) {
  double num{}, den{};

  for (const auto& e : entries) {
    double wt = std::pow(NEIGHBOUR_WEIGHT[bkt], std::abs(static_cast<int>(e.bucket) - bkt));
    num += wt * e.count * edgeScore(e);
    den += wt * e.count;
  }

  return num / den;
}

/*
 * How many pseudo-observations the neighbour prior is actually worth here. Its
 * own Kish size is capped by sPrior, so a prior assembled from a huge
 * neighbouring bucket still cannot outweigh the bucket-level budget.
 */
inline double priorWeight(Prior prior, double sPrior) {
  return sPrior * prior.kish / (sPrior + prior.kish);
}

/*
 * A candidate's ranking score: its own value pulled toward `prior`, by an
 * amount set by how thin `evidence` is next to `sPrior`. Still a white score,
 * so White maximises it and Black minimises it.
 *
 * `sPrior` is a parameter rather than the constant because a stored .ote is a
 * record of the run that produced it: reading one back with a different S_PRIOR
 * than it was built with silently re-ranks its moves, so anything explaining an
 * existing file has to be told which value that file used.
 */
inline double candidateScore(double value, double evidence, Prior prior, double chance,
                             double sPrior) {
  double sEff = priorWeight(prior, sPrior);
  return (evidence * value + sEff * prior.value + K0 * chance) / (evidence + sEff + K0);
}

/*
 * The standard deviation of the score above, treating it as a weighted mean of
 * per-game outcomes whose spread is at most 0.5 (the sd of a {0, 0.5, 1} result
 * is never larger than a coin flip's). The node's own games contribute
 * `evidence` of variance-carrying weight, and the prior contributes sEff of
 * weight spread over prior.kish independent games, hence sEff^2 / kish.
 *
 * This is what makes a thin move's score comparable to a well supported one:
 * both are shrunk numbers, but only one of them can move far on one more game.
 */
inline double candidateScoreSd(double evidence, Prior prior, double sPrior) {
  double sEff = priorWeight(prior, sPrior);
  double priorVar = prior.kish > 0 ? sEff * sEff / prior.kish : 0.0;

  return 0.5 * std::sqrt(evidence + priorVar) / (evidence + sEff + K0);
}

/* Games standing behind a candidate: its own in this bucket, plus the effective
 * games its neighbour prior is built from. */
inline double supportOf(double evidence, Prior prior) {
  return evidence + prior.kish;
}

/*
 * Scores are white sided throughout, so White maximises them and Black
 * minimises them. Folding the direction into a sign lets every comparison in
 * the pick rule be written once for both sides.
 */
inline constexpr double sideSign(bool whiteToMove) {
  return whiteToMove ? 1.0 : -1.0;
}

/*
 * One move as the ranking sees it. A default-built Candidate is the "nothing
 * here" value — move 0 is Move::none(), which no real candidate carries.
 */
struct Candidate {
  uint16_t move{};
  double score{};
  double sd{};
  double support{};

  bool present() const {
    return move != 0;
  }

  /* Enough behind the score for it to mean much. The ranking does not consult
   * this; it is what the UI greys out. */
  bool solid() const {
    return support > SOLID_SUPPORT;
  }

  /*
   * The number the ranking compares: the score pulled Z_LCB standard deviations
   * toward the side to move's own disadvantage. `sgn` is sideSign() of the side
   * to move, so White's bound sits below its score and Black's above it, both
   * pessimistic for whoever is choosing.
   *
   * Still a white score, so the caller maximises `sgn * bound(sgn)` exactly as
   * it would have maximised `sgn * score`.
   */
  double bound(double sgn) const {
    return score - sgn * Z_LCB * sd;
  }
};

/*
 * The pick, and the single definition of it.
 *
 * A plain arg max hands the bucket to whichever move happens to score highest,
 * and deep in the tree that is regularly a move with a handful of games whose
 * score sits high only because the shrink could not pull it down far enough.
 * Ranking on the bound instead judges every move by the worst its own noise
 * allows, so a thin move has to be good enough to survive being discounted.
 *
 * Only the pick changes: the value backed up is the picked candidate's score,
 * exactly as an arg max would have backed up its own winner's. The bound
 * decides the order and is not itself an estimate of anything.
 *
 * Candidates are fed in one at a time and nothing is stored beyond the running
 * arg max, so the evaluator can rank a node's moves inside the pass it already
 * makes over the blob, without allocating.
 */
struct Ranker {
  /* sideSign() of the side to move. */
  double sgn{1.0};

  Candidate best{};

  void add(const Candidate& cand) {
    if (!cand.present())
      return;

    /* Strictly better, so an exact tie keeps the candidate added first. */
    if (!best.present() || sgn * cand.bound(sgn) > sgn * best.bound(sgn))
      best = cand;
  }

  /* Not present() when no candidate was ever added. */
  Candidate pick() const {
    return best;
  }
};

/*
 * The order a move list is shown in: best bound first. The bound depends only
 * on the candidate, so this sort and the repeated pick above agree.
 *
 * The scores are still worth showing next to the moves and still will not
 * always agree with the order: a thin move can out-score the move above it and
 * sit lower because its own noise is discounted. solid() marks those.
 *
 * Returns indices into `cands`, ranked; candidates that are not present() are
 * left out entirely. Stable, so an exact tie keeps the earlier candidate first,
 * which is what makes a run over the same data reproducible.
 */
inline std::vector<size_t> rankAll(std::span<const Candidate> cands, bool whiteToMove) {
  const double sgn = sideSign(whiteToMove);

  std::vector<size_t> order;
  order.reserve(cands.size());

  for (size_t i = 0; i < cands.size(); ++i)
    if (cands[i].present())
      order.push_back(i);

  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return sgn * cands[a].bound(sgn) > sgn * cands[b].bound(sgn);
  });

  return order;
}

} // namespace ranking
