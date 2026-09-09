#include "SelfCheck.h"

#include "Analysis.h"

#include "position.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Stockfish;

namespace {

constexpr std::string_view START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void checkNode(const OtbStore& store, const DubiousMoves& dubious, Position& pos, int depth,
               size_t& budget, CheckResult& result) {
  if (budget == 0 || depth < 0)
    return;

  const auto index = store.find(pos.key());

  if (!index)
    return;

  --budget;

  OtbStore::Node node;
  store.readNode(*index, node);

  OteEntry ote{};
  store.readEvals(*index, ote);

  const Analysis analysis = analyse(store, pos, node, dubious);

  for (size_t b = 0; b < NUM_BUCKETS; ++b) {
    if (ote.moveMax[b] == 0)
      continue;

    /* Demoted on purpose, so the disagreement here is the feature rather than
     * evidence of a file built by a different rule. */
    if (dubious.reasonFor(pos.key(), ote.moveMax[b]))
      continue;

    ++result.checked;

    if (chosenMove(analysis, b) == ote.moveMax[b])
      ++result.agreed;
  }

  if (depth == 0)
    return;

  /* Follow the most played continuations, so the sample stays in the part of
   * the tree that is dense enough for the ranking to be interesting. */
  std::vector<size_t> order;

  for (size_t i = 0; i < analysis.rows.size(); ++i)
    if (analysis.playable[i])
      order.push_back(i);

  const size_t mid = NUM_BUCKETS / 2;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t c) {
    return analysis.rows[a].stats[mid].count > analysis.rows[c].stats[mid].count;
  });

  if (order.size() > 2)
    order.resize(2);

  for (size_t i : order) {
    StateInfo st;
    const Move move(analysis.rows[i].raw);

    pos.do_move(move, st);
    checkNode(store, dubious, pos, depth - 1, budget, result);
    pos.undo_move(move);
  }
}

} // namespace

CheckResult selfCheck(const OtbStore& store, const DubiousMoves& dubious) {
  Position pos;
  StateInfo st;
  pos.set(std::string(START_FEN), false, &st);

  size_t budget = 300;
  CheckResult result;
  checkNode(store, dubious, pos, 12, budget, result);

  return result;
}
