#include "MappedFile.h"
#include "MoveRanking.h"
#include "common.h"
#include "metrics.h"
#include "position.h"
#include "types.h"
#include "uci.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

/*
 * Carves a shallow, densely populated subtree out of the production .otb and
 * joins it against a single month's aggregate, so that the expectimax
 * experiments can be run in Python without any of them having to know about
 * Zobrist keys, Stockfish move encodings or the .otb layout.
 *
 * The subtree is the experiment's fixed stage. Every node in it has at least
 * `--min-games` games behind it in the oracle, which is what makes the oracle's
 * own ranking of the moves trustworthy; the same node set, the same edges and
 * the same leaves are then handed to a candidate algorithm carrying only the
 * month's counts. Because the topology is shared, any modelling choice baked
 * into the stage -- notably that a leaf is scored by its crowd average rather
 * than by continued optimal play -- applies identically to the oracle and to
 * the candidate, and cancels out of the comparison. What is left is the only
 * thing being measured: how well an algorithm recovers the right ordering from
 * thin evidence.
 *
 * Depth is strictly increasing along tree edges. A move that leads to a
 * position already discovered at the same or a shallower depth -- a repetition,
 * or a transposition that arrives late -- is kept as an edge, so its games
 * still count toward the crowd average, but it is not a tree edge and not a
 * ranked candidate. That keeps the subtree a DAG in depth order, so both the
 * oracle solve and every experiment are a single sweep from the deepest nodes
 * back to the root, with none of the Tarjan machinery the production evaluator
 * needs.
 */

namespace {

constexpr size_t NB = Stockfish::NUMBER_OF_BUCKETS;

/* Written into moves.bin so Python can tell the three kinds apart. */
enum MoveKind : uint8_t {
  KIND_TREE = 0,  // a real move whose child is a node of the subtree
  KIND_THIN = 1,  // a real move whose child did not clear --min-games
  KIND_BACK = 2,  // a real move back into an already-discovered, not-deeper node
  KIND_TERM = 3,  // Move::termination(), the games that ended here
  KIND_AGG = 4,   // Move::forcedAggregation(), the oracle's pooled dropped children
};

#pragma pack(push, 1)
struct NodeOut {
  uint64_t zobrist;
  int32_t depth;
  uint8_t whiteToMove;
  uint8_t isLeaf;
  uint16_t nMoves;
  uint32_t moveStart;
};
static_assert(sizeof(NodeOut) == 20);

struct MoveOut {
  uint32_t node;
  uint16_t move;
  uint8_t kind;
  uint8_t pad;
  int32_t child; // subtree node index, or -1

  uint32_t oCount[NB];
  uint32_t oWins[NB];
  uint32_t oDraws[NB];
  uint32_t mCount[NB];
  uint32_t mWins[NB];
  uint32_t mDraws[NB];
};
static_assert(sizeof(MoveOut) == 12 + 6 * NB * 4);
#pragma pack(pop)

struct IndexNode {
  uint64_t zobrist;
  uint32_t offsetDiv4;
};

struct ZobristCompare {
  bool operator()(const IndexNode& r, uint64_t key) const {
    return r.zobrist < key;
  }
  bool operator()(uint64_t key, const IndexNode& r) const {
    return key < r.zobrist;
  }
};

/* The subtree under construction. `moves` is grouped by node and each group is
 * kept sorted by raw move, which is the order the .otb blob already arrives in
 * and what lets the month join binary search inside a group. */
struct Subtree {
  std::vector<NodeOut> nodes;
  std::vector<std::vector<MoveOut>> moves; // parallel to nodes
  std::vector<std::string> fens;
  std::unordered_map<uint64_t, uint32_t> byKey;
};

std::optional<MappedFile> otbFile;
const std::byte* otbData = nullptr;
std::vector<IndexNode> index;

std::span<const NodeBlobEntry> blobEntriesOf(const IndexNode& node) {
  const uint64_t offset = static_cast<uint64_t>(node.offsetDiv4) * 4ULL;
  const auto& blob = *reinterpret_cast<const NodeBlobHeader*>(otbData + offset);

  return {blob.entries, blob.nEntries};
}

const IndexNode* lookup(uint64_t zobrist) {
  const auto [it, last] = std::equal_range(index.begin(), index.end(), zobrist, ZobristCompare{});

  return it == last ? nullptr : &*it;
}

uint64_t totalGamesOf(const IndexNode& node) {
  uint64_t total = 0;

  for (const auto& e : blobEntriesOf(node))
    total += e.count;

  return total;
}

/* Games behind one move of a node, summed over the buckets. */
uint64_t gamesOfMove(const IndexNode& node, uint16_t move) {
  uint64_t total = 0;

  for (const auto& e : blobEntriesOf(node))
    if (e.move == move)
      total += e.count;

  return total;
}

int loadIndex(const char* otbFilename) {
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
    std::cerr << "invalid magic in " << otbFilename << '\n';
    return 1;
  }
  if (header.version != OtbHeader::VERSION) {
    std::cerr << "invalid version in " << otbFilename << '\n';
    return 1;
  }
  if (header.indexOffset > otbFile->size() ||
      (otbFile->size() - header.indexOffset) / sizeof(IndexEntry) != header.nodes) {
    std::cerr << "OTB index does not match the node count in the header\n";
    return 1;
  }

  std::cout << "otb nodes: " << header.nodes << '\n';

  index.resize(header.nodes);

  for (uint64_t i = 0; i < header.nodes; ++i) {
    IndexEntry entry{};
    std::memcpy(&entry, otbData + header.indexOffset + i * sizeof(IndexEntry), sizeof(entry));

    index[i].zobrist = entry.zobrist;
    index[i].offsetDiv4 = entry.offsetDiv4;
  }

  std::cout << "index loaded\n";

  return 0;
}

struct Params {
  uint64_t minGames = 20'000;
  int maxDepth = 20;
  size_t maxNodes = 400'000;
};

Params params;

struct Stats {
  uint64_t treeEdges{};
  uint64_t thinEdges{};
  uint64_t backEdges{};
  uint64_t leaves{};
};

Stats stats;

/*
 * Adds `pos` to the subtree and recurses. Returns the subtree index of the node.
 *
 * A node is created only once; the second path into it links to the node the
 * first path built. `depth` is therefore the *first* depth the node was reached
 * at, and is what the depth-ordering invariant is stated in terms of.
 */
uint32_t build(Subtree& sub, Stockfish::Position& pos, const IndexNode& idxNode, int depth) {
  const uint32_t self = static_cast<uint32_t>(sub.nodes.size());

  sub.byKey.emplace(idxNode.zobrist, self);
  sub.nodes.push_back(NodeOut{.zobrist = idxNode.zobrist,
                              .depth = depth,
                              .whiteToMove =
                                  static_cast<uint8_t>(pos.side_to_move() == Stockfish::WHITE),
                              .isLeaf = 1,
                              .nMoves = 0,
                              .moveStart = 0});
  sub.moves.emplace_back();
  sub.fens.push_back(pos.fen());

  if (sub.nodes.size() > params.maxNodes)
    throw std::runtime_error("subtree exceeded --max-nodes; raise --min-games");

  /*
   * One MoveOut per distinct move of the node, filled from the blob's
   * per-bucket rows. The blob arrives sorted by (move, bucket), so a new move
   * simply starts a new row.
   *
   * These live on the stack until the node is finished rather than in
   * sub.moves[self]: the recursion below pushes onto sub.moves, which
   * reallocates the outer vector and would leave any reference into it
   * dangling. Nothing a descendant can reach reads this node's move list, so
   * handing it over at the end is safe.
   */
  std::vector<MoveOut> rows;
  uint16_t prev = 0;

  for (const auto& e : blobEntriesOf(idxNode)) {
    if (rows.empty() || e.move != prev) {
      prev = e.move;
      rows.push_back(MoveOut{.node = self, .move = e.move, .kind = KIND_THIN, .child = -1});
    }

    assert(e.bucket < NB && "bucket out of range");

    MoveOut& row = rows.back();
    row.oCount[e.bucket] += e.count;
    row.oWins[e.bucket] += e.white_wins;
    row.oDraws[e.bucket] += e.draws;
  }

  const bool expand = depth < params.maxDepth;

  Stockfish::StateInfo st;

  for (MoveOut& row : rows) {
    const Stockfish::Move move(row.move);

    if (move == Stockfish::Move::termination()) {
      row.kind = KIND_TERM;
      continue;
    }
    if (move == Stockfish::Move::forcedAggregation()) {
      row.kind = KIND_AGG;
      continue;
    }

    if (!expand) {
      ++stats.thinEdges;
      continue;
    }

    pos.do_move(move, st);
    const uint64_t childKey = pos.key();

    const auto seen = sub.byKey.find(childKey);

    if (seen != sub.byKey.end()) {
      /* Already in the subtree. Only a strictly deeper node keeps the DAG in
       * depth order; anything else stays an edge but not a tree edge. */
      if (sub.nodes[seen->second].depth > depth) {
        row.kind = KIND_TREE;
        row.child = static_cast<int32_t>(seen->second);
        ++stats.treeEdges;
      } else {
        row.kind = KIND_BACK;
        row.child = static_cast<int32_t>(seen->second);
        ++stats.backEdges;
      }

      pos.undo_move(move);
      continue;
    }

    const IndexNode* childIdx = lookup(childKey);
    const bool dense = childIdx != nullptr && totalGamesOf(*childIdx) >= params.minGames;

    if (dense) {
      row.kind = KIND_TREE;
      row.child = static_cast<int32_t>(build(sub, pos, *childIdx, depth + 1));
      ++stats.treeEdges;
    } else {
      ++stats.thinEdges;
    }

    pos.undo_move(move);
  }

  for (const MoveOut& row : rows) {
    if (row.kind == KIND_TREE) {
      sub.nodes[self].isLeaf = 0;
      break;
    }
  }

  sub.moves[self] = std::move(rows);

  if (sub.nodes[self].isLeaf)
    ++stats.leaves;

  return self;
}

/*
 * One sequential pass over the month's aggregate, filling in the m* fields.
 *
 * Both sides are sorted by Zobrist key -- the aggregate by construction, the
 * subtree because it is sorted here -- so this is a merge rather than 34 GB of
 * random reads. A month move that the oracle never kept is appended to the
 * node, with its oracle counts left at zero: the candidate algorithms should
 * see exactly the move list the month would have handed them, even though such
 * a move can never be ranked against the oracle.
 */
void joinMonth(Subtree& sub, const char* aggFilename) {
  std::vector<uint32_t> order(sub.nodes.size());
  for (uint32_t i = 0; i < order.size(); ++i)
    order[i] = i;

  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return sub.nodes[a].zobrist < sub.nodes[b].zobrist;
  });

  for (auto& rows : sub.moves)
    std::sort(rows.begin(), rows.end(),
              [](const MoveOut& a, const MoveOut& b) { return a.move < b.move; });

  const int fd = open(aggFilename, O_RDONLY);
  if (fd == -1)
    throw std::runtime_error(std::string("cannot open ") + aggFilename);

  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

  constexpr size_t BATCH = 1 << 20;
  std::vector<SpillRecord> buffer(BATCH);

  size_t cursor = 0; // into `order`
  uint64_t recordsSeen = 0, recordsMatched = 0, movesAdded = 0;

  while (cursor < order.size()) {
    const ssize_t n = read(fd, buffer.data(), BATCH * sizeof(SpillRecord));

    if (n < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      throw std::runtime_error("read failed on the month aggregate");
    }
    if (n == 0)
      break;

    if (static_cast<size_t>(n) % sizeof(SpillRecord) != 0) {
      close(fd);
      throw std::runtime_error("month aggregate is not a whole number of records");
    }

    const size_t count = static_cast<size_t>(n) / sizeof(SpillRecord);
    recordsSeen += count;

    for (size_t i = 0; i < count && cursor < order.size(); ++i) {
      const SpillRecord& record = buffer[i];

      while (cursor < order.size() && sub.nodes[order[cursor]].zobrist < record.zobrist)
        ++cursor;

      if (cursor >= order.size())
        break;

      if (sub.nodes[order[cursor]].zobrist != record.zobrist)
        continue;

      assert(record.bucket < NB && "bucket out of range");

      std::vector<MoveOut>& rows = sub.moves[order[cursor]];

      auto it = std::lower_bound(rows.begin(), rows.end(), record.move,
                                 [](const MoveOut& r, uint16_t m) { return r.move < m; });

      if (it == rows.end() || it->move != record.move) {
        /* A move the oracle never kept. Insert in place so the group stays
         * sorted for the rest of this node's records. */
        MoveOut fresh{.node = order[cursor], .move = record.move, .kind = KIND_THIN, .child = -1};

        if (record.move == Stockfish::Move::termination().raw())
          fresh.kind = KIND_TERM;
        else if (record.move == Stockfish::Move::forcedAggregation().raw())
          fresh.kind = KIND_AGG;

        it = rows.insert(it, fresh);
        ++movesAdded;
      }

      it->mCount[record.bucket] += record.count;
      it->mWins[record.bucket] += record.white_wins;
      it->mDraws[record.bucket] += record.draws;
      ++recordsMatched;
    }

    if (recordsSeen % (100ULL << 20) < BATCH)
      std::cout << "  month scan: " << (recordsSeen >> 20) << "M records, " << recordsMatched
                << " matched, node " << cursor << "/" << order.size() << '\n';
  }

  close(fd);

  std::cout << "month join: " << recordsSeen << " records scanned, " << recordsMatched
            << " matched, " << movesAdded << " month-only moves added\n";
}

void write(const Subtree& sub, const std::string& outDir) {
  std::ofstream nodesOut(outDir + "/nodes.bin", std::ios::binary | std::ios::trunc);
  std::ofstream movesOut(outDir + "/moves.bin", std::ios::binary | std::ios::trunc);
  std::ofstream namesOut(outDir + "/move_names.txt", std::ios::trunc);
  std::ofstream fensOut(outDir + "/fens.txt", std::ios::trunc);

  if (!nodesOut || !movesOut || !namesOut || !fensOut)
    throw std::runtime_error("cannot open the output files in " + outDir);

  uint32_t moveStart = 0;

  for (size_t i = 0; i < sub.nodes.size(); ++i) {
    NodeOut node = sub.nodes[i];
    node.moveStart = moveStart;
    node.nMoves = static_cast<uint16_t>(sub.moves[i].size());
    moveStart += node.nMoves;

    nodesOut.write(reinterpret_cast<const char*>(&node), sizeof(node));
    fensOut << sub.fens[i] << '\n';

    for (const MoveOut& row : sub.moves[i]) {
      movesOut.write(reinterpret_cast<const char*>(&row), sizeof(row));

      const Stockfish::Move move(row.move);

      if (row.kind == KIND_TERM)
        namesOut << "TERM\n";
      else if (row.kind == KIND_AGG)
        namesOut << "AGG\n";
      else
        namesOut << Stockfish::UCIEngine::move(move, false) << '\n';
    }
  }

  std::ofstream metaOut(outDir + "/meta.json", std::ios::trunc);
  metaOut << "{\n"
          << "  \"nodes\": " << sub.nodes.size() << ",\n"
          << "  \"moves\": " << moveStart << ",\n"
          << "  \"buckets\": " << NB << ",\n"
          << "  \"min_games\": " << params.minGames << ",\n"
          << "  \"max_depth\": " << params.maxDepth << ",\n"
          << "  \"leaves\": " << stats.leaves << ",\n"
          << "  \"tree_edges\": " << stats.treeEdges << ",\n"
          << "  \"thin_edges\": " << stats.thinEdges << ",\n"
          << "  \"back_edges\": " << stats.backEdges << "\n"
          << "}\n";

  std::cout << "wrote " << sub.nodes.size() << " nodes and " << moveStart << " moves to " << outDir
            << '\n';
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: export_subtree <oracle.otb> <month_agg.bin> <outdir>"
                 " [--min-games N] [--max-depth D] [--max-nodes M]\n";
    return 1;
  }

  for (int i = 4; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];

    if (flag == "--min-games")
      params.minGames = std::stoull(argv[i + 1]);
    else if (flag == "--max-depth")
      params.maxDepth = std::stoi(argv[i + 1]);
    else if (flag == "--max-nodes")
      params.maxNodes = std::stoull(argv[i + 1]);
    else {
      std::cerr << "unknown flag " << flag << '\n';
      return 1;
    }
  }

  std::cout << "min-games: " << params.minGames << " max-depth: " << params.maxDepth << '\n';

  if (int err = loadIndex(argv[1]))
    return err;

  Stockfish::Bitboards::init();
  Stockfish::Position::init();

  Stockfish::Position pos;
  Stockfish::StateInfo st;
  pos.set(Stockfish::StartFEN, false, &st);

  const IndexNode* root = lookup(pos.key());
  if (root == nullptr) {
    std::cerr << "the start position is not in the .otb\n";
    return 1;
  }

  Subtree sub;
  std::cout << "building subtree\n";
  build(sub, pos, *root, 0);

  std::cout << "subtree: " << sub.nodes.size() << " nodes, " << stats.leaves << " leaves, "
            << stats.treeEdges << " tree edges, " << stats.thinEdges << " thin edges, "
            << stats.backEdges << " back edges\n";

  std::cout << "joining the month aggregate\n";
  joinMonth(sub, argv[2]);

  write(sub, argv[3]);

  return 0;
}
