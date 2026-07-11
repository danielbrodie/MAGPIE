#ifndef CPEG_H
#define CPEG_H

#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

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
  Move best_mover; // The mover's swing-maximizing move.
  Move best_reply; // Opponent's best raw-score reply to best_mover.
  int mover_score; // Points scored by best_mover.
  int reply_score; // Points scored by best_reply (0 when has_reply is false).
  int swing;      // mover_score - reply_score (the two-ply value to the mover).
  bool has_reply; // False when the opponent has no tiles left to reply with.
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

// Maximum absolute raw point value of any tile in the distribution.
int cpeg_max_future_tile_score(const LetterDistribution *ld);

// A sound upper bound, in raw points, on the score of any single legal move
// on board. The bound depends only on board geometry, remaining premiums, the
// loaded letter distribution, and the game's bingo bonus. It is therefore
// also valid when called on an immutable post-placement template game.
int cpeg_score_upper_bound(const Board *board, const Game *game);

// Returns true when enumerating the distinct k-submultisets would require more
// than cap entries. Exposed so capacity assumptions can be tested directly.
bool cpeg_submultiset_capacity_overflows(const int *counts, int ld_size, int k,
                                         int cap);

// ---------------------------------------------------------------------------
// Certified pre-endgame coordinator.
//
// This layer is deliberately independent of the solver and scheduler. A later
// stage supplies one evaluated interval for each (candidate, world) pair; the
// coordinator only aggregates those intervals and applies the proof rules.
// ---------------------------------------------------------------------------

typedef struct CpegInterval {
  double lo;
  double hi;
} CpegInterval;

typedef struct CpegIntervalRecursionStats {
  int candidate_count;
  int world_count;
  int pair_count;
  double total_width;
  double maximum_width;
  bool all_contained;
  int failing_candidate_index;
  int failing_world_index;
  double failing_scalar;
  CpegInterval failing_interval;
} CpegIntervalRecursionStats;

typedef enum CpegCandKind {
  CPEG_CAND_PLACEMENT,
  CPEG_CAND_EXCHANGE,
  CPEG_CAND_PASS,
} CpegCandKind;

// The deterministic candidate ordering, in priority order. immediate_score is
// descending; kind, label, and generation_index are ascending.
typedef struct CpegStableRank {
  int immediate_score;
  CpegCandKind kind;
  char label[CPEG_MOVE_STR_LEN];
  int generation_index;
} CpegStableRank;

typedef struct CpegCandState {
  CpegInterval prior;
  // Symmetric magnitude retained for diagnostics; aggregation uses the tighter
  // asymmetric prior.lo/prior.hi endpoints.
  double prior_magnitude;
  CpegStableRank rank;

  int worlds_resolved;
  int64_t resolved_weight;
  double weighted_lower_sum;
  double weighted_upper_sum;
  CpegInterval expectation;
  bool eliminated;
} CpegCandState;

typedef struct CpegWorldEval {
  CpegInterval value;
  bool resolved;
} CpegWorldEval;

typedef enum CpegCoordinatorStatus {
  CPEG_COORDINATOR_PENDING,
  CPEG_COORDINATOR_CERTIFIED,
  CPEG_COORDINATOR_EXACT_VALUES,
} CpegCoordinatorStatus;

typedef struct CpegCoordinatorResult {
  CpegCoordinatorStatus status;
  int best_index;
  bool unique_best;
  double value_error_bound;
  double decision_regret_bound;
} CpegCoordinatorResult;

// Safe prior bounds derived from Stage 2's single-move score upper bound.
CpegInterval cpeg_placement_prior(int score, int bag, int tiles_played,
                                  int score_upper_bound);
CpegInterval cpeg_scoreless_prior(int bag, int score_upper_bound);

void cpeg_cand_state_init(CpegCandState *state, CpegInterval prior,
                          const CpegStableRank *rank);

// Recompute every candidate interval from a candidate-major evaluations array:
// evaluations[candidate_index * world_count + world_index]. Existing
// elimination flags are sticky. All weights must be positive and counts must
// be nonzero. EXACT_VALUES takes precedence over CERTIFIED; when the result is
// PENDING, best_index is the stable argmax of interval midpoint for an
// ESTIMATED result chosen by the caller.
void cpeg_coordinator_recompute(CpegCandState *states, int candidate_count,
                                const CpegWorldEval *evaluations,
                                const int64_t *world_weights, int world_count,
                                CpegCoordinatorResult *result);

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
  int score;              // Points scored by this first move (0 for pass/exch).
  double expected_spread; // Your points minus theirs over the remaining game.
} CpegPreCand;

// Result of an exact Crossplay pre-endgame solve: candidates ranked by expected
// spread, best first.
typedef struct CpegPreResult {
  CpegPreCand cands[CPEG_MAX_PRE_CANDS];
  int count;
} CpegPreResult;

typedef enum CpegPreStatus {
  CPEG_PRE_CERTIFIED,
  CPEG_PRE_EXACT_VALUES,
  CPEG_PRE_ESTIMATED,
} CpegPreStatus;

typedef struct CpegCertifiedArgs {
  int bag;
  bool allow_exchanges;
  int num_threads;
  double budget_seconds;
  int batch_size;
  // Internal/test-only deterministic work budget; zero is unbounded.
  int max_batches;
} CpegCertifiedArgs;

typedef struct CpegCertifiedCand {
  char label[CPEG_MOVE_STR_LEN];
  int score;
  double estimate;
  double lower;
  double upper;
  double value_error_bound;
  int worlds_resolved;
  bool eliminated;
} CpegCertifiedCand;

typedef struct CpegCertifiedResult {
  CpegPreStatus status;
  CpegCertifiedCand cands[CPEG_MAX_PRE_CANDS];
  int count;
  int best_index;
  int worlds_total;
  int jobs_completed;
  int batches_completed;
  double optimum_lower;
  double optimum_upper;
  double decision_regret_bound;
  bool unique_best;
} CpegCertifiedResult;

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
// ranked candidates and returns out->count. Returns -1, with out empty, if an
// enumeration/output capacity is exceeded or the implied opponent rack is
// invalid.
int cpeg_solve_pre_endgame(Game *game, int bag, bool allow_exchanges,
                           int num_threads, CpegPreResult *out);

// Deterministic batched certified solve. A zero wall-clock or work budget is
// unbounded. Wall-clock ESTIMATED results are not bit-identical across machines
// or load because different complete-batch prefixes may finish; the schedule is
// deterministic, and a given batches_completed count reproduces exactly. A
// certified move cannot change except among exact co-optima.
int cpeg_solve_pre_endgame_certified(Game *game, const CpegCertifiedArgs *args,
                                     CpegCertifiedResult *out);

// Evaluate the scalar and certified interval recursions for every root
// (candidate, world) pair without changing the normal solver path. Returns -1
// on invalid input or an enumeration-capacity failure.
int cpeg_measure_interval_recursion(Game *game, int bag, bool allow_exchanges,
                                    CpegIntervalRecursionStats *stats);

// The certified recursion's empty-bag leaf: the integer endgame swing is exact.
CpegInterval cpeg_solve_endgame_interval(Game *game);

#endif
