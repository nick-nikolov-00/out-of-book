#pragma once
#include <cctype>
#include <string_view>

#include "types.h"

struct GameMetadata
{
    int whiteElo;
    int blackElo;
    Stockfish::GameResult result;
    std::string_view moves;   // starts immediately after the blank line
};

bool extractMetadata(std::string_view game, GameMetadata& meta);

template <typename F>
void forEachMove(std::string_view moves, F&& callback)
{
    size_t i = 0;

    while (i < moves.size())
    {
        // Skip whitespace.
        while (i < moves.size() && std::isspace(static_cast<unsigned char>(moves[i])))
            ++i;

        if (i == moves.size())
            break;

        // Skip comments: { ... }
        if (moves[i] == '{')
        {
            size_t end = moves.find('}', i);
            if (end == std::string_view::npos)
                break;
            i = end + 1;
            continue;
        }

        // Skip move numbers: 12. or 12...
        if (std::isdigit(static_cast<unsigned char>(moves[i])))
        {
            size_t j = i;
            while (j < moves.size() && std::isdigit(static_cast<unsigned char>(moves[j])))
                ++j;

            if (j < moves.size() && moves[j] == '.')
            {
                while (j < moves.size() && moves[j] == '.')
                    ++j;

                i = j;
                continue;
            }

            // Game result
            if (moves.compare(i, 3, "1-0") == 0 ||
                moves.compare(i, 3, "0-1") == 0 ||
                moves.compare(i, 7, "1/2-1/2") == 0)
                break;
        }

        if (moves[i] == '*')
            break;

        // Extract SAN move.
        size_t start = i;
        while (i < moves.size() &&
               !std::isspace(static_cast<unsigned char>(moves[i])) &&
               moves[i] != '{')
        {
            ++i;
        }

        if (!callback(moves.substr(start, i - start))) {
            return;
        }
    }
}