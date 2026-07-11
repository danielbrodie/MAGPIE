#include "cpeg.h"

#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/peg_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../str/move_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include "peg_combinatorics.h"
#include "peg_pool.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Capacity for the exhaustive move lists. A single 7-tile endgame rack can
  // enumerate a few thousand plays at most; this cap has generous headroom so
  // MOVE_RECORD_ALL never silently drops a play.
  CPEG_MOVE_LIST_CAP = 16384,
  // Max recursion depth of the pre-endgame expectiminimax. The bag strictly
  // decreases on every placement (bounded by PEG_MAX_BAG = 4) and consecutive
  // scoreless plies are capped at CPEG_SCORELESS_CAP, so the reachable depth is
  // far below this; the margin is a robustness guard, not a real limit.
  CPEG_MAX_DEPTH = 20,
  // Two consecutive scoreless plies (pass or exchange, either player) make the
  // position terminal at the current spread. This is a STATED MODELLING CHOICE:
  // Crossplay has no six-scoreless-turns rule, but exchanges keep the bag size
  // constant, so without this cap an exchange/pass cycle would never terminate.
  // See the model note above cpeg_solve_pre_endgame.
  CPEG_SCORELESS_CAP = 2,
  // Capacity for a single draw/exchange submultiset enumeration. A draw takes
  // at
  // most PEG_MAX_BAG (4) tiles from a <=4-tile bag, and an exchange at most 4
  // tiles from a 7-tile rack (C(7,4) = 35), so 128 is comfortable headroom.
  CPEG_ENUM_CAP = 128,
  // Capacity for the opponent-rack world enumeration: the distinct
  // bag-submultisets of the <=11 unseen tiles. C(11,4) = 330 is the worst case;
  // 1024 leaves headroom.
  CPEG_WORLD_CAP = 1024,
  // Default scheduler barrier size. It is deliberately independent of the
  // thread count so changing only parallelism cannot change a committed batch.
  CPEG_CERT_DEFAULT_BATCH_SIZE = 16,
};

// Renders a move as "<coord> <word>" (or "pass") into dest, using board so
// played-through tiles resolve to their on-board letters. add_score is false:
// the score is emitted separately by the caller.
static void cpeg_render_move(char *dest, size_t dest_size, const Board *board,
                             const Move *move, const LetterDistribution *ld) {
  StringBuilder *builder = string_builder_create();
  string_builder_add_move(builder, board, move, ld, /*add_score=*/false);
  const char *rendered = string_builder_peek(builder);
  const size_t len = string_length(rendered);
  const size_t copy_len = len < dest_size - 1 ? len : dest_size - 1;
  memcpy(dest, rendered, copy_len);
  dest[copy_len] = '\0';
  string_builder_destroy(builder);
}

// Exact Crossplay endgame solve reusing caller-owned buffers (mover_moves,
// reply_moves, undo). Identical semantics to cpeg_solve_endgame; factored out
// so the pre-endgame recursion can hit thousands of empty-bag leaves without a
// per-leaf allocation. Fills *result and returns the swing (in points).
static int cpeg_endgame_core(Game *game, MoveList *mover_moves,
                             MoveList *reply_moves, MoveUndo *undo,
                             CpegResult *result) {
  memset(result, 0, sizeof(*result));

  const LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);

  // Cross-sets must be valid before enumerating the mover's plays.
  if (!board_get_cross_sets_valid(board)) {
    game_gen_all_cross_sets(game);
    board_set_cross_sets_valid(board, true);
  }

  const MoveGenArgs mover_args = {
      .game = game,
      .move_list = mover_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&mover_args);
  move_list_sort_moves(mover_moves);

  int best_swing = 0;
  int best_mover_score = 0;
  bool have_best = false;

  const int mover_count = move_list_get_count(mover_moves);
  for (int mover_idx = 0; mover_idx < mover_count; mover_idx++) {
    const Move *mover_move = move_list_get_move(mover_moves, mover_idx);
    const int mover_score = equity_to_int(move_get_score(mover_move));

    // Replies have non-negative raw scores, so this move's swing cannot exceed
    // its own score. The mover list is sorted by the same full move comparator
    // used by the tie-break below, making the cutoff exact even when the score
    // equals the incumbent swing: any equal-score move that could win the final
    // tie-break was already searched.
    if (have_best && mover_score <= best_swing) {
      break;
    }

    // Apply the mover's play. play_move_incremental switches the turn to the
    // opponent and (harmlessly, for us) may add the go-out bonus to the mover's
    // Game score; we never read that score -- the swing is computed purely from
    // move scores below.
    play_move_incremental(mover_move, game, undo);

    const MoveGenArgs reply_args = {
        .game = game,
        .move_list = reply_moves,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&reply_args);

    const int opponent_index = game_get_player_on_turn_index(game);
    const Rack *opponent_rack =
        player_get_rack(game_get_player(game, opponent_index));
    const bool opponent_can_reply = !rack_is_empty(opponent_rack);

    const Move *reply_move = move_list_get_move(reply_moves, 0);
    const int reply_score = equity_to_int(move_get_score(reply_move));
    const int swing = mover_score - reply_score;

    bool take = false;
    if (!have_best) {
      take = true;
    } else if (swing > best_swing) {
      take = true;
    } else if (swing == best_swing && mover_score > best_mover_score) {
      take = true;
    } else if (swing == best_swing && mover_score == best_mover_score &&
               compare_moves_without_equity(mover_move, &result->best_mover,
                                            /*allow_duplicates=*/true) == 1) {
      take = true;
    }

    if (take) {
      have_best = true;
      best_swing = swing;
      best_mover_score = mover_score;
      move_copy(&result->best_mover, mover_move);
      move_copy(&result->best_reply, reply_move);
      result->mover_score = mover_score;
      result->reply_score = reply_score;
      result->swing = swing;
      result->has_reply = opponent_can_reply;
      cpeg_render_move(result->mover_str, sizeof(result->mover_str), board,
                       &result->best_mover, ld);
      if (opponent_can_reply) {
        cpeg_render_move(result->reply_str, sizeof(result->reply_str), board,
                         &result->best_reply, ld);
      } else {
        result->reply_str[0] = '-';
        result->reply_str[1] = '\0';
      }
    }

    unplay_move_incremental(game, undo);
  }

  if (!have_best) {
    result->mover_str[0] = '-';
    result->mover_str[1] = '\0';
    result->reply_str[0] = '-';
    result->reply_str[1] = '\0';
  }

  return result->swing;
}

int cpeg_solve_endgame(Game *game, CpegResult *result) {
  MoveList *mover_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveList *reply_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveUndo *undo = malloc_or_die(sizeof(MoveUndo));
  const int swing =
      cpeg_endgame_core(game, mover_moves, reply_moves, undo, result);
  free(undo);
  move_list_destroy(reply_moves);
  move_list_destroy(mover_moves);
  return swing;
}

CpegInterval cpeg_solve_endgame_interval(Game *game) {
  CpegResult result;
  const int swing = cpeg_solve_endgame(game, &result);
  return (CpegInterval){.lo = (double)swing, .hi = (double)swing};
}

typedef struct CpegBoundSquare {
  uint64_t letter_multiplier;
  uint64_t word_multiplier;
  uint64_t perpendicular_sum;
} CpegBoundSquare;

static uint64_t cpeg_score_bound_fallback(void) {
  return (uint64_t)(EQUITY_MAX_VALUE / EQUITY_RESOLUTION);
}

static uint64_t cpeg_checked_add(uint64_t lhs, uint64_t rhs, bool *overflow) {
  if (UINT64_MAX - lhs < rhs) {
    *overflow = true;
    return cpeg_score_bound_fallback();
  }
  return lhs + rhs;
}

static uint64_t cpeg_checked_mul(uint64_t lhs, uint64_t rhs, bool *overflow) {
  if (lhs != 0 && rhs > UINT64_MAX / lhs) {
    *overflow = true;
    return cpeg_score_bound_fallback();
  }
  return lhs * rhs;
}

static uint64_t cpeg_abs_raw_equity(Equity equity) {
  const int64_t raw_equity = equity;
  const uint64_t magnitude =
      raw_equity < 0 ? (uint64_t)(-raw_equity) : (uint64_t)raw_equity;
  return (magnitude + EQUITY_RESOLUTION - 1) / EQUITY_RESOLUTION;
}

int cpeg_max_future_tile_score(const LetterDistribution *ld) {
  uint64_t max_score = 0;
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    const uint64_t score =
        cpeg_abs_raw_equity(ld_get_score(ld, (MachineLetter)ml));
    if (score > max_score) {
      max_score = score;
    }
  }
  return (int)max_score;
}

static uint64_t cpeg_board_square_upper_score(const Board *board,
                                              const LetterDistribution *ld,
                                              int row, int col,
                                              uint64_t max_tile_score) {
  if (board_is_empty(board, row, col)) {
    return max_tile_score;
  }
  const MachineLetter ml = board_get_letter(board, row, col);
  const MachineLetter scoring_ml =
      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
  return cpeg_abs_raw_equity(ld_get_score(ld, scoring_ml));
}

static uint64_t cpeg_perpendicular_segment_sum(const Board *board,
                                               const LetterDistribution *ld,
                                               int row, int col, int dir,
                                               uint64_t max_tile_score,
                                               bool *overflow) {
  const int perpendicular_dir = dir == BOARD_HORIZONTAL_DIRECTION
                                    ? BOARD_VERTICAL_DIRECTION
                                    : BOARD_HORIZONTAL_DIRECTION;
  uint64_t sum = 0;
  for (int offset = 1; offset < BOARD_DIM; offset++) {
    const int before_row =
        row - (perpendicular_dir == BOARD_VERTICAL_DIRECTION ? offset : 0);
    const int before_col =
        col - (perpendicular_dir == BOARD_HORIZONTAL_DIRECTION ? offset : 0);
    if (before_row < 0 || before_col < 0 ||
        board_get_is_brick(board, before_row, before_col)) {
      break;
    }
    sum =
        cpeg_checked_add(sum,
                         cpeg_board_square_upper_score(
                             board, ld, before_row, before_col, max_tile_score),
                         overflow);
  }
  for (int offset = 1; offset < BOARD_DIM; offset++) {
    const int after_row =
        row + (perpendicular_dir == BOARD_VERTICAL_DIRECTION ? offset : 0);
    const int after_col =
        col + (perpendicular_dir == BOARD_HORIZONTAL_DIRECTION ? offset : 0);
    if (after_row >= BOARD_DIM || after_col >= BOARD_DIM ||
        board_get_is_brick(board, after_row, after_col)) {
      break;
    }
    sum = cpeg_checked_add(sum,
                           cpeg_board_square_upper_score(
                               board, ld, after_row, after_col, max_tile_score),
                           overflow);
  }
  return sum;
}

static void cpeg_enumerate_bound_subsets(
    const CpegBoundSquare *empty_squares, int empty_count, int start_idx,
    int chosen_count, uint64_t main_sum, uint64_t word_multiplier,
    uint64_t cross_sum, uint64_t max_tile_score, uint64_t bingo_bonus,
    uint64_t *best, bool *overflow) {
  if (*overflow || chosen_count >= RACK_SIZE) {
    return;
  }
  for (int square_idx = start_idx; square_idx < empty_count; square_idx++) {
    const CpegBoundSquare *square = &empty_squares[square_idx];
    const uint64_t multiplied_tile =
        cpeg_checked_mul(max_tile_score, square->letter_multiplier, overflow);
    const uint64_t next_main_sum =
        cpeg_checked_add(main_sum - max_tile_score, multiplied_tile, overflow);
    const uint64_t next_word_multiplier =
        cpeg_checked_mul(word_multiplier, square->word_multiplier, overflow);
    uint64_t cross_word =
        cpeg_checked_add(multiplied_tile, square->perpendicular_sum, overflow);
    cross_word =
        cpeg_checked_mul(cross_word, square->word_multiplier, overflow);
    const uint64_t next_cross_sum =
        cpeg_checked_add(cross_sum, cross_word, overflow);
    uint64_t bound =
        cpeg_checked_mul(next_main_sum, next_word_multiplier, overflow);
    bound = cpeg_checked_add(bound, next_cross_sum, overflow);
    bound = cpeg_checked_add(bound, bingo_bonus, overflow);
    if (*overflow) {
      *best = cpeg_score_bound_fallback();
      return;
    }
    if (bound > *best) {
      *best = bound;
    }
    cpeg_enumerate_bound_subsets(empty_squares, empty_count, square_idx + 1,
                                 chosen_count + 1, next_main_sum,
                                 next_word_multiplier, next_cross_sum,
                                 max_tile_score, bingo_bonus, best, overflow);
  }
}

int cpeg_score_upper_bound(const Board *board, const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  const uint64_t max_tile_score = (uint64_t)cpeg_max_future_tile_score(ld);
  const uint64_t bingo_bonus = cpeg_abs_raw_equity(game_get_bingo_bonus(game));
  uint64_t best = 0;
  bool overflow = false;

  for (int dir = BOARD_HORIZONTAL_DIRECTION; dir <= BOARD_VERTICAL_DIRECTION;
       dir++) {
    for (int lane = 0; lane < BOARD_DIM; lane++) {
      int segment_start = 0;
      while (segment_start < BOARD_DIM) {
        const int start_row =
            dir == BOARD_HORIZONTAL_DIRECTION ? lane : segment_start;
        const int start_col =
            dir == BOARD_HORIZONTAL_DIRECTION ? segment_start : lane;
        if (board_get_is_brick(board, start_row, start_col)) {
          segment_start++;
          continue;
        }

        CpegBoundSquare empty_squares[BOARD_DIM];
        int empty_count = 0;
        uint64_t main_sum = 0;
        int segment_end = segment_start;
        while (segment_end < BOARD_DIM) {
          const int row =
              dir == BOARD_HORIZONTAL_DIRECTION ? lane : segment_end;
          const int col =
              dir == BOARD_HORIZONTAL_DIRECTION ? segment_end : lane;
          if (board_get_is_brick(board, row, col)) {
            break;
          }
          main_sum = cpeg_checked_add(main_sum,
                                      cpeg_board_square_upper_score(
                                          board, ld, row, col, max_tile_score),
                                      &overflow);
          if (board_is_empty(board, row, col)) {
            const BonusSquare bonus_square =
                board_get_bonus_square(board, row, col);
            CpegBoundSquare *square = &empty_squares[empty_count++];
            square->letter_multiplier =
                bonus_square_get_letter_multiplier(bonus_square);
            square->word_multiplier =
                bonus_square_get_word_multiplier(bonus_square);
            if (square->letter_multiplier < 1) {
              square->letter_multiplier = 1;
            }
            if (square->word_multiplier < 1) {
              square->word_multiplier = 1;
            }
            square->perpendicular_sum = cpeg_perpendicular_segment_sum(
                board, ld, row, col, dir, max_tile_score, &overflow);
          }
          segment_end++;
        }
        if (overflow) {
          return (int)cpeg_score_bound_fallback();
        }
        cpeg_enumerate_bound_subsets(empty_squares, empty_count, 0, 0, main_sum,
                                     1, 0, max_tile_score, bingo_bonus, &best,
                                     &overflow);
        if (overflow || best >= cpeg_score_bound_fallback()) {
          return (int)cpeg_score_bound_fallback();
        }
        segment_start = segment_end + 1;
      }
    }
  }
  return (int)best;
}

static double cpeg_down_add(double lhs, double rhs) {
  return nextafter(lhs + rhs, -INFINITY);
}

static double cpeg_up_add(double lhs, double rhs) {
  return nextafter(lhs + rhs, INFINITY);
}

static double cpeg_down_mul(double lhs, double rhs) {
  return nextafter(lhs * rhs, -INFINITY);
}

static double cpeg_up_mul(double lhs, double rhs) {
  return nextafter(lhs * rhs, INFINITY);
}

static double cpeg_down_div(double numerator, double denominator) {
  return nextafter(numerator / denominator, -INFINITY);
}

static double cpeg_up_div(double numerator, double denominator) {
  return nextafter(numerator / denominator, INFINITY);
}

CpegInterval cpeg_placement_prior(int score, int bag, int tiles_played,
                                  int score_upper_bound) {
  const int tiles_drawn = tiles_played < bag ? tiles_played : bag;
  const int remaining_bag = bag - tiles_drawn;
  const double radius = (double)(remaining_bag + 2) * (double)score_upper_bound;
  const CpegInterval prior = {
      .lo = (double)score - radius,
      .hi = (double)score + radius,
  };
  return prior;
}

CpegInterval cpeg_scoreless_prior(int bag, int score_upper_bound) {
  const double radius = (double)(bag + 2) * (double)score_upper_bound;
  const CpegInterval prior = {.lo = -radius, .hi = radius};
  return prior;
}

void cpeg_cand_state_init(CpegCandState *state, CpegInterval prior,
                          const CpegStableRank *rank) {
  memset(state, 0, sizeof(*state));
  state->prior = prior;
  state->prior_magnitude = fmax(fabs(prior.lo), fabs(prior.hi));
  state->rank = *rank;
  state->expectation = prior;
}

static bool cpeg_rank_precedes(const CpegCandState *lhs,
                               const CpegCandState *rhs) {
  if (lhs->rank.immediate_score != rhs->rank.immediate_score) {
    return lhs->rank.immediate_score > rhs->rank.immediate_score;
  }
  if (lhs->rank.kind != rhs->rank.kind) {
    return lhs->rank.kind < rhs->rank.kind;
  }
  const int label_comparison = strcmp(lhs->rank.label, rhs->rank.label);
  if (label_comparison != 0) {
    return label_comparison < 0;
  }
  return lhs->rank.generation_index < rhs->rank.generation_index;
}

static bool cpeg_better_lower(const CpegCandState *states, int candidate_idx,
                              int best_idx) {
  return best_idx < 0 ||
         states[candidate_idx].expectation.lo >
             states[best_idx].expectation.lo ||
         (states[candidate_idx].expectation.lo ==
              states[best_idx].expectation.lo &&
          cpeg_rank_precedes(&states[candidate_idx], &states[best_idx]));
}

static double cpeg_interval_midpoint(CpegInterval interval) {
  return interval.lo / 2.0 + interval.hi / 2.0;
}

static bool cpeg_better_midpoint(const CpegCandState *states, int candidate_idx,
                                 int best_idx) {
  if (best_idx < 0) {
    return true;
  }
  const double midpoint =
      cpeg_interval_midpoint(states[candidate_idx].expectation);
  const double best_midpoint =
      cpeg_interval_midpoint(states[best_idx].expectation);
  return midpoint > best_midpoint ||
         (midpoint == best_midpoint &&
          cpeg_rank_precedes(&states[candidate_idx], &states[best_idx]));
}

void cpeg_coordinator_recompute(CpegCandState *states, int candidate_count,
                                const CpegWorldEval *evaluations,
                                const int64_t *world_weights, int world_count,
                                CpegCoordinatorResult *result) {
  int64_t total_weight = 0;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    total_weight += world_weights[world_idx];
  }

  bool all_resolved = true;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    CpegCandState *state = &states[candidate_idx];
    state->worlds_resolved = 0;
    state->resolved_weight = 0;
    state->weighted_lower_sum = 0.0;
    state->weighted_upper_sum = 0.0;
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      const CpegWorldEval *evaluation =
          &evaluations[candidate_idx * world_count + world_idx];
      if (!evaluation->resolved) {
        all_resolved = false;
        continue;
      }
      const double weight = (double)world_weights[world_idx];
      state->worlds_resolved++;
      state->resolved_weight += world_weights[world_idx];
      state->weighted_lower_sum =
          cpeg_down_add(state->weighted_lower_sum,
                        cpeg_down_mul(weight, evaluation->value.lo));
      state->weighted_upper_sum = cpeg_up_add(
          state->weighted_upper_sum, cpeg_up_mul(weight, evaluation->value.hi));
    }

    const int64_t unresolved_weight = total_weight - state->resolved_weight;
    double lower_numerator = state->weighted_lower_sum;
    double upper_numerator = state->weighted_upper_sum;
    if (unresolved_weight > 0) {
      lower_numerator = cpeg_down_add(
          lower_numerator,
          cpeg_down_mul((double)unresolved_weight, state->prior.lo));
      upper_numerator =
          cpeg_up_add(upper_numerator,
                      cpeg_up_mul((double)unresolved_weight, state->prior.hi));
    }
    state->expectation.lo =
        cpeg_down_div(lower_numerator, (double)total_weight);
    state->expectation.hi = cpeg_up_div(upper_numerator, (double)total_weight);
  }

  int incumbent_idx = -1;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (!states[candidate_idx].eliminated &&
        cpeg_better_lower(states, candidate_idx, incumbent_idx)) {
      incumbent_idx = candidate_idx;
    }
  }
  const double best_active_lower = states[incumbent_idx].expectation.lo;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (!states[candidate_idx].eliminated &&
        states[candidate_idx].expectation.hi < best_active_lower) {
      states[candidate_idx].eliminated = true;
    }
  }

  int lower_best_idx = -1;
  int midpoint_best_idx = -1;
  double maximum_upper = -INFINITY;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (cpeg_better_lower(states, candidate_idx, lower_best_idx)) {
      lower_best_idx = candidate_idx;
    }
    if (!states[candidate_idx].eliminated &&
        cpeg_better_midpoint(states, candidate_idx, midpoint_best_idx)) {
      midpoint_best_idx = candidate_idx;
    }
    if (states[candidate_idx].expectation.hi > maximum_upper) {
      maximum_upper = states[candidate_idx].expectation.hi;
    }
  }

  double other_maximum_upper = -INFINITY;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx != lower_best_idx &&
        states[candidate_idx].expectation.hi > other_maximum_upper) {
      other_maximum_upper = states[candidate_idx].expectation.hi;
    }
  }
  const bool certified =
      candidate_count == 1 ||
      states[lower_best_idx].expectation.lo >= other_maximum_upper;

  if (all_resolved) {
    result->status = CPEG_COORDINATOR_EXACT_VALUES;
    result->best_index = midpoint_best_idx;
  } else if (certified) {
    result->status = CPEG_COORDINATOR_CERTIFIED;
    result->best_index = lower_best_idx;
  } else {
    result->status = CPEG_COORDINATOR_PENDING;
    result->best_index = midpoint_best_idx;
  }

  double selected_other_maximum_upper = -INFINITY;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx != result->best_index &&
        states[candidate_idx].expectation.hi > selected_other_maximum_upper) {
      selected_other_maximum_upper = states[candidate_idx].expectation.hi;
    }
  }
  const CpegInterval best = states[result->best_index].expectation;
  result->unique_best =
      result->status != CPEG_COORDINATOR_PENDING &&
      (candidate_count == 1 || best.lo > selected_other_maximum_upper);
  result->value_error_bound = (best.hi - best.lo) / 2.0;
  result->decision_regret_bound = maximum_upper - best.lo;
  if (result->decision_regret_bound < 0.0) {
    result->decision_regret_bound = 0.0;
  }
}

// ---------------------------------------------------------------------------
// Pre-endgame (bag 1-4): exact Crossplay expectiminimax.
//
// The model (every clause is load-bearing):
//   * Worlds. The opponent's rack is enumerated as a distinct submultiset of
//     the unseen tiles; each world's bag is the complement. Worlds are weighted
//     by their multiset count (peg_binomial products).
//   * Per-world perfect information. Inside a world both racks are known and
//     both sides play optimally. This is the "exact pre-endgame" objective, NOT
//     a solution of the imperfect-information game.
//   * Draws are chance nodes. After placing t tiles a player draws
//     k = min(t, bag) tiles: the distinct k-submultisets of the bag are
//     enumerated with their multiset weights and averaged.
//   * Scoring is Crossplay: value is arithmetic over move scores, from the root
//     mover's perspective (your points minus theirs). Game player scores are
//     NEVER read (they carry Scrabble's go-out bonus). Leftover racks count for
//     no one; there is no go-out bonus and no six-scoreless-turns rule.
//   * Termination. A placement draws >= 1 tile while the bag is non-empty, so
//     the bag strictly decreases each ply. When the bag reaches 0 the value is
//     cpeg_endgame_core from the on-turn player's perspective (the two-ply
//     Crossplay endgame).
//   * Exchanges. Modelled by enumerating the mover's rack subsets to swap (an
//     exchange scores 0, redraws, and keeps the bag size constant). Because a
//     swap does not shrink the bag, CONSECUTIVE scoreless plies (pass or
//     exchange) are capped at CPEG_SCORELESS_CAP: two in a row make the
//     position terminal at the current spread. This cap is a stated modelling
//     choice with no analogue in the real game; it is the only thing that keeps
//     the exchange search finite. Pass is searched only when there is no legal
//     placement.
// ---------------------------------------------------------------------------

// A distinct submultiset produced by cpeg_enum_submultisets, with its
// combinatorial weight (product of peg_binomial(counts[l], take[l])).
typedef struct CpegMultiset {
  MachineLetter tiles[RACK_SIZE + PEG_MAX_BAG];
  int n;
  int64_t weight;
} CpegMultiset;

// Recursive worker for cpeg_enum_submultisets.
static void cpeg_enum_rec(const int *counts, int ld_size, int start_ml,
                          int k_left, int64_t weight, MachineLetter *chosen,
                          int n_chosen, CpegMultiset *out, int *out_n, int cap,
                          bool *overflow) {
  if (*overflow) {
    return;
  }
  if (k_left == 0) {
    if (*out_n < cap) {
      CpegMultiset *entry = &out[*out_n];
      for (int tile_idx = 0; tile_idx < n_chosen; tile_idx++) {
        entry->tiles[tile_idx] = chosen[tile_idx];
      }
      entry->n = n_chosen;
      entry->weight = weight;
      (*out_n)++;
    } else {
      *overflow = true;
    }
    return;
  }
  for (int ml = start_ml; ml < ld_size; ml++) {
    const int avail = counts[ml];
    if (avail == 0) {
      continue;
    }
    const int max_take = avail < k_left ? avail : k_left;
    for (int take = 1; take <= max_take; take++) {
      const int64_t next_weight = weight * peg_binomial(avail, take);
      for (int tile_idx = 0; tile_idx < take; tile_idx++) {
        chosen[n_chosen + tile_idx] = (MachineLetter)ml;
      }
      cpeg_enum_rec(counts, ld_size, ml + 1, k_left - take, next_weight, chosen,
                    n_chosen + take, out, out_n, cap, overflow);
    }
  }
}

// Enumerate every distinct k-submultiset of counts[0..ld_size) into out (up to
// cap), each carrying its multiset weight. Returns the number produced. Letters
// are emitted in increasing order so each multiset appears exactly once.
static int cpeg_enum_submultisets(const int *counts, int ld_size, int k,
                                  CpegMultiset *out, int cap, bool *overflow) {
  int out_n = 0;
  MachineLetter chosen[RACK_SIZE + PEG_MAX_BAG];
  *overflow = false;
  cpeg_enum_rec(counts, ld_size, 0, k, 1, chosen, 0, out, &out_n, cap,
                overflow);
  return out_n;
}

bool cpeg_submultiset_capacity_overflows(const int *counts, int ld_size, int k,
                                         int cap) {
  CpegMultiset *entries =
      cap > 0 ? malloc_or_die((size_t)cap * sizeof(*entries)) : NULL;
  bool overflow = false;
  cpeg_enum_submultisets(counts, ld_size, k, entries, cap, &overflow);
  free(entries);
  return overflow;
}

// Solver-wide scratch, carried through the recursion so nothing is allocated
// per node. Move lists and incremental undos are indexed by recursion depth;
// scratch games remain for the root template draw path. A child recursion uses
// depth d+1, so a depth's slots stay stable across the child's whole subtree.
typedef struct CpegPreCtx {
  const LetterDistribution *ld;
  int ld_size;
  int mover_idx;
  bool allow_exchanges;
  bool capacity_exceeded;
  bool complete;
  int64_t deadline_ns;
  MoveList *movelists[CPEG_MAX_DEPTH];
  Game *scratch[CPEG_MAX_DEPTH];
  MoveUndo *move_undos[CPEG_MAX_DEPTH];
  Bag *bag_undos[CPEG_MAX_DEPTH];
  // Shared empty-bag-leaf endgame buffers (leaves never nest, so one set is
  // reused across every leaf).
  MoveList *eg_mover;
  MoveList *eg_reply;
  MoveUndo *eg_undo;
} CpegPreCtx;

static bool cpeg_interval_should_cancel(CpegPreCtx *ctx) {
  if (!ctx->complete) {
    return true;
  }
  if (ctx->deadline_ns != 0 && ctimer_monotonic_ns() >= ctx->deadline_ns) {
    ctx->complete = false;
    return true;
  }
  return false;
}

// The non-board portion of a CPEG chance branch. The bag itself is saved in
// ctx->bag_undos[depth]: bag_add_letter is not a valid inverse for an exact
// undo because it advances the PRNG and may permute the remaining tiles.
typedef struct CpegBranchUndo {
  Rack rack;
  int player_idx;
  int player_on_turn_idx;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
} CpegBranchUndo;

static MoveList *cpeg_get_movelist(CpegPreCtx *ctx, int depth) {
  if (ctx->movelists[depth] == NULL) {
    ctx->movelists[depth] = move_list_create(CPEG_MOVE_LIST_CAP);
  }
  return ctx->movelists[depth];
}

static MoveUndo *cpeg_get_move_undo(CpegPreCtx *ctx, int depth) {
  if (ctx->move_undos[depth] == NULL) {
    ctx->move_undos[depth] = malloc_or_die(sizeof(MoveUndo));
  }
  return ctx->move_undos[depth];
}

static void cpeg_save_branch(CpegPreCtx *ctx, const Game *game, int player_idx,
                             int depth, CpegBranchUndo *undo) {
  const Bag *bag = game_get_bag(game);
  if (ctx->bag_undos[depth] == NULL) {
    ctx->bag_undos[depth] = bag_duplicate(bag);
  } else {
    bag_copy(ctx->bag_undos[depth], bag);
  }
  rack_copy(&undo->rack, player_get_rack(game_get_player(game, player_idx)));
  undo->player_idx = player_idx;
  undo->player_on_turn_idx = game_get_player_on_turn_index(game);
  undo->consecutive_scoreless_turns =
      game_get_consecutive_scoreless_turns(game);
  undo->game_end_reason = game_get_game_end_reason(game);
}

static void cpeg_restore_branch(const CpegPreCtx *ctx, Game *game, int depth,
                                const CpegBranchUndo *undo) {
  bag_copy(game_get_bag(game), ctx->bag_undos[depth]);
  rack_copy(player_get_rack(game_get_player(game, undo->player_idx)),
            &undo->rack);
  game_set_player_on_turn_index(game, undo->player_on_turn_idx);
  game_set_consecutive_scoreless_turns(game, undo->consecutive_scoreless_turns);
  game_set_game_end_reason(game, undo->game_end_reason);
}

// Copy src into the depth-indexed scratch game (allocating it on first use) and
// return it.
static Game *cpeg_child_game(CpegPreCtx *ctx, int depth, const Game *src) {
  if (ctx->scratch[depth] == NULL) {
    ctx->scratch[depth] = game_duplicate(src);
  } else {
    game_copy(ctx->scratch[depth], src);
  }
  return ctx->scratch[depth];
}

static double cpeg_value(CpegPreCtx *ctx, Game *game, int scoreless, int depth);
static CpegInterval cpeg_value_interval(CpegPreCtx *ctx, Game *game,
                                        int scoreless, int depth);

// Value to the on-turn player of committing to placement `move`: its score
// minus the draw-averaged value of the resulting opponent-to-move position
// (negamax -- the child value is from the opponent's perspective).
static double cpeg_eval_place(CpegPreCtx *ctx, Game *game, const Move *move,
                              int depth) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  const int tiles_played = move_get_tiles_played(move);
  const int k_drawn = tiles_played < bag_count ? tiles_played : bag_count;
  const int score = equity_to_int(move_get_score(move));

  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, k_drawn,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    return 0.0;
  }

  MoveUndo *move_undo = cpeg_get_move_undo(ctx, depth);
  play_move_incremental(move, game, move_undo);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  double weighted_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    const CpegMultiset *draw = &draws[draw_idx];
    Bag *child_bag = game_get_bag(game);
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int i = 0; i < draw->n; i++) {
      bag_draw_letter(child_bag, draw->tiles[i], on_turn);
      rack_add_letter(mover_rack, draw->tiles[i]);
    }
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    weighted_sum += (double)draw->weight * cpeg_value(ctx, game, 0, depth + 1);
    weight_total += draw->weight;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  unplay_move_incremental(game, move_undo);
  const double child_expectation =
      weight_total > 0 ? weighted_sum / (double)weight_total : 0.0;
  return (double)score - child_expectation;
}

static CpegInterval cpeg_eval_place_interval(CpegPreCtx *ctx, Game *game,
                                             const Move *move, int depth) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  const int tiles_played = move_get_tiles_played(move);
  const int k_drawn = tiles_played < bag_count ? tiles_played : bag_count;
  const int score = equity_to_int(move_get_score(move));

  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, k_drawn,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }

  MoveUndo *move_undo = cpeg_get_move_undo(ctx, depth);
  play_move_incremental(move, game, move_undo);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  double weighted_lower_sum = 0.0;
  double weighted_upper_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      unplay_move_incremental(game, move_undo);
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const CpegMultiset *draw = &draws[draw_idx];
    Bag *child_bag = game_get_bag(game);
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int tile_idx = 0; tile_idx < draw->n; tile_idx++) {
      bag_draw_letter(child_bag, draw->tiles[tile_idx], on_turn);
      rack_add_letter(mover_rack, draw->tiles[tile_idx]);
    }
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const CpegInterval child = cpeg_value_interval(ctx, game, 0, depth + 1);
    if (!ctx->complete) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      unplay_move_incremental(game, move_undo);
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const double weight = (double)draw->weight;
    weighted_lower_sum =
        cpeg_down_add(weighted_lower_sum, cpeg_down_mul(weight, child.lo));
    weighted_upper_sum =
        cpeg_up_add(weighted_upper_sum, cpeg_up_mul(weight, child.hi));
    weight_total += draw->weight;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  unplay_move_incremental(game, move_undo);
  if (weight_total == 0) {
    return (CpegInterval){.lo = (double)score, .hi = (double)score};
  }
  const double denominator = (double)weight_total;
  const CpegInterval child_expectation = {
      .lo = cpeg_down_div(weighted_lower_sum, denominator),
      .hi = cpeg_up_div(weighted_upper_sum, denominator),
  };
  return (CpegInterval){
      .lo = cpeg_down_add((double)score, -child_expectation.hi),
      .hi = cpeg_up_add((double)score, -child_expectation.lo),
  };
}

// Value to the on-turn player of a scoreless move: a pass (exch_n == 0, no
// draw) or an exchange of exch_tiles[0..exch_n) (draw exch_n from the bag, then
// return the exchanged tiles to the bag). Scores 0; the child value is negated
// (negamax). Two consecutive scoreless plies terminate the game at the current
// spread, so this returns 0 when it would be the second in a row.
static double cpeg_eval_scoreless(CpegPreCtx *ctx, Game *game,
                                  const MachineLetter *exch_tiles, int exch_n,
                                  int scoreless, int depth) {
  if (scoreless + 1 >= CPEG_SCORELESS_CAP) {
    return 0.0;
  }
  const int on_turn = game_get_player_on_turn_index(game);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  if (exch_n == 0) {
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const double value = cpeg_value(ctx, game, scoreless + 1, depth + 1);
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    return 0.0 - value;
  }

  const Bag *bag = game_get_bag(game);
  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, exch_n,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    return 0.0;
  }

  double weighted_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    const CpegMultiset *draw = &draws[draw_idx];
    Bag *child_bag = game_get_bag(game);
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int i = 0; i < draw->n; i++) {
      bag_draw_letter(child_bag, draw->tiles[i], on_turn);
      rack_add_letter(mover_rack, draw->tiles[i]);
    }
    for (int i = 0; i < exch_n; i++) {
      rack_take_letter(mover_rack, exch_tiles[i]);
      bag_add_letter(child_bag, exch_tiles[i], on_turn);
    }
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    weighted_sum +=
        (double)draw->weight * cpeg_value(ctx, game, scoreless + 1, depth + 1);
    weight_total += draw->weight;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  const double child_expectation =
      weight_total > 0 ? weighted_sum / (double)weight_total : 0.0;
  return 0.0 - child_expectation;
}

static CpegInterval
cpeg_eval_scoreless_interval(CpegPreCtx *ctx, Game *game,
                             const MachineLetter *exch_tiles, int exch_n,
                             int scoreless, int depth) {
  if (scoreless + 1 >= CPEG_SCORELESS_CAP) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }
  const int on_turn = game_get_player_on_turn_index(game);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  if (exch_n == 0) {
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const CpegInterval child =
        cpeg_value_interval(ctx, game, scoreless + 1, depth + 1);
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    if (!ctx->complete) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    return (CpegInterval){.lo = -child.hi, .hi = -child.lo};
  }

  const Bag *bag = game_get_bag(game);
  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, exch_n,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }

  double weighted_lower_sum = 0.0;
  double weighted_upper_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const CpegMultiset *draw = &draws[draw_idx];
    Bag *child_bag = game_get_bag(game);
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int tile_idx = 0; tile_idx < draw->n; tile_idx++) {
      bag_draw_letter(child_bag, draw->tiles[tile_idx], on_turn);
      rack_add_letter(mover_rack, draw->tiles[tile_idx]);
    }
    for (int tile_idx = 0; tile_idx < exch_n; tile_idx++) {
      rack_take_letter(mover_rack, exch_tiles[tile_idx]);
      bag_add_letter(child_bag, exch_tiles[tile_idx], on_turn);
    }
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const CpegInterval child =
        cpeg_value_interval(ctx, game, scoreless + 1, depth + 1);
    if (!ctx->complete) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const double weight = (double)draw->weight;
    weighted_lower_sum =
        cpeg_down_add(weighted_lower_sum, cpeg_down_mul(weight, child.lo));
    weighted_upper_sum =
        cpeg_up_add(weighted_upper_sum, cpeg_up_mul(weight, child.hi));
    weight_total += draw->weight;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  if (weight_total == 0) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }
  const double denominator = (double)weight_total;
  const CpegInterval child_expectation = {
      .lo = cpeg_down_div(weighted_lower_sum, denominator),
      .hi = cpeg_up_div(weighted_upper_sum, denominator),
  };
  return (CpegInterval){.lo = -child_expectation.hi,
                        .hi = -child_expectation.lo};
}

// Enumerate the mover's distinct exchange multisets (1..min(rack, bag) tiles)
// into out; returns the count. Shared by the recursion and the root candidate
// list so both search the same exchange set.
static int cpeg_enum_exchanges(int ld_size, const Rack *rack, int bag_count,
                               CpegMultiset *out, int cap, bool *overflow) {
  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    counts[ml] = rack_get_letter(rack, (MachineLetter)ml);
  }
  const int rack_size = rack_get_total_letters(rack);
  const int max_swap = rack_size < bag_count ? rack_size : bag_count;
  int total = 0;
  *overflow = false;
  for (int swap = 1; swap <= max_swap; swap++) {
    bool size_overflow = false;
    total += cpeg_enum_submultisets(counts, ld_size, swap, out + total,
                                    cap - total, &size_overflow);
    if (size_overflow) {
      *overflow = true;
      break;
    }
  }
  return total;
}

// Expectiminimax value for the on-turn player: their optimal expected spread
// (their points minus the opponent's) over the remaining game, in points. The
// bag is known here (both racks fixed inside a world); when it is empty the
// value is the two-ply Crossplay endgame.
static double cpeg_value(CpegPreCtx *ctx, Game *game, int scoreless,
                         int depth) {
  if (ctx->capacity_exceeded) {
    return 0.0;
  }
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  if (bag_count == 0) {
    CpegResult leaf;
    return (double)cpeg_endgame_core(game, ctx->eg_mover, ctx->eg_reply,
                                     ctx->eg_undo, &leaf);
  }
  if (depth >= CPEG_MAX_DEPTH - 1) {
    // Unreachable given the scoreless cap and the monotone bag; a safety net.
    return 0.0;
  }

  MoveList *move_list = cpeg_get_movelist(ctx, depth);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);

  double best = 0.0;
  bool have_best = false;
  bool any_placement = false;
  const int count = move_list_get_count(move_list);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    any_placement = true;
    const double value = cpeg_eval_place(ctx, game, move, depth);
    if (!have_best || value > best) {
      best = value;
      have_best = true;
    }
  }

  // Pass is searched only when there is no legal placement.
  if (!any_placement) {
    const double value =
        cpeg_eval_scoreless(ctx, game, NULL, 0, scoreless, depth);
    if (!have_best || value > best) {
      best = value;
      have_best = true;
    }
  }

  if (ctx->allow_exchanges) {
    const Rack *mover_rack = player_get_rack(
        game_get_player(game, game_get_player_on_turn_index(game)));
    CpegMultiset exchanges[CPEG_ENUM_CAP];
    bool overflow = false;
    const int n_exch = cpeg_enum_exchanges(ctx->ld_size, mover_rack, bag_count,
                                           exchanges, CPEG_ENUM_CAP, &overflow);
    if (overflow) {
      ctx->capacity_exceeded = true;
      return 0.0;
    }
    for (int exch_idx = 0; exch_idx < n_exch; exch_idx++) {
      const double value =
          cpeg_eval_scoreless(ctx, game, exchanges[exch_idx].tiles,
                              exchanges[exch_idx].n, scoreless, depth);
      if (!have_best || value > best) {
        best = value;
        have_best = true;
      }
    }
  }

  return have_best ? best : 0.0;
}

static CpegInterval cpeg_value_interval(CpegPreCtx *ctx, Game *game,
                                        int scoreless, int depth) {
  if (cpeg_interval_should_cancel(ctx)) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }
  if (ctx->capacity_exceeded) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  if (bag_count == 0) {
    CpegResult leaf;
    const int swing = cpeg_endgame_core(game, ctx->eg_mover, ctx->eg_reply,
                                        ctx->eg_undo, &leaf);
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    return (CpegInterval){.lo = (double)swing, .hi = (double)swing};
  }
  if (depth >= CPEG_MAX_DEPTH - 1) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }

  MoveList *move_list = cpeg_get_movelist(ctx, depth);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);

  CpegInterval best = {.lo = 0.0, .hi = 0.0};
  bool have_best = false;
  bool any_placement = false;
  const int count = move_list_get_count(move_list);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    any_placement = true;
    const CpegInterval value = cpeg_eval_place_interval(ctx, game, move, depth);
    if (!ctx->complete) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    if (!have_best || value.lo > best.lo) {
      best.lo = value.lo;
    }
    if (!have_best || value.hi > best.hi) {
      best.hi = value.hi;
    }
    have_best = true;
  }

  if (!any_placement) {
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const CpegInterval value =
        cpeg_eval_scoreless_interval(ctx, game, NULL, 0, scoreless, depth);
    if (!ctx->complete) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    if (!have_best || value.lo > best.lo) {
      best.lo = value.lo;
    }
    if (!have_best || value.hi > best.hi) {
      best.hi = value.hi;
    }
    have_best = true;
  }

  if (ctx->allow_exchanges) {
    const Rack *mover_rack = player_get_rack(
        game_get_player(game, game_get_player_on_turn_index(game)));
    CpegMultiset exchanges[CPEG_ENUM_CAP];
    bool overflow = false;
    const int n_exch = cpeg_enum_exchanges(ctx->ld_size, mover_rack, bag_count,
                                           exchanges, CPEG_ENUM_CAP, &overflow);
    if (overflow) {
      ctx->capacity_exceeded = true;
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    for (int exch_idx = 0; exch_idx < n_exch; exch_idx++) {
      if (cpeg_interval_should_cancel(ctx)) {
        return (CpegInterval){.lo = 0.0, .hi = 0.0};
      }
      const CpegInterval value =
          cpeg_eval_scoreless_interval(ctx, game, exchanges[exch_idx].tiles,
                                       exchanges[exch_idx].n, scoreless, depth);
      if (!ctx->complete) {
        return (CpegInterval){.lo = 0.0, .hi = 0.0};
      }
      if (!have_best || value.lo > best.lo) {
        best.lo = value.lo;
      }
      if (!have_best || value.hi > best.hi) {
        best.hi = value.hi;
      }
      have_best = true;
    }
  }

  return have_best ? best : (CpegInterval){.lo = 0.0, .hi = 0.0};
}

// Tiles not visible to the mover: full distribution minus mover's rack minus
// the board. Returns the total count.
static int cpeg_compute_unseen(const Game *game, int mover_idx,
                               int unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = ld_get_dist(ld, (MachineLetter)ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= rack_get_letter(mover_rack, (MachineLetter)ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      const MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else if (unseen[ml] > 0) {
        unseen[ml]--;
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

static void cpeg_ctx_init(CpegPreCtx *ctx, const LetterDistribution *ld,
                          int ld_size, int mover_idx, bool allow_exchanges,
                          int64_t deadline_ns) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->ld = ld;
  ctx->ld_size = ld_size;
  ctx->mover_idx = mover_idx;
  ctx->allow_exchanges = allow_exchanges;
  ctx->complete = true;
  ctx->deadline_ns = deadline_ns;
  ctx->eg_mover = move_list_create(CPEG_MOVE_LIST_CAP);
  ctx->eg_reply = move_list_create(CPEG_MOVE_LIST_CAP);
  ctx->eg_undo = malloc_or_die(sizeof(MoveUndo));
}

static void cpeg_ctx_destroy(CpegPreCtx *ctx) {
  for (int depth = 0; depth < CPEG_MAX_DEPTH; depth++) {
    if (ctx->movelists[depth] != NULL) {
      move_list_destroy(ctx->movelists[depth]);
    }
    if (ctx->scratch[depth] != NULL) {
      game_destroy(ctx->scratch[depth]);
    }
    free(ctx->move_undos[depth]);
    if (ctx->bag_undos[depth] != NULL) {
      bag_destroy(ctx->bag_undos[depth]);
    }
  }
  free(ctx->eg_undo);
  move_list_destroy(ctx->eg_reply);
  move_list_destroy(ctx->eg_mover);
}

// A root candidate (the mover's fixed first move) with its expected-spread
// accumulator over the enumerated worlds.
typedef struct CpegRootCand {
  int kind; // 0 = placement, 1 = pass, 2 = exchange
  Move move;
  MachineLetter exch_tiles[RACK_SIZE];
  int exch_n;
  int score;
  double weighted_spread; // sum over worlds of world_weight * in-world value
} CpegRootCand;

// Build the immutable post-placement state shared by every world for one root
// candidate. The board, mover leave, side to move, and cross-sets are identical
// across worlds; only the bag and opponent rack vary.
static Game *cpeg_build_root_template(const Game *root_game, const Move *move) {
  Game *template_game = game_duplicate(root_game);
  play_move_without_drawing_tiles(move, template_game);
  game_gen_all_cross_sets(template_game);
  return template_game;
}

// Value to the root mover of a placement that has already been applied to
// post_place_game. This is the root-only counterpart of cpeg_eval_place: it
// enumerates the same draws, but copies the immutable candidate template
// instead of replaying the candidate and rebuilding its post-move state for
// every draw in every world.
static double cpeg_eval_post_place(CpegPreCtx *ctx, const Game *post_place_game,
                                   int tiles_played, int score, int depth) {
  const Bag *bag = game_get_bag(post_place_game);
  const int bag_count = bag_get_letters(bag);
  const int k_drawn = tiles_played < bag_count ? tiles_played : bag_count;

  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, k_drawn,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    return 0.0;
  }

  double weighted_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    const CpegMultiset *draw = &draws[draw_idx];
    Game *child = cpeg_child_game(ctx, depth, post_place_game);
    Bag *child_bag = game_get_bag(child);
    Rack *mover_rack = player_get_rack(game_get_player(child, ctx->mover_idx));
    for (int tile_idx = 0; tile_idx < draw->n; tile_idx++) {
      bag_draw_letter(child_bag, draw->tiles[tile_idx], ctx->mover_idx);
      rack_add_letter(mover_rack, draw->tiles[tile_idx]);
    }
    game_set_consecutive_scoreless_turns(child, 0);
    game_set_game_end_reason(child, GAME_END_REASON_NONE);
    weighted_sum += (double)draw->weight * cpeg_value(ctx, child, 0, depth + 1);
    weight_total += draw->weight;
  }
  const double child_expectation =
      weight_total > 0 ? weighted_sum / (double)weight_total : 0.0;
  return (double)score - child_expectation;
}

static CpegInterval cpeg_eval_post_place_interval(CpegPreCtx *ctx,
                                                  const Game *post_place_game,
                                                  int tiles_played, int score,
                                                  int depth) {
  const Bag *bag = game_get_bag(post_place_game);
  const int bag_count = bag_get_letters(bag);
  const int k_drawn = tiles_played < bag_count ? tiles_played : bag_count;

  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ctx->ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  CpegMultiset draws[CPEG_ENUM_CAP];
  bool overflow = false;
  const int n_draws = cpeg_enum_submultisets(counts, ctx->ld_size, k_drawn,
                                             draws, CPEG_ENUM_CAP, &overflow);
  if (overflow) {
    ctx->capacity_exceeded = true;
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }

  double weighted_lower_sum = 0.0;
  double weighted_upper_sum = 0.0;
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const CpegMultiset *draw = &draws[draw_idx];
    Game *child = cpeg_child_game(ctx, depth, post_place_game);
    Bag *child_bag = game_get_bag(child);
    Rack *mover_rack = player_get_rack(game_get_player(child, ctx->mover_idx));
    for (int tile_idx = 0; tile_idx < draw->n; tile_idx++) {
      bag_draw_letter(child_bag, draw->tiles[tile_idx], ctx->mover_idx);
      rack_add_letter(mover_rack, draw->tiles[tile_idx]);
    }
    game_set_consecutive_scoreless_turns(child, 0);
    game_set_game_end_reason(child, GAME_END_REASON_NONE);
    const CpegInterval child_value =
        cpeg_value_interval(ctx, child, 0, depth + 1);
    if (!ctx->complete) {
      return (CpegInterval){.lo = 0.0, .hi = 0.0};
    }
    const double weight = (double)draw->weight;
    weighted_lower_sum = cpeg_down_add(weighted_lower_sum,
                                       cpeg_down_mul(weight, child_value.lo));
    weighted_upper_sum =
        cpeg_up_add(weighted_upper_sum, cpeg_up_mul(weight, child_value.hi));
    weight_total += draw->weight;
  }
  if (weight_total == 0) {
    return (CpegInterval){.lo = (double)score, .hi = (double)score};
  }
  const double denominator = (double)weight_total;
  const CpegInterval child_expectation = {
      .lo = cpeg_down_div(weighted_lower_sum, denominator),
      .hi = cpeg_up_div(weighted_upper_sum, denominator),
  };
  return (CpegInterval){
      .lo = cpeg_down_add((double)score, -child_expectation.hi),
      .hi = cpeg_up_add((double)score, -child_expectation.lo),
  };
}

// In-world value to the mover of committing to one root candidate (its first
// move). The world_game is on the mover's turn with both racks and the bag set.
static double cpeg_eval_root_scoreless(CpegPreCtx *ctx, Game *world_game,
                                       const CpegRootCand *cand) {
  if (cand->kind == 1) {
    return cpeg_eval_scoreless(ctx, world_game, NULL, 0, /*scoreless=*/0,
                               /*depth=*/0);
  }
  return cpeg_eval_scoreless(ctx, world_game, cand->exch_tiles, cand->exch_n,
                             /*scoreless=*/0, /*depth=*/0);
}

static CpegInterval
cpeg_eval_root_scoreless_interval(CpegPreCtx *ctx, Game *world_game,
                                  const CpegRootCand *cand) {
  if (cand->kind == 1) {
    return cpeg_eval_scoreless_interval(ctx, world_game, NULL, 0,
                                        /*scoreless=*/0, /*depth=*/0);
  }
  return cpeg_eval_scoreless_interval(ctx, world_game, cand->exch_tiles,
                                      cand->exch_n, /*scoreless=*/0,
                                      /*depth=*/0);
}

// Per-pool-worker scratch. Pool jobs are independent (candidate, world) pairs;
// worker_idx selects one context and mutable game so no hot-path locking is
// needed.
typedef struct CpegWorker {
  CpegPreCtx ctx;
  Game *world_game;
} CpegWorker;

typedef struct CpegRootJob {
  CpegWorker *workers;
  const Game *source_game;
  const CpegMultiset *world;
  const int *unseen;
  int ld_size;
  int opp_idx;
  const CpegRootCand *cand;
  double value;
  CpegInterval interval;
  bool complete;
} CpegRootJob;

static void cpeg_set_world(Game *world_game, const Game *source_game,
                           const CpegMultiset *world, const int *unseen,
                           int ld_size, int opp_idx) {
  game_copy(world_game, source_game);
  // The world's bag is `world`; the opponent holds the rest of the unseen.
  bag_set_to_tiles(game_get_bag(world_game), world->tiles, world->n);
  Rack *opp_rack = player_get_rack(game_get_player(world_game, opp_idx));
  rack_reset(opp_rack);
  int remaining[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    remaining[ml] = unseen[ml];
  }
  for (int tile_idx = 0; tile_idx < world->n; tile_idx++) {
    remaining[world->tiles[tile_idx]]--;
  }
  for (int ml = 0; ml < ld_size; ml++) {
    for (int tile_idx = 0; tile_idx < remaining[ml]; tile_idx++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

static void cpeg_root_job_run(void *arg, int worker_idx) {
  CpegRootJob *job = (CpegRootJob *)arg;
  CpegWorker *worker = &job->workers[worker_idx];
  cpeg_set_world(worker->world_game, job->source_game, job->world, job->unseen,
                 job->ld_size, job->opp_idx);
  if (job->cand->kind == 0) {
    job->value = cpeg_eval_post_place(&worker->ctx, worker->world_game,
                                      move_get_tiles_played(&job->cand->move),
                                      job->cand->score, /*depth=*/0);
  } else {
    job->value =
        cpeg_eval_root_scoreless(&worker->ctx, worker->world_game, job->cand);
  }
}

static void cpeg_root_interval_job_run(void *arg, int worker_idx) {
  CpegRootJob *job = (CpegRootJob *)arg;
  CpegWorker *worker = &job->workers[worker_idx];
  worker->ctx.complete = true;
  cpeg_set_world(worker->world_game, job->source_game, job->world, job->unseen,
                 job->ld_size, job->opp_idx);
  if (job->cand->kind == 0) {
    job->interval = cpeg_eval_post_place_interval(
        &worker->ctx, worker->world_game,
        move_get_tiles_played(&job->cand->move), job->cand->score, /*depth=*/0);
  } else {
    job->interval = cpeg_eval_root_scoreless_interval(
        &worker->ctx, worker->world_game, job->cand);
  }
  job->complete = worker->ctx.complete;
}

// Descending sort: expected spread, then first-move score, then label order so
// the ranking is deterministic. Ties on all three keep insertion order.
static int cpeg_cand_compare(const void *lhs, const void *rhs) {
  const CpegPreCand *a = lhs;
  const CpegPreCand *b = rhs;
  if (a->expected_spread > b->expected_spread) {
    return -1;
  }
  if (a->expected_spread < b->expected_spread) {
    return 1;
  }
  if (a->score != b->score) {
    return a->score > b->score ? -1 : 1;
  }
  return strcmp(a->label, b->label);
}

int cpeg_solve_pre_endgame(Game *game, int bag, bool allow_exchanges,
                           int num_threads, CpegPreResult *out) {
  memset(out, 0, sizeof(*out));

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Board *board = game_get_board(game);
  game_gen_all_cross_sets(game);
  board_set_cross_sets_valid(board, true);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;

  int unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = cpeg_compute_unseen(game, mover_idx, unseen);
  const int opp_size = total_unseen - bag;
  if (opp_size < 0 || opp_size > RACK_SIZE) {
    return -1;
  }

  // Root candidates (the mover's fixed first moves). Placements come from
  // move generation on the root board+rack; pass and exchanges are synthesized.
  MoveList *root_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  const MoveGenArgs root_args = {
      .game = game,
      .move_list = root_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&root_args);

  const int root_count = move_list_get_count(root_moves);
  if (root_count > CPEG_MOVE_LIST_CAP) {
    move_list_destroy(root_moves);
    return -1;
  }
  CpegRootCand *cands =
      malloc_or_die((size_t)(root_count + 1 + CPEG_ENUM_CAP) * sizeof(*cands));
  int n_cands = 0;
  bool any_placement = false;
  for (int move_idx = 0; move_idx < root_count; move_idx++) {
    const Move *move = move_list_get_move(root_moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    any_placement = true;
    CpegRootCand *cand = &cands[n_cands++];
    cand->kind = 0;
    move_copy(&cand->move, move);
    cand->exch_n = 0;
    cand->score = equity_to_int(move_get_score(move));
    cand->weighted_spread = 0.0;
  }
  if (!any_placement) {
    CpegRootCand *cand = &cands[n_cands++];
    cand->kind = 1;
    cand->exch_n = 0;
    cand->score = 0;
    cand->weighted_spread = 0.0;
  }
  if (allow_exchanges) {
    const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
    CpegMultiset exchanges[CPEG_ENUM_CAP];
    bool overflow = false;
    const int n_exch = cpeg_enum_exchanges(ld_size, mover_rack, bag, exchanges,
                                           CPEG_ENUM_CAP, &overflow);
    if (overflow) {
      free(cands);
      move_list_destroy(root_moves);
      return -1;
    }
    for (int exch_idx = 0; exch_idx < n_exch; exch_idx++) {
      CpegRootCand *cand = &cands[n_cands++];
      cand->kind = 2;
      cand->exch_n = exchanges[exch_idx].n;
      for (int i = 0; i < cand->exch_n; i++) {
        cand->exch_tiles[i] = exchanges[exch_idx].tiles[i];
      }
      cand->score = 0;
      cand->weighted_spread = 0.0;
    }
  }
  if (n_cands > CPEG_MAX_PRE_CANDS) {
    free(cands);
    move_list_destroy(root_moves);
    return -1;
  }

  // Enumerate the opponent-rack worlds (distinct bag submultisets of the unseen
  // tiles). Each world is a perfect-information position; the workers evaluate
  // every candidate in every world and average by world weight.
  CpegMultiset *worlds = malloc_or_die(CPEG_WORLD_CAP * sizeof(*worlds));
  bool world_overflow = false;
  const int n_worlds = cpeg_enum_submultisets(unseen, ld_size, bag, worlds,
                                              CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow) {
    free(worlds);
    free(cands);
    move_list_destroy(root_moves);
    return -1;
  }

  // Build each placement once. These games are immutable after construction
  // and shared by every (candidate, world) job for that candidate.
  Game **templates = calloc_or_die((size_t)n_cands, sizeof(*templates));
  for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
    if (cands[cand_idx].kind == 0) {
      templates[cand_idx] =
          cpeg_build_root_template(game, &cands[cand_idx].move);
    }
  }

  const int n_threads = num_threads < 1 ? 1 : num_threads;
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 0) : NULL;
  if (pool != NULL) {
    // A single exact pre-endgame job may legitimately search for longer than
    // the generic pool watchdog interval on larger bags.
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  // Pool workers use [0, n_threads); the submitting thread helps at index
  // n_threads. Single-threaded inline execution uses index 0.
  const int n_scratch = pool != NULL ? n_threads + 1 : 1;
  CpegWorker *workers = malloc_or_die((size_t)n_scratch * sizeof(*workers));
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    CpegWorker *worker = &workers[worker_idx];
    cpeg_ctx_init(&worker->ctx, ld, ld_size, mover_idx, allow_exchanges,
                  /*deadline_ns=*/0);
    worker->world_game = game_duplicate(game);
  }

  const int n_jobs = n_cands * n_worlds;
  CpegRootJob *jobs = malloc_or_die((size_t)n_jobs * sizeof(*jobs));
  void **job_ptrs = malloc_or_die((size_t)n_jobs * sizeof(*job_ptrs));
  for (int world_idx = 0; world_idx < n_worlds; world_idx++) {
    for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
      const int job_idx = world_idx * n_cands + cand_idx;
      CpegRootJob *job = &jobs[job_idx];
      job->workers = workers;
      job->source_game =
          templates[cand_idx] != NULL ? templates[cand_idx] : game;
      job->world = &worlds[world_idx];
      job->unseen = unseen;
      job->ld_size = ld_size;
      job->opp_idx = opp_idx;
      job->cand = &cands[cand_idx];
      job->value = 0.0;
      job_ptrs[job_idx] = job;
    }
  }
  const int helper_worker_idx = pool != NULL ? n_threads : 0;
  peg_pool_submit_and_wait(pool, cpeg_root_job_run, job_ptrs, n_jobs,
                           helper_worker_idx);

  bool search_capacity_exceeded = false;
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    if (workers[worker_idx].ctx.capacity_exceeded) {
      search_capacity_exceeded = true;
      break;
    }
  }

  // Preserve the old deterministic floating-point reduction order: accumulate
  // each former contiguous world slice independently, then add slices in
  // worker order. Job execution order is deliberately irrelevant.
  int n_reducers = n_threads;
  if (n_reducers > n_worlds) {
    n_reducers = n_worlds < 1 ? 1 : n_worlds;
  }
  double *accum =
      calloc_or_die((size_t)n_reducers * (size_t)n_cands, sizeof(*accum));
  int64_t total_world_weight = 0;
  for (int reducer_idx = 0;
       reducer_idx < n_reducers && !search_capacity_exceeded; reducer_idx++) {
    const int world_start = (int)((int64_t)reducer_idx * n_worlds / n_reducers);
    const int world_end =
        (int)((int64_t)(reducer_idx + 1) * n_worlds / n_reducers);
    for (int world_idx = world_start; world_idx < world_end; world_idx++) {
      total_world_weight += worlds[world_idx].weight;
      for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
        const int job_idx = world_idx * n_cands + cand_idx;
        accum[reducer_idx * n_cands + cand_idx] +=
            (double)worlds[world_idx].weight * jobs[job_idx].value;
      }
    }
  }
  for (int reducer_idx = 0;
       reducer_idx < n_reducers && !search_capacity_exceeded; reducer_idx++) {
    for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
      cands[cand_idx].weighted_spread +=
          accum[reducer_idx * n_cands + cand_idx];
    }
  }

  // Materialize ranked output.
  const double weight_denom =
      total_world_weight > 0 ? (double)total_world_weight : 1.0;
  int emitted = 0;
  for (int cand_idx = 0; cand_idx < n_cands && !search_capacity_exceeded;
       cand_idx++) {
    const CpegRootCand *cand = &cands[cand_idx];
    CpegPreCand *slot = &out->cands[emitted++];
    slot->score = cand->score;
    slot->expected_spread = cand->weighted_spread / weight_denom;
    if (cand->kind == 0) {
      cpeg_render_move(slot->label, sizeof(slot->label), board, &cand->move,
                       ld);
    } else if (cand->kind == 1) {
      memcpy(slot->label, "pass", sizeof("pass"));
    } else {
      StringBuilder *builder = string_builder_create();
      string_builder_add_string(builder, "exch:");
      for (int i = 0; i < cand->exch_n; i++) {
        string_builder_add_string(builder,
                                  ld->ld_ml_to_hl[cand->exch_tiles[i]]);
      }
      const char *rendered = string_builder_peek(builder);
      const size_t len = string_length(rendered);
      const size_t copy_len =
          len < sizeof(slot->label) - 1 ? len : sizeof(slot->label) - 1;
      memcpy(slot->label, rendered, copy_len);
      slot->label[copy_len] = '\0';
      string_builder_destroy(builder);
    }
  }
  out->count = emitted;
  qsort(out->cands, (size_t)out->count, sizeof(out->cands[0]),
        cpeg_cand_compare);

  free(accum);
  free(job_ptrs);
  free(jobs);
  peg_pool_destroy(pool);
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    game_destroy(workers[worker_idx].world_game);
    cpeg_ctx_destroy(&workers[worker_idx].ctx);
  }
  free(workers);
  for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
    if (templates[cand_idx] != NULL) {
      game_destroy(templates[cand_idx]);
    }
  }
  free(templates);
  free(worlds);
  free(cands);
  move_list_destroy(root_moves);
  return search_capacity_exceeded ? -1 : out->count;
}

typedef struct CpegScheduledWorld {
  CpegMultiset multiset;
  int generation_index;
} CpegScheduledWorld;

typedef struct CpegScheduledPair {
  int candidate_idx;
  int world_idx;
} CpegScheduledPair;

static void cpeg_render_root_candidate(char dest[CPEG_MOVE_STR_LEN],
                                       const CpegRootCand *candidate,
                                       const Board *board,
                                       const LetterDistribution *ld) {
  if (candidate->kind == 0) {
    cpeg_render_move(dest, CPEG_MOVE_STR_LEN, board, &candidate->move, ld);
    return;
  }
  if (candidate->kind == 1) {
    memcpy(dest, "pass", sizeof("pass"));
    return;
  }

  StringBuilder *builder = string_builder_create();
  string_builder_add_string(builder, "exch:");
  for (int tile_idx = 0; tile_idx < candidate->exch_n; tile_idx++) {
    string_builder_add_string(builder,
                              ld->ld_ml_to_hl[candidate->exch_tiles[tile_idx]]);
  }
  const char *rendered = string_builder_peek(builder);
  const size_t len = string_length(rendered);
  const size_t copy_len =
      len < CPEG_MOVE_STR_LEN - 1 ? len : CPEG_MOVE_STR_LEN - 1;
  memcpy(dest, rendered, copy_len);
  dest[copy_len] = '\0';
  string_builder_destroy(builder);
}

static CpegCandKind cpeg_root_candidate_kind(const CpegRootCand *candidate) {
  if (candidate->kind == 0) {
    return CPEG_CAND_PLACEMENT;
  }
  if (candidate->kind == 2) {
    return CPEG_CAND_EXCHANGE;
  }
  return CPEG_CAND_PASS;
}

static bool cpeg_world_precedes(const CpegScheduledWorld *lhs,
                                const CpegScheduledWorld *rhs) {
  if (lhs->multiset.weight != rhs->multiset.weight) {
    return lhs->multiset.weight > rhs->multiset.weight;
  }
  for (int tile_idx = 0; tile_idx < lhs->multiset.n; tile_idx++) {
    if (lhs->multiset.tiles[tile_idx] != rhs->multiset.tiles[tile_idx]) {
      return lhs->multiset.tiles[tile_idx] < rhs->multiset.tiles[tile_idx];
    }
  }
  return lhs->generation_index < rhs->generation_index;
}

static void cpeg_sort_scheduled_worlds(CpegScheduledWorld *worlds,
                                       int world_count) {
  for (int world_idx = 1; world_idx < world_count; world_idx++) {
    const CpegScheduledWorld current = worlds[world_idx];
    int insertion_idx = world_idx;
    while (insertion_idx > 0 &&
           cpeg_world_precedes(&current, &worlds[insertion_idx - 1])) {
      worlds[insertion_idx] = worlds[insertion_idx - 1];
      insertion_idx--;
    }
    worlds[insertion_idx] = current;
  }
}

static void cpeg_sort_candidate_order(const CpegCandState *states, int *order,
                                      int candidate_count) {
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    order[candidate_idx] = candidate_idx;
  }
  for (int order_idx = 1; order_idx < candidate_count; order_idx++) {
    const int current = order[order_idx];
    int insertion_idx = order_idx;
    while (insertion_idx > 0 &&
           cpeg_rank_precedes(&states[current],
                              &states[order[insertion_idx - 1]])) {
      order[insertion_idx] = order[insertion_idx - 1];
      insertion_idx--;
    }
    order[insertion_idx] = current;
  }
}

static bool cpeg_better_upper(const CpegCandState *states, int candidate_idx,
                              int best_idx) {
  return best_idx < 0 ||
         states[candidate_idx].expectation.hi >
             states[best_idx].expectation.hi ||
         (states[candidate_idx].expectation.hi ==
              states[best_idx].expectation.hi &&
          cpeg_rank_precedes(&states[candidate_idx], &states[best_idx]));
}

static int cpeg_sort_active_by_upper(const CpegCandState *states,
                                     const int *stable_order, int *order,
                                     int candidate_count) {
  int count = 0;
  for (int order_idx = 0; order_idx < candidate_count; order_idx++) {
    const int candidate_idx = stable_order[order_idx];
    if (!states[candidate_idx].eliminated) {
      order[count++] = candidate_idx;
    }
  }
  for (int order_idx = 1; order_idx < count; order_idx++) {
    const int current = order[order_idx];
    int insertion_idx = order_idx;
    while (insertion_idx > 0 &&
           cpeg_better_upper(states, current, order[insertion_idx - 1])) {
      order[insertion_idx] = order[insertion_idx - 1];
      insertion_idx--;
    }
    order[insertion_idx] = current;
  }
  return count;
}

static bool cpeg_pair_is_selected(const CpegScheduledPair *pairs,
                                  int pair_count, int candidate_idx,
                                  int world_idx) {
  for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
    if (pairs[pair_idx].candidate_idx == candidate_idx &&
        pairs[pair_idx].world_idx == world_idx) {
      return true;
    }
  }
  return false;
}

static int cpeg_next_unresolved_world(const CpegWorldEval *evaluations,
                                      int world_count,
                                      const CpegScheduledPair *pairs,
                                      int pair_count, int candidate_idx) {
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    const CpegWorldEval *evaluation =
        &evaluations[candidate_idx * world_count + world_idx];
    if (!evaluation->resolved &&
        !cpeg_pair_is_selected(pairs, pair_count, candidate_idx, world_idx)) {
      return world_idx;
    }
  }
  return -1;
}

static bool cpeg_run_certified_batch(
    PegPool *pool, CpegWorker *workers, int helper_worker_idx,
    CpegRootJob *jobs, void **job_ptrs, const CpegScheduledPair *pairs,
    int pair_count, const CpegRootCand *candidates, Game *const *templates,
    const Game *game, const CpegScheduledWorld *worlds, const int *unseen,
    int ld_size, int opp_idx, int scratch_count, CpegWorldEval *evaluations,
    int world_count, bool *batch_complete) {
  *batch_complete = false;
  for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
    const int candidate_idx = pairs[pair_idx].candidate_idx;
    const int world_idx = pairs[pair_idx].world_idx;
    CpegRootJob *job = &jobs[pair_idx];
    job->workers = workers;
    job->source_game =
        templates[candidate_idx] != NULL ? templates[candidate_idx] : game;
    job->world = &worlds[world_idx].multiset;
    job->unseen = unseen;
    job->ld_size = ld_size;
    job->opp_idx = opp_idx;
    job->cand = &candidates[candidate_idx];
    job->interval = (CpegInterval){.lo = 0.0, .hi = 0.0};
    job->complete = false;
    job_ptrs[pair_idx] = job;
  }

  peg_pool_submit_and_wait(pool, cpeg_root_interval_job_run, job_ptrs,
                           pair_count, helper_worker_idx);
  for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
    if (workers[worker_idx].ctx.capacity_exceeded) {
      return false;
    }
  }
  for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
    if (!jobs[pair_idx].complete) {
      return true;
    }
  }
  // Results become visible only after the barrier, in scheduler order.
  for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
    const int candidate_idx = pairs[pair_idx].candidate_idx;
    const int world_idx = pairs[pair_idx].world_idx;
    evaluations[candidate_idx * world_count + world_idx] =
        (CpegWorldEval){.value = jobs[pair_idx].interval, .resolved = true};
  }
  *batch_complete = true;
  return true;
}

static int64_t cpeg_certified_deadline_ns(double budget_seconds) {
  if (budget_seconds <= 0.0) {
    return 0;
  }
  const int64_t start_ns = ctimer_monotonic_ns();
  const double budget_ns = budget_seconds * 1000000000.0;
  if (budget_ns >= (double)(INT64_MAX - start_ns)) {
    return INT64_MAX;
  }
  return start_ns + (int64_t)budget_ns;
}

static bool cpeg_certified_budget_reached(int64_t deadline_ns, int max_batches,
                                          int batches_completed) {
  return (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) ||
         (max_batches > 0 && batches_completed >= max_batches);
}

static int cpeg_fill_lucb_priority(const CpegCandState *states,
                                   const int *stable_order, int *priority,
                                   int *upper_order, int candidate_count) {
  int leader_idx = -1;
  int incumbent_idx = -1;
  for (int order_idx = 0; order_idx < candidate_count; order_idx++) {
    const int candidate_idx = stable_order[order_idx];
    if (states[candidate_idx].eliminated) {
      continue;
    }
    if (cpeg_better_midpoint(states, candidate_idx, leader_idx)) {
      leader_idx = candidate_idx;
    }
    if (cpeg_better_lower(states, candidate_idx, incumbent_idx)) {
      incumbent_idx = candidate_idx;
    }
  }

  int challenger_idx = -1;
  for (int order_idx = 0; order_idx < candidate_count; order_idx++) {
    const int candidate_idx = stable_order[order_idx];
    if (!states[candidate_idx].eliminated && candidate_idx != leader_idx &&
        cpeg_better_upper(states, candidate_idx, challenger_idx)) {
      challenger_idx = candidate_idx;
    }
  }

  int priority_count = 0;
  priority[priority_count++] = leader_idx;
  if (incumbent_idx != leader_idx) {
    priority[priority_count++] = incumbent_idx;
  }
  if (challenger_idx >= 0 && challenger_idx != incumbent_idx) {
    priority[priority_count++] = challenger_idx;
  }

  const int upper_count = cpeg_sort_active_by_upper(
      states, stable_order, upper_order, candidate_count);
  for (int upper_idx = 0; upper_idx < upper_count; upper_idx++) {
    const int candidate_idx = upper_order[upper_idx];
    bool already_present = false;
    for (int priority_idx = 0; priority_idx < priority_count; priority_idx++) {
      if (priority[priority_idx] == candidate_idx) {
        already_present = true;
        break;
      }
    }
    if (!already_present) {
      priority[priority_count++] = candidate_idx;
    }
  }
  return priority_count;
}

int cpeg_solve_pre_endgame_certified(Game *game, const CpegCertifiedArgs *args,
                                     CpegCertifiedResult *out) {
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->best_index = -1;
  if (game == NULL || args == NULL || args->bag < 1 ||
      args->bag > PEG_MAX_BAG || !isfinite(args->budget_seconds) ||
      args->budget_seconds < 0.0 || args->max_batches < 0) {
    return -1;
  }
  const int64_t deadline_ns = cpeg_certified_deadline_ns(args->budget_seconds);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Board *board = game_get_board(game);
  game_gen_all_cross_sets(game);
  board_set_cross_sets_valid(board, true);
  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  int unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = cpeg_compute_unseen(game, mover_idx, unseen);
  const int opp_size = total_unseen - args->bag;
  if (opp_size < 0 || opp_size > RACK_SIZE) {
    return -1;
  }

  MoveList *root_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  const MoveGenArgs root_args = {
      .game = game,
      .move_list = root_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&root_args);
  const int root_count = move_list_get_count(root_moves);
  if (root_count > CPEG_MOVE_LIST_CAP) {
    move_list_destroy(root_moves);
    return -1;
  }

  CpegRootCand *candidates = calloc_or_die(
      (size_t)(root_count + 1 + CPEG_ENUM_CAP), sizeof(*candidates));
  int candidate_count = 0;
  bool any_placement = false;
  for (int move_idx = 0; move_idx < root_count; move_idx++) {
    const Move *move = move_list_get_move(root_moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    any_placement = true;
    CpegRootCand *candidate = &candidates[candidate_count++];
    candidate->kind = 0;
    move_copy(&candidate->move, move);
    candidate->score = equity_to_int(move_get_score(move));
  }
  if (!any_placement) {
    candidates[candidate_count++].kind = 1;
  }
  if (args->allow_exchanges) {
    const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
    CpegMultiset exchanges[CPEG_ENUM_CAP];
    bool exchange_overflow = false;
    const int exchange_count =
        cpeg_enum_exchanges(ld_size, mover_rack, args->bag, exchanges,
                            CPEG_ENUM_CAP, &exchange_overflow);
    if (exchange_overflow) {
      free(candidates);
      move_list_destroy(root_moves);
      return -1;
    }
    for (int exchange_idx = 0; exchange_idx < exchange_count; exchange_idx++) {
      CpegRootCand *candidate = &candidates[candidate_count++];
      candidate->kind = 2;
      candidate->exch_n = exchanges[exchange_idx].n;
      for (int tile_idx = 0; tile_idx < candidate->exch_n; tile_idx++) {
        candidate->exch_tiles[tile_idx] =
            exchanges[exchange_idx].tiles[tile_idx];
      }
    }
  }
  if (candidate_count < 1 || candidate_count > CPEG_MAX_PRE_CANDS) {
    free(candidates);
    move_list_destroy(root_moves);
    return -1;
  }

  CpegMultiset *enumerated_worlds =
      malloc_or_die(CPEG_WORLD_CAP * sizeof(*enumerated_worlds));
  bool world_overflow = false;
  const int world_count =
      cpeg_enum_submultisets(unseen, ld_size, args->bag, enumerated_worlds,
                             CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow || world_count < 1) {
    free(enumerated_worlds);
    free(candidates);
    move_list_destroy(root_moves);
    return -1;
  }
  CpegScheduledWorld *worlds =
      malloc_or_die((size_t)world_count * sizeof(*worlds));
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    worlds[world_idx].multiset = enumerated_worlds[world_idx];
    worlds[world_idx].generation_index = world_idx;
  }
  free(enumerated_worlds);
  cpeg_sort_scheduled_worlds(worlds, world_count);

  Game **templates = calloc_or_die((size_t)candidate_count, sizeof(*templates));
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidates[candidate_idx].kind == 0) {
      templates[candidate_idx] =
          cpeg_build_root_template(game, &candidates[candidate_idx].move);
    }
  }

  CpegCandState *states =
      malloc_or_die((size_t)candidate_count * sizeof(*states));
  int *stable_order =
      malloc_or_die((size_t)candidate_count * sizeof(*stable_order));
  const int root_score_bound = cpeg_score_upper_bound(board, game);
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    CpegStableRank rank = {
        .immediate_score = candidates[candidate_idx].score,
        .kind = cpeg_root_candidate_kind(&candidates[candidate_idx]),
        .generation_index = candidate_idx,
    };
    cpeg_render_root_candidate(rank.label, &candidates[candidate_idx], board,
                               ld);
    CpegInterval prior;
    if (candidates[candidate_idx].kind == 0) {
      const int score_bound = cpeg_score_upper_bound(
          game_get_board(templates[candidate_idx]), templates[candidate_idx]);
      prior = cpeg_placement_prior(
          candidates[candidate_idx].score, args->bag,
          move_get_tiles_played(&candidates[candidate_idx].move), score_bound);
    } else {
      prior = cpeg_scoreless_prior(args->bag, root_score_bound);
    }
    cpeg_cand_state_init(&states[candidate_idx], prior, &rank);
  }
  cpeg_sort_candidate_order(states, stable_order, candidate_count);

  int64_t *world_weights =
      malloc_or_die((size_t)world_count * sizeof(*world_weights));
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    world_weights[world_idx] = worlds[world_idx].multiset.weight;
  }
  CpegWorldEval *evaluations = calloc_or_die(
      (size_t)candidate_count * (size_t)world_count, sizeof(*evaluations));

  const int thread_count = args->num_threads < 1 ? 1 : args->num_threads;
  PegPool *pool = thread_count > 1 ? peg_pool_create(thread_count, 0) : NULL;
  if (pool != NULL) {
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  const int scratch_count = pool != NULL ? thread_count + 1 : 1;
  CpegWorker *workers = malloc_or_die((size_t)scratch_count * sizeof(*workers));
  for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
    cpeg_ctx_init(&workers[worker_idx].ctx, ld, ld_size, mover_idx,
                  args->allow_exchanges, deadline_ns);
    workers[worker_idx].world_game = game_duplicate(game);
  }

  int batch_size =
      args->batch_size > 0 ? args->batch_size : CPEG_CERT_DEFAULT_BATCH_SIZE;
  const int pair_total = candidate_count * world_count;
  if (batch_size > pair_total) {
    batch_size = pair_total;
  }
  CpegScheduledPair *pairs = malloc_or_die((size_t)batch_size * sizeof(*pairs));
  CpegRootJob *jobs = malloc_or_die((size_t)batch_size * sizeof(*jobs));
  void **job_ptrs = malloc_or_die((size_t)batch_size * sizeof(*job_ptrs));
  int *priority = malloc_or_die((size_t)candidate_count * sizeof(*priority));
  int *upper_order =
      malloc_or_die((size_t)candidate_count * sizeof(*upper_order));
  const int helper_worker_idx = pool != NULL ? thread_count : 0;
  CpegCoordinatorResult coordinator = {
      .status = CPEG_COORDINATOR_PENDING,
      .best_index = stable_order[0],
  };
  bool search_ok = true;
  bool budget_exhausted = false;
  cpeg_coordinator_recompute(states, candidate_count, evaluations,
                             world_weights, world_count, &coordinator);

  // Phase A: every candidate's highest-weight world is scheduled in stable
  // candidate order. A barrier and proof check end each fixed-size batch.
  for (int pilot_idx = 0; pilot_idx < candidate_count &&
                          coordinator.status == CPEG_COORDINATOR_PENDING;
       pilot_idx += batch_size) {
    if (cpeg_certified_budget_reached(deadline_ns, args->max_batches,
                                      out->batches_completed)) {
      budget_exhausted = true;
      break;
    }
    int pair_count = candidate_count - pilot_idx;
    if (pair_count > batch_size) {
      pair_count = batch_size;
    }
    for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
      pairs[pair_idx] = (CpegScheduledPair){
          .candidate_idx = stable_order[pilot_idx + pair_idx],
          .world_idx = 0,
      };
    }
    bool batch_complete = false;
    search_ok = cpeg_run_certified_batch(
        pool, workers, helper_worker_idx, jobs, job_ptrs, pairs, pair_count,
        candidates, templates, game, worlds, unseen, ld_size, opp_idx,
        scratch_count, evaluations, world_count, &batch_complete);
    if (!search_ok || !batch_complete) {
      budget_exhausted = !batch_complete;
      break;
    }
    out->jobs_completed += pair_count;
    out->batches_completed++;
    cpeg_coordinator_recompute(states, candidate_count, evaluations,
                               world_weights, world_count, &coordinator);
  }

  // Phase B: LUCB-style contraction. Priority candidates and then the other
  // active candidates contribute one maximum-impact unresolved world per
  // round until the next deterministic batch is full.
  while (!budget_exhausted && coordinator.status == CPEG_COORDINATOR_PENDING) {
    if (cpeg_certified_budget_reached(deadline_ns, args->max_batches,
                                      out->batches_completed)) {
      budget_exhausted = true;
      break;
    }
    const int priority_count = cpeg_fill_lucb_priority(
        states, stable_order, priority, upper_order, candidate_count);
    int pair_count = 0;
    bool added = true;
    while (pair_count < batch_size && added) {
      added = false;
      for (int priority_idx = 0;
           priority_idx < priority_count && pair_count < batch_size;
           priority_idx++) {
        const int candidate_idx = priority[priority_idx];
        const int world_idx = cpeg_next_unresolved_world(
            evaluations, world_count, pairs, pair_count, candidate_idx);
        if (world_idx >= 0) {
          pairs[pair_count++] = (CpegScheduledPair){
              .candidate_idx = candidate_idx,
              .world_idx = world_idx,
          };
          added = true;
        }
      }
    }
    if (pair_count == 0) {
      search_ok = false;
      break;
    }
    bool batch_complete = false;
    search_ok = cpeg_run_certified_batch(
        pool, workers, helper_worker_idx, jobs, job_ptrs, pairs, pair_count,
        candidates, templates, game, worlds, unseen, ld_size, opp_idx,
        scratch_count, evaluations, world_count, &batch_complete);
    if (!search_ok || !batch_complete) {
      budget_exhausted = !batch_complete;
      break;
    }
    out->jobs_completed += pair_count;
    out->batches_completed++;
    cpeg_coordinator_recompute(states, candidate_count, evaluations,
                               world_weights, world_count, &coordinator);
  }

  if (search_ok &&
      (coordinator.status != CPEG_COORDINATOR_PENDING || budget_exhausted)) {
    if (coordinator.status == CPEG_COORDINATOR_EXACT_VALUES) {
      out->status = CPEG_PRE_EXACT_VALUES;
    } else if (coordinator.status == CPEG_COORDINATOR_CERTIFIED) {
      out->status = CPEG_PRE_CERTIFIED;
    } else {
      out->status = CPEG_PRE_ESTIMATED;
    }
    out->count = candidate_count;
    out->best_index = coordinator.best_index;
    out->worlds_total = world_count;
    out->unique_best = coordinator.unique_best;
    out->decision_regret_bound = coordinator.decision_regret_bound;
    out->optimum_lower = -INFINITY;
    out->optimum_upper = -INFINITY;
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      const CpegCandState *state = &states[candidate_idx];
      CpegCertifiedCand *result_candidate = &out->cands[candidate_idx];
      memcpy(result_candidate->label, state->rank.label,
             sizeof(result_candidate->label));
      result_candidate->score = candidates[candidate_idx].score;
      result_candidate->lower = state->expectation.lo;
      result_candidate->upper = state->expectation.hi;
      result_candidate->estimate = cpeg_interval_midpoint(state->expectation);
      result_candidate->value_error_bound =
          (state->expectation.hi - state->expectation.lo) / 2.0;
      result_candidate->worlds_resolved = state->worlds_resolved;
      result_candidate->eliminated = state->eliminated;
      if (state->expectation.lo > out->optimum_lower) {
        out->optimum_lower = state->expectation.lo;
      }
      if (state->expectation.hi > out->optimum_upper) {
        out->optimum_upper = state->expectation.hi;
      }
    }
  }

  free(upper_order);
  free(priority);
  free(job_ptrs);
  free(jobs);
  free(pairs);
  peg_pool_destroy(pool);
  for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
    game_destroy(workers[worker_idx].world_game);
    cpeg_ctx_destroy(&workers[worker_idx].ctx);
  }
  free(workers);
  free(evaluations);
  free(world_weights);
  free(stable_order);
  free(states);
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (templates[candidate_idx] != NULL) {
      game_destroy(templates[candidate_idx]);
    }
  }
  free(templates);
  free(worlds);
  free(candidates);
  move_list_destroy(root_moves);
  return out->count > 0 ? out->count : -1;
}

int cpeg_measure_interval_recursion(Game *game, int bag, bool allow_exchanges,
                                    CpegIntervalRecursionStats *stats) {
  memset(stats, 0, sizeof(*stats));
  stats->all_contained = true;
  stats->failing_candidate_index = -1;
  stats->failing_world_index = -1;

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Board *board = game_get_board(game);
  game_gen_all_cross_sets(game);
  board_set_cross_sets_valid(board, true);

  const int mover_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - mover_idx;
  int unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = cpeg_compute_unseen(game, mover_idx, unseen);
  const int opp_size = total_unseen - bag;
  if (bag < 1 || bag > PEG_MAX_BAG || opp_size < 0 || opp_size > RACK_SIZE) {
    return -1;
  }

  MoveList *root_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  const MoveGenArgs root_args = {
      .game = game,
      .move_list = root_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&root_args);

  const int root_count = move_list_get_count(root_moves);
  if (root_count > CPEG_MOVE_LIST_CAP) {
    move_list_destroy(root_moves);
    return -1;
  }
  CpegRootCand *cands =
      malloc_or_die((size_t)(root_count + 1 + CPEG_ENUM_CAP) * sizeof(*cands));
  int candidate_count = 0;
  bool any_placement = false;
  for (int move_idx = 0; move_idx < root_count; move_idx++) {
    const Move *move = move_list_get_move(root_moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    any_placement = true;
    CpegRootCand *cand = &cands[candidate_count++];
    cand->kind = 0;
    move_copy(&cand->move, move);
    cand->exch_n = 0;
    cand->score = equity_to_int(move_get_score(move));
  }
  if (!any_placement) {
    CpegRootCand *cand = &cands[candidate_count++];
    cand->kind = 1;
    cand->exch_n = 0;
    cand->score = 0;
  }
  if (allow_exchanges) {
    const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
    CpegMultiset exchanges[CPEG_ENUM_CAP];
    bool overflow = false;
    const int exchange_count = cpeg_enum_exchanges(
        ld_size, mover_rack, bag, exchanges, CPEG_ENUM_CAP, &overflow);
    if (overflow) {
      free(cands);
      move_list_destroy(root_moves);
      return -1;
    }
    for (int exchange_idx = 0; exchange_idx < exchange_count; exchange_idx++) {
      CpegRootCand *cand = &cands[candidate_count++];
      cand->kind = 2;
      cand->exch_n = exchanges[exchange_idx].n;
      for (int tile_idx = 0; tile_idx < cand->exch_n; tile_idx++) {
        cand->exch_tiles[tile_idx] = exchanges[exchange_idx].tiles[tile_idx];
      }
      cand->score = 0;
    }
  }
  if (candidate_count > CPEG_MAX_PRE_CANDS) {
    free(cands);
    move_list_destroy(root_moves);
    return -1;
  }

  CpegMultiset *worlds = malloc_or_die(CPEG_WORLD_CAP * sizeof(*worlds));
  bool world_overflow = false;
  const int world_count = cpeg_enum_submultisets(
      unseen, ld_size, bag, worlds, CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow) {
    free(worlds);
    free(cands);
    move_list_destroy(root_moves);
    return -1;
  }

  Game **templates = calloc_or_die((size_t)candidate_count, sizeof(*templates));
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (cands[candidate_idx].kind == 0) {
      templates[candidate_idx] =
          cpeg_build_root_template(game, &cands[candidate_idx].move);
    }
  }

  CpegPreCtx ctx;
  cpeg_ctx_init(&ctx, ld, ld_size, mover_idx, allow_exchanges,
                /*deadline_ns=*/0);
  Game *world_game = game_duplicate(game);
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      const CpegRootCand *cand = &cands[candidate_idx];
      const Game *source_game =
          templates[candidate_idx] != NULL ? templates[candidate_idx] : game;
      cpeg_set_world(world_game, source_game, &worlds[world_idx], unseen,
                     ld_size, opp_idx);
      double scalar;
      if (cand->kind == 0) {
        scalar = cpeg_eval_post_place(&ctx, world_game,
                                      move_get_tiles_played(&cand->move),
                                      cand->score, /*depth=*/0);
      } else {
        scalar = cpeg_eval_root_scoreless(&ctx, world_game, cand);
      }

      cpeg_set_world(world_game, source_game, &worlds[world_idx], unseen,
                     ld_size, opp_idx);
      CpegInterval interval;
      if (cand->kind == 0) {
        interval = cpeg_eval_post_place_interval(
            &ctx, world_game, move_get_tiles_played(&cand->move), cand->score,
            /*depth=*/0);
      } else {
        interval = cpeg_eval_root_scoreless_interval(&ctx, world_game, cand);
      }

      const double width = interval.hi - interval.lo;
      stats->pair_count++;
      stats->total_width += width;
      if (width > stats->maximum_width) {
        stats->maximum_width = width;
      }
      if (stats->all_contained &&
          (scalar < interval.lo || scalar > interval.hi)) {
        stats->all_contained = false;
        stats->failing_candidate_index = candidate_idx;
        stats->failing_world_index = world_idx;
        stats->failing_scalar = scalar;
        stats->failing_interval = interval;
      }
    }
  }
  stats->candidate_count = candidate_count;
  stats->world_count = world_count;

  const bool capacity_exceeded = ctx.capacity_exceeded;
  game_destroy(world_game);
  cpeg_ctx_destroy(&ctx);
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (templates[candidate_idx] != NULL) {
      game_destroy(templates[candidate_idx]);
    }
  }
  free(templates);
  free(worlds);
  free(cands);
  move_list_destroy(root_moves);
  return capacity_exceeded ? -1 : stats->pair_count;
}
