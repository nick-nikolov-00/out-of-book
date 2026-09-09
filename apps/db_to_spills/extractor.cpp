#include "extractor.h"

#include <charconv>
#include <cmath>
#include <iostream>

bool extractMetadata(std::string_view game, GameMetadata &meta) {
    int whiteElo = -1;
    int blackElo = -1;
    bool haveWhiteElo = false;
    bool haveBlackElo = false;
    bool haveResult = false;

    auto parseInt = [](std::string_view s, int &out) {
        auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        assert(r.ec == std::errc{});
    };

    auto quotedValue = [](std::string_view line) -> std::string_view {
        size_t a = line.find('"');
        size_t b = line.rfind('"');
        return line.substr(a + 1, b - a - 1);
    };

    size_t pos = 0;

    while (pos < game.size()) {
        size_t end = game.find('\n', pos);
        if (end == std::string_view::npos)
            end = game.size();

        std::string_view line = game.substr(pos, end - pos);

        // Strip trailing '\r'
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        // End of headers.
        if (line.empty()) {
            meta.whiteElo = whiteElo;
            meta.blackElo = blackElo;
            meta.moves = game.substr(end + 1);

            // std::cout << haveWhiteElo << " " << haveBlackElo << " " << haveResult << "\n";
            return haveWhiteElo && haveBlackElo && haveResult;
        }

        if (line.starts_with("[WhiteTitle ")) {
            if (quotedValue(line) == "BOT") {
                // std::cout << "bot\n";
                return false;
            }
        } else if (line.starts_with("[BlackTitle ")) {
            if (quotedValue(line) == "BOT") {
                // std::cout << "bot v2\n";
                return false;
            }
        } else if (line.starts_with("[WhiteElo ")) {
            parseInt(quotedValue(line), whiteElo);
            haveWhiteElo = true;

            if (haveBlackElo && std::abs(whiteElo - blackElo) > 200) {
                std::cout<<">200\n";
                return false;
            }
        } else if (line.starts_with("[BlackElo ")) {
            parseInt(quotedValue(line), blackElo);
            haveBlackElo = true;

            if (haveWhiteElo && std::abs(whiteElo - blackElo) > 200) {
                // std::cout<<whiteElo<<" "<<blackElo<<" >200 v2\n";
                return false;
            }
        } else if (line.starts_with("[WhiteRatingDiff ")) {
            int diff;
            parseInt(quotedValue(line).substr(1), diff);

            if (std::abs(diff) >= 40) {
                // std::cout<<">40\n";
                return false;
            }
        } else if (line.starts_with("[BlackRatingDiff ")) {
            int diff;
            parseInt(quotedValue(line).substr(1), diff);

            if (std::abs(diff) >= 40) {
                // std::cout<<line<<" "<<quotedValue(line)<<" "<<diff<<"\n";
                // std::cout<<">40 v2\n";
                return false;
            }
        } else if (line.starts_with("[TimeControl ")) {
            auto tc = quotedValue(line);

            size_t plus = tc.find('+');
            if (plus == std::string_view::npos) {
                // std::cout<<"tc not found\n";
                return false;
            }

            int base;
            parseInt(tc.substr(0, plus), base);

            if (base < 180) { // at least 3 minutes
                // std::cout<<"<3m\n";
                return false;
            }
        } else if (line.starts_with("[Result ")) {
            auto r = quotedValue(line);

            if (r == "1-0")
                meta.result = Stockfish::GameResult::WhiteWin;
            else if (r == "0-1")
                meta.result = Stockfish::GameResult::BlackWin;
            else if (r == "1/2-1/2")
                meta.result = Stockfish::GameResult::Draw;
            else {
                // std::cout<<"unk result\n";
                return false;
            }

            haveResult = true;
        }

        pos = end + 1;
    }

    // std::cout<<"catchall\n";
    return false;
}
