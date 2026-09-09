#include "San.h"

#include "movegen.h"

#include <cctype>
#include <vector>

using namespace Stockfish;

namespace {

/* Trailing marks say nothing the position does not already: whether a move
 * gives check is a fact about the board, and "!?" is an opinion. */
constexpr std::string_view DECORATION = "+#!?";

std::string_view trim(std::string_view text) {
  const auto begin = text.find_first_not_of(" \t\r\n");

  if (begin == std::string_view::npos)
    return {};

  return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

/* Written O-O by convention and 0-0 by half the world's keyboards. */
bool isCastling(std::string_view text, bool& kingside) {
  std::string upper;

  for (char c : text)
    upper.push_back(c == '0' ? 'O' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

  if (upper == "O-O-O") {
    kingside = false;
    return true;
  }

  if (upper == "O-O") {
    kingside = true;
    return true;
  }

  return false;
}

PieceType pieceLetter(char c) {
  switch (c) {
  case 'K':
    return KING;
  case 'Q':
    return QUEEN;
  case 'R':
    return ROOK;
  case 'B':
    return BISHOP;
  case 'N':
    return KNIGHT;
  default:
    return NO_PIECE_TYPE;
  }
}

bool isFile(char c) {
  return c >= 'a' && c <= 'h';
}

bool isRank(char c) {
  return c >= '1' && c <= '8';
}

Move fail(std::string* reason, std::string text) {
  if (reason)
    *reason = std::move(text);

  return Move::none();
}

/*
 * The castling move as Stockfish encodes it: king square to *rook* square, not
 * to the square the king ends on. Which side it is therefore has to be read off
 * the rook, and does so correctly under Chess960 as well.
 */
Move findCastling(const Position& pos, bool kingside, std::string* reason) {
  for (const Move& m : MoveList<LEGAL>(pos))
    if (m.type_of() == CASTLING && (file_of(m.to_sq()) > file_of(m.from_sq())) == kingside)
      return m;

  return fail(reason, kingside ? "castling short is not legal here"
                               : "castling long is not legal here");
}

} // namespace

Move sanToMove(const Position& pos, std::string_view san, std::string* reason) {
  std::string_view text = trim(san);

  while (!text.empty() && DECORATION.find(text.back()) != std::string_view::npos)
    text.remove_suffix(1);

  if (text.empty())
    return fail(reason, "empty move");

  bool kingside = false;

  if (isCastling(text, kingside))
    return findCastling(pos, kingside, reason);

  /* "e8=Q" and "e8Q" both appear in the wild. Without the "=" only an uppercase
   * letter counts, so that the b-file of "exb8" is not read as a bishop. */
  PieceType promotion = NO_PIECE_TYPE;

  if (const auto equals = text.find('='); equals != std::string_view::npos) {
    if (equals + 2 != text.size())
      return fail(reason, "cannot read the promotion in '" + std::string(san) + "'");

    promotion = pieceLetter(static_cast<char>(std::toupper(static_cast<unsigned char>(text[equals + 1]))));
    text.remove_suffix(2);
  } else if (text.size() > 2 && pieceLetter(text.back()) != NO_PIECE_TYPE) {
    promotion = pieceLetter(text.back());
    text.remove_suffix(1);
  }

  if (promotion == KING || promotion == PAWN)
    return fail(reason, "cannot promote to that piece in '" + std::string(san) + "'");

  if (text.size() < 2 || !isFile(text[text.size() - 2]) || !isRank(text.back()))
    return fail(reason, "'" + std::string(san) + "' does not name a destination square");

  const Square target = make_square(File(text[text.size() - 2] - 'a'), Rank(text.back() - '1'));
  text.remove_suffix(2);

  /* What is left in front of the destination: the piece, then whatever the
   * writer needed to say which one of them moved. */
  PieceType piece = PAWN;

  if (!text.empty() && pieceLetter(text.front()) != NO_PIECE_TYPE) {
    piece = pieceLetter(text.front());
    text.remove_prefix(1);
  }

  File fromFile = FILE_NB;
  Rank fromRank = RANK_NB;

  for (char c : text) {
    if (c == 'x' || c == 'X' || c == ':')
      continue;
    else if (isFile(c) && fromFile == FILE_NB)
      fromFile = File(c - 'a');
    else if (isRank(c) && fromRank == RANK_NB)
      fromRank = Rank(c - '1');
    else
      return fail(reason, "cannot read '" + std::string(san) + "'");
  }

  std::vector<Move> matches;

  for (const Move& m : MoveList<LEGAL>(pos)) {
    /* Castling is named O-O and nothing else, so leaving it out here is what
     * keeps a king move from matching it. */
    if (m.type_of() == CASTLING || m.to_sq() != target)
      continue;

    if (type_of(pos.piece_on(m.from_sq())) != piece)
      continue;

    if (promotion == NO_PIECE_TYPE ? m.type_of() == PROMOTION
                                   : m.type_of() != PROMOTION || m.promotion_type() != promotion)
      continue;

    if (fromFile != FILE_NB && file_of(m.from_sq()) != fromFile)
      continue;

    if (fromRank != RANK_NB && rank_of(m.from_sq()) != fromRank)
      continue;

    matches.push_back(m);
  }

  if (matches.empty())
    return fail(reason, "'" + std::string(san) + "' is not legal in this position");

  if (matches.size() > 1)
    return fail(reason, "'" + std::string(san) + "' could be more than one move here; say which");

  return matches.front();
}
