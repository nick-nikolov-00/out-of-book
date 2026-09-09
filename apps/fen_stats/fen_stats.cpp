#include "BoardDB.h"
#include "MappedArray.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.h"
#include "movegen.h"
#include "position.h"
#include "uci.h"

using namespace Stockfish;

constexpr const char* BucketNames[] = {"<1000",     "1000-1199", "1200-1399",
                                       "1400-1599", "1600-1799", "1800-1999",
                                       "2000-2199", "2200-2399", "2400+"};

struct ZobristCompare {
  bool operator()(const SpillRecord& r, uint64_t key) const {
    return r.zobrist < key;
  }
  bool operator()(uint64_t key, const SpillRecord& r) const {
    return key < r.zobrist;
  }
};

struct BucketStats {
  uint64_t count = 0;
  uint64_t white = 0;
  uint64_t draws = 0;
  std::vector<SpillRecord> moves;
};

bool isOtbFile(const std::string& filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open input file: " + filename);

  uint64_t magic = 0;
  in.read(reinterpret_cast<char*>(&magic), sizeof(magic));

  if (!in)
    throw std::runtime_error("cannot read file header: " + filename);

  return magic == OtbHeader::MAGIC;
}

std::vector<SpillRecord> makeRecords(const NodeBlobHeader& node, uint64_t zobrist) {
  std::vector<SpillRecord> records;
  records.reserve(node.nEntries);

  for (uint16_t i = 0; i < node.nEntries; ++i) {
    const NodeBlobEntry& e = node.entries[i];

    SpillRecord r{};
    r.zobrist = zobrist;
    r.move = e.move;
    r.bucket = e.bucket;
    r.pad = e.pad;
    r.count = e.count;
    r.white_wins = e.white_wins;
    r.draws = e.draws;

    records.push_back(r);
  }

  return records;
}

std::optional<float> getChildEval(Position& pos, Move move, uint8_t bucket, BoardDB& db) {
  StateInfo st;

  pos.do_move(move, st);
  uint64_t zobrist = pos.key();
  pos.undo_move(move);

  BoardDB::NodeResult result;
  if (!db.find(zobrist, result) || !result.hasOte())
    return std::nullopt;

  const OteEntry& ote = *result.ote;

  return pos.side_to_move() == WHITE ? std::optional(ote.evalW[bucket])
                                     : std::optional(ote.evalB[bucket]);
}

void printStats(Position& pos, uint64_t zobrist, const std::vector<SpillRecord>& records,
                BoardDB* db = nullptr, const OteEntry* nodeOte = nullptr) {
  std::array<BucketStats, 9> buckets;

  for (const SpillRecord& r : records) {
    if (r.bucket >= buckets.size()) {
      std::cerr << "Warning: invalid bucket " << unsigned(r.bucket) << "\n";
      continue;
    }

    auto& b = buckets[r.bucket];
    b.count += r.count;
    b.white += r.white_wins;
    b.draws += r.draws;
    b.moves.push_back(r);
  }

  std::cout << "Position: " << pos << "\n";
  std::cout << "Zobrist: " << zobrist << "\n";
  std::cout << "Shard bucket: " << (zobrist >> (64 - 6)) << "\n\n";

  for (int bucket = 0; bucket < 9; ++bucket) {
    auto& b = buckets[bucket];

    std::cout << "============================================================\n";
    std::cout << "Bucket: " << BucketNames[bucket] << "\n\n";

    if (b.count == 0) {
      std::cout << "Position never reached\n\n";
      continue;
    }

    std::sort(b.moves.begin(), b.moves.end(), [](const SpillRecord& a, const SpillRecord& b) {
      return a.count > b.count;
    });

    uint64_t black = b.count - b.white - b.draws;

    std::cout << "Position stats\n";
    std::cout << "--------------\n";
    std::cout << "Games:       " << b.count << "\n";
    std::cout << "White wins:  " << b.white << " (" << 100.0 * b.white / b.count << "%)\n";
    std::cout << "Draws:       " << b.draws << " (" << 100.0 * b.draws / b.count << "%)\n";
    std::cout << "Black wins:  " << black << " (" << 100.0 * black / b.count << "%)\n";

    if (nodeOte) {
      const float eval =
          pos.side_to_move() == WHITE ? nodeOte->evalW[bucket] : nodeOte->evalB[bucket];

      const Move bestMove = Stockfish::Move(nodeOte->moveMax[bucket]);

      std::cout << "Eval:        " << std::fixed << std::setprecision(3) << eval << "\n";
      std::cout << "Best move:   " << UCIEngine::move(bestMove, false) << "\n";
    }

    std::cout << "\n";

    std::cout << std::left << std::setw(10) << "Move" << std::right << std::setw(10) << "Share"
              << std::setw(12) << "Games" << std::setw(10) << "White%" << std::setw(10) << "Draw%"
              << std::setw(10) << "Black%";

    if (db)
      std::cout << std::setw(12) << "Eval";

    std::cout << "\n";

    std::cout << std::string(db ? 74 : 62, '-') << "\n";

    size_t totalMoves = 0;

    for (const SpillRecord& r : b.moves) {
      ++totalMoves;

      const uint32_t blackWins = r.count - r.white_wins - r.draws;
      const double share = 100.0 * r.count / b.count;

      std::cout << std::left << std::setw(10) << UCIEngine::move(Stockfish::Move(r.move), false)
                << std::right << std::setw(9) << std::fixed << std::setprecision(2) << share << "%"
                << std::setw(12) << r.count << std::setw(9) << (100.0 * r.white_wins / r.count)
                << "%" << std::setw(9) << (100.0 * r.draws / r.count) << "%" << std::setw(9)
                << (100.0 * blackWins / r.count) << "%";

      auto move = Move(r.move);
      if (db && move != Move::termination() && move != Move::forcedAggregation()) {
        auto eval = getChildEval(pos, move, r.bucket, *db);

        if (eval) {
          std::cout << std::setw(12) << std::setprecision(3) << *eval;
        } else {
          std::cout << std::setw(12) << "N/A";
        }
      }

      std::cout << "\n";
    }

    std::cout << "Total moves: " << totalMoves << "\n";
  }
}

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: fen_stats \"<fen>\" <aggregate.bin|data.otb> [evals.ote]\n";
    return 1;
  }

  try {
    Stockfish::Bitboards::init();
    Stockfish::Position::init();

    const std::string fen = argv[1];
    const std::string input = argv[2];

    Stockfish::Position pos;
    Stockfish::StateInfo st;
    pos.set(fen, false, &st);

    const uint64_t zobrist = pos.key();
    const bool otb = isOtbFile(input);

    if (otb) {
      std::cout << "OTB detected\n\n";

      if (argc == 4) {
        BoardDB db(input, argv[3]);
        BoardDB::NodeResult result;

        if (!db.find(zobrist, result)) {
          std::cout << "Position not found.\n";
          return 0;
        }

        const auto records = makeRecords(*result.header(), zobrist);

        printStats(pos, zobrist, records, &db, result.hasOte() ? &*result.ote : nullptr);
      } else {
        BoardDB db(input);
        BoardDB::NodeResult result;

        if (!db.find(zobrist, result)) {
          std::cout << "Position not found.\n";
          return 0;
        }

        const auto records = makeRecords(*result.header(), zobrist);
        printStats(pos, zobrist, records);
      }
    } else {
      if (argc == 4) {
        throw std::runtime_error("an OTE file can only be used with an OTB input file");
      }

      std::cout << "Assuming FEN aggregate format\n\n";

      MappedArray<SpillRecord> records(input);

      auto [first, last] =
          std::equal_range(records.begin(), records.end(), zobrist, ZobristCompare{});

      if (first == last) {
        std::cout << "Position not found.\n";
        return 0;
      }

      std::vector<SpillRecord> allRecords;
      allRecords.reserve(static_cast<size_t>(last - first));

      for (auto it = first; it != last; ++it)
        allRecords.push_back(*it);

      printStats(pos, zobrist, allRecords);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}