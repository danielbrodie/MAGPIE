#ifndef CPEG_H
#define CPEG_H

#include "../ent/cpeg_belief.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

// Maximum rendered length of a single move string ("<coord> <word>") plus its
// terminator. A move spans at most BOARD_DIM tiles with a two-character coord,
// so 64 bytes is comfortably sufficient.
enum { CPEG_MOVE_STR_LEN = 64, CPEG_MAX_WORLDS = 1024 };

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

// Complete accounting for the root actions admitted by a pre-endgame solve.
// `generation_complete` is false when move generation or a synthesized action
// class exceeded an internal, explicitly detected capacity; such a collection
// must never be used to recommend a move.
typedef struct CpegRootCoverage {
  int placements;
  int exchanges;
  int passes;
  int total;
  bool generation_complete;
} CpegRootCoverage;

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
  CpegPreCand *cands;
  int count;
  CpegRootCoverage coverage;
} CpegPreResult;

// Root-perspective strict-outcome value. Chance nodes average all four
// components; choice nodes compare win probability first, then tie
// probability, then expected final margin.
typedef struct CpegWtlValue {
  double win;
  double tie;
  double loss;
  double expected_final_margin;
} CpegWtlValue;

typedef struct CpegWtlArgs {
  int bag;
  bool allow_exchanges;
  int num_threads;
  // Root mover score minus opponent score before the candidate is played.
  // This is explicit: the solver deliberately never reads Game player scores.
  int64_t initial_lead;
} CpegWtlArgs;

typedef struct CpegWtlCand {
  char label[CPEG_MOVE_STR_LEN];
  int score;
  CpegWtlValue value;
} CpegWtlCand;

typedef struct CpegWtlResult {
  CpegWtlCand *cands;
  int count;
  int worlds_distinct;
  int64_t world_weight_mass;
  CpegRootCoverage coverage;
} CpegWtlResult;

typedef enum CpegPreStatus {
  CPEG_PRE_CERTIFIED,
  CPEG_PRE_EXACT_VALUES,
  CPEG_PRE_BOUNDED,
  CPEG_PRE_ESTIMATED,
  CPEG_PRE_STATISTICAL,
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
  CpegCertifiedCand *cands;
  int count;
  int best_index;
  int worlds_total;
  int jobs_completed;
  int batches_completed;
  double optimum_lower;
  double optimum_upper;
  double decision_regret_bound;
  bool unique_best;
  CpegRootCoverage coverage;
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
// Python reference solver). Voluntary pass is always included at the root;
// deeper pass is searched only when the mover has no legal placement.
//
// num_threads (>= 1) parallelizes the per-world evaluation. Fills *out with the
// ranked candidates and returns out->count. Returns -1, with out empty, if an
// enumeration/output capacity is exceeded or the implied opponent rack is
// invalid.
int cpeg_solve_pre_endgame(Game *game, int bag, bool allow_exchanges,
                           int num_threads, CpegPreResult *out);

// Exact score-aware Crossplay pre-endgame solve. Every recursive value is
// carried from the original mover's perspective. The mover maximizes and the
// opponent minimizes the lexicographic tuple (P(win), P(tie), expected final
// margin). Returns the complete ranked root action count, or -1 on invalid
// input or an explicitly detected incomplete search.
int cpeg_solve_pre_endgame_wtl(const Game *game, const CpegWtlArgs *args,
                               CpegWtlResult *out);

// Pure outcome helpers, also useful for focused tests and callers that need to
// combine exact chance branches.
CpegWtlValue cpeg_wtl_classify_margin(int64_t final_margin);
int cpeg_wtl_compare(const CpegWtlValue *lhs, const CpegWtlValue *rhs);
int cpeg_wtl_weighted_average(const CpegWtlValue *values,
                              const int64_t *weights, int count,
                              CpegWtlValue *out);

// Classify the exact two-ply empty-bag result from root_player_idx's
// perspective, starting from the supplied explicit lead.
int cpeg_solve_endgame_wtl(Game *game, int root_player_idx,
                           int64_t initial_lead, CpegWtlValue *out);

// Deterministic batched certified solve. A zero wall-clock or work budget is
// unbounded. Wall-clock ESTIMATED results are not bit-identical across machines
// or load because different complete-batch prefixes may finish; the schedule is
// deterministic, and a given batches_completed count reproduces exactly. A
// certified move cannot change except among exact co-optima.
int cpeg_solve_pre_endgame_certified(Game *game, const CpegCertifiedArgs *args,
                                     CpegCertifiedResult *out);

// Release dynamically allocated candidate storage. The result may be zeroed or
// already destroyed; after return it is reset to an empty state.
void cpeg_pre_result_destroy(CpegPreResult *result);
void cpeg_wtl_result_destroy(CpegWtlResult *result);
void cpeg_certified_result_destroy(CpegCertifiedResult *result);

// A deterministic enclosure of the score-aware outcome. `estimate` is only a
// display point inside the intervals; proof decisions use interval endpoints.
typedef struct CpegWtlEnvelope {
  CpegWtlValue estimate;
  CpegInterval win;
  CpegInterval tie;
  CpegInterval loss;
  CpegInterval expected_final_margin;
} CpegWtlEnvelope;

typedef enum CpegWtlProofKind {
  CPEG_WTL_PROOF_UNRESOLVED,
  CPEG_WTL_PROOF_DEFENSE_BOUND,
  CPEG_WTL_PROOF_EXACT,
} CpegWtlProofKind;

typedef enum CpegWtlReplyPhase {
  CPEG_WTL_REPLY_PHASE_INCUMBENT,
  CPEG_WTL_REPLY_PHASE_PLACEMENT_SCREEN,
  CPEG_WTL_REPLY_PHASE_HORIZON_REFINE,
  CPEG_WTL_REPLY_PHASE_SCORELESS_SCREEN,
  CPEG_WTL_REPLY_PHASE_SURVIVING_REFINE,
  CPEG_WTL_REPLY_PHASE_FIXED_REFINE,
  CPEG_WTL_REPLY_PHASE_COUNT,
} CpegWtlReplyPhase;

typedef struct CpegWtlReplyPhaseTrace {
  int64_t queries;
  int64_t replies_generated;
  int64_t cache_hits;
  int64_t movegen_work_ns;
  int64_t threshold_short_circuits;
} CpegWtlReplyPhaseTrace;

// Default-off producer instrumentation. Timings ending in `_work_ns` are sums
// across jobs and may exceed wall time when work runs in parallel.
typedef struct CpegWtlTrace {
  int64_t wall_ns;
  int64_t setup_ns;
  int64_t incumbent_ns;
  int64_t placement_screen_ns;
  int64_t horizon_refine_ns;
  int64_t scoreless_screen_ns;
  int64_t surviving_refine_ns;
  int64_t fixed_refine_ns;
  int64_t finalize_ns;
  int64_t root_actions;
  int64_t challengers;
  int64_t worlds;
  int64_t opponent_information_states;
  int64_t compute_participant_capacity;
  // Zero until move-generation traversal is shared across public-state groups.
  int64_t public_state_batches;
  int64_t scheduler_batches;
  int64_t defense_world_jobs;
  int64_t opponent_movegen_calls;
  int64_t opponent_moves_generated;
  int64_t opponent_movegen_work_ns;
  int64_t opponent_sort_calls;
  int64_t opponent_moves_sorted;
  int64_t opponent_sort_work_ns;
  int64_t defenses_threshold_tested;
  int64_t defenses_accepted;
  int64_t defenses_refuted;
  int64_t compatible_draws_tested;
  int64_t final_reply_queries;
  int64_t final_replies_generated;
  int64_t final_reply_cache_hits;
  int64_t final_reply_movegen_work_ns;
  int64_t threshold_short_circuits;
  CpegWtlReplyPhaseTrace reply_phases[CPEG_WTL_REPLY_PHASE_COUNT];
  int64_t exact_endgame_queries;
  int64_t exact_endgame_cache_hits;
  int64_t exact_endgame_work_ns;
  int64_t fixed_endgame_queries;
  int64_t fixed_endgame_cache_hits;
  int64_t fixed_endgame_work_ns;
} CpegWtlTrace;

typedef struct CpegWtlCertifiedArgs {
  int bag;
  bool allow_exchanges;
  int num_threads;
  int64_t initial_lead;
  double budget_seconds;
  int batch_size;
  // Internal/test-only deterministic batch budget; zero is unbounded.
  int max_batches;
  // Optional exact posterior support. NULL + zero preserves the neutral
  // physical-inventory prior; otherwise every supplied world is validated and
  // used verbatim. This axis is independent of the policy/observation model.
  const CpegWeightedWorld *weighted_worlds;
  int weighted_world_count;
  // Exact unseen inventory bound by the supplied posterior manifest. Requiring
  // it prevents a valid bag table for one position from being replayed against
  // a different Game whose visible inventory happens to admit those bags.
  const MachineLetter *weighted_unseen_tiles;
  int weighted_unseen_count;
  // Experiment only: evaluate an exhaustive bag-emptying incumbent with one
  // exact two-ply solve per world instead of one exact reply per defense.
  bool use_exact_two_ply_incumbent;
  // Experiment only: find the placement screen's highest-scoring bag-emptying
  // defense directly instead of materializing and sorting every legal move.
  bool use_best_bag_emptying_screen;
  // Experiment only: use the small reply generator for one-sided threshold
  // queries and never consume a threshold witness as an exact score.
  bool use_threshold_reply_screen;
  // Experiment only: reuse exact empty-bag swings by a collision-safe key over
  // the complete board, both racks, and the player on turn.
  bool use_exact_endgame_cache;
  // Collect the versioned producer trace. False preserves the normal hot path.
  bool collect_trace;
} CpegWtlCertifiedArgs;

typedef struct CpegWtlCertifiedCand {
  char label[CPEG_MOVE_STR_LEN];
  int score;
  CpegWtlEnvelope outcome;
  int64_t outcome_den;
  int64_t win_lower_num;
  int64_t win_upper_num;
  int64_t tie_lower_num;
  int64_t tie_upper_num;
  int64_t loss_lower_num;
  int64_t loss_upper_num;
  int worlds_exact;
  int worlds_bounded;
  int worlds_unresolved;
  int64_t exact_weight;
  int64_t bounded_weight;
  int64_t unresolved_weight;
  bool eliminated;
} CpegWtlCertifiedCand;

typedef struct CpegWtlCertifiedResult {
  CpegPreStatus status;
  CpegWtlCertifiedCand *cands;
  int count;
  int best_index;
  int worlds_distinct;
  int64_t world_weight_mass;
  int exact_jobs;
  int bound_jobs;
  int batches_completed;
  double decision_regret_bound;
  int64_t regret_num;
  int64_t regret_den;
  bool unique_best;
  CpegRootCoverage coverage;
  CpegWtlTrace trace;
  // Parallel to `cands`; allocated only when collect_trace is true.
  CpegWtlTrace *candidate_traces;
} CpegWtlCertifiedResult;

// Construct the sound outcome enclosure implied by a concrete opponent
// defense and a root-favorable final-margin upper bound.
CpegWtlEnvelope cpeg_wtl_envelope_from_margin_upper(int64_t margin_upper,
                                                    CpegInterval margin_prior);

// True only when lhs is proved lexicographically better than rhs. Later
// components are consulted only after the earlier component is a point on
// both sides and the two points are equal.
bool cpeg_wtl_envelope_dominates(const CpegWtlEnvelope *lhs,
                                 const CpegWtlEnvelope *rhs);

// Candidate-complete, score-aware certification. Every root action is
// retained. Staged concrete-defense witnesses contract strict-win envelopes;
// surviving non-horizon actions are finished with exact bag-empty endgames.
int cpeg_solve_pre_endgame_wtl_certified(const Game *game,
                                         const CpegWtlCertifiedArgs *args,
                                         CpegWtlCertifiedResult *out);

void cpeg_wtl_certified_result_destroy(CpegWtlCertifiedResult *result);

typedef struct CpegStatisticalArgs {
  int bag;
  bool allow_exchanges;
  int num_threads;
  double budget_seconds;
  uint64_t seed;
  double confidence;
  // Internal/test-only deterministic sample budget; zero samples every world.
  int max_worlds;
} CpegStatisticalArgs;

typedef struct CpegStatisticalCand {
  char label[CPEG_MOVE_STR_LEN];
  int score;
  double estimate;
  double lower;
  double upper;
  double sample_variance;
  int worlds_sampled;
  bool eliminated;
  int elimination_round;
} CpegStatisticalCand;

typedef struct CpegStatisticalResult {
  CpegPreStatus status;
  CpegStatisticalCand *cands;
  int count;
  int best_index;
  int worlds_sampled;
  int worlds_total;
  int jobs_completed;
  int rounds_completed;
  double confidence;
  uint64_t seed;
  int sampled_world_indices[CPEG_MAX_WORLDS];
  int64_t sampled_world_weights[CPEG_MAX_WORLDS];
  CpegRootCoverage coverage;
} CpegStatisticalResult;

// Seeded, paired sampling of outer opponent-rack worlds. Inner draws are still
// enumerated exactly. A zero wall-clock budget is unbounded; max_worlds is a
// deterministic test/replay cap. Incomplete paired rounds are discarded.
int cpeg_solve_pre_endgame_statistical(Game *game,
                                       const CpegStatisticalArgs *args,
                                       CpegStatisticalResult *out);

void cpeg_statistical_result_destroy(CpegStatisticalResult *result);

// Collect and count the exact root action set without evaluating any hidden
// worlds. This uses the same collector as every production pre-endgame path and
// is useful for fast completeness checks. Returns -1 on invalid input or an
// explicitly detected generation-capacity failure.
int cpeg_count_root_actions(Game *game, int bag, bool allow_exchanges,
                            CpegRootCoverage *coverage);

// Pure sampling/CI core, exposed for finite-population coverage tests. Values
// and positive hypergeometric weights describe the complete outer population.
int cpeg_statistical_resample_values(const double *values,
                                     const int64_t *weights, int world_count,
                                     int sample_count, uint64_t seed,
                                     double confidence,
                                     CpegStatisticalCand *out);

// Evaluate the scalar and certified interval recursions for every root
// (candidate, world) pair without changing the normal solver path. Returns -1
// on invalid input or an enumeration-capacity failure.
int cpeg_measure_interval_recursion(Game *game, int bag, bool allow_exchanges,
                                    CpegIntervalRecursionStats *stats);

// The certified recursion's empty-bag leaf: the integer endgame swing is exact.
CpegInterval cpeg_solve_endgame_interval(Game *game);

#endif
