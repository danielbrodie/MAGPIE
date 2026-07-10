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

// ---------------------------------------------------------------------------
// Pre-endgame (bag 1-4): exact expectiminimax under Crossplay rules.
// ---------------------------------------------------------------------------

// Upper bound on the number of ranked candidates returned by the pre-endgame
// solver. A pre-endgame rack generates at most a few hundred placements; the
// synthesized pass and exchange candidates add a bounded handful. 1024 is
// generous headroom.
enum { CPEG_MAX_PRE_CANDS = 1024 };

// One ranked pre-endgame candidate (your first move), scored by its expected
// spread over the enumerated opponent-rack worlds and random draws.
typedef struct CpegPreCand {
  // Rendered first move: "<coord> <word>", or "pass", or "exch:<tiles>".
  char label[CPEG_MOVE_STR_LEN];
  int score;               // Points scored by this first move (0 for pass/exch).
  double expected_spread;  // Your points minus theirs over the remaining game.
} CpegPreCand;

// Result of an exact Crossplay pre-endgame solve: candidates ranked by expected
// spread, best first.
typedef struct CpegPreResult {
  CpegPreCand cands[CPEG_MAX_PRE_CANDS];
  int count;
} CpegPreResult;

// Exact Crossplay pre-endgame value for the player on turn (the "mover").
//
// `bag` is the true number of tiles in the bag (1..4). The position's CGP is
// expected to carry the mover's rack with the opponent's rack EMPTY, so every
// unseen tile currently sits in MAGPIE's bag; the solver enumerates the
// opponent's rack as a distinct submultiset of those unseen tiles (each world's
// bag being the complement), then runs an exact expectiminimax over the
// per-world random draws that bottoms out in cpeg_solve_endgame when the bag
// empties. See cpeg.c for the full, load-bearing statement of the model
// (worlds, draw weights, the scoreless-ply cap, and the exchange model).
//
// When allow_exchanges is false, exchanges are not searched (matching the
// Python reference solver); pass is searched only when the mover has no legal
// placement.
//
// num_threads (>= 1) parallelizes the per-world evaluation. Fills *out with the
// ranked candidates and returns out->count.
int cpeg_solve_pre_endgame(Game *game, int bag, bool allow_exchanges,
                           int num_threads, CpegPreResult *out);

#endif
