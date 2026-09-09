#pragma once

#include "movegen.h"
#include "position.h"
#include "types.h"
#include "uci.h"

#include <iostream>

template<bool Root>
uint64_t perft(Stockfish::Position &pos, Stockfish::Depth depth) {
    Stockfish::StateInfo st;

    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto &m: Stockfish::MoveList<Stockfish::LEGAL>(pos)) {
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else {
            pos.do_move(m, st);
            cnt = leaf ? Stockfish::MoveList<Stockfish::LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            std::cout << Stockfish::UCIEngine::move(m, pos.is_chess960()) << ": " << cnt << std::endl;
    }
    return nodes;
}
