#include "TargetGraph.h"

#include "movegen.h"
#include "uci.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <unordered_map>

using namespace Stockfish;

namespace lineodds {

namespace {

constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool isPseudoMove(Move m) {
  return m == Move::termination() || m == Move::forcedAggregation();
}

} // namespace

TargetGraph::TargetGraph(const Book& book, const Position& target, int targetPly,
                         std::optional<Color> hero, const std::vector<std::string>& planFilter,
                         Limits limits)
    : book(book),
      shape(target),
      targetKey(target.key()),
      targetPly(targetPly),
      hero(hero),
      planFilter(planFilter) {
  const double startedAt = now();

  layers.resize(size_t(targetPly) + 1);

  Position start;
  StateInfo st;
  start.set(START_FEN, false, &st);

  layers[0].nodes.push_back(Node{.key = start.key(), .parent = NO_PARENT, .move = 0});
  counts.enumerated = 1;

  for (int ply = 0; ply < targetPly; ++ply)
    expand(ply, limits, startedAt);

  filterBackward();

  counts.seconds = now() - startedAt;
}

void TargetGraph::positionAt(int ply, uint32_t index, Position& pos,
                             std::vector<StateInfo>& states) const {
  static thread_local std::vector<uint16_t> path;
  path.clear();

  for (int p = ply; p > 0; --p) {
    const Node& node = layers[size_t(p)].nodes[index];
    path.push_back(node.move);
    index = node.parent;
  }

  states.resize(size_t(ply) + 1);
  pos.set(START_FEN, false, &states[0]);

  for (size_t i = path.size(); i-- > 0;)
    pos.do_move(Move(path[i]), states[path.size() - i]);
}

void TargetGraph::expand(int ply, Limits limits, double startedAt) {
  Layer& here = layers[size_t(ply)];
  Layer& next = layers[size_t(ply) + 1];

  /* Half-moves each side still gets between this layer and the target. */
  const int remaining = targetPly - ply - 1;

  std::unordered_map<uint64_t, uint32_t> seen;
  seen.reserve(here.nodes.size() * 4);

  Position pos;
  std::vector<StateInfo> states;
  StateInfo childState;

  const auto admit = [&](Position& parent, Move move, uint32_t parentIndex) {
    parent.do_move(move, childState);

    const Color stm = parent.side_to_move();
    const int forStm = (remaining + 1) / 2;
    const int forOther = remaining / 2;

    if (shape.reachable(parent, stm == WHITE ? forStm : forOther,
                        stm == WHITE ? forOther : forStm)) {
      const uint64_t key = parent.key();
      auto [it, inserted] = seen.try_emplace(key, uint32_t(next.nodes.size()));

      if (inserted)
        next.nodes.push_back(
            Node{.key = key, .parent = parentIndex, .move = move.raw()});

      here.edges.push_back(Edge{.child = it->second, .move = move.raw()});
    }

    parent.undo_move(move);
  };

  for (uint32_t i = 0; i < here.nodes.size(); ++i) {
    if ((i & 0x3FF) == 0) {
      if (now() - startedAt > limits.maxSeconds)
        throw std::runtime_error("line_odds ran past its --timeout while enumerating half-move " +
                                 std::to_string(ply) + "; raise --timeout or narrow the target");

      if (next.nodes.size() > limits.maxStates)
        throw std::runtime_error(
            "line_odds outgrew --max-states while enumerating half-move " + std::to_string(ply) +
            "; the target is too loosely constrained to enumerate exactly");
    }

    Node& node = here.nodes[i];
    node.firstEdge = uint32_t(here.edges.size());

    positionAt(ply, i, pos, states);

    if (hero && pos.side_to_move() == *hero) {
      for (const auto& m : MoveList<LEGAL>(pos)) {
        if (!planFilter.empty() &&
            std::find(planFilter.begin(), planFilter.end(), UCIEngine::move(m, false)) ==
                planFilter.end())
          continue;

        admit(pos, m, i);
      }
    } else {
      uint16_t previous = 0;
      bool first = true;

      for (const auto& e : book.at(node.key)) {
        /* A blob holds one row per (move, band), and a move's rows are
         * adjacent, so this collapses them into one child. */
        if (!first && e.move == previous)
          continue;

        previous = e.move;
        first = false;

        if (!isPseudoMove(Move(e.move)))
          admit(pos, Move(e.move), i);
      }
    }

    node.edgeCount = uint32_t(here.edges.size()) - node.firstEdge;
  }

  counts.enumerated += next.nodes.size();
  counts.widestRaw = std::max(counts.widestRaw, next.nodes.size());
}

void TargetGraph::filterBackward() {
  Layer& last = layers[size_t(targetPly)];

  for (Node& node : last.nodes)
    if (node.key == targetKey) {
      node.alive = true;
      found = true;
    }

  for (int ply = targetPly - 1; ply >= 0; --ply) {
    Layer& here = layers[size_t(ply)];
    const Layer& next = layers[size_t(ply) + 1];

    for (Node& node : here.nodes)
      for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e)
        if (next.nodes[here.edges[e].child].alive) {
          node.alive = true;
          break;
        }
  }

  for (int ply = 0; ply <= targetPly; ++ply) {
    size_t alive = 0;

    for (const Node& node : layers[size_t(ply)].nodes)
      alive += node.alive;

    counts.survivors += alive;
    counts.widest = std::max(counts.widest, alive);
  }
}

} // namespace lineodds
