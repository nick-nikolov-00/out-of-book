#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*
 * A FEN the engine will accept: the position parser wants a position a game
 * could have reached, written in the canonical form, and this is where that is
 * established. A FEN that does not describe one is refused with a sentence
 * saying what is wrong with it.
 */
namespace fen {

constexpr size_t MAX_LENGTH = 200;

namespace detail {

enum Type { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, TYPE_NB };

/* The type of a lowercased piece letter, or TYPE_NB if it is not one. */
inline Type typeOf(char lower) {
  switch (lower) {
  case 'p':
    return PAWN;
  case 'n':
    return KNIGHT;
  case 'b':
    return BISHOP;
  case 'r':
    return ROOK;
  case 'q':
    return QUEEN;
  case 'k':
    return KING;
  default:
    return TYPE_NB;
  }
}

/*
 * The board as a grid in FEN reading order: row 0 is rank 8 and column 0 is
 * file a, so board[7][4] is e1 and board[0][7] is h8. '\0' is an empty square.
 *
 * The squares are kept, not just tallied, because the castling check has to
 * name the two specific squares a right depends on.
 */
struct Board {
  char squares[8][8]{};
  int count[2][TYPE_NB]{}; /* [0] is White, [1] is Black */

  char at(int row, int col) const {
    return squares[row][col];
  }
};

} // namespace detail

inline std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> fields;
  size_t i = 0;

  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;

    const size_t start = i;

    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
      ++i;

    if (i > start)
      fields.push_back(s.substr(start, i - start));
  }

  return fields;
}

/* Returns a human-readable reason, or nullopt when the FEN is usable. */
inline std::optional<std::string> validate(std::string_view fenStr) {
  if (fenStr.empty())
    return "FEN is empty";

  if (fenStr.size() > MAX_LENGTH)
    return "FEN is too long";

  const auto fields = split(fenStr);

  if (fields.size() < 4)
    return "FEN must have at least 4 fields";

  if (fields.size() > 6)
    return "FEN has more than 6 fields";

  std::string canonical;

  for (const std::string_view field : fields) {
    if (!canonical.empty())
      canonical += ' ';

    canonical += field;
  }

  if (canonical != fenStr)
    return "FEN fields must be separated by single spaces, with none at either end";

  // --- board ---
  detail::Board board;
  int row = 0;
  int col = 0;

  for (const char c : fields[0]) {
    if (c == '/') {
      if (col != 8)
        return "rank " + std::to_string(8 - row) + " does not describe 8 squares";

      ++row;
      col = 0;

      if (row > 7)
        return "board must describe 8 ranks of 8 squares";

      continue;
    }

    if (c >= '1' && c <= '8') {
      col += c - '0';
      continue;
    }

    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const detail::Type type = detail::typeOf(lower);

    if (type == detail::TYPE_NB)
      return std::string("invalid piece character '") + c + "'";

    /* The digit branch above advances `col` without a bound of its own, so the
     * row is checked here, before the write. */
    if (col > 7)
      return "rank " + std::to_string(8 - row) + " describes more than 8 squares";

    /* Pawns on the back ranks are not reachable and break promotion logic. */
    if (type == detail::PAWN && (row == 0 || row == 7))
      return "pawn on the first or last rank";

    board.squares[row][col] = c;
    ++board.count[c == lower ? 1 : 0][type];
    ++col;
  }

  if (row != 7 || col != 8)
    return "board must describe 8 ranks of 8 squares";

  // --- material ---
  if (board.count[0][detail::KING] != 1 || board.count[1][detail::KING] != 1)
    return "each side must have exactly one king";

  int kingRow[2] = {0, 0};
  int kingCol[2] = {0, 0};

  for (int r = 0; r < 8; ++r) {
    for (int f = 0; f < 8; ++f) {
      if (board.squares[r][f] == 'K') {
        kingRow[0] = r;
        kingCol[0] = f;
      } else if (board.squares[r][f] == 'k') {
        kingRow[1] = r;
        kingCol[1] = f;
      }
    }
  }

  /* No legal game reaches a position with the kings touching. */
  if (std::abs(kingRow[0] - kingRow[1]) <= 1 && std::abs(kingCol[0] - kingCol[1]) <= 1)
    return "the kings are on adjacent squares";

  /*
   * Promotion accounting. A side begins with eight pawns and can gain a piece
   * only by promoting one, so every piece beyond the opening complement costs
   * a pawn and pawns plus promotions cannot exceed eight. It also caps a side
   * at sixteen pieces, which is why no separate check says so.
   */
  for (int c = 0; c < 2; ++c) {
    const auto beyond = [&](detail::Type type, int initial) {
      return std::max(0, board.count[c][type] - initial);
    };

    const int promotions = beyond(detail::QUEEN, 1) + beyond(detail::ROOK, 2) +
                           beyond(detail::BISHOP, 2) + beyond(detail::KNIGHT, 2);

    if (board.count[c][detail::PAWN] + promotions > 8)
      return std::string(c == 0 ? "White" : "Black") +
             " has more pieces than promotions can account for";
  }

  // --- side to move ---
  if (fields[1] != "w" && fields[1] != "b")
    return "side to move must be 'w' or 'b'";

  // --- castling rights ---
  /*
   * Cross-checked against the board rather than only against the alphabet.
   *
   * Demanding the home squares is not an extra restriction layered on top. A
   * castling right is lost the moment either the king or its rook moves, so in
   * standard chess — and Chess960 rights are rejected here — these are exactly
   * the positions in which the right can still be held.
   */
  if (fields[2] != "-") {
    if (fields[2].size() > 4)
      return "invalid castling rights";

    for (const char c : fields[2]) {
      const bool white = std::isupper(static_cast<unsigned char>(c)) != 0;
      const char side = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

      if (side != 'K' && side != 'Q')
        return "invalid castling rights (Chess960 rights are not supported)";

      /* Row 7 is rank 1 and row 0 is rank 8; column 4 is the e file, and the
       * rook stands on h for a kingside right and a for a queenside one. */
      const int backRow = white ? 7 : 0;
      const int rookCol = side == 'K' ? 7 : 0;

      if (board.at(backRow, 4) != (white ? 'K' : 'k'))
        return std::string("castling right '") + c + "' with no king on its home square";

      if (board.at(backRow, rookCol) != (white ? 'R' : 'r'))
        return std::string("castling right '") + c + "' with no rook on its home square";
    }
  }

  // --- en passant square ---
  if (fields[3] != "-") {
    if (fields[3].size() != 2 || fields[3][0] < 'a' || fields[3][0] > 'h' ||
        (fields[3][1] != '3' && fields[3][1] != '6'))
      return "invalid en passant square";
  }

  // --- clocks (optional) ---
  for (size_t i = 4; i < fields.size() && i < 6; ++i) {
    for (const char c : fields[i]) {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return "move counters must be numeric";
    }

    if (fields[i].size() > 6)
      return "move counters are out of range";
  }

  if (fields.size() >= 6 && fields[5].find_first_not_of('0') == std::string_view::npos)
    return "fullmove counter must be at least 1";

  return std::nullopt;
}

} // namespace fen
