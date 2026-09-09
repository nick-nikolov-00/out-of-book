#pragma once

#include "DubiousMoves.h"
#include "OtbStore.h"

#include <string>

/*
 * The /api/position response body for one FEN.
 *
 * `foundOut`, when given, says whether the position was in the tree — which is
 * a 200 either way, so the handler cannot infer it from the status.
 */
std::string describePosition(const OtbStore& store, const DubiousMoves& dubious,
                             const std::string& fenStr, bool* foundOut = nullptr);
