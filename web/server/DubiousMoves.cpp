#include "DubiousMoves.h"

#include "San.h"

#include "position.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace Stockfish;

namespace {

constexpr std::string_view START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

std::string_view trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");

  if (begin == std::string_view::npos)
    return {};

  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::vector<std::string> tokensOf(std::string_view text) {
  std::istringstream stream{std::string(text)};
  std::vector<std::string> tokens;
  std::string token;

  while (stream >> token)
    tokens.push_back(token);

  return tokens;
}

} // namespace

DubiousMoves DubiousMoves::load(const std::string& path) {
  std::ifstream file(path);

  if (!file)
    throw std::runtime_error("cannot open the dubious-move list: " + path);

  DubiousMoves out;
  std::string line;
  size_t lineNumber = 0;

  const auto reject = [&](const std::string& what) {
    throw std::runtime_error(path + ":" + std::to_string(lineNumber) + ": " + what);
  };

  while (std::getline(file, line)) {
    ++lineNumber;

    const std::string_view text = trim(line);

    /* A '#' only opens a comment at the start of a line, so a reason is free to
     * write a move with a check in it. */
    if (text.empty() || text.front() == '#')
      continue;

    const auto firstBar = text.find('|');
    const auto secondBar = firstBar == std::string_view::npos
                             ? std::string_view::npos
                             : text.find('|', firstBar + 1);

    if (secondBar == std::string_view::npos)
      reject("expected three fields separated by '|': the line, the move, the reason");

    const std::string_view reason = trim(text.substr(secondBar + 1));

    if (reason.empty())
      reject("the reason is empty, and it is what the move table shows");

    /*
     * Replay the line to reach the position. The states are held in a deque
     * because a Position keeps a pointer into the StateInfo it was given, so
     * every one of them has to outlive the walk and stay where it was put.
     */
    Position pos;
    std::deque<StateInfo> states;

    states.emplace_back();
    pos.set(std::string(START_FEN), false, &states.back());

    for (const std::string& san : tokensOf(text.substr(0, firstBar))) {
      std::string problem;
      const Move move = sanToMove(pos, san, &problem);

      if (move == Move::none())
        reject("in the line, " + problem);

      states.emplace_back();
      pos.do_move(move, states.back());
    }

    const std::vector<std::string> named = tokensOf(text.substr(firstBar + 1, secondBar - firstBar - 1));

    if (named.size() != 1)
      reject("the middle field must name exactly one move to flag");

    std::string problem;
    const Move flagged = sanToMove(pos, named.front(), &problem);

    if (flagged == Move::none())
      reject(problem);

    std::vector<DubiousMove>& here = out.byPosition_[pos.key()];

    if (std::any_of(here.begin(), here.end(),
                    [&](const DubiousMove& d) { return d.move == flagged.raw(); }))
      reject("this move is already flagged in this position");

    here.push_back({flagged.raw(), std::string(reason)});
    ++out.count_;
  }

  return out;
}

std::span<const DubiousMove> DubiousMoves::at(uint64_t zobrist) const {
  const auto it = byPosition_.find(zobrist);

  return it == byPosition_.end() ? std::span<const DubiousMove>{} : it->second;
}

const std::string* DubiousMoves::reasonFor(uint64_t zobrist, uint16_t move) const {
  for (const DubiousMove& dubious : at(zobrist))
    if (dubious.move == move)
      return &dubious.reason;

  return nullptr;
}
