#include "MappedFile.h"
#include "MoveRanking.h"
#include "common.h"
#include "position.h"
#include "types.h"
#include "uci.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/*
 * Does the number on a position actually pay out?
 *
 * An .ote stores, for every position and every rating band, the score a player
 * can expect from it. That number is produced by a backup rule, and a backup
 * rule is a claim rather than a measurement: it says that if you keep playing
 * the move it recommends and your opponent keeps behaving like the field, this
 * is what you will average. Nothing so far has tested the claim end to end --
 * the experiment harness in experiments/ measures whether the *ordering* of the
 * moves is right, which is a different question from whether the number beside
 * them is right.
 *
 * This plays the games out. From a sampled position it runs many complete
 * playouts in which one side -- the hero, the side to move at the sampled
 * position -- always plays the move the .ote recommends, and the other side is
 * a draw from the empirical move distribution of the band. A playout ends where
 * the search itself stopped expanding, and the result is then drawn from the
 * real games standing behind that last edge: white_wins, draws and the rest.
 * So every simulated game ends in an outcome that some set of real players at
 * that rating actually produced from that position.
 *
 * That makes the comparison a resampling of the same games the .ote was built
 * from rather than an out-of-sample test, and it is worth being blunt about
 * what that can and cannot show. It cannot show that the book generalises to
 * next month's players. It can show the thing that is actually in doubt: that
 * the stored number is the value of the policy it is attached to, that the
 * shrinkage applied at every level of the backup does not accumulate into a
 * systematic lie, and that the recommendations are worth more than playing the
 * way the field plays.
 *
 * Three arms are run over the identical set of positions so the last of those
 * has a control:
 *
 *   policy   the hero plays the .ote's recommended move
 *   popular  the hero plays whatever move is most played in the band
 *   crowd    the hero is drawn from the distribution too, so nobody is choosing
 *
 * `crowd` is the number the position already carries in the raw data, and the
 * gap between it and `policy` is what following the recommendations is worth in
 * points per game.
 */

using namespace Stockfish;

namespace {

constexpr size_t NB = NUMBER_OF_BUCKETS;

constexpr const char* BUCKET_NAMES[NB] = {"<1000",     "1000-1199", "1200-1399",
                                          "1400-1599", "1600-1799", "1800-1999",
                                          "2000-2199", "2200-2399", "2400+"};

constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

/* ------------------------------------------------------------------ book */

std::optional<MappedFile> otbFile;
std::optional<MappedFile> oteFile;
const std::byte* otbData = nullptr;
const std::byte* oteData = nullptr;
std::span<const IndexEntry> bookIndex;

struct ZobristCompare {
  bool operator()(const IndexEntry& e, uint64_t key) const {
    return e.zobrist < key;
  }
  bool operator()(uint64_t key, const IndexEntry& e) const {
    return key < e.zobrist;
  }
};

/* Index of a position in the .otb, which is also its index in the .ote: the two
 * files are parallel arrays over the same node ordering. */
int64_t lookup(uint64_t zobrist) {
  const auto [first, last] =
      std::equal_range(bookIndex.begin(), bookIndex.end(), zobrist, ZobristCompare{});

  return first == last ? -1 : first - bookIndex.begin();
}

std::span<const NodeBlobEntry> blobEntriesOf(int64_t node) {
  const uint64_t offset = static_cast<uint64_t>(bookIndex[node].offsetDiv4) * 4ULL;
  const auto& blob = *reinterpret_cast<const NodeBlobHeader*>(otbData + offset);

  return {blob.entries, blob.nEntries};
}

OteEntry oteOf(int64_t node) {
  OteEntry entry{};
  std::memcpy(&entry, oteData + sizeof(OteHeader) + static_cast<uint64_t>(node) * sizeof(OteEntry),
              sizeof(entry));

  return entry;
}

void openBook(const std::string& otbPath, const std::string& otePath) {
  otbFile.emplace(otbPath, MmapAdvice::RANDOM);
  otbData = otbFile->data();

  if (otbFile->size() < sizeof(OtbHeader))
    throw std::runtime_error("file is too small to hold an OTB header: " + otbPath);

  OtbHeader header{};
  std::memcpy(&header, otbData, sizeof(header));

  if (header.magic != OtbHeader::MAGIC)
    throw std::runtime_error("invalid magic in " + otbPath);
  if (header.version != OtbHeader::VERSION)
    throw std::runtime_error("unsupported OTB version in " + otbPath);
  if (header.indexOffset > otbFile->size() ||
      (otbFile->size() - header.indexOffset) / sizeof(IndexEntry) != header.nodes)
    throw std::runtime_error("OTB index does not match the node count in the header");

  const std::byte* start = otbData + header.indexOffset;
  assert(reinterpret_cast<uintptr_t>(start) % alignof(IndexEntry) == 0);
  bookIndex = {reinterpret_cast<const IndexEntry*>(start), header.nodes};

  oteFile.emplace(otePath, MmapAdvice::RANDOM);
  oteData = oteFile->data();

  OteHeader oteHeader{};
  if (oteFile->size() < sizeof(OteHeader))
    throw std::runtime_error("file is too small to hold an OTE header: " + otePath);
  std::memcpy(&oteHeader, oteData, sizeof(oteHeader));

  if (oteHeader.magic != OteHeader::MAGIC)
    throw std::runtime_error("invalid magic in " + otePath);
  if (oteHeader.version != OteHeader::VERSION)
    throw std::runtime_error("unsupported OTE version in " + otePath);

  /* The .ote being a flat array parallel to the .otb index is the one thing
   * that cannot be checked move by move later, so it is checked here. */
  if (oteFile->size() != sizeof(OteHeader) + header.nodes * sizeof(OteEntry))
    throw std::runtime_error(otePath + " is not parallel to " + otbPath +
                             ": wrong size for " + std::to_string(header.nodes) + " nodes");

  std::cerr << "book: " << header.nodes << " nodes\n";
}

/* -------------------------------------------------------------- one node */

bool isPseudo(uint16_t move) {
  return move == Move::termination().raw() || move == Move::forcedAggregation().raw();
}

/*
 * What a node looks like to one rating band: the rows of its blob that belong
 * to that band, the games in them, and the two moves an arm might play.
 *
 * The rows are left in mapped memory and referred to by span, so reading a node
 * costs a binary search and a page touch rather than an allocation.
 */
struct BandView {
  std::span<const NodeBlobEntry> rows;
  uint8_t bucket{};

  uint64_t games{};      // every row of the band, pseudo-moves included
  uint64_t whiteWins{};
  uint64_t draws{};

  uint16_t popular{};    // most played real move, 0 when there is none
};

BandView bandViewOf(int64_t node, uint8_t bucket) {
  BandView view;
  view.rows = blobEntriesOf(node);
  view.bucket = bucket;

  uint32_t popularCount = 0;

  for (const auto& row : view.rows) {
    if (row.bucket != bucket)
      continue;

    view.games += row.count;
    view.whiteWins += row.white_wins;
    view.draws += row.draws;

    if (!isPseudo(row.move) && row.count > popularCount) {
      popularCount = row.count;
      view.popular = row.move;
    }
  }

  return view;
}

/* Only the recommendation is read out of the .ote during a playout: the whole
 * 92-byte entry is a random touch into a 2.2 GB file, and two of the three arms
 * never need it. */
uint16_t bestMoveOf(int64_t node, uint8_t bucket) {
  uint16_t move = 0;
  std::memcpy(&move,
              oteData + sizeof(OteHeader) + static_cast<uint64_t>(node) * sizeof(OteEntry) +
                  bucket * sizeof(uint16_t),
              sizeof(move));

  return move;
}

const NodeBlobEntry* rowOf(const BandView& view, uint16_t move) {
  for (const auto& row : view.rows)
    if (row.bucket == view.bucket && row.move == move)
      return &row;

  return nullptr;
}

/* ------------------------------------------------------------- the rollout */

enum class Arm { Policy, Popular, Crowd };

const char* armName(Arm arm) {
  switch (arm) {
  case Arm::Policy:
    return "policy";
  case Arm::Popular:
    return "popular";
  case Arm::Crowd:
    return "crowd";
  }

  return "?";
}

/*
 * Why a playout stopped. Every one of these is a place the backup rule also
 * stopped expanding, except PlyCap, which is the guard against a repetition
 * cycle -- the book is a graph and a playout can walk a loop in it forever.
 */
enum class Stop {
  Ended,        // the games in this position ended here (Move::termination)
  Pooled,       // the crowd played one of the rare children the tree pooled away
  UnseenChild,  // the child has no games in this band, so the edge is the leaf
  NoMove,       // this band has no real move here at all
  PlyCap,
  Count
};

const char* stopName(Stop stop) {
  switch (stop) {
  case Stop::Ended:
    return "ended";
  case Stop::Pooled:
    return "pooled";
  case Stop::UnseenChild:
    return "unseen_child";
  case Stop::NoMove:
    return "no_move";
  case Stop::PlyCap:
    return "ply_cap";
  case Stop::Count:
    break;
  }

  return "?";
}

using Rng = std::mt19937_64;

/* One game's result out of the real games behind an edge, as White's points. */
double sampleWhitePoints(uint32_t count, uint32_t whiteWins, uint32_t draws, Rng& rng) {
  assert(count > 0);
  assert(whiteWins + draws <= count);

  const uint32_t roll = std::uniform_int_distribution<uint32_t>(0, count - 1)(rng);

  if (roll < whiteWins)
    return 1.0;
  if (roll < whiteWins + draws)
    return 0.5;

  return 0.0;
}

/* The row the field plays here, drawn in proportion to games. Pseudo-moves are
 * in the draw: a band that ends 30% of its games in this position spends 30% of
 * its probability on ending them, exactly as the backup rule's chance value
 * does. */
const NodeBlobEntry* sampleRow(const BandView& view, Rng& rng) {
  assert(view.games > 0);

  uint64_t roll = std::uniform_int_distribution<uint64_t>(0, view.games - 1)(rng);

  for (const auto& row : view.rows) {
    if (row.bucket != view.bucket)
      continue;

    if (roll < row.count)
      return &row;

    roll -= row.count;
  }

  assert(false && "band game total disagrees with its rows");
  return nullptr;
}

struct Playout {
  double heroPoints{};
  Stop stop{};
  int plies{};
};

struct RolloutContext {
  Position pos;
  std::vector<StateInfo> states;
  int plyCap = 80;
};

/*
 * One complete game from `fen`, with `hero` playing by `arm` and the other side
 * drawn from the band. Returns the hero's points.
 */
Playout playout(RolloutContext& ctx, const std::string& fen, uint8_t bucket, Color hero, Arm arm,
                Rng& rng) {
  Position& pos = ctx.pos;
  pos.set(fen, false, &ctx.states[0]);

  int ply = 0;

  /* The row a playout leaves the tree on, or the position it is cut off in.
   * Whichever is set when the loop breaks is what the outcome is drawn from. */
  const NodeBlobEntry* leafRow = nullptr;
  const BandView* leafNode = nullptr;
  Stop stop = Stop::PlyCap;

  int64_t node = lookup(pos.key());

  /* A playout only ever moves to a child that was checked to be in the book
   * with games in this band, and its root was checked the same way, so a miss
   * is a corrupt file rather than a normal leaf. */
  if (node < 0)
    throw std::runtime_error("playout started from a position the book does not hold: " + fen);

  BandView view = bandViewOf(node, bucket);
  assert(view.games > 0);

  while (true) {
    if (ply >= ctx.plyCap) {
      leafNode = &view;
      stop = Stop::PlyCap;
      break;
    }

    uint16_t move;

    if (pos.side_to_move() == hero && arm != Arm::Crowd) {
      move = arm == Arm::Policy ? bestMoveOf(node, bucket) : view.popular;

      if (move == 0) {
        leafNode = &view;
        stop = Stop::NoMove;
        break;
      }

      leafRow = rowOf(view, move);

      /* A recommendation is only ever picked from a row of its own band, so if
       * there is no such row the .ote does not belong to this .otb. */
      if (!leafRow)
        throw std::runtime_error("the chosen move has no row in its own band: " + pos.fen());
    } else {
      leafRow = sampleRow(view, rng);
      move = leafRow->move;

      if (move == Move::termination().raw()) {
        stop = Stop::Ended;
        break;
      }

      if (move == Move::forcedAggregation().raw()) {
        stop = Stop::Pooled;
        break;
      }
    }

    pos.do_move(Move(move), ctx.states[ply + 1]);
    ++ply;

    node = lookup(pos.key());

    /* Where the backup rule stops trusting the child and falls back to the
     * edge's own crowd score, the playout stops on the same edge. */
    if (node < 0) {
      stop = Stop::UnseenChild;
      break;
    }

    const BandView child = bandViewOf(node, bucket);

    if (child.games == 0) {
      stop = Stop::UnseenChild;
      break;
    }

    view = child;
  }

  double whitePoints;

  if (leafNode) {
    whitePoints = sampleWhitePoints(static_cast<uint32_t>(leafNode->games),
                                    static_cast<uint32_t>(leafNode->whiteWins),
                                    static_cast<uint32_t>(leafNode->draws), rng);
  } else {
    assert(leafRow);
    whitePoints = sampleWhitePoints(leafRow->count, leafRow->white_wins, leafRow->draws, rng);
  }

  return {hero == WHITE ? whitePoints : 1.0 - whitePoints, stop, ply};
}

/* ------------------------------------------------------ sampling positions */

/*
 * A position the simulation will be run from, and everything about it that is
 * known before a single game is played.
 */
struct Sample {
  std::string fen;
  uint64_t zobrist{};
  int depth{};
  bool whiteToMove{};
  uint8_t bucket{};

  uint64_t games{};        // games in this band standing in the position
  uint64_t visits{};       // times the descent walk passed through it
  float evalW{};
  float evalB{};
  uint16_t best{};
  double bestShare{};      // the recommendation's share of the band's games
  double crowdScore{};     // white's points per game in the raw data here
};

/*
 * Positions to test, found by walking the tree the way the band walks it: from
 * the start position with both sides drawn from the empirical distribution,
 * over and over. Nodes are kept with the number of walks that touched them, so
 * the caller can either sample uniformly over distinct positions or weight by
 * how often a player would actually stand in one.
 *
 * Sampling this way rather than by scanning the .otb is not a shortcut. A node
 * of the book is a Zobrist key and a move list; the position itself is not
 * stored, so the only way to get a FEN to play from is to arrive at it.
 */
std::vector<Sample> descend(uint8_t bucket, uint64_t minGames, int minDepth, int maxDepth,
                            size_t walks, Rng& rng) {
  std::unordered_map<uint64_t, Sample> found;

  Position pos;
  std::vector<StateInfo> states(static_cast<size_t>(maxDepth) + 2);

  for (size_t walk = 0; walk < walks; ++walk) {
    pos.set(START_FEN, false, &states[0]);

    for (int ply = 0; ply < maxDepth; ++ply) {
      const int64_t node = lookup(pos.key());
      if (node < 0)
        break;

      const BandView view = bandViewOf(node, bucket);

      if (view.games == 0)
        break;

      if (ply >= minDepth && view.games >= minGames && bestMoveOf(node, bucket) != 0) {
        auto [it, inserted] = found.try_emplace(pos.key());
        Sample& sample = it->second;

        if (inserted) {
          const OteEntry ote = oteOf(node);

          sample.fen = pos.fen();
          sample.zobrist = pos.key();
          sample.depth = ply;
          sample.whiteToMove = pos.side_to_move() == WHITE;
          sample.bucket = bucket;
          sample.games = view.games;
          sample.evalW = ote.evalW[bucket];
          sample.evalB = ote.evalB[bucket];
          sample.best = ote.moveMax[bucket];
          sample.crowdScore =
              ranking::edgeScore(static_cast<double>(view.games),
                                 static_cast<double>(view.whiteWins),
                                 static_cast<double>(view.draws));

          const NodeBlobEntry* row = rowOf(view, sample.best);
          sample.bestShare = row ? static_cast<double>(row->count) / view.games : 0.0;
        }

        ++sample.visits;
      }

      const NodeBlobEntry* row = sampleRow(view, rng);
      if (isPseudo(row->move))
        break;

      pos.do_move(Move(row->move), states[ply + 1]);
    }
  }

  std::vector<Sample> out;
  out.reserve(found.size());

  for (auto& [zobrist, sample] : found)
    out.push_back(std::move(sample));

  /* The map's order is a hash order, which would make the run irreproducible
   * for a fixed seed. */
  std::sort(out.begin(), out.end(),
            [](const Sample& a, const Sample& b) { return a.zobrist < b.zobrist; });

  return out;
}

/* An equal number of white-to-move and black-to-move positions, drawn without
 * replacement from what the descent found. The split is deliberate: the two
 * evals are produced by different halves of the backup rule -- one maximises
 * where the other averages -- and a bias in one of them would be invisible in a
 * pooled sample dominated by whichever side the walk happened to find more of. */
std::vector<Sample> chooseSamples(std::vector<Sample>& pool, size_t perSide, Rng& rng) {
  std::vector<Sample> white, black;

  for (auto& sample : pool)
    (sample.whiteToMove ? white : black).push_back(std::move(sample));

  std::vector<Sample> out;

  for (auto* side : {&white, &black}) {
    std::shuffle(side->begin(), side->end(), rng);

    const size_t take = std::min(perSide, side->size());
    for (size_t i = 0; i < take; ++i)
      out.push_back(std::move((*side)[i]));
  }

  return out;
}

/* ------------------------------------------------------------------- run */

struct Result {
  Arm arm{};
  uint64_t wins{};
  uint64_t draws{};
  uint64_t losses{};
  uint64_t stops[static_cast<size_t>(Stop::Count)]{};
  uint64_t plies{};
};

struct Job {
  size_t sample{};
  Arm arm{};
};

struct Params {
  std::string otb;
  std::string ote;
  std::string out;
  std::vector<uint8_t> buckets;
  uint64_t minGames = 2000;
  int minDepth = 2;
  int maxDepth = 24;
  int plyCap = 80;
  size_t walks = 20000;
  size_t perSide = 250;
  size_t sims = 20000;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
  uint64_t seed = 20260905;
  int timeoutSeconds = 3600;
};

std::atomic<size_t> nextJob{0};
std::atomic<size_t> jobsDone{0};
std::atomic<bool> timedOut{false};

void runJobs(const Params& params, const std::vector<Sample>& samples, const std::vector<Job>& jobs,
             std::vector<Result>& results, std::chrono::steady_clock::time_point deadline,
             uint64_t seed) {
  RolloutContext ctx;
  ctx.states.resize(static_cast<size_t>(params.plyCap) + 2);
  ctx.plyCap = params.plyCap;

  while (true) {
    const size_t index = nextJob.fetch_add(1, std::memory_order_relaxed);
    if (index >= jobs.size())
      return;

    if (std::chrono::steady_clock::now() > deadline) {
      timedOut.store(true, std::memory_order_relaxed);
      return;
    }

    const Job& job = jobs[index];
    const Sample& sample = samples[job.sample];
    Result& result = results[index];
    result.arm = job.arm;

    /* Seeded from the job rather than from the thread, so the same run gives
     * the same answer however the work is distributed across cores. */
    Rng rng(seed + 0x9E37'79B9'7F4A'7C15ULL * (index + 1));

    const Color hero = sample.whiteToMove ? WHITE : BLACK;

    for (size_t i = 0; i < params.sims; ++i) {
      const Playout game = playout(ctx, sample.fen, sample.bucket, hero, job.arm, rng);

      if (game.heroPoints == 1.0)
        ++result.wins;
      else if (game.heroPoints == 0.5)
        ++result.draws;
      else
        ++result.losses;

      ++result.stops[static_cast<size_t>(game.stop)];
      result.plies += static_cast<uint64_t>(game.plies);
    }

    jobsDone.fetch_add(1, std::memory_order_relaxed);
  }
}

Params parseArgs(int argc, char** argv) {
  Params params;

  const auto next = [&](int& i) -> std::string {
    if (++i >= argc)
      throw std::runtime_error(std::string("missing value after ") + argv[i - 1]);
    return argv[i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--out") {
      params.out = next(i);
    } else if (arg == "--buckets") {
      std::string list = next(i);
      for (char& c : list)
        if (c == ',')
          c = ' ';

      std::istringstream in(list);
      int bucket;
      while (in >> bucket) {
        if (bucket < 0 || bucket >= static_cast<int>(NB))
          throw std::runtime_error("bucket out of range: " + std::to_string(bucket));
        params.buckets.push_back(static_cast<uint8_t>(bucket));
      }
    } else if (arg == "--min-games") {
      params.minGames = std::stoull(next(i));
    } else if (arg == "--min-depth") {
      params.minDepth = std::stoi(next(i));
    } else if (arg == "--max-depth") {
      params.maxDepth = std::stoi(next(i));
    } else if (arg == "--ply-cap") {
      params.plyCap = std::stoi(next(i));
    } else if (arg == "--walks") {
      params.walks = std::stoull(next(i));
    } else if (arg == "--positions") {
      params.perSide = std::stoull(next(i));
    } else if (arg == "--sims") {
      params.sims = std::stoull(next(i));
    } else if (arg == "--threads") {
      params.threads = static_cast<unsigned>(std::stoul(next(i)));
    } else if (arg == "--seed") {
      params.seed = std::stoull(next(i));
    } else if (arg == "--timeout") {
      params.timeoutSeconds = std::stoi(next(i));
    } else if (params.otb.empty()) {
      params.otb = arg;
    } else if (params.ote.empty()) {
      params.ote = arg;
    } else {
      throw std::runtime_error("unexpected argument: " + arg);
    }
  }

  if (params.otb.empty() || params.ote.empty())
    throw std::runtime_error("an .otb and an .ote are both required");

  if (params.out.empty())
    throw std::runtime_error("--out is required");

  if (params.buckets.empty())
    for (uint8_t bucket = 0; bucket < NB; ++bucket)
      params.buckets.push_back(bucket);

  return params;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr
        << "usage: policy_sim <data.otb> <evals.ote> --out <results.csv> [options]\n\n"
        << "  --buckets 0,4,8      rating bands to run (default: all nine)\n"
        << "  --min-games N        games a band needs in a position to sample it (default 2000)\n"
        << "  --min-depth N        earliest half-move a sampled position may sit at (default 2)\n"
        << "  --max-depth N        how deep the descent walk looks for positions (default 24)\n"
        << "  --ply-cap N          half-moves a playout may last before it is cut (default 80)\n"
        << "  --walks N            descent walks per band (default 20000)\n"
        << "  --positions N        positions per side to move per band (default 250)\n"
        << "  --sims N             playouts per position per arm (default 20000)\n"
        << "  --threads N          worker threads (default: half the cores)\n"
        << "  --seed N             seed for both the sampling and the playouts\n"
        << "  --timeout N          seconds before the run gives up (default 3600)\n";
    return 1;
  }

  try {
    const Params params = parseArgs(argc, argv);

    Bitboards::init();
    Position::init();

    openBook(params.otb, params.ote);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(params.timeoutSeconds);

    std::vector<Sample> samples;

    for (uint8_t bucket : params.buckets) {
      Rng rng(params.seed + bucket);

      std::vector<Sample> pool =
          descend(bucket, params.minGames, params.minDepth, params.maxDepth, params.walks, rng);

      std::vector<Sample> chosen = chooseSamples(pool, params.perSide, rng);

      size_t white = 0;
      for (const auto& sample : chosen)
        white += sample.whiteToMove ? 1 : 0;

      std::cerr << "band " << BUCKET_NAMES[bucket] << ": " << pool.size() << " positions with >= "
                << params.minGames << " games, sampling " << chosen.size() << " (" << white
                << " white to move, " << chosen.size() - white << " black)\n";

      samples.insert(samples.end(), std::make_move_iterator(chosen.begin()),
                     std::make_move_iterator(chosen.end()));
    }

    if (samples.empty())
      throw std::runtime_error("no position met the sampling criteria");

    std::vector<Job> jobs;
    jobs.reserve(samples.size() * 3);

    for (size_t i = 0; i < samples.size(); ++i)
      for (Arm arm : {Arm::Policy, Arm::Popular, Arm::Crowd})
        jobs.push_back({i, arm});

    std::vector<Result> results(jobs.size());

    std::cerr << samples.size() << " positions, " << jobs.size() << " position-arms, "
              << params.sims << " playouts each = " << jobs.size() * params.sims
              << " games, on " << params.threads << " threads\n";

    std::vector<std::thread> workers;
    for (unsigned t = 0; t < params.threads; ++t)
      workers.emplace_back(runJobs, std::cref(params), std::cref(samples), std::cref(jobs),
                           std::ref(results), deadline, params.seed);

    while (jobsDone.load(std::memory_order_relaxed) < jobs.size() &&
           !timedOut.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(5));

      const size_t done = jobsDone.load(std::memory_order_relaxed);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started)
              .count();

      std::cerr << "  " << done << "/" << jobs.size() << " position-arms after " << elapsed
                << "s\n";
    }

    for (auto& worker : workers)
      worker.join();

    if (timedOut.load(std::memory_order_relaxed))
      throw std::runtime_error("timed out after " + std::to_string(params.timeoutSeconds) +
                               "s with " + std::to_string(jobsDone.load()) + "/" +
                               std::to_string(jobs.size()) + " position-arms done");

    std::ofstream out(params.out, std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot write " + params.out);

    out << "bucket,band,fen,depth,white_to_move,hero,games,visits,eval_w,eval_b,pred_hero,"
           "best_move,best_share,crowd_hero,arm,sims,wins,draws,losses,obs_hero,se,mean_plies";

    for (size_t s = 0; s < static_cast<size_t>(Stop::Count); ++s)
      out << ",stop_" << stopName(static_cast<Stop>(s));

    out << "\n";

    for (size_t j = 0; j < jobs.size(); ++j) {
      const Sample& sample = samples[jobs[j].sample];
      const Result& result = results[j];

      const double n = static_cast<double>(params.sims);
      const double mean =
          (static_cast<double>(result.wins) + 0.5 * static_cast<double>(result.draws)) / n;

      /* Standard error of the mean of n independent playouts. The playouts are
       * independent draws given the position, so this is the whole uncertainty
       * the simulation itself contributes -- it says nothing about how well the
       * games behind the tree represent the band. */
      const double variance = std::max(
          0.0, (static_cast<double>(result.wins) + 0.25 * static_cast<double>(result.draws)) / n -
                   mean * mean);
      const double se = std::sqrt(variance / n);

      /* Both evals are White's points per game. evalW is the value when White
       * is the one choosing and evalB when Black is, so the hero's own number
       * is whichever of the two belongs to the side to move, flipped for
       * Black. */
      const double predWhite = sample.whiteToMove ? sample.evalW : sample.evalB;
      const double predHero = sample.whiteToMove ? predWhite : 1.0 - predWhite;
      const double crowdHero =
          sample.whiteToMove ? sample.crowdScore : 1.0 - sample.crowdScore;

      out << unsigned(sample.bucket) << ',' << BUCKET_NAMES[sample.bucket] << ",\"" << sample.fen
          << "\"," << sample.depth << ',' << (sample.whiteToMove ? 1 : 0) << ','
          << (sample.whiteToMove ? "white" : "black") << ',' << sample.games << ','
          << sample.visits << ',' << sample.evalW << ',' << sample.evalB << ',' << predHero << ','
          << UCIEngine::move(Move(sample.best), false) << ',' << sample.bestShare << ','
          << crowdHero << ',' << armName(result.arm) << ',' << params.sims << ',' << result.wins
          << ',' << result.draws << ',' << result.losses << ',' << mean << ',' << se << ','
          << static_cast<double>(result.plies) / n;

      for (size_t s = 0; s < static_cast<size_t>(Stop::Count); ++s)
        out << ',' << result.stops[s];

      out << "\n";
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    std::cerr << "wrote " << params.out << " in " << elapsed << "s\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
