#pragma once

#include "position.h"
#include "types.h"

#include <string>
#include <string_view>

/*
 * Standard Algebraic Notation resolved against one position, strictly.
 *
 * This reads notation that a person has just typed, so every way of getting it
 * wrong has to come back as something the person can act on. Nothing here
 * asserts and nothing guesses: a move that names no legal move and a move that
 * names two are both refusals, with a sentence saying which.
 *
 * Refusing the ambiguous case is the point rather than a nicety. Every
 * candidate is drawn from the position's own legal moves, so an "Nd2" with two
 * knights able to reach d2 has two perfectly legal answers, and taking
 * whichever one the generator emitted first would silently flag a move nobody
 * asked about.
 */

/*
 * The move `san` names in `pos`, or Move::none() when it names no legal move or
 * more than one; `reason` is then filled with a sentence saying which.
 *
 * Accepts what a person writes: castling as O-O or 0-0, captures with or
 * without the "x", promotions as "e8=Q" or "e8Q", and trailing "+", "#" and
 * annotation marks, none of which say anything the position does not.
 */
Stockfish::Move sanToMove(const Stockfish::Position& pos, std::string_view san,
                          std::string* reason = nullptr);
