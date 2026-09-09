/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "uci.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "memory.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "types.h"

namespace Stockfish {

std::string UCIEngine::square(Square s) {
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string UCIEngine::move(Move m, bool chess960) {
    if (m == Move::termination())
      return "(TERM)";

  if (m == Move::forcedAggregation())
    return "(AGG)";

    if (m == Move::none())
        return "(none)";

    if (m == Move::null())
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    if (m.type_of() == CASTLING && !chess960)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

    std::string move = square(from) + square(to);

    if (m.type_of() == PROMOTION)
        move += " pnbrqk"[m.promotion_type()];

    return move;
}

std::string UCIEngine::to_lower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](auto c) { return std::tolower(c); });

    return str;
}

Move UCIEngine::to_move(const Position& pos, std::string str) {
    str = to_lower(str);

    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m, pos.is_chess960()))
            return m;

    return Move::none();
}

void playGameString(const std::string& gameStr) {
    using namespace Stockfish;

    Position  pos;
    StateInfo st;
    pos.set(StartFEN, false, &st);

    int       currState = 0;
    StateInfo states[100];

    std::istringstream iss(gameStr);
    std::string        token;

    while (iss >> token)
    {
        if (token.find('.') != std::string::npos)
            continue;
        if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*")
            break;

        Move move = getMove(token, pos);
        if (!move.is_ok())
        {
            std::cerr << "Invalid move: " << token << std::endl;
            break;
        }

        std::cout << UCIEngine::move(move, pos.is_chess960()) << std::endl;

        pos.do_move(move, states[currState++]);
    }
}

}  // namespace Stockfish
