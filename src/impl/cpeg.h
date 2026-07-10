#ifndef CPEG_H
#define CPEG_H

#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>

// Maximum rendered length of a single move string ("<coord> <word>") plus its
// terminator. A move spans at most BOARD_DIM tiles with a two-character coord,
// so 64 bytes is comfortably sufficient.
enum { CPEG_MOVE_STR_LEN = 64 };

// Result of an exact Crossplay endgame solve.
//
// All scores are in points (NOT Equity millipoints): the swing is arithmetic
// over move scores, deliberately never over Game player scores, so Scrabble's
// go-out bonus (applied to the player score inside play_move_incremental when a
// rack is emptied) cannot contaminate the Crossplay value -- Crossplay counts
// neither a go-out bonus nor a leftover-rack deduction.
typedef struct CpegResult {
  Move best_mover;  // The mover's swing-maximizing move.
  Move best_reply;  // Opponent's best raw-score reply to best_mover.
  int mover_score;  // Points scored by best_mover.
  int reply_score;  // Points scored by best_reply (0 when has_reply is false).
  int swing;        // mover_score - reply_score (the two-ply value to the mover).
  bool has_reply;   // False when the opponent has no tiles left to reply with.
  // Rendered "<coord> <word>" (or "pass") for each move, produced at the exact
  // board state that makes played-through tiles resolve correctly.
  char mover_str[CPEG_MOVE_STR_LEN];
  char reply_str[CPEG_MOVE_STR_LEN];
} CpegResult;

// Exact Crossplay endgame value for the player on turn, in points.
//
// Requires: the bag is empty and both racks are known. Exactly two plies
// remain under Crossplay rules -- the mover plays, the opponent replies once by
// raw score (a leave is worth nothing on the final turn), and the game ends.
// The mover chooses the move maximizing (its score - the opponent's best reply
// to it). Leftover rack tiles are not counted for anyone; there is no go-out
// bonus and no six-scoreless-turns rule.
//
// Fills *result and returns the swing (in points).
int cpeg_solve_endgame(Game *game, CpegResult *result);

#endif
