#include "FloatBits.h"
#include "MappedFile.h"
#include "MoveRanking.h"
#include "common.h"
#include "position.h"
#include "types.h"
#include "uci.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

/*
 * Checks a built .ote against the .otb it was built from.
 *
 * The point is to catch the class of bug that keeps reaching the UI: a node
 * whose recommended move rests on a handful of games. Those are not violations
 * of the algorithm, they are the algorithm faithfully executing on evidence
 * that cannot support a recommendation, so no amount of re-deriving the search
 * would find them. They are reported as WARN and ranked, so the worst ones can
 * be read off the top of the list and pasted straight into fen_stats.
 *
 * Everything else here is a real invariant, reported as FAIL: the file lines up
 * with its .otb, every stored number is a finite score, every recommended move
 * is a move that exists at that node in that bucket, and re-running the backup
 * rule over the stored child values reproduces both the stored move and the
 * stored value. That last one is what catches a stale .ote, a non-converged
 * component, or the evaluator and MoveRanking.h drifting apart.
 */

namespace {

/* Tolerance on re-deriving a stored value. The evals are float, the search
 * stops at EPS = 1e-6, and the recomputation runs in double off the stored
 * (already rounded) child values, so exact equality is not on offer. */
constexpr double VALUE_TOL = 2e-3;

/*
 * Support thresholds for the WARN checks. A recommendation resting on fewer
 * games than this in its own bucket, or on a neighbour prior assembled from
 * fewer weighted games than this, is not wrong so much as unfounded.
 */
constexpr double MIN_MOVE_GAMES = 20;
/*
 * Total pseudo-observations behind a score, below which it is barely anchored
 * to anything: evidence + sEff + K0. The Kish cap means a prior assembled from
 * almost nothing now contributes almost nothing, which is the point of it --
 * but that also means a move with thin evidence AND no neighbour data falls
 * through to (evidence * value + K0 * chance), where a single game still moves
 * the score a long way. This is the check that sees that case.
 */
constexpr double MIN_SUPPORT = 50;
constexpr size_t TOP_OFFENDERS = 15;

/*
 * The support checks only mean something at a node the data actually knows
 * about. Deep in the tree almost every bucket holds a handful of games, so a
 * thin recommendation there is unavoidable and uninteresting -- ungated, these
 * checks fire on over half of all recommendations and say nothing. What is
 * worth reading is the disproportion: a node with tens of thousands of games in
 * a bucket that nonetheless recommends a move nobody played. Both the gated and
 * the whole-tree tallies are printed, the first as the actionable list and the
 * second because "the prior decides most of the tree" is itself a finding.
 */
constexpr double MIN_NODE_GAMES = 1000;

struct NodeRef {
  uint64_t zobrist{};
  uint32_t offsetDiv4{};
  bool visited{false};
};

std::optional<MappedFile> otbFile;
std::optional<MappedFile> oteFile;
const std::byte* otbData = nullptr;
const OteEntry* oteEntries = nullptr;
std::vector<NodeRef> nodes;

struct ZobristCompare {
  bool operator()(const NodeRef& r, uint64_t key) const {
    return r.zobrist < key;
  }
  bool operator()(uint64_t key, const NodeRef& r) const {
    return key < r.zobrist;
  }
};

std::span<const NodeBlobEntry> blobEntriesOf(const NodeRef& node) {
  const uint64_t offset = static_cast<uint64_t>(node.offsetDiv4) * 4ULL;
  const auto& blob = *reinterpret_cast<const NodeBlobHeader*>(otbData + offset);

  return {blob.entries, blob.nEntries};
}

/* Same grouping the evaluator uses: k indexes distinct real moves in blob
 * order, and is -1 for the termination and aggregation pseudo-moves. */
template <typename F> void forEachEntry(const NodeRef& n, F&& f) {
  int k = -1;
  uint16_t prev = 0;

  for (const auto& e : blobEntriesOf(n)) {
    bool real = e.move != Stockfish::Move::termination().raw() &&
                e.move != Stockfish::Move::forcedAggregation().raw();
    if (real && e.move != prev) {
      ++k;
      prev = e.move;
    }
    f(e, real ? k : -1);
  }
}

double ownScore(const NodeRef& node, int bkt) {
  double num = 0, den = 0;

  for (const auto& e : blobEntriesOf(node)) {
    if (e.bucket == bkt) {
      num += e.count * ranking::edgeScore(e);
      den += e.count;
    }
  }

  return den > 0 ? num / den : ranking::emptyBucketPrior(blobEntriesOf(node), bkt);
}

/* The weight the prior is actually granted after the Kish cap, which is the
 * quantity that decides whether it can overpower the candidate's own games. */
double effectivePriorWeight(const ranking::Prior& prior, int bkt) {
  return ranking::S_PRIOR[bkt] * prior.kish / (ranking::S_PRIOR[bkt] + prior.kish);
}

struct Counts {
  uint64_t indexNodes{};
  uint64_t visited{};
  uint64_t checked{};   // node/bucket pairs with a recommendation
  uint64_t evalNotFinite{};
  uint64_t evalOutOfRange{};
  uint64_t moveNotInBlob{};
  uint64_t moveIsPseudo{};
  uint64_t moveMissing{};   // bucket had candidates but no move stored
  uint64_t moveSpurious{};  // bucket had no candidates but a move was stored
  uint64_t moveMismatch{};
  uint64_t valueMismatch{};
  uint64_t chanceMismatch{};
  uint64_t thinMove{};
  uint64_t thinSupport{};
  uint64_t outsideChildRange{};
  uint64_t thinMoveAll{};
  uint64_t thinSupportAll{};
  uint64_t outsideChildRangeAll{};
  uint64_t wellPlayed{};
  double worstValueDelta{};
  double worstChanceDelta{};
};

struct Offender {
  double severity{};
  std::string fen;
  std::string detail;
};

std::vector<Offender> offenders;
Counts counts;

void note(double severity, const std::string& fen, const std::string& detail) {
  offenders.push_back({severity, fen, detail});

  /* Kept bounded rather than sorted every time: 25M nodes can produce a lot of
   * these, and only the head of the list is ever printed. */
  if (offenders.size() >= 4 * TOP_OFFENDERS) {
    std::sort(offenders.begin(), offenders.end(),
              [](const Offender& a, const Offender& b) { return a.severity > b.severity; });
    offenders.resize(TOP_OFFENDERS);
  }
}

std::string bucketName(int b) {
  static const char* names[] = {"<1000",     "1000-1199", "1200-1399", "1400-1599", "1600-1799",
                                "1800-1999", "2000-2199", "2200-2399", "2400+"};
  return names[b];
}

std::vector<NodeRef>::iterator lookup(uint64_t zobrist) {
  const auto [it, last] = std::equal_range(nodes.begin(), nodes.end(), zobrist, ZobristCompare{});

  return it == last ? nodes.end() : it;
}

void checkNode(std::vector<NodeRef>::iterator it, Stockfish::Position& pos,
               const std::vector<std::pair<uint16_t, std::vector<NodeRef>::iterator>>& children) {
  const NodeRef& node = *it;
  const OteEntry& ote = oteEntries[it - nodes.begin()];
  const bool whiteToMove = pos.side_to_move() == Stockfish::Color::WHITE;

  std::vector<ranking::BucketSums> stats(children.size());
  for (size_t k = 0; k < children.size(); ++k)
    stats[k] = ranking::bucketSumsOf(blobEntriesOf(*children[k].second));

  auto resolvedIn = [&](int k, int bkt) {
    return k >= 0 && stats[k].cnt[bkt] > 0;
  };

  /* --- the chance pass, re-derived exactly as the evaluator does it --- */
  double sumW[Stockfish::NUMBER_OF_BUCKETS]{};
  double sumB[Stockfish::NUMBER_OF_BUCKETS]{};
  double games[Stockfish::NUMBER_OF_BUCKETS]{};

  forEachEntry(node, [&](const NodeBlobEntry& e, int k) {
    double w{}, b{};
    if (resolvedIn(k, e.bucket)) {
      const OteEntry& childOte = oteEntries[children[k].second - nodes.begin()];
      w = childOte.evalW[e.bucket];
      b = childOte.evalB[e.bucket];
    } else {
      w = b = ranking::edgeScore(e);
    }

    sumW[e.bucket] += e.count * w;
    sumB[e.bucket] += e.count * b;
    games[e.bucket] += e.count;
  });

  double chanceW[Stockfish::NUMBER_OF_BUCKETS]{};
  double chanceB[Stockfish::NUMBER_OF_BUCKETS]{};
  for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
    if (games[bkt] > 0) {
      chanceW[bkt] = sumW[bkt] / games[bkt];
      chanceB[bkt] = sumB[bkt] / games[bkt];
    } else {
      chanceW[bkt] = chanceB[bkt] = ranking::emptyBucketPrior(blobEntriesOf(node), bkt);
    }
  }

  /* --- the selection pass, through the evaluator's own pick rule --- */
  ranking::Ranker rankers[Stockfish::NUMBER_OF_BUCKETS];
  for (auto& ranker : rankers)
    ranker.sgn = ranking::sideSign(whiteToMove);

  /* What a candidate's score was assembled from. The warnings below are about
   * the *recommended* move, so this travels alongside the arg max the rule
   * keeps rather than alongside every candidate. */
  struct Support {
    double evidence{};
    double sEff{};
    double kish{};
  };

  Support bestSupport[Stockfish::NUMBER_OF_BUCKETS]{};

  double childLo[Stockfish::NUMBER_OF_BUCKETS];
  double childHi[Stockfish::NUMBER_OF_BUCKETS];
  std::fill(std::begin(childLo), std::end(childLo), 1e30);
  std::fill(std::begin(childHi), std::end(childHi), -1e30);
  bool moveInBucket[Stockfish::NUMBER_OF_BUCKETS]{};

  forEachEntry(node, [&](const NodeBlobEntry& e, int k) {
    if (k < 0)
      return;

    const int bkt = e.bucket;
    moveInBucket[bkt] = true;

    bool res = resolvedIn(k, bkt);
    const OteEntry& childOte = oteEntries[children[k].second - nodes.begin()];
    double value = !res         ? ranking::edgeScore(e)
                   : whiteToMove ? childOte.evalW[bkt]
                                 : childOte.evalB[bkt];
    double evidence = res ? stats[k].cnt[bkt] : e.count;
    double chance = whiteToMove ? chanceW[bkt] : chanceB[bkt];

    childLo[bkt] = std::min(childLo[bkt], value);
    childHi[bkt] = std::max(childHi[bkt], value);

    const ranking::Prior prior = ranking::nbrPrior(stats[k], bkt);
    const double sEff = effectivePriorWeight(prior, bkt);
    const ranking::Candidate cand{
        e.move, ranking::candidateScore(value, evidence, prior, chance, ranking::S_PRIOR[bkt]),
        ranking::candidateScoreSd(evidence, prior, ranking::S_PRIOR[bkt]),
        ranking::supportOf(evidence, prior)};

    /* Reading the arg max back rather than re-testing it here is what keeps this
     * from becoming a third copy of the comparison. A move appears once per
     * bucket, so a changed move means this candidate is the new leader. */
    const uint16_t had = rankers[bkt].best.move;

    rankers[bkt].add(cand);

    if (rankers[bkt].best.move != had)
      bestSupport[bkt] = {evidence, sEff, prior.kish};
  });

  double bestScore[Stockfish::NUMBER_OF_BUCKETS]{};
  uint16_t bestMove[Stockfish::NUMBER_OF_BUCKETS]{};
  double bestEvidence[Stockfish::NUMBER_OF_BUCKETS]{};
  double bestSEff[Stockfish::NUMBER_OF_BUCKETS]{};
  double bestKish[Stockfish::NUMBER_OF_BUCKETS]{};

  for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
    const ranking::Candidate picked = rankers[bkt].pick();

    if (!picked.present())
      continue;

    const Support& support = bestSupport[bkt];

    bestMove[bkt] = picked.move;
    bestScore[bkt] = picked.score;
    bestEvidence[bkt] = support.evidence;
    bestSEff[bkt] = support.sEff;
    bestKish[bkt] = support.kish;
  }

  /* --- compare against what the file says --- */
  for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
    const double storedW = ote.evalW[bkt];
    const double storedB = ote.evalB[bkt];
    const uint16_t storedMove = ote.moveMax[bkt];

    if (!floatbits::isFinite(storedW) || !floatbits::isFinite(storedB)) {
      ++counts.evalNotFinite;
      note(1e9, pos.fen(), "bucket " + bucketName(bkt) + ": stored eval is not finite");
      continue;
    }
    if (storedW < -VALUE_TOL || storedW > 1 + VALUE_TOL || storedB < -VALUE_TOL ||
        storedB > 1 + VALUE_TOL) {
      ++counts.evalOutOfRange;
      note(1e9, pos.fen(), "bucket " + bucketName(bkt) + ": stored eval outside [0, 1]");
    }

    if (storedMove == Stockfish::Move::termination().raw() ||
        storedMove == Stockfish::Move::forcedAggregation().raw()) {
      ++counts.moveIsPseudo;
      note(1e9, pos.fen(), "bucket " + bucketName(bkt) + ": recommended move is a pseudo-move");
      continue;
    }

    if (storedMove == 0 && moveInBucket[bkt]) {
      ++counts.moveMissing;
      note(1e8, pos.fen(), "bucket " + bucketName(bkt) + ": no move stored but candidates exist");
      continue;
    }
    if (storedMove != 0 && !moveInBucket[bkt]) {
      ++counts.moveSpurious;
      note(1e8, pos.fen(), "bucket " + bucketName(bkt) + ": move stored but no candidate exists");
      continue;
    }
    if (storedMove == 0)
      continue;

    bool present = false;
    forEachEntry(node, [&](const NodeBlobEntry& e, int k) {
      if (k >= 0 && e.move == storedMove && e.bucket == bkt)
        present = true;
    });
    if (!present) {
      ++counts.moveNotInBlob;
      note(1e9, pos.fen(),
           "bucket " + bucketName(bkt) + ": recommended move absent from this node's blob");
      continue;
    }

    ++counts.checked;

    if (storedMove != bestMove[bkt]) {
      ++counts.moveMismatch;
      note(1e7, pos.fen(),
           "bucket " + bucketName(bkt) + ": stored move " +
               Stockfish::UCIEngine::move(Stockfish::Move(storedMove), false) + " but rule picks " +
               Stockfish::UCIEngine::move(Stockfish::Move(bestMove[bkt]), false));
    }

    const double expectChooser = bestMove[bkt] != 0 ? bestScore[bkt] : ownScore(node, bkt);
    const double storedChooser = whiteToMove ? storedW : storedB;
    const double storedChance = whiteToMove ? storedB : storedW;
    const double expectChance = whiteToMove ? chanceB[bkt] : chanceW[bkt];

    const double dv = std::abs(storedChooser - expectChooser);
    const double dc = std::abs(storedChance - expectChance);
    counts.worstValueDelta = std::max(counts.worstValueDelta, dv);
    counts.worstChanceDelta = std::max(counts.worstChanceDelta, dc);

    if (dv > VALUE_TOL) {
      ++counts.valueMismatch;
      note(1e6 + dv, pos.fen(),
           "bucket " + bucketName(bkt) + ": stored value " + std::to_string(storedChooser) +
               " but rule gives " + std::to_string(expectChooser));
    }
    if (dc > VALUE_TOL) {
      ++counts.chanceMismatch;
      note(1e6 + dc, pos.fen(),
           "bucket " + bucketName(bkt) + ": stored crowd value " + std::to_string(storedChance) +
               " but rule gives " + std::to_string(expectChance));
    }

    /* --- support warnings: the recommendation is derivable but unfounded --- */
    const bool thinMove = bestEvidence[bkt] < MIN_MOVE_GAMES;
    const double support = bestEvidence[bkt] + bestSEff[bkt] + ranking::K0;
    const bool thinSupport = support < MIN_SUPPORT;
    const bool outside =
        childHi[bkt] > -1e29 &&
        (storedChooser > childHi[bkt] + VALUE_TOL || storedChooser < childLo[bkt] - VALUE_TOL);

    counts.thinMoveAll += thinMove;
    counts.thinSupportAll += thinSupport;
    counts.outsideChildRangeAll += outside;

    if (games[bkt] < MIN_NODE_GAMES)
      continue;

    ++counts.wellPlayed;

    /* How far out of proportion the recommendation is to what the node knows:
     * a well travelled position that recommends a move nobody played. */
    const double disproportion = games[bkt] / (bestEvidence[bkt] + 1);
    const std::string where = "bucket " + bucketName(bkt) + " (" +
                              std::to_string((uint64_t)games[bkt]) + " games at this node)";
    const std::string moveName =
        Stockfish::UCIEngine::move(Stockfish::Move(storedMove), false);

    if (thinMove) {
      ++counts.thinMove;
      note(disproportion, pos.fen(),
           where + ": recommends " + moveName + " on " +
               std::to_string((uint64_t)bestEvidence[bkt]) + " games");
    }

    if (thinSupport) {
      ++counts.thinSupport;
      note(disproportion, pos.fen(),
           where + ": " + moveName + " scored on " + std::to_string(support) +
               " total observations (" + std::to_string((uint64_t)bestEvidence[bkt]) +
               " games, prior kish " + std::to_string(bestKish[bkt]) + " capped to sEff " +
               std::to_string(bestSEff[bkt]) + " against S_PRIOR " +
               std::to_string((uint64_t)ranking::S_PRIOR[bkt]) + ")");
    }

    /* The chooser picks among child values, so landing outside their range can
     * only come from a prior, and a prior that pulls the node past every real
     * option is one that has overpowered the data. */
    if (outside) {
      ++counts.outsideChildRange;
      double excess = std::max(storedChooser - childHi[bkt], childLo[bkt] - storedChooser);
      note(disproportion * (1 + excess), pos.fen(),
           where + ": value " + std::to_string(storedChooser) +
               " lies outside every child value [" + std::to_string(childLo[bkt]) + ", " +
               std::to_string(childHi[bkt]) + "]");
    }
  }
}

void walk(Stockfish::Position& pos) {
  auto it = lookup(pos.key());
  if (it == nodes.end() || it->visited)
    return;

  it->visited = true;
  ++counts.visited;

  if (counts.visited % 2'000'000 == 0)
    std::cerr << "  visited " << counts.visited << " / " << counts.indexNodes << " nodes\n";

  std::vector<std::pair<uint16_t, std::vector<NodeRef>::iterator>> children;
  Stockfish::StateInfo state;
  Stockfish::Move prevMove = Stockfish::Move::none();

  for (const auto& blob : blobEntriesOf(*it)) {
    Stockfish::Move move = Stockfish::Move(blob.move);
    if (move == prevMove || move == Stockfish::Move::termination() ||
        move == Stockfish::Move::forcedAggregation())
      continue;

    prevMove = move;

    pos.do_move(move, state);
    auto child = lookup(pos.key());
    if (child != nodes.end())
      walk(pos);
    pos.undo_move(move);

    if (child == nodes.end()) {
      std::cerr << "FAIL: child of " << pos.fen() << " after "
                << Stockfish::UCIEngine::move(move, false) << " is missing from the index\n";
      continue;
    }

    children.emplace_back(move.raw(), child);
  }

  checkNode(it, pos, children);
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: verify_ote <data.otb> <evals.ote>\n";
    return 1;
  }

  otbFile.emplace(argv[1], MmapAdvice::RANDOM);
  oteFile.emplace(argv[2], MmapAdvice::RANDOM);
  otbData = otbFile->data();

  OtbHeader header{};
  if (otbFile->size() < sizeof(header)) {
    std::cerr << "FAIL: otb is too small to hold a header\n";
    return 2;
  }
  std::memcpy(&header, otbData, sizeof(header));

  bool fileFail = false;
  if (header.magic != OtbHeader::MAGIC) {
    std::cerr << "FAIL: otb magic mismatch\n";
    fileFail = true;
  }

  OteHeader oteHeader{};
  if (oteFile->size() < sizeof(oteHeader)) {
    std::cerr << "FAIL: ote is too small to hold a header\n";
    return 2;
  }
  std::memcpy(&oteHeader, oteFile->data(), sizeof(oteHeader));

  const uint64_t expectedSize = sizeof(OteHeader) + header.nodes * sizeof(OteEntry);
  if (oteFile->size() != expectedSize) {
    std::cerr << "FAIL: ote is " << oteFile->size() << " bytes, expected " << expectedSize
              << " for " << header.nodes << " nodes\n";
    fileFail = true;
  }
  if (fileFail)
    return 2;

  oteEntries = reinterpret_cast<const OteEntry*>(oteFile->data() + sizeof(OteHeader));

  counts.indexNodes = header.nodes;
  nodes.resize(header.nodes);
  for (uint64_t i = 0; i < header.nodes; ++i) {
    IndexEntry entry{};
    std::memcpy(&entry, otbData + header.indexOffset + i * sizeof(IndexEntry), sizeof(entry));
    nodes[i].zobrist = entry.zobrist;
    nodes[i].offsetDiv4 = entry.offsetDiv4;
  }

  std::cout << "verifying " << argv[2] << " against " << argv[1] << " (" << header.nodes
            << " nodes)\n";

  /* The scoring constants are compiled in, so a verifier built against a
   * different MoveRanking.h than the .ote was is checking the wrong rule and would
   * report the difference as a mismatch. Printing them makes that visible
   * rather than mysterious. */
  std::cout << "rule: K0 = " << ranking::K0 << "\n      S_PRIOR         =";
  for (int b = 0; b < Stockfish::NUMBER_OF_BUCKETS; ++b)
    std::cout << " " << ranking::S_PRIOR[b];
  std::cout << "\n      NEIGHBOUR_WEIGHT=";
  for (int b = 0; b < Stockfish::NUMBER_OF_BUCKETS; ++b)
    std::cout << " " << ranking::NEIGHBOUR_WEIGHT[b];
  std::cout << "\n";

  Stockfish::Bitboards::init();
  Stockfish::Position::init();

  Stockfish::Position pos;
  Stockfish::StateInfo st;
  pos.set(Stockfish::StartFEN, false, &st);

  walk(pos);

  std::sort(offenders.begin(), offenders.end(),
            [](const Offender& a, const Offender& b) { return a.severity > b.severity; });
  if (offenders.size() > TOP_OFFENDERS)
    offenders.resize(TOP_OFFENDERS);

  const uint64_t fails = counts.evalNotFinite + counts.evalOutOfRange + counts.moveNotInBlob +
                         counts.moveIsPseudo + counts.moveMissing + counts.moveSpurious +
                         counts.moveMismatch + counts.valueMismatch + counts.chanceMismatch +
                         (counts.indexNodes - counts.visited);

  std::cout << "\n===== structure =====\n";
  std::cout << "index nodes:              " << counts.indexNodes << "\n";
  std::cout << "reached by the walk:      " << counts.visited << "\n";
  std::cout << "unreachable:              " << counts.indexNodes - counts.visited << "\n";
  std::cout << "node/bucket recs checked: " << counts.checked << "\n";

  std::cout << "\n===== invariants (FAIL) =====\n";
  std::cout << "eval not finite:          " << counts.evalNotFinite << "\n";
  std::cout << "eval outside [0, 1]:      " << counts.evalOutOfRange << "\n";
  std::cout << "move absent from blob:    " << counts.moveNotInBlob << "\n";
  std::cout << "move is TERM or AGG:      " << counts.moveIsPseudo << "\n";
  std::cout << "no move but candidates:   " << counts.moveMissing << "\n";
  std::cout << "move but no candidates:   " << counts.moveSpurious << "\n";
  std::cout << "move disagrees with rule: " << counts.moveMismatch << "\n";
  std::cout << "value disagrees:          " << counts.valueMismatch << "  (worst delta "
            << std::setprecision(6) << counts.worstValueDelta << ")\n";
  std::cout << "crowd value disagrees:    " << counts.chanceMismatch << "  (worst delta "
            << counts.worstChanceDelta << ")\n";

  auto pct = [](uint64_t n, uint64_t of) {
    return of > 0 ? 100.0 * double(n) / double(of) : 0.0;
  };

  std::cout << "\n===== support, whole tree =====\n";
  std::cout << std::setprecision(3);
  std::cout << "rec on < " << MIN_MOVE_GAMES << " games:        " << counts.thinMoveAll << "  ("
            << pct(counts.thinMoveAll, counts.checked) << "%)\n";
  std::cout << "support under " << MIN_SUPPORT << ":       " << counts.thinSupportAll << "  ("
            << pct(counts.thinSupportAll, counts.checked) << "%)\n";
  std::cout << "value outside child range:" << counts.outsideChildRangeAll << "  ("
            << pct(counts.outsideChildRangeAll, counts.checked) << "%)\n";

  std::cout << "\n===== support, nodes with >= " << MIN_NODE_GAMES << " games (WARN) =====\n";
  std::cout << "recommendations in scope:  " << counts.wellPlayed << "\n";
  std::cout << "rec on < " << MIN_MOVE_GAMES << " games:        " << counts.thinMove << "  ("
            << pct(counts.thinMove, counts.wellPlayed) << "%)\n";
  std::cout << "support under " << MIN_SUPPORT << ":       " << counts.thinSupport << "  ("
            << pct(counts.thinSupport, counts.wellPlayed) << "%)\n";
  std::cout << "value outside child range:" << counts.outsideChildRange << "  ("
            << pct(counts.outsideChildRange, counts.wellPlayed) << "%)\n";

  if (!offenders.empty()) {
    std::cout << "\n===== worst offenders =====\n";
    for (const auto& o : offenders)
      std::cout << "  " << o.detail << "\n    " << o.fen << "\n";
  }

  const uint64_t warns = counts.thinMove + counts.thinSupport + counts.outsideChildRange;
  std::cout << "\n"
            << (fails == 0 ? "PASS" : "FAIL") << ": " << fails << " invariant violations, " << warns
            << " support warnings" << std::endl;

  return fails == 0 ? 0 : 1;
}
