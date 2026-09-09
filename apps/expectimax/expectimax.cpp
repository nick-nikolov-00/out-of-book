#include "MappedFile.h"
#include "MoveRanking.h"
#include "common.h"
#include "metrics.h"
#include "position.h"
#include "types.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stack>
#include <stdfloat>
#include <vector>

struct NODES_VISITED : metrics::Counter<uint64_t> {};
struct BACKUPS : metrics::Counter<uint64_t> {};
struct CYCLIC_SCCS : metrics::Counter<uint64_t> {};
struct LARGEST_SCC : metrics::Counter<uint64_t> {};
struct SWEEPS_TOTAL : metrics::Counter<uint64_t> {};
struct SLOWEST_SCC_SWEEPS : metrics::Counter<uint64_t> {};
struct FROZEN_SCCS : metrics::Counter<uint64_t> {};

using AppMetrics = metrics::Metrics<NODES_VISITED, BACKUPS, CYCLIC_SCCS, LARGEST_SCC, SWEEPS_TOTAL,
                                    SLOWEST_SCC_SWEEPS, FROZEN_SCCS>;

/*
 * LARGEST_SCC and SLOWEST_SCC_SWEEPS are high-water marks rather than running
 * totals, so they are stored rather than added to. The traversal is single
 * threaded, which is what makes a plain load/store enough here.
 */
template <typename Counter> void recordMax(AppMetrics& metrics, uint64_t value) {
  auto& cell = metrics.get<Counter>();

  if (value > cell.load(std::memory_order::relaxed))
    cell.store(value, std::memory_order::relaxed);
}

struct NodeData {
  NodeData() : low{std::numeric_limits<uint32_t>::max()} {}

  uint64_t zobrist{};
  uint32_t offsetDiv4{};

  // tarjan stuff (maybe we can skip one of these and have a bit in timein or low to represent
  // rooted ones?
  uint32_t timeIn{};
  uint32_t low;
  bool onStack{false};

  uint16_t moveMax[Stockfish::NUMBER_OF_BUCKETS]{};

  float evalW[Stockfish::NUMBER_OF_BUCKETS]{};
  float evalB[Stockfish::NUMBER_OF_BUCKETS]{};
};

struct ZobristCompare {
  bool operator()(const NodeData& r, uint64_t key) const {
    return r.zobrist < key;
  }

  bool operator()(uint64_t key, const NodeData& r) const {
    return key < r.zobrist;
  }
};

namespace {
/*
 * The OTB is mapped rather than read into a buffer: the data section is
 * several GB and grows with the tree, so copying it would put the whole file
 * in the process's resident set alongside the node array. Mapping lets the
 * kernel page it in and evict it as the walk moves around.
 */
std::optional<MappedFile> otbFile;
const std::byte* otbData = nullptr;
std::vector<NodeData> nodes;

struct ChildEl {
  uint16_t move;
  decltype(nodes)::iterator node;
};
struct StackElement {
  decltype(nodes)::iterator node;
  std::vector<ChildEl> children;
  bool whiteToMove;
};

constexpr size_t MAX_SWEEPS = 10'000;

/* If an SCC hasn't converged in that many sweeps, freeze its moves.
 * This is needed for discontinuous eval functions. We still expect the eval
 * to converge after MAX_SWEEPS once we freeze the picks.
 */
constexpr size_t FREEZE_PICKS_AFTER = 1'000;
constexpr double EPS = 1e-6;
// Tarjan keeps every visited node on this stack until its SCC is closed.
std::deque<StackElement> stack;
} // namespace

int loadData(const char* otbFilename) {
  try {
    otbFile.emplace(otbFilename, MmapAdvice::NORMAL);
  } catch (const std::exception& e) {
    std::cerr << "cannot map " << otbFilename << ": " << e.what() << '\n';
    return 1;
  }

  otbData = otbFile->data();

  if (otbFile->size() < sizeof(OtbHeader)) {
    std::cerr << "file is too small to hold an OTB header\n";
    return 1;
  }

  OtbHeader header{};
  std::memcpy(&header, otbData, sizeof(header));

  if (header.magic != OtbHeader::MAGIC) {
    std::cerr << "Invalid magic\n";
    return 1;
  }
  if (header.version != OtbHeader::VERSION) {
    std::cerr << "Invalid version\n";
    return 1;
  }

  if (header.indexOffset > otbFile->size() ||
      (otbFile->size() - header.indexOffset) / sizeof(IndexEntry) != header.nodes) {
    std::cerr << "OTB index does not match the node count in the header\n";
    return 1;
  }

  std::cout << "nodes: " << header.nodes << std::endl;
  std::cout << "loading index\n";

  nodes.resize(header.nodes);

  for (uint64_t i = 0; i < header.nodes; ++i) {
    IndexEntry entry{};
    std::memcpy(&entry, otbData + header.indexOffset + i * sizeof(IndexEntry), sizeof(entry));

    if (i != 0 && entry.zobrist <= nodes[i - 1].zobrist) {
      std::cerr << "OTB index is not sorted by Zobrist key at entry " << i << '\n';
      return 1;
    }

    /* Offsets stay absolute: the whole file is mapped, header included. */
    nodes[i].zobrist = entry.zobrist;
    nodes[i].offsetDiv4 = entry.offsetDiv4;
  }

  std::cout << "loaded index\n";

  return 0;
}

std::span<const NodeBlobEntry> blobEntriesOf(const NodeData& node) {
  const uint64_t offset = static_cast<uint64_t>(node.offsetDiv4) * 4ULL;
  const auto& blob = *reinterpret_cast<const NodeBlobHeader*>(otbData + offset);

  return {blob.entries, blob.nEntries};
}

template <typename F> void forEachEntry(const NodeData& n, F&& f) {
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

using ranking::BucketSums;
using ranking::edgeScore;

BucketSums bucketSumsOf(const NodeData& node) {
  return ranking::bucketSumsOf(blobEntriesOf(node));
}

double emptyBucketPrior(const NodeData& node, int bkt) {
  return ranking::emptyBucketPrior(blobEntriesOf(node), bkt);
}

double ownScore(const NodeData& node, int bkt) {
  double num = 0, den = 0;

  for (const auto& entry : blobEntriesOf(node)) {
    if (entry.bucket == bkt) {
      num += entry.count * edgeScore(entry);
      den += entry.count;
    }
  }

  return den > 0 ? num / den : emptyBucketPrior(node, bkt);
}

/*
 * `freezePicks` keeps each bucket on the move already stored in moveMax rather
 * than re-running the pick rule. Scores are still recomputed from the current child evals;
 * only the choice is held.
 */
float backup(const StackElement& stackElement, bool freezePicks = false) {
  NodeData& node = *stackElement.node;

  std::vector<BucketSums> stats(stackElement.children.size());
  for (size_t k = 0; k < stackElement.children.size(); k++)
    stats[k] = bucketSumsOf(*stackElement.children[k].node);

  auto resolvedIn = [&](int k, int bkt) {
    return k >= 0 && stats[k].cnt[bkt] > 0;
  };

  // chance value for both white and black in this position
  double sumW[Stockfish::NUMBER_OF_BUCKETS] = {};
  double sumB[Stockfish::NUMBER_OF_BUCKETS] = {};
  double games[Stockfish::NUMBER_OF_BUCKETS] = {};

  forEachEntry(node, [&](const NodeBlobEntry& e, int k) {
    assert(k < 0 || stackElement.children[k].move == e.move);

    double w{}, b{};
    if (resolvedIn(k, e.bucket)) {
      w = stackElement.children[k].node->evalW[e.bucket];
      b = stackElement.children[k].node->evalB[e.bucket];
    } else {
      w = b = edgeScore(e);
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
      chanceW[bkt] = chanceB[bkt] = emptyBucketPrior(node, bkt);
    }
  }

  /*
   * One Ranker per bucket, fed from the pass below. The pick rule itself lives
   * in ranking::Ranker so that the evaluator, the server and verify_ote cannot
   * hold three slightly different versions of it.
   */
  ranking::Ranker rankers[Stockfish::NUMBER_OF_BUCKETS];
  for (auto& ranker : rankers)
    ranker.sgn = ranking::sideSign(stackElement.whiteToMove);

  ranking::Candidate held[Stockfish::NUMBER_OF_BUCKETS]{};

  forEachEntry(node, [&](const NodeBlobEntry& e, int k) {
    if (k < 0)
      return;

    const int bkt = e.bucket;
    bool res = resolvedIn(k, bkt);
    double value = !res                       ? edgeScore(e)
                   : stackElement.whiteToMove ? stackElement.children[k].node->evalW[e.bucket]
                                              : stackElement.children[k].node->evalB[e.bucket];
    double evidence = res ? stats[k].cnt[bkt] : e.count;
    double chance = stackElement.whiteToMove ? chanceW[bkt] : chanceB[bkt];
    ranking::Prior prior = ranking::nbrPrior(stats[k], bkt);
    const double sPrior = ranking::S_PRIOR[bkt];

    const ranking::Candidate cand{
        e.move, ranking::candidateScore(value, evidence, prior, chance, sPrior),
        ranking::candidateScoreSd(evidence, prior, sPrior), ranking::supportOf(evidence, prior)};

    rankers[bkt].add(cand);

    if (freezePicks && cand.move == node.moveMax[bkt])
      held[bkt] = cand;
  });

  double bestScore[Stockfish::NUMBER_OF_BUCKETS]{};
  uint16_t bestMove[Stockfish::NUMBER_OF_BUCKETS]{};

  for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
    const ranking::Candidate picked =
        freezePicks && held[bkt].present() ? held[bkt] : rankers[bkt].pick();

    bestMove[bkt] = picked.move;
    bestScore[bkt] = picked.score;
  }

  float residual = 0;
  for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
    double maxV = bestMove[bkt] != 0 ? bestScore[bkt] : ownScore(node, bkt);
    float w = stackElement.whiteToMove ? maxV : chanceW[bkt];
    float b = stackElement.whiteToMove ? chanceB[bkt] : maxV;
    residual = std::max({residual, std::abs(w - node.evalW[bkt]), std::abs(b - node.evalB[bkt])});

    node.evalW[bkt] = w;
    node.evalB[bkt] = b;
    node.moveMax[bkt] = bestMove[bkt];
  }

  return residual;
}

decltype(nodes)::iterator lookup(uint64_t zobrist) {
  const auto [nodeIt, last] =
      std::equal_range(nodes.begin(), nodes.end(), zobrist, ZobristCompare{});

  return nodeIt == last ? nodes.end() : nodeIt;
}

std::pair<decltype(nodes)::iterator, bool> dfs(AppMetrics& metrics, Stockfish::Position& pos) {
  static uint32_t timeInCounter = 1;

  metrics.maybePrint();

  uint64_t myZobrist = pos.key();
  auto it = lookup(myZobrist);

  if (it == nodes.end())
    return {it, false};

  auto& node = *it;
  if (node.timeIn != 0)
    return {it, false};

  node.low = node.timeIn = timeInCounter++;

  stack.push_back({it, {}, pos.side_to_move() == Stockfish::Color::WHITE});
  node.onStack = true;

  /* Safe across the recursive calls below: deque::push_back does not
   * invalidate references to elements already in the deque. */
  auto& childrenVec = stack.back().children;

  ++metrics.get<NODES_VISITED>();

  Stockfish::StateInfo state;

  Stockfish::Move prevMove = Stockfish::Move::none();

  for (const auto& blob : blobEntriesOf(node)) {
    Stockfish::Move move = Stockfish::Move(blob.move);
    if (move == prevMove || move == Stockfish::Move::termination() ||
        move == Stockfish::Move::forcedAggregation()) {
      continue;
    }

    prevMove = move;

    pos.do_move(move, state);
    auto [child, firstVisit] = dfs(metrics, pos);
    pos.undo_move(move);

    assert(child != nodes.end());
    childrenVec.emplace_back(move.raw(), child);

    if (firstVisit) {
      node.low = std::min(node.low, child->low);
    } else if (child->onStack) {
      node.low = std::min(node.low, child->timeIn);
    }
  }

  if (node.timeIn == node.low) { // SCC root
    std::vector<StackElement> scc;
    while (true) {
      assert(!stack.empty());

      StackElement sccMember = std::move(stack.back());
      stack.pop_back();

      sccMember.node->onStack = false;

      const bool isRoot = sccMember.node->zobrist == myZobrist;
      scc.emplace_back(std::move(sccMember));

      if (isRoot)
        break;
    }

    // single node, no need to iterate
    if (scc.size() == 1) {
      backup(scc[0]);
      ++metrics.get<BACKUPS>();
    } else {
      ++metrics.get<CYCLIC_SCCS>();
      recordMax<LARGEST_SCC>(metrics, scc.size());

      // sort in reverse to help the backup
      std::sort(scc.begin(), scc.end(), [](const StackElement& a, const StackElement& b) {
        return a.node->timeIn > b.node->timeIn;
      });

      for (auto& sccElem : scc) {
        for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
          sccElem.node->evalW[bkt] = sccElem.node->evalB[bkt] = 0.5;
        }
      }

      float residual;
      size_t sweeps = 0;
      bool freezePicks = false;
      do {
        residual = 0;
        for (const auto& sccElem : scc) {
          residual = std::max(residual, backup(sccElem, freezePicks));
        }

        metrics.get<BACKUPS>() += scc.size();

        ++sweeps;

        if (sweeps == FREEZE_PICKS_AFTER) {
          freezePicks = true;
          ++metrics.get<FROZEN_SCCS>();

          std::cout << "scc of " << scc.size() << " nodes has not settled after " << sweeps
                    << " sweeps (residual " << residual << "), freezing its picks\n";
        }

        assert(sweeps < MAX_SWEEPS);

        // a slowly converging component can iterate for a long time without dfs
        // being re-entered, so the progress line has to be driven from here too
        metrics.maybePrint();
      } while (residual >= EPS);

      metrics.get<SWEEPS_TOTAL>() += sweeps;
      recordMax<SLOWEST_SCC_SWEEPS>(metrics, sweeps);
    }
  }

  return {it, true};
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: expectimax <data.otb> <output.ote>\n";
    return 1;
  }

  if (int err = loadData(argv[1])) {
    return err;
  }

  const char* outputFilename = argv[2];
  std::ofstream out(outputFilename, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("Failed to open " + std::string(outputFilename));

  OteHeader header{};
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));

  Stockfish::Bitboards::init();
  Stockfish::Position::init();

  Stockfish::Position pos;
  Stockfish::StateInfo st;
  pos.set(Stockfish::StartFEN, false, &st);

  {
    // every node in the .otb was written because the tree walk reached it, so
    // this is an exact target rather than a ceiling
    const size_t total = nodes.size();

    AppMetrics metrics(
        std::chrono::seconds(1),
        [total](AppMetrics::Snapshot snapshot) {
          const auto nodesVisited = snapshot.get<NODES_VISITED>();

          std::cout << "nodes: " << nodesVisited << "/" << total << " ("
                    << (total != 0 ? 100.0 * nodesVisited / total : 0.0) << "%)"
                    << " backups: " << snapshot.get<BACKUPS>()
                    << " cyclic sccs: " << snapshot.get<CYCLIC_SCCS>()
                    << " largest scc: " << snapshot.get<LARGEST_SCC>()
                    << " sweeps: " << snapshot.get<SWEEPS_TOTAL>()
                    << " worst scc: " << snapshot.get<SLOWEST_SCC_SWEEPS>() << " sweeps"
                    << " frozen: " << snapshot.get<FROZEN_SCCS>() << "\n";
        },
        [total](AppMetrics::Snapshot snapshot) {
          std::cout << "\n===== Final Metrics =====\n";
          std::cout
              << "Finished in: "
              << std::chrono::duration_cast<std::chrono::seconds>(snapshot.getRuntime()).count()
              << " seconds\n";
          std::cout << "nodes visited: " << snapshot.get<NODES_VISITED>() << "/" << total << "\n";
          std::cout << "backups run: " << snapshot.get<BACKUPS>() << "\n";
          std::cout << "cyclic sccs: " << snapshot.get<CYCLIC_SCCS>() << "\n";
          std::cout << "largest scc: " << snapshot.get<LARGEST_SCC>() << " nodes\n";
          std::cout << "total sweeps: " << snapshot.get<SWEEPS_TOTAL>() << "\n";
          std::cout << "worst scc took: " << snapshot.get<SLOWEST_SCC_SWEEPS>() << " sweeps\n";
          std::cout << "sccs frozen: " << snapshot.get<FROZEN_SCCS>() << "\n";
        });

    dfs(metrics, pos);
  }

  std::cout << "writing OTE\n";

  for (const auto& node : nodes) {
    OteEntry oteEntry{};
    for (int bkt = 0; bkt < Stockfish::NUMBER_OF_BUCKETS; ++bkt) {
      oteEntry.moveMax[bkt] = node.moveMax[bkt];
      oteEntry.evalW[bkt] = node.evalW[bkt];
      oteEntry.evalB[bkt] = node.evalB[bkt];
    }

    out.write(reinterpret_cast<const char*>(&oteEntry), sizeof(oteEntry));
  }

  std::cout << "done\n";

  return 0;
}