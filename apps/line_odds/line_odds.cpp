#include "Book.h"
#include "TargetGraph.h"
#include "position.h"
#include "types.h"
#include "uci.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * How often a player who is trying to reach a particular position actually
 * gets to stand in it.
 *
 * The question the book exists to answer is what the field plays, and the
 * question this asks is the consequence of that: a repertoire is a plan for
 * one side only, and every move the other side makes is a chance for the plan
 * to be refused.
 *
 * The target is a whole position, not a sequence, and it pins the placement of
 * every piece on the board. That is enough to say what both sides still have
 * to do -- each has to walk its own pieces onto their target squares, and a
 * side with no move to spare cannot afford one that does not make progress.
 * So the hero needs no written move list: the moves that keep the target
 * reachable are derived from the target itself, in every order the crowd
 * allows, which is what makes the answer a statement about the position rather
 * than about one route into it.
 *
 * The set of positions from which the target is still reachable is enumerated
 * exhaustively rather than beamed, so nothing is dropped for being unlikely,
 * and the answer is computed twice: backwards as a value, and forwards as a
 * flow whose every escape is named and counted. The two must agree, and the
 * named escapes must account for the whole of what does not arrive.
 */

using namespace Stockfish;
using lineodds::Book;
using lineodds::TargetGraph;

namespace {

constexpr size_t NB = NUMBER_OF_BUCKETS;

constexpr const char* BUCKET_NAMES[NB] = {"<1000",     "1000-1199", "1200-1399",
                                          "1400-1599", "1600-1799", "1800-1999",
                                          "2000-2199", "2200-2399", "2400+"};

constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

using Dist = std::array<double, NB>;

/* How the hero picks among the moves that keep the target reachable. */
enum class Policy {
  Field, // as the field of its band plays them, renormalised over those moves
  Best,  // whichever maximises the chance of arriving
  Plan,  // whichever stands earliest in a written move order
};

struct Params {
  std::string otb;
  std::string targetFen;
  std::vector<std::string> plan;
  std::optional<Color> hero;
  Policy policy = Policy::Field;
  bool heroEndings = false;
  int depth = -1;
  size_t beam = 20'000;
  TargetGraph::Limits limits;
};

/* A node's move table, with each move's share of its band gathered across the
 * blob's rows. */
struct Shares {
  bool present = false;
  Dist total{};
  std::vector<std::pair<uint16_t, Dist>> moves;

  const Dist* find(uint16_t move) const {
    for (const auto& [raw, share] : moves)
      if (raw == move)
        return &share;

    return nullptr;
  }
};

Shares sharesAt(const Book& book, uint64_t key) {
  Shares out;
  const auto entries = book.at(key);

  if (entries.empty())
    return out;

  out.present = true;

  for (const auto& e : entries)
    out.total[e.bucket] += e.count;

  for (const auto& e : entries) {
    if (out.moves.empty() || out.moves.back().first != e.move)
      out.moves.emplace_back(e.move, Dist{});

    if (out.total[e.bucket] > 0)
      out.moves.back().second[e.bucket] += e.count / out.total[e.bucket];
  }

  return out;
}

/* The alive child a written move order asks for: the one whose move stands
 * earliest in the list. An entry needs no retiring, because a move already
 * made is one the target no longer has anything to gain from, so it is not an
 * alive child a second time. */
uint32_t planChoice(const TargetGraph& graph, int ply, const TargetGraph::Node& node,
                    const std::vector<std::string>& plan) {
  const auto& here = graph.layer(ply);
  const auto& next = graph.layer(ply + 1);

  uint32_t chosen = UINT32_MAX;
  size_t rank = plan.size();

  for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e) {
    if (!next.nodes[here.edges[e].child].alive)
      continue;

    const std::string name = UCIEngine::move(Move(here.edges[e].move), false);
    const auto it = std::find(plan.begin(), plan.end(), name);

    if (it != plan.end() && size_t(it - plan.begin()) < rank) {
      rank = size_t(it - plan.begin());
      chosen = here.edges[e].child;
    }
  }

  return chosen;
}

/* Everywhere probability can go other than into the target. Between them and
 * the arrival these account for the whole of every band. */
struct Sinks {
  Dist crowdDeviated{}; // the crowd played a move that gives the target up
  Dist ended{};         // games that stopped in the position
  Dist pooled{};        // children too rare for the tree to keep separately
  Dist offBook{};       // a position the book does not hold
  Dist bandDry{};       // the position holds no games of this band at all
  Dist heroStuck{};     // no move left that keeps the target reachable
  Dist heroUnplayed{};  // the field of this band never plays the moves needed

  Dist sum() const {
    Dist out{};

    for (const Dist* d : {&crowdDeviated, &ended, &pooled, &offBook, &bandDry, &heroStuck,
                          &heroUnplayed})
      for (size_t b = 0; b < NB; ++b)
        out[b] += (*d)[b];

    return out;
  }
};

struct Result {
  Dist arrived{};      // from the forward flow
  Dist value{};        // from the backward recurrence; must match
  Dist commitment{};   // share of its band's play the hero's own moves held
  Sinks sinks;
};

/*
 * The chance of arriving, worked out from the target backwards. A crowd node
 * is worth what its children are worth weighted by how often the band plays
 * into them; a hero node is worth the best of its children, or what the band's
 * own preference among them comes to.
 */
std::vector<std::vector<Dist>> valueOf(const TargetGraph& graph, const Book& book,
                                       const Params& params) {
  const int P = graph.plies();
  std::vector<std::vector<Dist>> value(size_t(P) + 1);

  for (int ply = 0; ply <= P; ++ply)
    value[size_t(ply)].assign(graph.layer(ply).nodes.size(), Dist{});

  for (size_t i = 0; i < graph.layer(P).nodes.size(); ++i)
    if (graph.layer(P).nodes[i].alive)
      value[size_t(P)][i].fill(1.0);

  Position pos;
  std::vector<StateInfo> states;

  for (int ply = P - 1; ply >= 0; --ply) {
    const auto& here = graph.layer(ply);
    const auto& next = graph.layer(ply + 1);

    for (uint32_t i = 0; i < here.nodes.size(); ++i) {
      const auto& node = here.nodes[i];
      if (!node.alive)
        continue;

      graph.positionAt(ply, i, pos, states);

      const bool heroToMove = params.hero && pos.side_to_move() == *params.hero;
      Dist& v = value[size_t(ply)][i];

      if (heroToMove && params.policy == Policy::Best) {
        for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e) {
          const uint32_t child = here.edges[e].child;
          if (!next.nodes[child].alive)
            continue;

          for (size_t b = 0; b < NB; ++b)
            v[b] = std::max(v[b], value[size_t(ply) + 1][child][b]);
        }

        continue;
      }

      if (heroToMove && params.policy == Policy::Plan) {
        const uint32_t child = planChoice(graph, ply, node, params.plan);

        if (child != UINT32_MAX)
          v = value[size_t(ply) + 1][child];

        continue;
      }

      const Shares shares = sharesAt(book, node.key);
      if (!shares.present)
        continue;

      /* The hero is not being predicted, so its band's preference is read only
       * among the moves that keep the target reachable, and renormalised. */
      Dist denominator{};

      if (heroToMove) {
        for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e) {
          if (!next.nodes[here.edges[e].child].alive)
            continue;

          if (const Dist* s = shares.find(here.edges[e].move))
            for (size_t b = 0; b < NB; ++b)
              denominator[b] += (*s)[b];
        }

        if (params.heroEndings)
          if (const Dist* s = shares.find(Move::termination().raw()))
            for (size_t b = 0; b < NB; ++b)
              denominator[b] += (*s)[b];
      } else {
        denominator.fill(1.0);
      }

      for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e) {
        const uint32_t child = here.edges[e].child;
        if (!next.nodes[child].alive)
          continue;

        const Dist* s = shares.find(here.edges[e].move);
        if (!s)
          continue;

        for (size_t b = 0; b < NB; ++b)
          if (denominator[b] > 0)
            v[b] += (*s)[b] / denominator[b] * value[size_t(ply) + 1][child][b];
      }
    }
  }

  return value;
}

/*
 * The same walk forwards, carrying probability and naming every place it
 * leaves. The arrival this produces has to match the backward value, and what
 * arrives plus what leaks has to be the whole of each band.
 */
Result flowOf(const TargetGraph& graph, const Book& book, const Params& params,
              const std::vector<std::vector<Dist>>& value) {
  const int P = graph.plies();
  Result out;

  std::vector<std::vector<Dist>> flow(size_t(P) + 1);
  for (int ply = 0; ply <= P; ++ply)
    flow[size_t(ply)].assign(graph.layer(ply).nodes.size(), Dist{});

  flow[0][0].fill(1.0);

  Dist heroMass{};

  Position pos;
  std::vector<StateInfo> states;

  for (int ply = 0; ply < P; ++ply) {
    const auto& here = graph.layer(ply);
    const auto& next = graph.layer(ply + 1);
    auto& out0 = flow[size_t(ply) + 1];

    for (uint32_t i = 0; i < here.nodes.size(); ++i) {
      const auto& node = here.nodes[i];
      const Dist in = flow[size_t(ply)][i];

      if (!node.alive || std::accumulate(in.begin(), in.end(), 0.0) == 0.0)
        continue;

      graph.positionAt(ply, i, pos, states);

      const bool heroToMove = params.hero && pos.side_to_move() == *params.hero;
      const auto isAlive = [&](uint32_t e) { return next.nodes[here.edges[e].child].alive; };
      const uint32_t lastEdge = node.firstEdge + node.edgeCount;

      if (!heroToMove) {
        const Shares shares = sharesAt(book, node.key);

        if (!shares.present) {
          for (size_t b = 0; b < NB; ++b)
            out.sinks.offBook[b] += in[b];
          continue;
        }

        /* A band with no games here has nothing to say about what happens
         * next, and its probability stops rather than vanishing unnamed. */
        for (size_t b = 0; b < NB; ++b)
          if (shares.total[b] == 0)
            out.sinks.bandDry[b] += in[b];

        for (const auto& [raw, share] : shares.moves) {
          const Move move = Move(raw);

          if (move == Move::termination()) {
            for (size_t b = 0; b < NB; ++b)
              out.sinks.ended[b] += in[b] * share[b];
            continue;
          }

          if (move == Move::forcedAggregation()) {
            for (size_t b = 0; b < NB; ++b)
              out.sinks.pooled[b] += in[b] * share[b];
            continue;
          }

          uint32_t child = UINT32_MAX;
          for (uint32_t e = node.firstEdge; e < lastEdge; ++e)
            if (here.edges[e].move == raw && isAlive(e))
              child = here.edges[e].child;

          /* Anything that leads somewhere the target can no longer be reached
           * from is the crowd refusing the line. */
          if (child == UINT32_MAX) {
            for (size_t b = 0; b < NB; ++b)
              out.sinks.crowdDeviated[b] += in[b] * share[b];
            continue;
          }

          for (size_t b = 0; b < NB; ++b)
            out0[child][b] += in[b] * share[b];
        }

        continue;
      }

      bool anyAlive = false;
      for (uint32_t e = node.firstEdge; e < lastEdge; ++e)
        anyAlive |= isAlive(e);

      if (!anyAlive) {
        for (size_t b = 0; b < NB; ++b)
          out.sinks.heroStuck[b] += in[b];
        continue;
      }

      if (params.policy == Policy::Plan) {
        const uint32_t child = planChoice(graph, ply, node, params.plan);

        if (child == UINT32_MAX) {
          for (size_t b = 0; b < NB; ++b)
            out.sinks.heroStuck[b] += in[b];
          continue;
        }

        const Shares planShares = sharesAt(book, node.key);
        const Dist* taken = planShares.find(here.edges[node.firstEdge].move);

        for (uint32_t e = node.firstEdge; e < node.firstEdge + node.edgeCount; ++e)
          if (here.edges[e].child == child)
            taken = planShares.find(here.edges[e].move);

        for (size_t b = 0; b < NB; ++b) {
          out.commitment[b] += in[b] * (taken ? (*taken)[b] : 0.0);
          heroMass[b] += in[b];
          out0[child][b] += in[b];
        }

        continue;
      }

      if (params.policy == Policy::Best) {
        /* The best move order is a different one for each band, so each band
         * is pushed into its own child. */
        for (size_t b = 0; b < NB; ++b) {
          uint32_t bestChild = UINT32_MAX;
          double best = -1.0;

          for (uint32_t e = node.firstEdge; e < lastEdge; ++e) {
            if (!isAlive(e))
              continue;

            const uint32_t child = here.edges[e].child;
            if (value[size_t(ply) + 1][child][b] > best) {
              best = value[size_t(ply) + 1][child][b];
              bestChild = child;
            }
          }

          out0[bestChild][b] += in[b];
        }

        continue;
      }

      const Shares shares = sharesAt(book, node.key);

      if (!shares.present) {
        for (size_t b = 0; b < NB; ++b)
          out.sinks.heroUnplayed[b] += in[b];
        continue;
      }

      /* The hero is being steered rather than predicted, so its band's
       * preference is read only among the moves that keep the target
       * reachable, and renormalised over them. What that renormalisation
       * costs is the commitment reported alongside the answer. */
      Dist denominator{};

      for (uint32_t e = node.firstEdge; e < lastEdge; ++e) {
        if (!isAlive(e))
          continue;

        if (const Dist* s = shares.find(here.edges[e].move))
          for (size_t b = 0; b < NB; ++b)
            denominator[b] += (*s)[b];
      }

      for (size_t b = 0; b < NB; ++b) {
        out.commitment[b] += in[b] * denominator[b];
        heroMass[b] += in[b];
      }

      if (params.heroEndings)
        if (const Dist* s = shares.find(Move::termination().raw()))
          for (size_t b = 0; b < NB; ++b) {
            denominator[b] += (*s)[b];
            out.sinks.ended[b] += in[b] * (*s)[b] / (denominator[b] > 0 ? denominator[b] : 1.0);
          }

      /* A band that never plays a single one of the moves the target needs
       * cannot be steered into it at all. */
      for (size_t b = 0; b < NB; ++b)
        if (denominator[b] == 0)
          out.sinks.heroUnplayed[b] += in[b];

      for (uint32_t e = node.firstEdge; e < lastEdge; ++e) {
        if (!isAlive(e))
          continue;

        const Dist* s = shares.find(here.edges[e].move);
        if (!s)
          continue;

        for (size_t b = 0; b < NB; ++b)
          if (denominator[b] > 0)
            out0[here.edges[e].child][b] += in[b] * (*s)[b] / denominator[b];
      }
    }
  }

  for (size_t i = 0; i < graph.layer(P).nodes.size(); ++i)
    if (graph.layer(P).nodes[i].alive)
      for (size_t b = 0; b < NB; ++b)
        out.arrived[b] += flow[size_t(P)][i][b];

  out.value = value[0].empty() ? Dist{} : value[0][0];

  for (size_t b = 0; b < NB; ++b)
    out.commitment[b] = heroMass[b] > 0 ? out.commitment[b] / heroMass[b] : 0.0;

  return out;
}


/* Games standing in a position, per band, straight out of the book. */
std::optional<Dist> gamesAt(const Book& book, uint64_t zobrist) {
  const auto entries = book.at(zobrist);
  if (entries.empty())
    return std::nullopt;

  Dist games{};
  for (const auto& e : entries)
    games[e.bucket] += e.count;

  return games;
}

/*
 * How concentrated the field is at a half-move: how many positions it takes to
 * account for a given share of the games still in play. With no target there
 * is nothing to prune against, so this walk is beamed.
 */
struct Spread {
  double mass{};
  size_t positions{};
  size_t cover50{};
  size_t cover90{};
  size_t cover99{};
};

std::array<Spread, NB> spreadAt(const Book& book, int depth, size_t beam) {
  struct State {
    uint64_t key{};
    Dist prob{};
  };

  std::unordered_map<uint64_t, State> layer;

  Position pos;
  StateInfo si;
  pos.set(START_FEN, false, &si);

  {
    State& root = layer[pos.key()];
    root.key = pos.key();
    root.prob.fill(1.0);
  }

  std::unordered_map<uint64_t, std::string> fens{{pos.key(), std::string(START_FEN)}};

  for (int ply = 0; ply < depth; ++ply) {
    std::unordered_map<uint64_t, State> next;
    std::unordered_map<uint64_t, std::string> nextFens;

    for (const auto& [key, state] : layer) {
      pos.set(fens.at(key), false, &si);

      const Shares shares = sharesAt(book, key);
      if (!shares.present)
        continue;

      for (const auto& [raw, share] : shares.moves) {
        const Move move = Move(raw);
        if (move == Move::termination() || move == Move::forcedAggregation())
          continue;

        StateInfo st;
        pos.do_move(move, st);

        State& child = next[pos.key()];
        child.key = pos.key();
        for (size_t b = 0; b < NB; ++b)
          child.prob[b] += state.prob[b] * share[b];

        nextFens.try_emplace(pos.key(), pos.fen());
        pos.undo_move(move);
      }
    }

    if (next.size() > beam) {
      std::vector<const State*> states;
      states.reserve(next.size());

      for (const auto& [key, state] : next)
        states.push_back(&state);

      /* A state is ranked by its largest share of any single band, so a line
       * only some bands play is not dropped for the bands that do play it. */
      const auto weight = [](const State* s) {
        return *std::max_element(s->prob.begin(), s->prob.end());
      };

      std::nth_element(states.begin(), states.begin() + beam, states.end(),
                       [&](const State* a, const State* b) { return weight(a) > weight(b); });

      std::unordered_map<uint64_t, State> kept;
      for (size_t i = 0; i < beam; ++i)
        kept.emplace(states[i]->key, *states[i]);

      next = std::move(kept);
    }

    layer = std::move(next);
    fens = std::move(nextFens);
  }

  std::array<Spread, NB> out{};

  for (size_t b = 0; b < NB; ++b) {
    std::vector<double> probs;
    probs.reserve(layer.size());

    for (const auto& [key, state] : layer)
      if (state.prob[b] > 0)
        probs.push_back(state.prob[b]);

    std::sort(probs.begin(), probs.end(), std::greater<>{});

    Spread& s = out[b];
    s.positions = probs.size();
    s.mass = std::accumulate(probs.begin(), probs.end(), 0.0);

    double running = 0;
    for (size_t i = 0; i < probs.size(); ++i) {
      running += probs[i];

      if (!s.cover50 && running >= 0.50 * s.mass)
        s.cover50 = i + 1;
      if (!s.cover90 && running >= 0.90 * s.mass)
        s.cover90 = i + 1;
      if (!s.cover99 && running >= 0.99 * s.mass)
        s.cover99 = i + 1;
    }
  }

  return out;
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

    if (arg == "--target") {
      params.targetFen = next(i);
    } else if (arg == "--hero") {
      const std::string side = next(i);

      if (side == "white")
        params.hero = WHITE;
      else if (side == "black")
        params.hero = BLACK;
      else if (side == "none")
        params.hero.reset();
      else
        throw std::runtime_error("--hero takes white, black or none");
    } else if (arg == "--policy") {
      const std::string policy = next(i);

      if (policy == "field")
        params.policy = Policy::Field;
      else if (policy == "best")
        params.policy = Policy::Best;
      else if (policy == "plan")
        params.policy = Policy::Plan;
      else
        throw std::runtime_error("--policy takes field, best or plan");
    } else if (arg == "--plan") {
      std::string list = next(i);

      for (char& c : list)
        if (c == ',')
          c = ' ';

      std::string move;
      std::istringstream in(list);
      while (in >> move)
        params.plan.push_back(move);
    } else if (arg == "--hero-endings") {
      params.heroEndings = true;
    } else if (arg == "--depth") {
      params.depth = std::stoi(next(i));
    } else if (arg == "--beam") {
      params.beam = std::stoul(next(i));
    } else if (arg == "--max-states") {
      params.limits.maxStates = std::stoul(next(i));
    } else if (arg == "--timeout") {
      params.limits.maxSeconds = std::stod(next(i));
    } else if (params.otb.empty()) {
      params.otb = arg;
    } else {
      throw std::runtime_error("unexpected argument: " + arg);
    }
  }

  if (params.otb.empty())
    throw std::runtime_error("an .otb is required");

  if (params.targetFen.empty() && params.depth < 0)
    throw std::runtime_error("give a --target FEN, a --depth, or both");

  if (!params.plan.empty() && !params.hero)
    throw std::runtime_error("--plan restricts the hero's moves, so it needs a --hero");

  if (params.policy == Policy::Plan && params.plan.empty())
    throw std::runtime_error("--policy plan needs a --plan to follow");

  return params;
}

void reportSpread(const std::array<Spread, NB>& spread, int ply) {
  std::cout << "How the field is spread at half-move " << ply << "\n\n";
  std::cout << std::left << std::setw(12) << "Band" << std::right << std::setw(12) << "Live mass"
            << std::setw(14) << "Positions" << std::setw(12) << "Half in" << std::setw(12)
            << "90% in" << std::setw(12) << "99% in" << '\n';
  std::cout << std::string(74, '-') << '\n';

  for (size_t b = 0; b < NB; ++b) {
    const Spread& s = spread[b];

    std::cout << std::left << std::setw(12) << BUCKET_NAMES[b] << std::right << std::setw(11)
              << std::fixed << std::setprecision(1) << 100.0 * s.mass << '%' << std::setw(14)
              << s.positions << std::setw(12) << s.cover50 << std::setw(12) << s.cover90
              << std::setw(12) << s.cover99 << '\n';
  }
}

void report(const Result& result, const Dist& wild, bool haveWild, bool haveHero) {
  std::cout << std::left << std::setw(12) << "Band" << std::right << std::setw(14) << "Steered"
            << std::setw(12) << "One in" << std::setw(14) << "In the wild" << std::setw(12)
            << "One in" << std::setw(14) << "Commitment" << '\n';
  std::cout << std::string(78, '-') << '\n';

  for (size_t b = 0; b < NB; ++b) {
    std::cout << std::left << std::setw(12) << BUCKET_NAMES[b] << std::right << std::setw(13)
              << std::fixed << std::setprecision(4) << 100.0 * result.arrived[b] << '%';

    if (result.arrived[b] > 0)
      std::cout << std::setw(12) << std::setprecision(0) << 1.0 / result.arrived[b];
    else
      std::cout << std::setw(12) << "-";

    if (haveWild) {
      std::cout << std::setw(13) << std::setprecision(4) << 100.0 * wild[b] << '%';

      if (wild[b] > 0)
        std::cout << std::setw(12) << std::setprecision(0) << 1.0 / wild[b];
      else
        std::cout << std::setw(12) << "-";
    } else {
      std::cout << std::setw(14) << "-" << std::setw(12) << "-";
    }

    if (haveHero)
      std::cout << std::setw(13) << std::setprecision(1) << 100.0 * result.commitment[b] << '%';

    std::cout << '\n';
  }

  const Sinks& s = result.sinks;

  std::cout << "\nWhere the rest of the probability went\n\n";
  std::cout << std::left << std::setw(12) << "Band" << std::right << std::setw(12) << "Refused"
            << std::setw(10) << "Ended" << std::setw(10) << "Pooled" << std::setw(11) << "Off book"
            << std::setw(11) << "Band dry" << std::setw(10) << "Stuck" << std::setw(12)
            << "Unplayed" << std::setw(10) << "Total" << '\n';
  std::cout << std::string(98, '-') << '\n';

  const Dist leaked = s.sum();

  for (size_t b = 0; b < NB; ++b)
    std::cout << std::left << std::setw(12) << BUCKET_NAMES[b] << std::right << std::setw(11)
              << std::fixed << std::setprecision(2) << 100.0 * s.crowdDeviated[b] << '%'
              << std::setw(9) << 100.0 * s.ended[b] << '%' << std::setw(9) << 100.0 * s.pooled[b]
              << '%' << std::setw(10) << 100.0 * s.offBook[b] << '%' << std::setw(10)
              << 100.0 * s.bandDry[b] << '%' << std::setw(9) << 100.0 * s.heroStuck[b] << '%'
              << std::setw(11) << 100.0 * s.heroUnplayed[b] << '%' << std::setw(9)
              << 100.0 * (leaked[b] + result.arrived[b]) << '%' << '\n';
}

/* The two answers are computed by different routes over the same graph, and a
 * disagreement between them, or a band that does not add up, is a bug rather
 * than a result. */
void check(const Result& result) {
  const Dist leaked = result.sinks.sum();

  for (size_t b = 0; b < NB; ++b) {
    const double total = result.arrived[b] + leaked[b];

    if (std::abs(total - 1.0) > 1e-6)
      std::cerr << "warning: band " << BUCKET_NAMES[b] << " accounts for " << 100.0 * total
                << "% of its probability, not 100%\n";

    if (std::abs(result.arrived[b] - result.value[b]) > 1e-9)
      std::cerr << "warning: band " << BUCKET_NAMES[b] << " arrives at " << result.arrived[b]
                << " going forwards but " << result.value[b] << " going backwards\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: line_odds <data.otb> [--target \"<fen>\"] [--depth N]\n"
              << "                 [--hero white|black|none] [--policy field|best|plan]\n"
              << "                 [--plan \"e7e5,g8f6,...\"] [--hero-endings]\n"
              << "                 [--beam N] [--max-states N] [--timeout SECONDS]\n";
    return 1;
  }

  try {
    Bitboards::init();
    Position::init();

    const Params params = parseArgs(argc, argv);
    const Book book(params.otb);

    StateInfo st;
    std::optional<Position> target;

    if (!params.targetFen.empty()) {
      target.emplace();
      target->set(params.targetFen, false, &st);
    }

    const int targetPly = target ? target->game_ply() : params.depth;

    if (target && params.depth >= 0 && params.depth != targetPly)
      throw std::runtime_error("--depth disagrees with the half-move the --target FEN stands at");

    if (target) {
      std::cout << "Target at half-move " << targetPly << ", hero "
                << (params.hero ? (*params.hero == WHITE ? "white" : "black") : "none")
                << ", policy "
                << (params.policy == Policy::Best    ? "best"
                    : params.policy == Policy::Plan  ? "plan"
                                                     : "field")
                << "\n\n";

      Position start;
      StateInfo startSt;
      start.set(START_FEN, false, &startSt);

      const auto wild = gamesAt(book, target->key());
      const auto root = gamesAt(book, start.key());

      Dist wildShare{};
      if (wild && root)
        for (size_t b = 0; b < NB; ++b)
          wildShare[b] = (*root)[b] > 0 ? (*wild)[b] / (*root)[b] : 0.0;

      std::cout << (wild ? "Games standing in the target: " +
                               std::to_string(static_cast<uint64_t>(
                                   std::accumulate(wild->begin(), wild->end(), 0.0)))
                         : std::string("The book does not hold the target position"))
                << '\n';

      const TargetGraph graph(book, *target, targetPly, params.hero, params.plan, params.limits);
      const auto& census = graph.census();

      std::cout << "Positions from which the target is still reachable: " << census.survivors
                << " of " << census.enumerated << " enumerated, widest layer " << census.widest
                << " (" << census.widestRaw << " before the backward pass), " << std::fixed
                << std::setprecision(1) << census.seconds << "s\n\n";

      if (!graph.targetFound()) {
        std::cout << "No line reaches the target at half-move " << targetPly
                  << ". Check the move number in the FEN"
                  << (params.plan.empty() ? "." : ", and whether --plan holds every move needed.")
                  << '\n';
        return 0;
      }

      const auto value = valueOf(graph, book, params);
      const Result result = flowOf(graph, book, params, value);

      report(result, wildShare, wild.has_value(), params.hero.has_value());
      check(result);
      std::cout << '\n';
    }

    if (params.depth >= 0 && !target)
      reportSpread(spreadAt(book, params.depth, params.beam), params.depth);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
