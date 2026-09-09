#pragma once

#include "DubiousMoves.h"
#include "OtbStore.h"

#include <cstddef>

/*
 * Walks a few hundred real positions and checks that the ranking here picks the
 * move the evaluator recorded in the .ote.
 *
 * The .ote does not record the constants that built it, and they live in a
 * header this binary compiles against, so a server pointed at a file from an
 * older build would otherwise explain every move list with a rule that file
 * never used — quietly, and visible only as "the best move is not the
 * best-scoring one". A mismatch is worth failing loudly at startup over. The
 * fix is always to rebuild one side or regenerate the other, never to reach
 * for a knob: the rule is one thing, defined once.
 *
 * A bucket whose recorded move is one the book will not recommend is left out
 * of the count. There the two are *meant* to differ — the flag exists to
 * override the rule's answer — so counting it would report an editorial
 * decision as a stale file.
 */
struct CheckResult {
  size_t checked = 0;
  size_t agreed = 0;
};

CheckResult selfCheck(const OtbStore& store, const DubiousMoves& dubious);
