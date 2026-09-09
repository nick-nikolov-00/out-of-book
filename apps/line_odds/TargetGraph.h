#pragma once

#include "Book.h"
#include "Reachability.h"
#include "position.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lineodds {

/*
 * Every position from which the target is still reachable at its own
 * half-move, laid out one layer per half-move.
 *
 * Built in two passes. The forward pass walks out from the starting position
 * keeping whatever TargetShape cannot rule out, which is a superset of what
 * matters; the backward pass then keeps only the states with a path that
 * actually lands on the target, which is exact. What survives is small --
 * both sides have to spend nearly every move walking pieces onto their target
 * squares -- so nothing here is beamed or truncated, and a run that would
 * outgrow its limits stops with an error rather than quietly answering for a
 * tree it did not finish.
 *
 * The crowd's children come from the book, because a move nobody played is a
 * move the walk has no share for. The hero's children are every legal move,
 * since the hero is not being predicted but steered; the ones leading
 * somewhere the book does not hold are found by the crowd a half-move later.
 */
class TargetGraph {
 public:
  struct Edge {
    uint32_t child{};
    uint16_t move{};
  };

  struct Node {
    uint64_t key{};
    uint32_t parent{};
    uint32_t firstEdge{};
    uint32_t edgeCount{};
    uint16_t move{};
    bool alive = false;
  };

  struct Layer {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
  };

  struct Limits {
    size_t maxStates = 20'000'000;
    double maxSeconds = 900.0;
  };

  struct Census {
    size_t enumerated{};  // states the forward pass kept
    size_t survivors{};   // states with a real path to the target
    size_t widest{};      // widest surviving layer
    size_t widestRaw{};   // widest layer before the backward pass
    double seconds{};
  };

  TargetGraph(const Book& book, const Stockfish::Position& target, int targetPly,
              std::optional<Stockfish::Color> hero, const std::vector<std::string>& planFilter,
              Limits limits);

  const Layer& layer(int ply) const { return layers[size_t(ply)]; }
  int plies() const { return targetPly; }
  bool targetFound() const { return found; }
  const Census& census() const { return counts; }

  /* Replays a node's moves from the starting position. */
  void positionAt(int ply, uint32_t index, Stockfish::Position& pos,
                  std::vector<Stockfish::StateInfo>& states) const;

  static constexpr uint32_t NO_PARENT = UINT32_MAX;

 private:
  void expand(int ply, Limits limits, double startedAt);
  void filterBackward();

  const Book& book;
  TargetShape shape;
  uint64_t targetKey{};
  int targetPly{};
  std::optional<Stockfish::Color> hero;
  std::vector<std::string> planFilter;

  std::vector<Layer> layers;
  Census counts{};
  bool found = false;
};

} // namespace lineodds
