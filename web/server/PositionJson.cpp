#include "PositionJson.h"

#include "Analysis.h"
#include "Json.h"

#include "position.h"
#include "uci.h"

#include <array>
#include <optional>
#include <vector>

using namespace Stockfish;

namespace {

/* Black's wins are not sent: they are count - whiteWins - draws, exactly, for
 * every record, since the three outcomes partition the games. The client
 * subtracts. On a busy opening node that is a tenth of the payload. */
void writeBucketStats(Json& json, const BucketStat& stat) {
  json.beginObject();
  json.field("count", stat.count);
  json.field("whiteWins", stat.whiteWins);
  json.field("draws", stat.draws);
  json.endObject();
}

/*
 * Evaluations are stored per bucket from both perspectives. evalW is the score
 * if White plays the expectimax-optimal move and Black follows the empirical
 * distribution; evalB is the mirror image. Both are in white-score units, so
 * the side to move picks the maximum of evalW or the minimum of evalB.
 *
 * A child's eval is not what the search ranked it by — that is the "pick"
 * field, and the gap between the two is the shrink.
 */
void writeEvals(Json& json, const OteEntry& ote) {
  json.beginObject();

  json.key("w");
  json.beginArray();
  for (size_t b = 0; b < NUM_BUCKETS; ++b)
    json.value(static_cast<double>(ote.evalW[b]));
  json.endArray();

  json.key("b");
  json.beginArray();
  for (size_t b = 0; b < NUM_BUCKETS; ++b)
    json.value(static_cast<double>(ote.evalB[b]));
  json.endArray();

  json.endObject();
}

} // namespace

std::string describePosition(const OtbStore& store, const DubiousMoves& dubious,
                             const std::string& fenStr, bool* foundOut) {
  Position pos;
  StateInfo st;
  pos.set(fenStr, false, &st);

  const uint64_t zobrist = pos.key();
  const bool whiteToMove = pos.side_to_move() == WHITE;

  Json json;
  json.beginObject();

  json.field("fen", std::string_view(fenStr));
  json.field("sideToMove", std::string_view(whiteToMove ? "w" : "b"));
  json.field("hasEvals", store.hasEvals());

  const auto index = store.find(zobrist);

  if (!index) {
    if (foundOut)
      *foundOut = false;

    json.field("found", false);
    json.endObject();
    return json.take();
  }

  if (foundOut)
    *foundOut = true;

  json.field("found", true);

  OtbStore::Node node;
  store.readNode(*index, node);

  std::optional<OteEntry> ote;

  if (store.hasEvals()) {
    OteEntry entry{};
    store.readEvals(*index, entry);
    ote = entry;
  }

  const Analysis analysis = analyse(store, pos, node, dubious);
  const std::vector<MoveRow>& rows = analysis.rows;

  /* Position totals include the termination and aggregate rows, matching how
   * a bucket's game count is reported everywhere else. */
  std::array<BucketStat, NUM_BUCKETS> totals{};

  for (const MoveRow& row : rows) {
    for (size_t b = 0; b < NUM_BUCKETS; ++b) {
      totals[b].count += row.stats[b].count;
      totals[b].whiteWins += row.stats[b].whiteWins;
      totals[b].draws += row.stats[b].draws;
    }
  }

  json.key("totals");
  json.beginArray();
  for (const BucketStat& stat : totals)
    writeBucketStats(json, stat);
  json.endArray();

  if (ote) {
    json.key("eval");
    writeEvals(json, *ote);

    json.key("bestMove");
    json.beginArray();
    for (size_t b = 0; b < NUM_BUCKETS; ++b) {
      uint16_t best = ote->moveMax[b];

      /* The evaluator's recorded pick, unless it is one of the moves the book
       * will not recommend — in which case what the book plays is whatever the
       * flagged move was demoted below. */
      if (best != 0 && dubious.reasonFor(zobrist, best))
        best = chosenMove(analysis, b);

      if (best == 0 || kindOf(best) != RowKind::Move)
        json.null();
      else
        json.value(std::string_view(UCIEngine::move(Move(best), false)));
    }
    json.endArray();
  } else {
    json.nullField("eval");
    json.nullField("bestMove");
  }

  json.key("moves");
  json.beginArray();

  for (size_t i = 0; i < rows.size(); ++i) {
    const MoveRow& row = rows[i];
    const RowKind kind = kindOf(row.raw);

    json.beginObject();
    json.field("kind", std::string_view(kindName(kind)));

    const Move move(row.raw);

    if (analysis.playable[i])
      json.field("uci", std::string_view(UCIEngine::move(move, false)));
    else
      json.nullField("uci");

    json.key("stats");
    json.beginArray();
    for (const BucketStat& stat : row.stats) {
      if (stat.empty())
        json.null();
      else
        writeBucketStats(json, stat);
    }
    json.endArray();

    if (analysis.children[i]) {
      json.key("child");
      writeEvals(json, analysis.children[i]->ote);
    } else {
      json.nullField("child");
    }

    /* Why the book will not recommend this move, and null for the moves it is
     * willing to. The row keeps every number it had: the flag says what to do
     * with the move, not what happened when it was played. */
    if (analysis.dubious[i].empty())
      json.nullField("dubious");
    else
      json.field("dubious", analysis.dubious[i]);

    /* Where the search puts this move and what it scored it. Non-null exactly
     * where the move has games in the bucket. */
    json.key("pick");
    json.beginArray();

    for (const Analysis::Pick& pick : analysis.picks[i]) {
      if (!pick.present) {
        json.null();
        continue;
      }

      json.beginObject();
      /* The order to show the moves in — sorting on `score` does not reproduce
       * it, which is the whole reason this is sent. */
      json.field("rank", pick.rank);
      json.field("score", pick.candidate.score);
      /* False when too few games stand behind the score for it to mean much;
       * such a move is also the one whose place in the order the score will not
       * explain, so this is what marks it in the table. */
      json.field("solid", pick.candidate.solid());
      json.endObject();
    }

    json.endArray();

    json.endObject();
  }

  json.endArray();
  json.endObject();

  return json.take();
}
