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

#ifndef UCI_H_INCLUDED
#define UCI_H_INCLUDED

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

// #include "engine.h"
#include "misc.h"
#include "search.h"

namespace Stockfish {

class Position;
class Move;
class Score;
enum Square : int8_t;
using Value = int;

class UCIEngine {
   public:
    static std::string square(Square s);
    static std::string move(Move m, bool chess960);
    static std::string to_lower(std::string str);
    static Move        to_move(const Position& pos, std::string str);
};

void playGameString(const std::string& gameStr);

}  // namespace Stockfish

#endif  // #ifndef UCI_H_INCLUDED
