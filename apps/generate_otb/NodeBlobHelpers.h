#pragma once

#include "common.h"
#include "position.h"

void insertRecord(NodeBlobHeader& node, const SpillRecord& record) {
  assert(node.nEntries != NodeBlobHeader::MAX_CHILDREN && "too many children");

  node.entries[node.nEntries].move = record.move;
  node.entries[node.nEntries].bucket = record.bucket;
  node.entries[node.nEntries].count = record.count;
  node.entries[node.nEntries].white_wins = record.white_wins;
  node.entries[node.nEntries].draws = record.draws;
  node.nEntries++;
}