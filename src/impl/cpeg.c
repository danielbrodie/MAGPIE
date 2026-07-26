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
#include "../ent/xoshiro.h"
#include "../str/move_string.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "crossplay_oracle.h"
#include "move_gen.h"
#include "peg_combinatorics.h"
#include "peg_pool.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
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
  CPEG_WORLD_CAP = CPEG_MAX_WORLDS,
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
static int cpeg_endgame_core_until(Game *game, MoveList *mover_moves,
                                   MoveList *reply_moves, MoveUndo *undo,
                                   CpegResult *result, bool *capacity_exceeded,
                                   int64_t deadline_ns, bool *complete) {
  memset(result, 0, sizeof(*result));
  if (complete != NULL) {
    *complete = true;
  }
  if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
    if (complete != NULL) {
      *complete = false;
    }
    return 0;
  }

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

  if (move_list_get_count(mover_moves) > CPEG_MOVE_LIST_CAP) {
    if (capacity_exceeded != NULL) {
      *capacity_exceeded = true;
    }
    return 0;
  }

  int best_swing = 0;
  int best_mover_score = 0;
  bool have_best = false;

  const int mover_count = move_list_get_count(mover_moves);
  for (int mover_idx = 0; mover_idx < mover_count; mover_idx++) {
    if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
      if (complete != NULL) {
        *complete = false;
      }
      break;
    }
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

// Decide only whether the empty-bag two-ply swing is strictly above threshold.
// Reply scores are non-negative, so once the score-sorted mover list reaches a
// move at or below threshold, no remaining move can cross it.
static bool cpeg_endgame_swing_above(
    Game *game, MoveList *mover_moves, MoveList *reply_moves, MoveUndo *undo,
    int64_t threshold, bool *above_threshold, bool *capacity_exceeded,
    int64_t deadline_ns, bool *complete) {
  *above_threshold = false;
  if (complete != NULL) {
    *complete = true;
  }
  if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
    if (complete != NULL) {
      *complete = false;
    }
    return true;
  }

  Board *board = game_get_board(game);
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
  if (move_list_get_count(mover_moves) > CPEG_MOVE_LIST_CAP) {
    if (capacity_exceeded != NULL) {
      *capacity_exceeded = true;
    }
    return true;
  }

  const int mover_count = move_list_get_count(mover_moves);
  for (int mover_idx = 0; mover_idx < mover_count; mover_idx++) {
    if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
      if (complete != NULL) {
        *complete = false;
      }
      break;
    }
    const Move *mover_move = move_list_get_move(mover_moves, mover_idx);
    const int mover_score = equity_to_int(move_get_score(mover_move));
    if ((int64_t)mover_score <= threshold) {
      break;
    }
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
    const Move *reply_move = move_list_get_move(reply_moves, 0);
    const int reply_score = equity_to_int(move_get_score(reply_move));
    const int swing = mover_score - reply_score;
    unplay_move_incremental(game, undo);
    if ((int64_t)swing > threshold) {
      *above_threshold = true;
      break;
    }
  }
  return true;
}

static int cpeg_endgame_core(Game *game, MoveList *mover_moves,
                             MoveList *reply_moves, MoveUndo *undo,
                             CpegResult *result, bool *capacity_exceeded) {
  return cpeg_endgame_core_until(game, mover_moves, reply_moves, undo, result,
                                 capacity_exceeded, /*deadline_ns=*/0,
                                 /*complete=*/NULL);
}

int cpeg_solve_endgame(Game *game, CpegResult *result) {
  MoveList *mover_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  MoveList *reply_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveUndo *undo = malloc_or_die(sizeof(MoveUndo));
  const int swing = cpeg_endgame_core(game, mover_moves, reply_moves, undo,
                                      result, /*capacity_exceeded=*/NULL);
  free(undo);
  move_list_destroy(reply_moves);
  move_list_destroy(mover_moves);
  return swing;
}

int cpeg_solve_endgame_swing_above(Game *game, int64_t threshold,
                                   bool *above_threshold) {
  if (game == NULL || above_threshold == NULL ||
      !bag_is_empty(game_get_bag(game))) {
    return -1;
  }
  MoveList *mover_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  MoveList *reply_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveUndo *undo = malloc_or_die(sizeof(MoveUndo));
  bool capacity_exceeded = false;
  bool complete = true;
  const bool valid = cpeg_endgame_swing_above(
      game, mover_moves, reply_moves, undo, threshold, above_threshold,
      &capacity_exceeded, /*deadline_ns=*/0, &complete);
  free(undo);
  move_list_destroy(reply_moves);
  move_list_destroy(mover_moves);
  return valid && !capacity_exceeded && complete ? 0 : -1;
}

CpegWtlValue cpeg_wtl_classify_margin(int64_t final_margin) {
  CpegWtlValue value = {
      .expected_final_margin = (double)final_margin,
  };
  if (final_margin > 0) {
    value.win = 1.0;
  } else if (final_margin == 0) {
    value.tie = 1.0;
  } else {
    value.loss = 1.0;
  }
  return value;
}

static bool cpeg_checked_margin_add(int64_t margin, int64_t delta,
                                    int64_t *result) {
  if ((delta > 0 && margin > INT64_MAX - delta) ||
      (delta < 0 && margin < INT64_MIN - delta)) {
    return false;
  }
  *result = margin + delta;
  return true;
}

static int cpeg_wtl_compare_component(double lhs, double rhs) {
  // Equal rational probabilities can reach a node through different nested
  // chance-denominator shapes. Their floating-point reconstructions may differ
  // by a handful of ulps even though the mathematical values are equal. The
  // reachable CPEG tree is shallow, so 256 scaled ulps comfortably absorbs
  // that reduction noise while remaining many orders below the smallest
  // meaningful bag-1..4 outcome-mass increment.
  const double scale = fmax(1.0, fmax(fabs(lhs), fabs(rhs)));
  const double tolerance = 256.0 * DBL_EPSILON * scale;
  if (lhs > rhs + tolerance) {
    return 1;
  }
  if (lhs < rhs - tolerance) {
    return -1;
  }
  return 0;
}

int cpeg_wtl_compare(const CpegWtlValue *lhs, const CpegWtlValue *rhs) {
  // Wire objective lexicographic_win_tie_margin_v1: these components are
  // compared in order. There is deliberately no scalar value assigned to a
  // tie and no weighted sum that could trade tie mass against win mass.
  int comparison = cpeg_wtl_compare_component(lhs->win, rhs->win);
  if (comparison != 0) {
    return comparison;
  }
  comparison = cpeg_wtl_compare_component(lhs->tie, rhs->tie);
  if (comparison != 0) {
    return comparison;
  }
  comparison = cpeg_wtl_compare_component(lhs->expected_final_margin,
                                          rhs->expected_final_margin);
  if (comparison != 0) {
    return comparison;
  }
  return 0;
}

static CpegWtlEnvelope cpeg_wtl_unresolved_envelope(CpegInterval margin_prior) {
  return (CpegWtlEnvelope){
      .estimate =
          {
              .win = 1.0 / 3.0,
              .tie = 1.0 / 3.0,
              .loss = 1.0 / 3.0,
              .expected_final_margin =
                  margin_prior.lo / 2.0 + margin_prior.hi / 2.0,
          },
      .win = {.lo = 0.0, .hi = 1.0},
      .tie = {.lo = 0.0, .hi = 1.0},
      .loss = {.lo = 0.0, .hi = 1.0},
      .expected_final_margin = margin_prior,
  };
}

CpegWtlEnvelope cpeg_wtl_envelope_from_margin_upper(int64_t margin_upper,
                                                    CpegInterval margin_prior) {
  CpegWtlEnvelope envelope = cpeg_wtl_unresolved_envelope(margin_prior);
  if ((double)margin_upper < envelope.expected_final_margin.hi) {
    envelope.expected_final_margin.hi = (double)margin_upper;
  }
  if (envelope.expected_final_margin.lo > envelope.expected_final_margin.hi) {
    envelope.expected_final_margin.lo = envelope.expected_final_margin.hi;
  }
  envelope.estimate.expected_final_margin =
      envelope.expected_final_margin.lo / 2.0 +
      envelope.expected_final_margin.hi / 2.0;
  if (margin_upper < 0) {
    envelope.win = (CpegInterval){.lo = 0.0, .hi = 0.0};
    envelope.tie = (CpegInterval){.lo = 0.0, .hi = 0.0};
    envelope.loss = (CpegInterval){.lo = 1.0, .hi = 1.0};
    envelope.estimate.win = 0.0;
    envelope.estimate.tie = 0.0;
    envelope.estimate.loss = 1.0;
  } else if (margin_upper == 0) {
    envelope.win = (CpegInterval){.lo = 0.0, .hi = 0.0};
    envelope.estimate.win = 0.0;
  }
  return envelope;
}

static bool cpeg_interval_is_point(CpegInterval interval) {
  return interval.lo == interval.hi;
}

bool cpeg_wtl_envelope_dominates(const CpegWtlEnvelope *lhs,
                                 const CpegWtlEnvelope *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return false;
  }
  if (lhs->win.lo > rhs->win.hi) {
    return true;
  }
  if (!cpeg_interval_is_point(lhs->win) || !cpeg_interval_is_point(rhs->win) ||
      lhs->win.lo != rhs->win.lo) {
    return false;
  }
  if (lhs->tie.lo > rhs->tie.hi) {
    return true;
  }
  if (!cpeg_interval_is_point(lhs->tie) || !cpeg_interval_is_point(rhs->tie) ||
      lhs->tie.lo != rhs->tie.lo) {
    return false;
  }
  return lhs->expected_final_margin.lo > rhs->expected_final_margin.hi;
}

int cpeg_wtl_weighted_average(const CpegWtlValue *values,
                              const int64_t *weights, int count,
                              CpegWtlValue *out) {
  if (values == NULL || weights == NULL || out == NULL || count < 1) {
    return -1;
  }
  CpegWtlValue weighted = {0};
  int64_t weight_total = 0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    const int64_t weight = weights[value_idx];
    if (weight <= 0 || INT64_MAX - weight_total < weight) {
      return -1;
    }
    weighted.win += (double)weight * values[value_idx].win;
    weighted.tie += (double)weight * values[value_idx].tie;
    weighted.loss += (double)weight * values[value_idx].loss;
    weighted.expected_final_margin +=
        (double)weight * values[value_idx].expected_final_margin;
    weight_total += weight;
  }
  const double denominator = (double)weight_total;
  *out = (CpegWtlValue){
      .win = weighted.win / denominator,
      .tie = weighted.tie / denominator,
      .loss = weighted.loss / denominator,
      .expected_final_margin = weighted.expected_final_margin / denominator,
  };
  return 0;
}

int cpeg_solve_endgame_wtl(Game *game, int root_player_idx,
                           int64_t initial_lead, CpegWtlValue *out) {
  if (game == NULL || out == NULL || root_player_idx < 0 ||
      root_player_idx > 1 || !bag_is_empty(game_get_bag(game))) {
    return -1;
  }
  const int on_turn = game_get_player_on_turn_index(game);
  MoveList *mover_moves = move_list_create(CPEG_MOVE_LIST_CAP + 1);
  MoveList *reply_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveUndo *undo = malloc_or_die(sizeof(MoveUndo));
  CpegResult result;
  bool capacity_exceeded = false;
  const int on_turn_swing = cpeg_endgame_core(
      game, mover_moves, reply_moves, undo, &result, &capacity_exceeded);
  free(undo);
  move_list_destroy(reply_moves);
  move_list_destroy(mover_moves);
  if (capacity_exceeded) {
    return -1;
  }
  const int64_t root_swing = on_turn == root_player_idx
                                 ? (int64_t)on_turn_swing
                                 : -(int64_t)on_turn_swing;
  int64_t final_margin;
  if (!cpeg_checked_margin_add(initial_lead, root_swing, &final_margin)) {
    return -1;
  }
  *out = cpeg_wtl_classify_margin(final_margin);
  return 0;
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
//     the exchange search finite. At the root voluntary pass is included;
//     below the root pass is searched only when there is no legal placement.
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
    ctx->movelists[depth] = move_list_create(CPEG_MOVE_LIST_CAP + 1);
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
static CpegWtlValue cpeg_wtl_value(CpegPreCtx *ctx, Game *game,
                                   int64_t root_margin, int scoreless,
                                   int depth);

static bool cpeg_wtl_accumulate(CpegWtlValue *weighted_sum,
                                int64_t *weight_total,
                                const CpegWtlValue *value, int64_t weight) {
  if (weight <= 0 || INT64_MAX - *weight_total < weight) {
    return false;
  }
  weighted_sum->win += (double)weight * value->win;
  weighted_sum->tie += (double)weight * value->tie;
  weighted_sum->loss += (double)weight * value->loss;
  weighted_sum->expected_final_margin +=
      (double)weight * value->expected_final_margin;
  *weight_total += weight;
  return true;
}

static CpegWtlValue cpeg_wtl_normalize(const CpegWtlValue *weighted_sum,
                                       int64_t weight_total) {
  if (weight_total <= 0) {
    return (CpegWtlValue){0};
  }
  const double denominator = (double)weight_total;
  return (CpegWtlValue){
      .win = weighted_sum->win / denominator,
      .tie = weighted_sum->tie / denominator,
      .loss = weighted_sum->loss / denominator,
      .expected_final_margin =
          weighted_sum->expected_final_margin / denominator,
  };
}

static bool cpeg_wtl_should_take(const CpegWtlValue *candidate,
                                 const CpegWtlValue *incumbent,
                                 bool root_turn) {
  const int comparison = cpeg_wtl_compare(candidate, incumbent);
  return root_turn ? comparison > 0 : comparison < 0;
}

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
    if (cpeg_interval_should_cancel(ctx)) {
      unplay_move_incremental(game, move_undo);
      return 0.0;
    }
    const CpegMultiset *draw = &draws[draw_idx];
    Bag *child_bag = game_get_bag(game);
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int i = 0; i < draw->n; i++) {
      bag_draw_letter(child_bag, draw->tiles[i], on_turn);
      rack_add_letter(mover_rack, draw->tiles[i]);
    }
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const double child_value = cpeg_value(ctx, game, 0, depth + 1);
    if (!ctx->complete) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      unplay_move_incremental(game, move_undo);
      return 0.0;
    }
    weighted_sum += (double)draw->weight * child_value;
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

// Root-perspective counterpart of cpeg_eval_place. The move score updates the
// explicit root margin according to the actor, and chance branches average a
// fully normalized outcome vector at this draw node.
static CpegWtlValue cpeg_eval_place_wtl(CpegPreCtx *ctx, Game *game,
                                        const Move *move, int64_t root_margin,
                                        int depth) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  const int tiles_played = move_get_tiles_played(move);
  const int k_drawn = tiles_played < bag_count ? tiles_played : bag_count;
  const int score = equity_to_int(move_get_score(move));
  int64_t next_margin;
  const int64_t score_delta =
      on_turn == ctx->mover_idx ? (int64_t)score : -(int64_t)score;
  if (!cpeg_checked_margin_add(root_margin, score_delta, &next_margin)) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }

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
    ctx->complete = false;
    return (CpegWtlValue){0};
  }

  MoveUndo *move_undo = cpeg_get_move_undo(ctx, depth);
  play_move_incremental(move, game, move_undo);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  CpegWtlValue weighted_sum = {0};
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      unplay_move_incremental(game, move_undo);
      return (CpegWtlValue){0};
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
    const CpegWtlValue child =
        cpeg_wtl_value(ctx, game, next_margin, 0, depth + 1);
    if (!ctx->complete || !cpeg_wtl_accumulate(&weighted_sum, &weight_total,
                                               &child, draw->weight)) {
      ctx->complete = false;
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      unplay_move_incremental(game, move_undo);
      return (CpegWtlValue){0};
    }
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  unplay_move_incremental(game, move_undo);
  if (weight_total <= 0) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }
  return cpeg_wtl_normalize(&weighted_sum, weight_total);
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
    if (!ctx->complete) {
      return 0.0;
    }
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
    if (cpeg_interval_should_cancel(ctx)) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return 0.0;
    }
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
    const double child_value = cpeg_value(ctx, game, scoreless + 1, depth + 1);
    if (!ctx->complete) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return 0.0;
    }
    weighted_sum += (double)draw->weight * child_value;
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

static CpegWtlValue cpeg_eval_scoreless_wtl(CpegPreCtx *ctx, Game *game,
                                            const MachineLetter *exch_tiles,
                                            int exch_n, int64_t root_margin,
                                            int scoreless, int depth) {
  // The model's second consecutive scoreless action ends the game without
  // changing the score. Classify the accumulated margin, not a neutral value.
  if (scoreless + 1 >= CPEG_SCORELESS_CAP) {
    return cpeg_wtl_classify_margin(root_margin);
  }
  const int on_turn = game_get_player_on_turn_index(game);
  CpegBranchUndo branch_undo;
  cpeg_save_branch(ctx, game, on_turn, depth, &branch_undo);

  if (exch_n == 0) {
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    const CpegWtlValue value =
        cpeg_wtl_value(ctx, game, root_margin, scoreless + 1, depth + 1);
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    return ctx->complete ? value : (CpegWtlValue){0};
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
    ctx->complete = false;
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
    return (CpegWtlValue){0};
  }

  CpegWtlValue weighted_sum = {0};
  int64_t weight_total = 0;
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return (CpegWtlValue){0};
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
    const CpegWtlValue child =
        cpeg_wtl_value(ctx, game, root_margin, scoreless + 1, depth + 1);
    if (!ctx->complete || !cpeg_wtl_accumulate(&weighted_sum, &weight_total,
                                               &child, draw->weight)) {
      ctx->complete = false;
      cpeg_restore_branch(ctx, game, depth, &branch_undo);
      return (CpegWtlValue){0};
    }
    cpeg_restore_branch(ctx, game, depth, &branch_undo);
  }
  if (weight_total <= 0) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }
  return cpeg_wtl_normalize(&weighted_sum, weight_total);
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
  if (cpeg_interval_should_cancel(ctx)) {
    return 0.0;
  }
  if (ctx->capacity_exceeded) {
    return 0.0;
  }
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  if (bag_count == 0) {
    CpegResult leaf;
    const double value =
        (double)cpeg_endgame_core(game, ctx->eg_mover, ctx->eg_reply,
                                  ctx->eg_undo, &leaf, &ctx->capacity_exceeded);
    return cpeg_interval_should_cancel(ctx) ? 0.0 : value;
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
    if (cpeg_interval_should_cancel(ctx)) {
      return 0.0;
    }
    any_placement = true;
    const double value = cpeg_eval_place(ctx, game, move, depth);
    if (!ctx->complete) {
      return 0.0;
    }
    if (!have_best || value > best) {
      best = value;
      have_best = true;
    }
  }

  // Pass is searched only when there is no legal placement.
  if (!any_placement) {
    const double value =
        cpeg_eval_scoreless(ctx, game, NULL, 0, scoreless, depth);
    if (!ctx->complete) {
      return 0.0;
    }
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
      if (cpeg_interval_should_cancel(ctx)) {
        return 0.0;
      }
      const double value =
          cpeg_eval_scoreless(ctx, game, exchanges[exch_idx].tiles,
                              exchanges[exch_idx].n, scoreless, depth);
      if (!ctx->complete) {
        return 0.0;
      }
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
    const int swing =
        cpeg_endgame_core(game, ctx->eg_mover, ctx->eg_reply, ctx->eg_undo,
                          &leaf, &ctx->capacity_exceeded);
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

// Strict-outcome expectiminimax from the original root mover's perspective.
// Root turns maximize and opponent turns minimize the same lexicographic W/T/L
// objective; values are never negated when the side to move changes.
static CpegWtlValue cpeg_wtl_value(CpegPreCtx *ctx, Game *game,
                                   int64_t root_margin, int scoreless,
                                   int depth) {
  if (cpeg_interval_should_cancel(ctx) || ctx->capacity_exceeded) {
    return (CpegWtlValue){0};
  }
  const Bag *bag = game_get_bag(game);
  const int bag_count = bag_get_letters(bag);
  if (bag_count == 0) {
    CpegResult leaf;
    bool capacity_exceeded = false;
    const int on_turn_swing =
        cpeg_endgame_core(game, ctx->eg_mover, ctx->eg_reply, ctx->eg_undo,
                          &leaf, &capacity_exceeded);
    if (capacity_exceeded) {
      ctx->capacity_exceeded = true;
      ctx->complete = false;
      return (CpegWtlValue){0};
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const int64_t root_swing = on_turn == ctx->mover_idx
                                   ? (int64_t)on_turn_swing
                                   : -(int64_t)on_turn_swing;
    int64_t final_margin;
    if (!cpeg_checked_margin_add(root_margin, root_swing, &final_margin)) {
      ctx->complete = false;
      return (CpegWtlValue){0};
    }
    return cpeg_wtl_classify_margin(final_margin);
  }
  if (depth >= CPEG_MAX_DEPTH - 1) {
    ctx->complete = false;
    return (CpegWtlValue){0};
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
  const int count = move_list_get_count(move_list);
  if (count > CPEG_MOVE_LIST_CAP) {
    ctx->capacity_exceeded = true;
    ctx->complete = false;
    return (CpegWtlValue){0};
  }

  const bool root_turn = game_get_player_on_turn_index(game) == ctx->mover_idx;
  CpegWtlValue best = {0};
  bool have_best = false;
  bool any_placement = false;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegWtlValue){0};
    }
    any_placement = true;
    const CpegWtlValue value =
        cpeg_eval_place_wtl(ctx, game, move, root_margin, depth);
    if (!ctx->complete) {
      return (CpegWtlValue){0};
    }
    if (!have_best || cpeg_wtl_should_take(&value, &best, root_turn)) {
      best = value;
      have_best = true;
    }
  }

  // Preserve the legacy model: deeper voluntary pass is searched only when no
  // placement exists. Root pass remains part of the complete root collector.
  if (!any_placement) {
    const CpegWtlValue value = cpeg_eval_scoreless_wtl(
        ctx, game, NULL, 0, root_margin, scoreless, depth);
    if (!ctx->complete) {
      return (CpegWtlValue){0};
    }
    if (!have_best || cpeg_wtl_should_take(&value, &best, root_turn)) {
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
      ctx->complete = false;
      return (CpegWtlValue){0};
    }
    for (int exch_idx = 0; exch_idx < n_exch; exch_idx++) {
      if (cpeg_interval_should_cancel(ctx)) {
        return (CpegWtlValue){0};
      }
      const CpegWtlValue value = cpeg_eval_scoreless_wtl(
          ctx, game, exchanges[exch_idx].tiles, exchanges[exch_idx].n,
          root_margin, scoreless, depth);
      if (!ctx->complete) {
        return (CpegWtlValue){0};
      }
      if (!have_best || cpeg_wtl_should_take(&value, &best, root_turn)) {
        best = value;
        have_best = true;
      }
    }
  }

  if (!have_best) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }
  return best;
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
  ctx->eg_mover = move_list_create(CPEG_MOVE_LIST_CAP + 1);
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

typedef struct CpegRootCollection {
  CpegRootCand *candidates;
  int count;
  CpegRootCoverage coverage;
} CpegRootCollection;

static void cpeg_root_collection_destroy(CpegRootCollection *collection) {
  free(collection->candidates);
  memset(collection, 0, sizeof(*collection));
}

// Build the complete root action set once for every solver mode. Placement
// storage is sized from the generated count; exchanges have the proven
// CPEG_ENUM_CAP bound; voluntary pass is always a root action.
static bool cpeg_collect_root_candidates(Game *game, int bag,
                                         bool allow_exchanges,
                                         CpegRootCollection *collection) {
  memset(collection, 0, sizeof(*collection));
  if (game == NULL || bag < 1 || bag > PEG_MAX_BAG) {
    return false;
  }

  CrossplayOracleActionSet actions;
  const CrossplayOracleStatus status = crossplay_oracle_generate_actions(
      game, bag, allow_exchanges, &actions);
  if (status != CROSSPLAY_ORACLE_OK || !actions.coverage.complete) {
    crossplay_oracle_action_set_destroy(&actions);
    return false;
  }

  CpegRootCand *candidates =
      calloc_or_die((size_t)actions.count, sizeof(*candidates));
  for (int action_idx = 0; action_idx < actions.count; action_idx++) {
    const CrossplayOracleAction *action = &actions.actions[action_idx];
    CpegRootCand *candidate = &candidates[action_idx];
    candidate->score = action->score;
    if (action->kind == CROSSPLAY_ORACLE_PLACEMENT) {
      candidate->kind = 0;
      move_copy(&candidate->move, &action->move);
    } else if (action->kind == CROSSPLAY_ORACLE_PASS) {
      candidate->kind = 1;
    } else {
      candidate->kind = 2;
      candidate->exch_n = action->exchange_count;
      memcpy(candidate->exch_tiles, action->exchange_tiles,
             (size_t)candidate->exch_n * sizeof(*candidate->exch_tiles));
    }
  }

  collection->candidates = candidates;
  collection->count = actions.count;
  collection->coverage = (CpegRootCoverage){
      .placements = actions.coverage.placements,
      .exchanges = actions.coverage.exchanges,
      .passes = actions.coverage.passes,
      .total = actions.coverage.total,
      .generation_complete = actions.coverage.complete,
  };
  crossplay_oracle_action_set_destroy(&actions);
  return true;
}

int cpeg_count_root_actions(Game *game, int bag, bool allow_exchanges,
                            CpegRootCoverage *coverage) {
  if (coverage == NULL) {
    return -1;
  }
  memset(coverage, 0, sizeof(*coverage));
  CpegRootCollection collection;
  if (!cpeg_collect_root_candidates(game, bag, allow_exchanges, &collection)) {
    return -1;
  }
  *coverage = collection.coverage;
  const int count = collection.count;
  cpeg_root_collection_destroy(&collection);
  return count;
}

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
    if (cpeg_interval_should_cancel(ctx)) {
      return 0.0;
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
    const double child_value = cpeg_value(ctx, child, 0, depth + 1);
    if (!ctx->complete) {
      return 0.0;
    }
    weighted_sum += (double)draw->weight * child_value;
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

static CpegWtlValue cpeg_eval_post_place_wtl(CpegPreCtx *ctx,
                                             const Game *post_place_game,
                                             int tiles_played, int score,
                                             int64_t initial_lead, int depth) {
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
    ctx->complete = false;
    return (CpegWtlValue){0};
  }

  CpegWtlValue weighted_sum = {0};
  int64_t weight_total = 0;
  int64_t root_margin;
  if (!cpeg_checked_margin_add(initial_lead, (int64_t)score, &root_margin)) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }
  for (int draw_idx = 0; draw_idx < n_draws; draw_idx++) {
    if (cpeg_interval_should_cancel(ctx)) {
      return (CpegWtlValue){0};
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
    const CpegWtlValue child_value =
        cpeg_wtl_value(ctx, child, root_margin, 0, depth + 1);
    if (!ctx->complete || !cpeg_wtl_accumulate(&weighted_sum, &weight_total,
                                               &child_value, draw->weight)) {
      ctx->complete = false;
      return (CpegWtlValue){0};
    }
  }
  if (weight_total <= 0) {
    ctx->complete = false;
    return (CpegWtlValue){0};
  }
  return cpeg_wtl_normalize(&weighted_sum, weight_total);
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

static CpegWtlValue cpeg_eval_root_scoreless_wtl(CpegPreCtx *ctx,
                                                 Game *world_game,
                                                 const CpegRootCand *cand,
                                                 int64_t initial_lead) {
  if (cand->kind == 1) {
    return cpeg_eval_scoreless_wtl(ctx, world_game, NULL, 0, initial_lead,
                                   /*scoreless=*/0, /*depth=*/0);
  }
  return cpeg_eval_scoreless_wtl(ctx, world_game, cand->exch_tiles,
                                 cand->exch_n, initial_lead,
                                 /*scoreless=*/0, /*depth=*/0);
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
  CpegWtlValue wtl_value;
  int64_t initial_lead;
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
  worker->ctx.complete = true;
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
  job->complete = worker->ctx.complete;
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

static void cpeg_root_wtl_job_run(void *arg, int worker_idx) {
  CpegRootJob *job = (CpegRootJob *)arg;
  CpegWorker *worker = &job->workers[worker_idx];
  worker->ctx.complete = true;
  cpeg_set_world(worker->world_game, job->source_game, job->world, job->unseen,
                 job->ld_size, job->opp_idx);
  if (job->cand->kind == 0) {
    job->wtl_value = cpeg_eval_post_place_wtl(
        &worker->ctx, worker->world_game,
        move_get_tiles_played(&job->cand->move), job->cand->score,
        job->initial_lead, /*depth=*/0);
  } else {
    job->wtl_value = cpeg_eval_root_scoreless_wtl(
        &worker->ctx, worker->world_game, job->cand, job->initial_lead);
  }
  job->complete = worker->ctx.complete && !worker->ctx.capacity_exceeded;
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

void cpeg_pre_result_destroy(CpegPreResult *result) {
  if (result == NULL) {
    return;
  }
  free(result->cands);
  memset(result, 0, sizeof(*result));
}

void cpeg_wtl_result_destroy(CpegWtlResult *result) {
  if (result == NULL) {
    return;
  }
  free(result->cands);
  memset(result, 0, sizeof(*result));
}

void cpeg_certified_result_destroy(CpegCertifiedResult *result) {
  if (result == NULL) {
    return;
  }
  free(result->cands);
  memset(result, 0, sizeof(*result));
  result->best_index = -1;
}

void cpeg_wtl_certified_result_destroy(CpegWtlCertifiedResult *result) {
  if (result == NULL) {
    return;
  }
  free(result->candidate_traces);
  free(result->cands);
  memset(result, 0, sizeof(*result));
  result->best_index = -1;
}

void cpeg_statistical_result_destroy(CpegStatisticalResult *result) {
  if (result == NULL) {
    return;
  }
  free(result->cands);
  memset(result, 0, sizeof(*result));
  result->best_index = -1;
}

int cpeg_solve_pre_endgame(Game *game, int bag, bool allow_exchanges,
                           int num_threads, CpegPreResult *out) {
  if (game == NULL || out == NULL) {
    return -1;
  }
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

  CpegRootCollection root_collection;
  if (!cpeg_collect_root_candidates(game, bag, allow_exchanges,
                                    &root_collection)) {
    return -1;
  }
  CpegRootCand *cands = root_collection.candidates;
  const int n_cands = root_collection.count;
  out->coverage = root_collection.coverage;

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
  if (!search_capacity_exceeded) {
    out->cands = calloc_or_die((size_t)n_cands, sizeof(*out->cands));
  }
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

int cpeg_solve_pre_endgame_wtl(const Game *game, const CpegWtlArgs *args,
                               CpegWtlResult *out) {
  if (game == NULL || args == NULL || out == NULL || args->bag < 1 ||
      args->bag > PEG_MAX_BAG) {
    return -1;
  }
  memset(out, 0, sizeof(*out));

  int result = -1;
  Game *root_game = game_duplicate(game);
  const LetterDistribution *ld = game_get_ld(root_game);
  const int ld_size = ld_get_size(ld);
  Board *board = game_get_board(root_game);
  game_gen_all_cross_sets(root_game);
  board_set_cross_sets_valid(board, true);
  const int mover_idx = game_get_player_on_turn_index(root_game);
  const int opp_idx = 1 - mover_idx;

  CpegRootCollection root_collection = {0};
  CpegMultiset *worlds = NULL;
  Game **templates = NULL;
  PegPool *pool = NULL;
  CpegWorker *workers = NULL;
  CpegRootJob *jobs = NULL;
  void **job_ptrs = NULL;
  int n_scratch = 0;

  int unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = cpeg_compute_unseen(root_game, mover_idx, unseen);
  const int opp_size = total_unseen - args->bag;
  if (opp_size < 0 || opp_size > RACK_SIZE ||
      !cpeg_collect_root_candidates(root_game, args->bag, args->allow_exchanges,
                                    &root_collection)) {
    goto cleanup;
  }
  CpegRootCand *candidates = root_collection.candidates;
  const int candidate_count = root_collection.count;

  worlds = malloc_or_die(CPEG_WORLD_CAP * sizeof(*worlds));
  bool world_overflow = false;
  const int world_count = cpeg_enum_submultisets(
      unseen, ld_size, args->bag, worlds, CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow || world_count < 1) {
    goto cleanup;
  }

  int64_t world_weight_mass = 0;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    if (worlds[world_idx].weight <= 0 ||
        INT64_MAX - world_weight_mass < worlds[world_idx].weight) {
      goto cleanup;
    }
    world_weight_mass += worlds[world_idx].weight;
  }

  templates = calloc_or_die((size_t)candidate_count, sizeof(*templates));
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidates[candidate_idx].kind == 0) {
      templates[candidate_idx] =
          cpeg_build_root_template(root_game, &candidates[candidate_idx].move);
    }
  }

  const int thread_count = args->num_threads < 1 ? 1 : args->num_threads;
  pool = thread_count > 1 ? peg_pool_create(thread_count, 0) : NULL;
  if (pool != NULL) {
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  n_scratch = pool != NULL ? thread_count + 1 : 1;
  workers = malloc_or_die((size_t)n_scratch * sizeof(*workers));
  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    cpeg_ctx_init(&workers[worker_idx].ctx, ld, ld_size, mover_idx,
                  args->allow_exchanges, /*deadline_ns=*/0);
    workers[worker_idx].world_game = game_duplicate(root_game);
  }

  const int job_count = candidate_count * world_count;
  jobs = calloc_or_die((size_t)job_count, sizeof(*jobs));
  job_ptrs = malloc_or_die((size_t)job_count * sizeof(*job_ptrs));
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      const int job_idx = world_idx * candidate_count + candidate_idx;
      CpegRootJob *job = &jobs[job_idx];
      job->workers = workers;
      job->source_game = templates[candidate_idx] != NULL
                             ? templates[candidate_idx]
                             : root_game;
      job->world = &worlds[world_idx];
      job->unseen = unseen;
      job->ld_size = ld_size;
      job->opp_idx = opp_idx;
      job->cand = &candidates[candidate_idx];
      job->initial_lead = args->initial_lead;
      job_ptrs[job_idx] = job;
    }
  }
  const int helper_worker_idx = pool != NULL ? thread_count : 0;
  peg_pool_submit_and_wait(pool, cpeg_root_wtl_job_run, job_ptrs, job_count,
                           helper_worker_idx);

  for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
    if (!workers[worker_idx].ctx.complete ||
        workers[worker_idx].ctx.capacity_exceeded) {
      goto cleanup;
    }
  }
  for (int job_idx = 0; job_idx < job_count; job_idx++) {
    if (!jobs[job_idx].complete) {
      goto cleanup;
    }
  }

  out->cands = calloc_or_die((size_t)candidate_count, sizeof(*out->cands));
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    CpegWtlValue weighted_sum = {0};
    int64_t weight_total = 0;
    // Fixed generation-order reduction makes results independent of worker
    // completion order and thread count.
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      const int job_idx = world_idx * candidate_count + candidate_idx;
      if (!cpeg_wtl_accumulate(&weighted_sum, &weight_total,
                               &jobs[job_idx].wtl_value,
                               worlds[world_idx].weight)) {
        goto cleanup;
      }
    }
    if (weight_total != world_weight_mass) {
      goto cleanup;
    }
    CpegWtlCand *slot = &out->cands[candidate_idx];
    slot->score = candidates[candidate_idx].score;
    slot->value = cpeg_wtl_normalize(&weighted_sum, weight_total);
    cpeg_render_root_candidate(slot->label, &candidates[candidate_idx], board,
                               ld);
  }

  // Stable insertion sort: exact objective ties use the rendered action label
  // as a stable identity; duplicate identities retain generation order.
  for (int candidate_idx = 1; candidate_idx < candidate_count;
       candidate_idx++) {
    const CpegWtlCand candidate = out->cands[candidate_idx];
    int insertion_idx = candidate_idx;
    while (insertion_idx > 0) {
      const CpegWtlCand *previous = &out->cands[insertion_idx - 1];
      const int objective_comparison =
          cpeg_wtl_compare(&candidate.value, &previous->value);
      if (objective_comparison < 0 ||
          (objective_comparison == 0 &&
           strcmp(candidate.label, previous->label) >= 0)) {
        break;
      }
      out->cands[insertion_idx] = out->cands[insertion_idx - 1];
      insertion_idx--;
    }
    out->cands[insertion_idx] = candidate;
  }
  out->count = candidate_count;
  out->worlds_distinct = world_count;
  out->world_weight_mass = world_weight_mass;
  out->coverage = root_collection.coverage;
  result = candidate_count;

cleanup:
  free(job_ptrs);
  free(jobs);
  peg_pool_destroy(pool);
  if (workers != NULL) {
    for (int worker_idx = 0; worker_idx < n_scratch; worker_idx++) {
      game_destroy(workers[worker_idx].world_game);
      cpeg_ctx_destroy(&workers[worker_idx].ctx);
    }
  }
  free(workers);
  if (templates != NULL) {
    for (int candidate_idx = 0; candidate_idx < root_collection.count;
         candidate_idx++) {
      if (templates[candidate_idx] != NULL) {
        game_destroy(templates[candidate_idx]);
      }
    }
  }
  free(templates);
  free(worlds);
  cpeg_root_collection_destroy(&root_collection);
  game_destroy(root_game);
  if (result < 0) {
    cpeg_wtl_result_destroy(out);
  }
  return result;
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

// Build the hidden-world schedule from either the neutral physical inventory
// prior or a caller-supplied exact integer posterior. The supplied form is
// deliberately bag-only: the opponent rack is the unique unseen complement.
// Invalid, duplicate, nonconserving, or overflowing worlds fail closed.
static int cpeg_prepare_scheduled_worlds(
    const int unseen[MAX_ALPHABET_SIZE], int ld_size, int bag_size,
    const CpegWeightedWorld *weighted_worlds, int weighted_world_count,
    CpegScheduledWorld **worlds_out, int64_t *mass_out) {
  if (worlds_out == NULL || mass_out == NULL || bag_size < 1 ||
      bag_size > PEG_MAX_BAG || weighted_world_count < 0 ||
      weighted_world_count > CPEG_WORLD_CAP ||
      ((weighted_worlds == NULL) != (weighted_world_count == 0))) {
    return -1;
  }

  int64_t maximum_draw_mass = 1;
  for (int draw_count = 0; draw_count <= bag_size; draw_count++) {
    const int64_t draw_mass = peg_binomial(bag_size, draw_count);
    if (draw_mass > maximum_draw_mass) {
      maximum_draw_mass = draw_mass;
    }
  }
  const int64_t safe_world_mass = INT64_MAX / maximum_draw_mass;

  if (weighted_worlds == NULL) {
    CpegMultiset enumerated_worlds[CPEG_WORLD_CAP];
    bool world_overflow = false;
    const int world_count =
        cpeg_enum_submultisets(unseen, ld_size, bag_size, enumerated_worlds,
                               CPEG_WORLD_CAP, &world_overflow);
    if (world_overflow || world_count < 1) {
      return -1;
    }
    CpegScheduledWorld *worlds =
        malloc_or_die((size_t)world_count * sizeof(*worlds));
    int64_t mass = 0;
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      if (enumerated_worlds[world_idx].weight <= 0 ||
          safe_world_mass - mass < enumerated_worlds[world_idx].weight) {
        free(worlds);
        return -1;
      }
      worlds[world_idx] = (CpegScheduledWorld){
          .multiset = enumerated_worlds[world_idx],
          .generation_index = world_idx,
      };
      mass += enumerated_worlds[world_idx].weight;
    }
    cpeg_sort_scheduled_worlds(worlds, world_count);
    *worlds_out = worlds;
    *mass_out = mass;
    return world_count;
  }

  if (weighted_world_count < 1) {
    return -1;
  }
  CpegScheduledWorld *worlds =
      malloc_or_die((size_t)weighted_world_count * sizeof(*worlds));
  int64_t mass = 0;
  for (int world_idx = 0; world_idx < weighted_world_count; world_idx++) {
    const CpegWeightedWorld *supplied = &weighted_worlds[world_idx];
    if (supplied->bag_count != bag_size || supplied->weight <= 0 ||
        safe_world_mass - mass < supplied->weight) {
      free(worlds);
      return -1;
    }
    int used[MAX_ALPHABET_SIZE] = {0};
    MachineLetter previous = 0;
    for (int tile_idx = 0; tile_idx < supplied->bag_count; tile_idx++) {
      const MachineLetter tile = supplied->bag_tiles[tile_idx];
      if ((int)tile >= ld_size || (tile_idx > 0 && tile < previous) ||
          ++used[tile] > unseen[tile]) {
        free(worlds);
        return -1;
      }
      previous = tile;
    }
    for (int prior_idx = 0; prior_idx < world_idx; prior_idx++) {
      bool duplicate = true;
      for (int tile_idx = 0; tile_idx < bag_size; tile_idx++) {
        if (weighted_worlds[prior_idx].bag_tiles[tile_idx] !=
            supplied->bag_tiles[tile_idx]) {
          duplicate = false;
          break;
        }
      }
      if (duplicate) {
        free(worlds);
        return -1;
      }
    }
    CpegMultiset multiset = {
        .n = supplied->bag_count,
        .weight = supplied->weight,
    };
    for (int tile_idx = 0; tile_idx < supplied->bag_count; tile_idx++) {
      multiset.tiles[tile_idx] = supplied->bag_tiles[tile_idx];
    }
    worlds[world_idx] = (CpegScheduledWorld){
        .multiset = multiset,
        .generation_index = world_idx,
    };
    mass += supplied->weight;
  }
  cpeg_sort_scheduled_worlds(worlds, weighted_world_count);
  *worlds_out = worlds;
  *mass_out = mass;
  return weighted_world_count;
}

typedef struct CpegWtlProofWorld {
  CpegWtlEnvelope envelope;
  CpegWtlProofKind proof;
  int64_t draw_mass;
  int64_t win_lower_mass;
  int64_t win_upper_mass;
  int64_t tie_lower_mass;
  int64_t tie_upper_mass;
  int64_t loss_lower_mass;
  int64_t loss_upper_mass;
} CpegWtlProofWorld;

typedef struct CpegWtlProofState {
  int64_t win_lower_mass;
  int64_t win_upper_mass;
  int64_t tie_lower_mass;
  int64_t tie_upper_mass;
  int64_t loss_lower_mass;
  int64_t loss_upper_mass;
  int64_t outcome_mass;
  CpegWtlEnvelope outcome;
} CpegWtlProofState;

enum {
  CPEG_ROOT_REPLY_CACHE_CAPACITY = 4096,
  CPEG_EXACT_ENDGAME_CACHE_CAPACITY = 32768,
};

typedef struct CpegRootReplyCacheEntry {
  uint64_t hash;
  int exact_score;
  uint16_t rack_dist_size;
  MachineLetter board_letters[BOARD_DIM * BOARD_DIM];
  uint8_t rack_counts[MAX_ALPHABET_SIZE];
} CpegRootReplyCacheEntry;

typedef struct CpegRootReplyCacheKey {
  uint64_t hash;
  uint16_t rack_dist_size;
  uint8_t rack_counts[MAX_ALPHABET_SIZE];
  const Board *board;
} CpegRootReplyCacheKey;

typedef struct CpegExactEndgameCacheEntry {
  uint64_t hash;
  int swing;
  uint16_t rack_dist_size;
  uint8_t player_on_turn;
  MachineLetter board_letters[BOARD_DIM * BOARD_DIM];
  uint8_t rack_counts[2][MAX_ALPHABET_SIZE];
} CpegExactEndgameCacheEntry;

typedef struct CpegExactEndgameCacheKey {
  uint64_t hash;
  uint16_t rack_dist_size;
  uint8_t player_on_turn;
  uint8_t rack_counts[2][MAX_ALPHABET_SIZE];
  const Board *board;
} CpegExactEndgameCacheKey;

typedef struct CpegExactEndgameCache {
  atomic_flag lock;
  CpegExactEndgameCacheEntry entries[CPEG_EXACT_ENDGAME_CACHE_CAPACITY];
} CpegExactEndgameCache;

typedef struct CpegDefenseWorker {
  Game *opponent_game;
  Game *draw_game;
  Game *defense_game;
  MoveList *opponent_moves;
  MoveList *root_best;
  MoveList *root_best_small;
  CpegRootReplyCacheEntry *root_reply_cache;
  CpegExactEndgameCache *exact_endgame_cache;
  MoveUndo *defense_undo;
  MoveList *endgame_mover;
  MoveList *endgame_reply;
  MoveUndo *endgame_undo;
} CpegDefenseWorker;

typedef struct CpegDefenseJob {
  CpegDefenseWorker *workers;
  const Game *source_game;
  const CpegMultiset *world;
  const int *unseen;
  int ld_size;
  int opponent_idx;
  int root_idx;
  const CpegRootCand *candidate;
  int64_t initial_lead;
  int64_t deadline_ns;
  CpegInterval margin_prior;
  bool exhaustive_horizon;
  bool use_exact_two_ply;
  bool use_threshold_reply_screen;
  CpegWtlReplyPhase reply_phase;
  int max_defenses;
  bool collect_trace;
  bool complete;
  bool capacity_exceeded;
  CpegWtlProofWorld result;
  CpegWtlTrace trace;
} CpegDefenseJob;

static CpegWtlEnvelope cpeg_wtl_exact_envelope(int64_t final_margin) {
  const CpegWtlValue value = cpeg_wtl_classify_margin(final_margin);
  return (CpegWtlEnvelope){
      .estimate = value,
      .win = {.lo = value.win, .hi = value.win},
      .tie = {.lo = value.tie, .hi = value.tie},
      .loss = {.lo = value.loss, .hi = value.loss},
      .expected_final_margin =
          {
              .lo = (double)final_margin,
              .hi = (double)final_margin,
          },
  };
}

static int cpeg_small_move_pointer_compare(const void *lhs, const void *rhs) {
  const SmallMove *const lhs_move = *(const SmallMove *const *)lhs;
  const SmallMove *const rhs_move = *(const SmallMove *const *)rhs;
  const int lhs_score = small_move_get_score(lhs_move);
  const int rhs_score = small_move_get_score(rhs_move);
  if (lhs_score != rhs_score) {
    return lhs_score > rhs_score ? -1 : 1;
  }
  if (lhs_move->tiny_move != rhs_move->tiny_move) {
    return lhs_move->tiny_move < rhs_move->tiny_move ? -1 : 1;
  }
  return 0;
}

static bool cpeg_defense_deadline_reached(int64_t deadline_ns) {
  return deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns;
}

static void cpeg_wtl_trace_add_work(CpegWtlTrace *dest,
                                    const CpegWtlTrace *source) {
  if (dest == NULL || source == NULL) {
    return;
  }
  dest->public_state_batches += source->public_state_batches;
  dest->scheduler_batches += source->scheduler_batches;
  dest->defense_world_jobs += source->defense_world_jobs;
  dest->opponent_movegen_calls += source->opponent_movegen_calls;
  dest->opponent_moves_generated += source->opponent_moves_generated;
  dest->opponent_movegen_work_ns += source->opponent_movegen_work_ns;
  dest->opponent_sort_calls += source->opponent_sort_calls;
  dest->opponent_moves_sorted += source->opponent_moves_sorted;
  dest->opponent_sort_work_ns += source->opponent_sort_work_ns;
  dest->defenses_threshold_tested += source->defenses_threshold_tested;
  dest->defenses_accepted += source->defenses_accepted;
  dest->defenses_refuted += source->defenses_refuted;
  dest->compatible_draws_tested += source->compatible_draws_tested;
  dest->final_reply_queries += source->final_reply_queries;
  dest->final_replies_generated += source->final_replies_generated;
  dest->final_reply_cache_hits += source->final_reply_cache_hits;
  dest->final_reply_movegen_work_ns +=
      source->final_reply_movegen_work_ns;
  dest->threshold_short_circuits += source->threshold_short_circuits;
  for (int phase = 0; phase < CPEG_WTL_REPLY_PHASE_COUNT; phase++) {
    dest->reply_phases[phase].queries +=
        source->reply_phases[phase].queries;
    dest->reply_phases[phase].replies_generated +=
        source->reply_phases[phase].replies_generated;
    dest->reply_phases[phase].cache_hits +=
        source->reply_phases[phase].cache_hits;
    dest->reply_phases[phase].movegen_work_ns +=
        source->reply_phases[phase].movegen_work_ns;
    dest->reply_phases[phase].threshold_short_circuits +=
        source->reply_phases[phase].threshold_short_circuits;
  }
  dest->exact_endgame_queries += source->exact_endgame_queries;
  dest->exact_endgame_cache_hits += source->exact_endgame_cache_hits;
  dest->exact_endgame_work_ns += source->exact_endgame_work_ns;
  dest->fixed_endgame_queries += source->fixed_endgame_queries;
  dest->fixed_endgame_cache_hits += source->fixed_endgame_cache_hits;
  dest->fixed_endgame_work_ns += source->fixed_endgame_work_ns;
  dest->fixed_endgame_threshold_queries +=
      source->fixed_endgame_threshold_queries;
  dest->fixed_endgame_threshold_proofs +=
      source->fixed_endgame_threshold_proofs;
  dest->fixed_endgame_threshold_work_ns +=
      source->fixed_endgame_threshold_work_ns;
  for (int rack_tiles = 0; rack_tiles <= RACK_SIZE; rack_tiles++) {
    dest->fixed_endgame_queries_by_opponent_rack[rack_tiles] +=
        source->fixed_endgame_queries_by_opponent_rack[rack_tiles];
    dest->fixed_endgame_cache_hits_by_opponent_rack[rack_tiles] +=
        source->fixed_endgame_cache_hits_by_opponent_rack[rack_tiles];
    dest->fixed_endgame_work_ns_by_opponent_rack[rack_tiles] +=
        source->fixed_endgame_work_ns_by_opponent_rack[rack_tiles];
  }
}

static bool cpeg_generate_small_moves_at_least(
    Game *game, MoveList *moves, move_record_t record_type,
    int minimum_tiles_played, CpegWtlTrace *trace) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = moves,
      .move_record_type = record_type,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .minimum_tiles_played = minimum_tiles_played,
  };
  const int64_t start_ns = trace != NULL ? ctimer_monotonic_ns() : 0;
  generate_moves(&args);
  if (trace != NULL) {
    trace->opponent_movegen_calls++;
    trace->opponent_moves_generated += moves->count;
    trace->opponent_movegen_work_ns += ctimer_monotonic_ns() - start_ns;
  }
  return moves->count <= CPEG_MOVE_LIST_CAP;
}

static bool cpeg_generate_small_moves(Game *game, MoveList *moves,
                                      move_record_t record_type,
                                      CpegWtlTrace *trace) {
  return cpeg_generate_small_moves_at_least(game, moves, record_type, 0, trace);
}

static void cpeg_sort_small_moves(MoveList *moves, CpegWtlTrace *trace) {
  const int64_t start_ns = trace != NULL ? ctimer_monotonic_ns() : 0;
  qsort(moves->small_moves, (size_t)moves->count,
        sizeof(*moves->small_moves), cpeg_small_move_pointer_compare);
  if (trace != NULL) {
    trace->opponent_sort_calls++;
    trace->opponent_moves_sorted += moves->count;
    trace->opponent_sort_work_ns += ctimer_monotonic_ns() - start_ns;
  }
}

typedef enum CpegRootReplyResultKind {
  CPEG_ROOT_REPLY_EXACT,
  CPEG_ROOT_REPLY_ABOVE_THRESHOLD,
} CpegRootReplyResultKind;

typedef struct CpegRootReplyResult {
  CpegRootReplyResultKind kind;
  // Meaningful only when kind is CPEG_ROOT_REPLY_EXACT. A threshold witness
  // proves only that the exact score is greater than the supplied threshold.
  int exact_score;
} CpegRootReplyResult;

static void cpeg_root_reply_cache_key(const Game *game,
                                      CpegRootReplyCacheKey *key) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  key->board = game_get_board(game);
  const int root_idx = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, root_idx));
  key->rack_dist_size = rack_get_dist_size(rack);
  hash = fnv64a_step(hash, key->rack_dist_size);
  memset(key->rack_counts, 0, sizeof(key->rack_counts));
  for (uint16_t ml = 0; ml < key->rack_dist_size; ml++) {
    key->rack_counts[ml] =
        (uint8_t)rack_get_letter(rack, (MachineLetter)ml);
    hash = fnv64a_step(hash, key->rack_counts[ml]);
  }
  key->hash = hash == 0 ? 1 : hash;
}

static bool cpeg_root_reply_cache_entry_matches(
    const CpegRootReplyCacheEntry *entry,
    const CpegRootReplyCacheKey *key) {
  if (entry->hash != key->hash ||
      entry->rack_dist_size != key->rack_dist_size ||
      memcmp(entry->rack_counts, key->rack_counts,
             sizeof(entry->rack_counts)) != 0) {
    return false;
  }
  int square_idx = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (entry->board_letters[square_idx++] !=
          board_get_letter(key->board, row, col)) {
        return false;
      }
    }
  }
  return true;
}

static CpegRootReplyCacheEntry *cpeg_root_reply_cache_lookup(
    CpegRootReplyCacheEntry *cache, const CpegRootReplyCacheKey *key) {
  int slot = (int)(key->hash % CPEG_ROOT_REPLY_CACHE_CAPACITY);
  for (int probe = 0; probe < CPEG_ROOT_REPLY_CACHE_CAPACITY; probe++) {
    CpegRootReplyCacheEntry *entry = &cache[slot];
    if (entry->hash == 0) {
      return NULL;
    }
    if (cpeg_root_reply_cache_entry_matches(entry, key)) {
      return entry;
    }
    slot = (slot + 1) % CPEG_ROOT_REPLY_CACHE_CAPACITY;
  }
  return NULL;
}

static void cpeg_root_reply_cache_insert(
    CpegRootReplyCacheEntry *cache, const CpegRootReplyCacheKey *key,
    int exact_score) {
  int slot = (int)(key->hash % CPEG_ROOT_REPLY_CACHE_CAPACITY);
  for (int probe = 0; probe < CPEG_ROOT_REPLY_CACHE_CAPACITY; probe++) {
    CpegRootReplyCacheEntry *entry = &cache[slot];
    if (entry->hash == 0 || cpeg_root_reply_cache_entry_matches(entry, key)) {
      *entry = (CpegRootReplyCacheEntry){
          .hash = key->hash,
          .exact_score = exact_score,
          .rack_dist_size = key->rack_dist_size,
      };
      int square_idx = 0;
      for (int row = 0; row < BOARD_DIM; row++) {
        for (int col = 0; col < BOARD_DIM; col++) {
          entry->board_letters[square_idx++] =
              board_get_letter(key->board, row, col);
        }
      }
      memcpy(entry->rack_counts, key->rack_counts,
             sizeof(entry->rack_counts));
      return;
    }
    slot = (slot + 1) % CPEG_ROOT_REPLY_CACHE_CAPACITY;
  }
}

static void
cpeg_exact_endgame_cache_key(const Game *game,
                             CpegExactEndgameCacheKey *key) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  key->board = game_get_board(game);
  key->player_on_turn = (uint8_t)game_get_player_on_turn_index(game);
  hash = fnv64a_step(hash, key->player_on_turn);
  const Rack *first_rack = player_get_rack(game_get_player(game, 0));
  key->rack_dist_size = rack_get_dist_size(first_rack);
  hash = fnv64a_step(hash, key->rack_dist_size);
  memset(key->rack_counts, 0, sizeof(key->rack_counts));
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const Rack *rack = player_get_rack(game_get_player(game, player_idx));
    for (uint16_t ml = 0; ml < key->rack_dist_size; ml++) {
      key->rack_counts[player_idx][ml] =
          (uint8_t)rack_get_letter(rack, (MachineLetter)ml);
      hash = fnv64a_step(hash, key->rack_counts[player_idx][ml]);
    }
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      hash = fnv64a_step(hash, board_get_letter(key->board, row, col));
    }
  }
  key->hash = hash == 0 ? 1 : hash;
}

static bool cpeg_exact_endgame_cache_entry_matches(
    const CpegExactEndgameCacheEntry *entry,
    const CpegExactEndgameCacheKey *key) {
  if (entry->hash != key->hash ||
      entry->rack_dist_size != key->rack_dist_size ||
      entry->player_on_turn != key->player_on_turn ||
      memcmp(entry->rack_counts, key->rack_counts,
             sizeof(entry->rack_counts)) != 0) {
    return false;
  }
  int square_idx = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (entry->board_letters[square_idx++] !=
          board_get_letter(key->board, row, col)) {
        return false;
      }
    }
  }
  return true;
}

static void cpeg_exact_endgame_cache_lock(CpegExactEndgameCache *cache) {
  while (atomic_flag_test_and_set_explicit(&cache->lock,
                                            memory_order_acquire)) {
  }
}

static void cpeg_exact_endgame_cache_unlock(CpegExactEndgameCache *cache) {
  atomic_flag_clear_explicit(&cache->lock, memory_order_release);
}

static bool cpeg_exact_endgame_cache_lookup(
    CpegExactEndgameCache *cache, const CpegExactEndgameCacheKey *key,
    int *swing) {
  bool found = false;
  cpeg_exact_endgame_cache_lock(cache);
  int slot = (int)(key->hash % CPEG_EXACT_ENDGAME_CACHE_CAPACITY);
  for (int probe = 0; probe < CPEG_EXACT_ENDGAME_CACHE_CAPACITY; probe++) {
    const CpegExactEndgameCacheEntry *entry = &cache->entries[slot];
    if (entry->hash == 0) {
      break;
    }
    if (cpeg_exact_endgame_cache_entry_matches(entry, key)) {
      *swing = entry->swing;
      found = true;
      break;
    }
    slot = (slot + 1) % CPEG_EXACT_ENDGAME_CACHE_CAPACITY;
  }
  cpeg_exact_endgame_cache_unlock(cache);
  return found;
}

static void cpeg_exact_endgame_cache_insert(
    CpegExactEndgameCache *cache, const CpegExactEndgameCacheKey *key,
    int swing) {
  cpeg_exact_endgame_cache_lock(cache);
  int slot = (int)(key->hash % CPEG_EXACT_ENDGAME_CACHE_CAPACITY);
  for (int probe = 0; probe < CPEG_EXACT_ENDGAME_CACHE_CAPACITY; probe++) {
    CpegExactEndgameCacheEntry *entry = &cache->entries[slot];
    if (entry->hash == 0 ||
        cpeg_exact_endgame_cache_entry_matches(entry, key)) {
      *entry = (CpegExactEndgameCacheEntry){
          .hash = key->hash,
          .swing = swing,
          .rack_dist_size = key->rack_dist_size,
          .player_on_turn = key->player_on_turn,
      };
      int square_idx = 0;
      for (int row = 0; row < BOARD_DIM; row++) {
        for (int col = 0; col < BOARD_DIM; col++) {
          entry->board_letters[square_idx++] =
              board_get_letter(key->board, row, col);
        }
      }
      memcpy(entry->rack_counts, key->rack_counts,
             sizeof(entry->rack_counts));
      break;
    }
    slot = (slot + 1) % CPEG_EXACT_ENDGAME_CACHE_CAPACITY;
  }
  cpeg_exact_endgame_cache_unlock(cache);
}

static int cpeg_exact_endgame_swing(
    Game *game, MoveList *mover_moves, MoveList *reply_moves, MoveUndo *undo,
    CpegExactEndgameCache *cache, bool *capacity_exceeded,
    int64_t deadline_ns, bool *complete, bool *cache_hit) {
  *cache_hit = false;
  if (cache != NULL) {
    CpegExactEndgameCacheKey key = {0};
    cpeg_exact_endgame_cache_key(game, &key);
    int cached_swing = 0;
    if (cpeg_exact_endgame_cache_lookup(cache, &key, &cached_swing)) {
      *cache_hit = true;
      if (capacity_exceeded != NULL) {
        *capacity_exceeded = false;
      }
      if (complete != NULL) {
        *complete = true;
      }
      return cached_swing;
    }
    CpegResult result;
    const int swing = cpeg_endgame_core_until(
        game, mover_moves, reply_moves, undo, &result, capacity_exceeded,
        deadline_ns, complete);
    const bool solved =
        (capacity_exceeded == NULL || !*capacity_exceeded) &&
        (complete == NULL || *complete);
    if (solved) {
      cpeg_exact_endgame_cache_insert(cache, &key, swing);
    }
    return swing;
  }
  CpegResult result;
  return cpeg_endgame_core_until(game, mover_moves, reply_moves, undo, &result,
                                 capacity_exceeded, deadline_ns, complete);
}

static void cpeg_root_reply_trace_query(CpegWtlTrace *trace,
                                        CpegWtlReplyPhase phase,
                                        int replies_generated,
                                        int64_t movegen_work_ns,
                                        bool cache_hit) {
  if (trace == NULL) {
    return;
  }
  trace->final_reply_queries++;
  trace->final_replies_generated += replies_generated;
  trace->final_reply_movegen_work_ns += movegen_work_ns;
  CpegWtlReplyPhaseTrace *phase_trace = &trace->reply_phases[phase];
  phase_trace->queries++;
  phase_trace->replies_generated += replies_generated;
  phase_trace->movegen_work_ns += movegen_work_ns;
  if (cache_hit) {
    trace->final_reply_cache_hits++;
    phase_trace->cache_hits++;
  }
}

static void cpeg_root_reply_trace_threshold_short_circuit(
    CpegWtlTrace *trace, CpegWtlReplyPhase phase) {
  if (trace != NULL) {
    trace->threshold_short_circuits++;
    trace->reply_phases[phase].threshold_short_circuits++;
  }
}

static bool cpeg_root_reply(
    Game *defended_game, MoveList *root_moves,
    MoveList *root_small_moves, CpegRootReplyCacheEntry *cache,
    bool use_threshold_reply_screen, int64_t threshold,
    bool require_exact_score, CpegWtlReplyPhase phase,
    CpegRootReplyResult *result, CpegWtlTrace *trace) {
  CpegRootReplyCacheKey key = {0};
  CpegRootReplyCacheEntry *cache_entry = NULL;
  const bool use_small_reply =
      use_threshold_reply_screen &&
      (phase == CPEG_WTL_REPLY_PHASE_PLACEMENT_SCREEN ||
       phase == CPEG_WTL_REPLY_PHASE_SURVIVING_REFINE ||
       (require_exact_score &&
        (phase == CPEG_WTL_REPLY_PHASE_INCUMBENT ||
         phase == CPEG_WTL_REPLY_PHASE_HORIZON_REFINE)));
  const bool use_cache =
      use_small_reply && !require_exact_score &&
      (phase == CPEG_WTL_REPLY_PHASE_PLACEMENT_SCREEN ||
       phase == CPEG_WTL_REPLY_PHASE_SURVIVING_REFINE);
  if (use_threshold_reply_screen) {
    if (root_small_moves == NULL || cache == NULL) {
      return false;
    }
  }
  if (use_cache) {
    cpeg_root_reply_cache_key(defended_game, &key);
    cache_entry = cpeg_root_reply_cache_lookup(cache, &key);
  }
  if (cache_entry != NULL) {
    cpeg_root_reply_trace_query(trace, phase, 0, 0, true);
    if (!require_exact_score && cache_entry->exact_score > threshold) {
      *result = (CpegRootReplyResult){
          .kind = CPEG_ROOT_REPLY_ABOVE_THRESHOLD,
      };
      cpeg_root_reply_trace_threshold_short_circuit(trace, phase);
    } else {
      *result = (CpegRootReplyResult){
          .kind = CPEG_ROOT_REPLY_EXACT,
          .exact_score = cache_entry->exact_score,
      };
    }
    return true;
  }
  Equity target = EQUITY_MAX_VALUE;
  if (!require_exact_score && threshold >= (int64_t)EQUITY_MIN_DOUBLE &&
      threshold <= (int64_t)EQUITY_MAX_DOUBLE) {
    target = int_to_equity((int)threshold);
  }
  MoveList *selected_moves =
      use_small_reply ? root_small_moves : root_moves;
  if (selected_moves == NULL) {
    return false;
  }
  const MoveGenArgs root_args = {
      .game = defended_game,
      .move_list = selected_moves,
      .move_record_type =
          use_small_reply ? MOVE_RECORD_BEST_SMALL : MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = target,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  const int64_t start_ns = trace != NULL ? ctimer_monotonic_ns() : 0;
  generate_moves(&root_args);
  const int64_t movegen_work_ns =
      trace != NULL ? ctimer_monotonic_ns() - start_ns : 0;
  const int reply_count = move_list_get_count(selected_moves);
  cpeg_root_reply_trace_query(trace, phase, reply_count, movegen_work_ns, false);
  if (reply_count < 1) {
    return false;
  }
  const int score =
      use_small_reply
          ? small_move_get_score(selected_moves->small_moves[0])
          : equity_to_int(move_get_score(move_list_get_move(selected_moves, 0)));
  if (!require_exact_score && score > threshold) {
    *result = (CpegRootReplyResult){
        .kind = CPEG_ROOT_REPLY_ABOVE_THRESHOLD,
    };
    cpeg_root_reply_trace_threshold_short_circuit(trace, phase);
    return true;
  }
  *result = (CpegRootReplyResult){
      .kind = CPEG_ROOT_REPLY_EXACT,
      .exact_score = score,
  };
  if (use_cache) {
    cpeg_root_reply_cache_insert(cache, &key, score);
  }
  return true;
}

static bool
cpeg_root_best_after_defense(CpegDefenseWorker *worker, const Game *draw_game,
                             const SmallMove *defense, int64_t threshold,
                             bool require_exact_score,
                             bool use_threshold_reply_screen,
                             CpegWtlReplyPhase phase,
                             CpegRootReplyResult *result, CpegWtlTrace *trace) {
  game_copy(worker->defense_game, draw_game);
  if (defense == NULL) {
    game_start_next_player_turn(worker->defense_game);
  } else {
    small_move_to_move(worker->opponent_moves->spare_move, defense,
                       game_get_board(worker->defense_game));
    play_move_incremental(worker->opponent_moves->spare_move,
                          worker->defense_game, worker->defense_undo);
  }
  bag_set_to_tiles(game_get_bag(worker->defense_game), NULL, 0);
  game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
  game_set_consecutive_scoreless_turns(worker->defense_game, 0);
  return cpeg_root_reply(
      worker->defense_game, worker->root_best, worker->root_best_small,
      worker->root_reply_cache, use_threshold_reply_screen, threshold,
      require_exact_score, phase, result, trace);
}

static bool cpeg_apply_root_draw(const CpegDefenseJob *job,
                                 CpegDefenseWorker *worker,
                                 const CpegMultiset *draw) {
  cpeg_set_world(worker->draw_game, job->source_game, job->world, job->unseen,
                 job->ld_size, job->opponent_idx);
  if (job->candidate->kind == 1) {
    game_start_next_player_turn(worker->draw_game);
    return true;
  }

  Bag *bag = game_get_bag(worker->draw_game);
  Rack *root_rack =
      player_get_rack(game_get_player(worker->draw_game, job->root_idx));
  for (int tile_idx = 0; tile_idx < draw->n; tile_idx++) {
    bag_draw_letter(bag, draw->tiles[tile_idx], job->root_idx);
    rack_add_letter(root_rack, draw->tiles[tile_idx]);
  }
  if (job->candidate->kind == 2) {
    for (int tile_idx = 0; tile_idx < job->candidate->exch_n; tile_idx++) {
      rack_take_letter(root_rack, job->candidate->exch_tiles[tile_idx]);
      bag_add_letter(bag, job->candidate->exch_tiles[tile_idx], job->root_idx);
    }
    game_start_next_player_turn(worker->draw_game);
  }
  game_set_game_end_reason(worker->draw_game, GAME_END_REASON_NONE);
  game_set_consecutive_scoreless_turns(worker->draw_game, 0);
  return true;
}

static bool
cpeg_defense_for_draw(CpegDefenseJob *job, CpegDefenseWorker *worker,
                      int remaining_bag, int64_t margin_after_root,
                      bool include_voluntary_pass, bool defense_list_complete,
                      CpegWtlEnvelope *out, CpegWtlProofKind *proof) {
  bool have_bound = false;
  int64_t best_margin_upper = INT64_MAX;
  int defenses_tested = 0;
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;

  if (remaining_bag == 0 && include_voluntary_pass) {
    CpegRootReplyResult reply;
    if (!cpeg_root_best_after_defense(
            worker, worker->draw_game, NULL, INT64_MAX,
            /*require_exact_score=*/true, job->use_threshold_reply_screen,
            job->reply_phase, &reply, trace) ||
        reply.kind != CPEG_ROOT_REPLY_EXACT ||
        !cpeg_checked_margin_add(margin_after_root, reply.exact_score,
                                 &best_margin_upper)) {
      return false;
    }
    if (trace != NULL) {
      trace->defenses_threshold_tested++;
      trace->defenses_accepted++;
    }
    have_bound = true;
    defenses_tested++;
  }

  for (int move_idx = 0; move_idx < worker->opponent_moves->count; move_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->complete = false;
      return false;
    }
    const SmallMove *defense = worker->opponent_moves->small_moves[move_idx];
    if (small_move_is_pass(defense) ||
        small_move_get_tiles_played(defense) < remaining_bag) {
      continue;
    }
    CpegRootReplyResult reply;
    const int64_t threshold =
        (int64_t)small_move_get_score(defense) - margin_after_root;
    const bool require_exact_score =
        remaining_bag == 0 && job->max_defenses == 0;
    if (trace != NULL) {
      trace->defenses_threshold_tested++;
    }
    if (!cpeg_root_best_after_defense(
            worker, worker->draw_game, defense, threshold, require_exact_score,
            job->use_threshold_reply_screen, job->reply_phase, &reply, trace)) {
      job->capacity_exceeded = true;
      return false;
    }
    defenses_tested++;
    if (reply.kind == CPEG_ROOT_REPLY_ABOVE_THRESHOLD) {
      if (trace != NULL) {
        trace->defenses_refuted++;
      }
      if (job->max_defenses > 0 && defenses_tested >= job->max_defenses) {
        break;
      }
      continue;
    }
    if (trace != NULL) {
      trace->defenses_accepted++;
    }
    int64_t margin_upper;
    if (!cpeg_checked_margin_add(margin_after_root,
                                 -(int64_t)small_move_get_score(defense),
                                 &margin_upper) ||
        !cpeg_checked_margin_add(margin_upper, reply.exact_score,
                                 &margin_upper)) {
      return false;
    }
    have_bound = true;
    if (margin_upper < best_margin_upper) {
      best_margin_upper = margin_upper;
    }
    if (!job->exhaustive_horizon && remaining_bag == 0 &&
        best_margin_upper < 0) {
      break;
    }
    if (remaining_bag > 0 && best_margin_upper <= 0) {
      break;
    }
    if (job->max_defenses > 0 && defenses_tested >= job->max_defenses) {
      break;
    }
  }

  if (!have_bound) {
    *out = cpeg_wtl_unresolved_envelope(job->margin_prior);
    *proof = CPEG_WTL_PROOF_UNRESOLVED;
    return true;
  }
  if (remaining_bag == 0 && defense_list_complete &&
      (job->exhaustive_horizon || best_margin_upper >= 0)) {
    *out = cpeg_wtl_exact_envelope(best_margin_upper);
    *proof = CPEG_WTL_PROOF_EXACT;
    return true;
  }
  *out =
      cpeg_wtl_envelope_from_margin_upper(best_margin_upper, job->margin_prior);
  *proof = CPEG_WTL_PROOF_DEFENSE_BOUND;
  return true;
}

static CpegWtlEnvelope
cpeg_weighted_draw_envelope(const CpegWtlEnvelope *values,
                            const int64_t *weights, int count,
                            int64_t weight_total) {
  CpegWtlEnvelope result = {0};
  double win_lower = 0.0;
  double win_upper = 0.0;
  double tie_lower = 0.0;
  double tie_upper = 0.0;
  double loss_lower = 0.0;
  double loss_upper = 0.0;
  double margin_lower = 0.0;
  double margin_upper = 0.0;
  for (int value_idx = 0; value_idx < count; value_idx++) {
    const double weight = (double)weights[value_idx];
    win_lower = cpeg_down_add(win_lower,
                              cpeg_down_mul(weight, values[value_idx].win.lo));
    win_upper =
        cpeg_up_add(win_upper, cpeg_up_mul(weight, values[value_idx].win.hi));
    tie_lower = cpeg_down_add(tie_lower,
                              cpeg_down_mul(weight, values[value_idx].tie.lo));
    tie_upper =
        cpeg_up_add(tie_upper, cpeg_up_mul(weight, values[value_idx].tie.hi));
    loss_lower = cpeg_down_add(
        loss_lower, cpeg_down_mul(weight, values[value_idx].loss.lo));
    loss_upper =
        cpeg_up_add(loss_upper, cpeg_up_mul(weight, values[value_idx].loss.hi));
    margin_lower = cpeg_down_add(
        margin_lower,
        cpeg_down_mul(weight, values[value_idx].expected_final_margin.lo));
    margin_upper = cpeg_up_add(
        margin_upper,
        cpeg_up_mul(weight, values[value_idx].expected_final_margin.hi));
  }
  const double denominator = (double)weight_total;
  result.win = (CpegInterval){.lo = cpeg_down_div(win_lower, denominator),
                              .hi = cpeg_up_div(win_upper, denominator)};
  result.tie = (CpegInterval){.lo = cpeg_down_div(tie_lower, denominator),
                              .hi = cpeg_up_div(tie_upper, denominator)};
  result.loss = (CpegInterval){.lo = cpeg_down_div(loss_lower, denominator),
                               .hi = cpeg_up_div(loss_upper, denominator)};
  if (result.win.lo < 0.0) {
    result.win.lo = 0.0;
  }
  if (result.win.hi > 1.0) {
    result.win.hi = 1.0;
  }
  if (result.tie.lo < 0.0) {
    result.tie.lo = 0.0;
  }
  if (result.tie.hi > 1.0) {
    result.tie.hi = 1.0;
  }
  if (result.loss.lo < 0.0) {
    result.loss.lo = 0.0;
  }
  if (result.loss.hi > 1.0) {
    result.loss.hi = 1.0;
  }
  result.expected_final_margin = (CpegInterval){
      .lo = cpeg_down_div(margin_lower, denominator),
      .hi = cpeg_up_div(margin_upper, denominator),
  };
  const double estimate_win = result.win.lo;
  const double estimate_tie = result.tie.lo;
  double estimate_loss = 1.0 - estimate_win - estimate_tie;
  if (estimate_loss < result.loss.lo) {
    estimate_loss = result.loss.lo;
  }
  if (estimate_loss > result.loss.hi) {
    estimate_loss = result.loss.hi;
  }
  result.estimate = (CpegWtlValue){
      .win = estimate_win,
      .tie = estimate_tie,
      .loss = estimate_loss,
      .expected_final_margin = result.expected_final_margin.lo / 2.0 +
                               result.expected_final_margin.hi / 2.0,
  };
  return result;
}

static void cpeg_defense_job_run(void *arg, int worker_idx) {
  CpegDefenseJob *job = (CpegDefenseJob *)arg;
  CpegDefenseWorker *worker = &job->workers[worker_idx];
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;
  job->complete = true;
  job->capacity_exceeded = false;
  memset(&job->result, 0, sizeof(job->result));
  memset(&job->trace, 0, sizeof(job->trace));
  if (trace != NULL) {
    trace->defense_world_jobs++;
  }

  const bool horizon =
      job->candidate->kind == 0 &&
      move_get_tiles_played(&job->candidate->move) >= job->world->n;
  if (horizon && job->exhaustive_horizon && job->use_exact_two_ply) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->complete = false;
      return;
    }
    if (!cpeg_apply_root_draw(job, worker, job->world) ||
        bag_get_letters(game_get_bag(worker->draw_game)) != 0) {
      job->capacity_exceeded = true;
      return;
    }
    int64_t margin_after_root;
    if (!cpeg_checked_margin_add(job->initial_lead, job->candidate->score,
                                 &margin_after_root)) {
      job->capacity_exceeded = true;
      return;
    }
    bool endgame_capacity_exceeded = false;
    bool endgame_complete = true;
    bool endgame_cache_hit = false;
    const int64_t start_ns = trace != NULL ? ctimer_monotonic_ns() : 0;
    const int opponent_swing = cpeg_exact_endgame_swing(
        worker->draw_game, worker->endgame_mover, worker->endgame_reply,
        worker->endgame_undo, worker->exact_endgame_cache,
        &endgame_capacity_exceeded, job->deadline_ns, &endgame_complete,
        &endgame_cache_hit);
    if (trace != NULL) {
      trace->compatible_draws_tested++;
      trace->exact_endgame_queries++;
      if (endgame_cache_hit) {
        trace->exact_endgame_cache_hits++;
      }
      trace->exact_endgame_work_ns += ctimer_monotonic_ns() - start_ns;
    }
    if (endgame_capacity_exceeded) {
      job->capacity_exceeded = true;
      return;
    }
    if (!endgame_complete) {
      job->complete = false;
      return;
    }
    int64_t final_margin;
    if (!cpeg_checked_margin_add(margin_after_root,
                                 -(int64_t)opponent_swing, &final_margin)) {
      job->capacity_exceeded = true;
      return;
    }
    const CpegWtlEnvelope exact_envelope =
        cpeg_wtl_exact_envelope(final_margin);
    const int64_t exact_weight = 1;
    job->result = (CpegWtlProofWorld){
        // Preserve the existing one-draw outward-rounding boundary so the
        // experiment is byte-for-byte identical, not merely rationally equal.
        .envelope = cpeg_weighted_draw_envelope(
            &exact_envelope, &exact_weight, /*count=*/1, /*weight_total=*/1),
        .proof = CPEG_WTL_PROOF_EXACT,
        .draw_mass = 1,
        .win_lower_mass = final_margin > 0 ? 1 : 0,
        .win_upper_mass = final_margin > 0 ? 1 : 0,
        .tie_lower_mass = final_margin == 0 ? 1 : 0,
        .tie_upper_mass = final_margin == 0 ? 1 : 0,
        .loss_lower_mass = final_margin < 0 ? 1 : 0,
        .loss_upper_mass = final_margin < 0 ? 1 : 0,
    };
    return;
  }

  cpeg_set_world(worker->opponent_game, job->source_game, job->world,
                 job->unseen, job->ld_size, job->opponent_idx);
  if (job->candidate->kind != 0) {
    game_start_next_player_turn(worker->opponent_game);
  }
  int remaining_after_root = job->world->n;
  if (job->candidate->kind == 0) {
    const int tiles_played = move_get_tiles_played(&job->candidate->move);
    const int tiles_drawn =
        tiles_played < job->world->n ? tiles_played : job->world->n;
    remaining_after_root -= tiles_drawn;
  }
  const bool fast_horizon = horizon && !job->exhaustive_horizon;
  const bool fast_single_defense =
      job->max_defenses == 1 && remaining_after_root <= 1;
  const move_record_t opponent_record_type = fast_horizon || fast_single_defense
                                                 ? MOVE_RECORD_BEST_SMALL
                                                 : MOVE_RECORD_ALL_SMALL;
  if (!cpeg_generate_small_moves(worker->opponent_game, worker->opponent_moves,
                                 opponent_record_type, trace)) {
    job->capacity_exceeded = true;
    return;
  }
  cpeg_sort_small_moves(worker->opponent_moves, trace);

  CpegMultiset draws[CPEG_ENUM_CAP] = {0};
  int draw_count = 1;
  if (job->candidate->kind == 0) {
    const int tiles_played = move_get_tiles_played(&job->candidate->move);
    const int draw_size =
        tiles_played < job->world->n ? tiles_played : job->world->n;
    int counts[MAX_ALPHABET_SIZE] = {0};
    for (int tile_idx = 0; tile_idx < job->world->n; tile_idx++) {
      counts[job->world->tiles[tile_idx]]++;
    }
    bool overflow = false;
    draw_count = cpeg_enum_submultisets(counts, job->ld_size, draw_size, draws,
                                        CPEG_ENUM_CAP, &overflow);
    if (overflow || draw_count < 1) {
      job->capacity_exceeded = true;
      return;
    }
  } else if (job->candidate->kind == 2) {
    int counts[MAX_ALPHABET_SIZE] = {0};
    for (int tile_idx = 0; tile_idx < job->world->n; tile_idx++) {
      counts[job->world->tiles[tile_idx]]++;
    }
    bool overflow = false;
    draw_count =
        cpeg_enum_submultisets(counts, job->ld_size, job->candidate->exch_n,
                               draws, CPEG_ENUM_CAP, &overflow);
    if (overflow || draw_count < 1) {
      job->capacity_exceeded = true;
      return;
    }
  } else {
    draws[0].weight = 1;
  }
  if (trace != NULL) {
    trace->compatible_draws_tested += draw_count;
  }

  CpegWtlEnvelope draw_values[CPEG_ENUM_CAP] = {0};
  int64_t draw_weights[CPEG_ENUM_CAP] = {0};
  CpegWtlProofKind aggregate_proof = CPEG_WTL_PROOF_EXACT;
  int64_t draw_mass = 0;
  int64_t win_lower_mass = 0;
  int64_t win_upper_mass = 0;
  int64_t tie_lower_mass = 0;
  int64_t tie_upper_mass = 0;
  int64_t loss_lower_mass = 0;
  int64_t loss_upper_mass = 0;
  int64_t margin_after_root;
  if (!cpeg_checked_margin_add(job->initial_lead, job->candidate->score,
                               &margin_after_root)) {
    job->complete = false;
    return;
  }
  for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->complete = false;
      return;
    }
    if (!cpeg_apply_root_draw(job, worker, &draws[draw_idx])) {
      job->complete = false;
      return;
    }
    const int remaining_bag = bag_get_letters(game_get_bag(worker->draw_game));
    CpegWtlProofKind draw_proof;
    if (!cpeg_defense_for_draw(
            job, worker, remaining_bag, margin_after_root,
            /*include_voluntary_pass=*/job->exhaustive_horizon,
            /*defense_list_complete=*/
            !fast_horizon && !fast_single_defense, &draw_values[draw_idx],
            &draw_proof)) {
      if (job->complete) {
        job->capacity_exceeded = true;
      }
      return;
    }
    if (fast_horizon && job->max_defenses != 1 &&
        draw_values[draw_idx].loss.lo < 1.0) {
      if (!cpeg_generate_small_moves(worker->opponent_game,
                                     worker->opponent_moves,
                                     MOVE_RECORD_ALL_SMALL, trace)) {
        job->capacity_exceeded = true;
        return;
      }
      cpeg_sort_small_moves(worker->opponent_moves, trace);
      if (!cpeg_defense_for_draw(job, worker, remaining_bag, margin_after_root,
                                 /*include_voluntary_pass=*/true,
                                 /*defense_list_complete=*/true,
                                 &draw_values[draw_idx], &draw_proof)) {
        if (job->complete) {
          job->capacity_exceeded = true;
        }
        return;
      }
    }
    const int64_t draw_weight = draws[draw_idx].weight;
    draw_weights[draw_idx] = draw_weight;
    draw_mass += draw_weight;
    if (draw_values[draw_idx].win.lo == 1.0) {
      win_lower_mass += draw_weight;
    }
    if (draw_values[draw_idx].win.hi > 0.0) {
      win_upper_mass += draw_weight;
    }
    if (draw_values[draw_idx].tie.lo == 1.0) {
      tie_lower_mass += draw_weight;
    }
    if (draw_values[draw_idx].tie.hi > 0.0) {
      tie_upper_mass += draw_weight;
    }
    if (draw_values[draw_idx].loss.lo == 1.0) {
      loss_lower_mass += draw_weight;
    }
    if (draw_values[draw_idx].loss.hi > 0.0) {
      loss_upper_mass += draw_weight;
    }
    if (draw_proof < aggregate_proof) {
      aggregate_proof = draw_proof;
    }
  }
  job->result = (CpegWtlProofWorld){
      .envelope = cpeg_weighted_draw_envelope(draw_values, draw_weights,
                                              draw_count, draw_mass),
      .proof = aggregate_proof,
      .draw_mass = draw_mass,
      .win_lower_mass = win_lower_mass,
      .win_upper_mass = win_upper_mass,
      .tie_lower_mass = tie_lower_mass,
      .tie_upper_mass = tie_upper_mass,
      .loss_lower_mass = loss_lower_mass,
      .loss_upper_mass = loss_upper_mass,
  };
}

static int64_t cpeg_wtl_candidate_draw_mass(const CpegRootCand *candidate,
                                            int bag) {
  int draw_count = 0;
  if (candidate->kind == 0) {
    draw_count = move_get_tiles_played(&candidate->move);
    if (draw_count > bag) {
      draw_count = bag;
    }
  } else if (candidate->kind == 2) {
    draw_count = candidate->exch_n;
  }
  return peg_binomial(bag, draw_count);
}

static CpegInterval cpeg_wtl_rational_interval(int64_t numerator,
                                               int64_t denominator) {
  if (numerator <= 0) {
    return (CpegInterval){.lo = 0.0, .hi = 0.0};
  }
  if (numerator >= denominator) {
    return (CpegInterval){.lo = 1.0, .hi = 1.0};
  }
  const double value = (double)numerator / (double)denominator;
  return (CpegInterval){
      .lo = nextafter(value, -INFINITY),
      .hi = nextafter(value, INFINITY),
  };
}

static void cpeg_wtl_set_rational_outcome(CpegWtlCertifiedCand *candidate) {
  const int64_t denominator = candidate->outcome_den;
  candidate->outcome.win = (CpegInterval){
      .lo =
          cpeg_wtl_rational_interval(candidate->win_lower_num, denominator).lo,
      .hi =
          cpeg_wtl_rational_interval(candidate->win_upper_num, denominator).hi,
  };
  candidate->outcome.tie = (CpegInterval){
      .lo =
          cpeg_wtl_rational_interval(candidate->tie_lower_num, denominator).lo,
      .hi =
          cpeg_wtl_rational_interval(candidate->tie_upper_num, denominator).hi,
  };
  candidate->outcome.loss = (CpegInterval){
      .lo =
          cpeg_wtl_rational_interval(candidate->loss_lower_num, denominator).lo,
      .hi =
          cpeg_wtl_rational_interval(candidate->loss_upper_num, denominator).hi,
  };

  int64_t point_win = candidate->win_lower_num;
  int64_t point_tie = candidate->tie_lower_num;
  int64_t point_loss = candidate->loss_lower_num;
  int64_t remaining = denominator - point_win - point_tie - point_loss;
  int64_t available = candidate->win_upper_num - point_win;
  int64_t allocated = remaining < available ? remaining : available;
  point_win += allocated;
  remaining -= allocated;
  available = candidate->tie_upper_num - point_tie;
  allocated = remaining < available ? remaining : available;
  point_tie += allocated;
  remaining -= allocated;
  point_loss += remaining;

  const CpegInterval point_win_interval =
      cpeg_wtl_rational_interval(point_win, denominator);
  const CpegInterval point_tie_interval =
      cpeg_wtl_rational_interval(point_tie, denominator);
  const double win_choices[3] = {
      (double)point_win / (double)denominator,
      point_win_interval.lo,
      point_win_interval.hi,
  };
  const double tie_choices[3] = {
      (double)point_tie / (double)denominator,
      point_tie_interval.lo,
      point_tie_interval.hi,
  };
  bool found = false;
  for (int win_idx = 0; win_idx < 3 && !found; win_idx++) {
    for (int tie_idx = 0; tie_idx < 3; tie_idx++) {
      const double loss = 1.0 - win_choices[win_idx] - tie_choices[tie_idx];
      if (loss >= candidate->outcome.loss.lo &&
          loss <= candidate->outcome.loss.hi) {
        candidate->outcome.estimate.win = win_choices[win_idx];
        candidate->outcome.estimate.tie = tie_choices[tie_idx];
        candidate->outcome.estimate.loss = loss;
        found = true;
        break;
      }
    }
  }
  if (!found) {
    candidate->outcome.estimate.win = (double)point_win / (double)denominator;
    candidate->outcome.estimate.tie = (double)point_tie / (double)denominator;
    candidate->outcome.estimate.loss = (double)point_loss / (double)denominator;
  }
}

static void cpeg_wtl_recompute_candidate(
    CpegWtlCertifiedCand *candidate, CpegWtlProofState *state,
    const CpegWtlProofWorld *world_evaluations,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    int64_t draw_mass, CpegInterval margin_prior) {
  CpegWtlEnvelope values[CPEG_WORLD_CAP] = {0};
  int64_t weights[CPEG_WORLD_CAP] = {0};
  memset(state, 0, sizeof(*state));
  candidate->worlds_exact = 0;
  candidate->worlds_bounded = 0;
  candidate->worlds_unresolved = 0;
  candidate->exact_weight = 0;
  candidate->bounded_weight = 0;
  candidate->unresolved_weight = 0;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    const CpegWtlProofWorld *evaluation = &world_evaluations[world_idx];
    const int64_t world_weight = worlds[world_idx].multiset.weight;
    weights[world_idx] = world_weight;
    if (evaluation->proof == CPEG_WTL_PROOF_UNRESOLVED) {
      values[world_idx] = cpeg_wtl_unresolved_envelope(margin_prior);
      state->win_upper_mass += world_weight * draw_mass;
      state->tie_upper_mass += world_weight * draw_mass;
      state->loss_upper_mass += world_weight * draw_mass;
      candidate->worlds_unresolved++;
      candidate->unresolved_weight += world_weight;
      continue;
    }
    values[world_idx] = evaluation->envelope;
    state->win_lower_mass += world_weight * evaluation->win_lower_mass;
    state->win_upper_mass += world_weight * evaluation->win_upper_mass;
    state->tie_lower_mass += world_weight * evaluation->tie_lower_mass;
    state->tie_upper_mass += world_weight * evaluation->tie_upper_mass;
    state->loss_lower_mass += world_weight * evaluation->loss_lower_mass;
    state->loss_upper_mass += world_weight * evaluation->loss_upper_mass;
    if (evaluation->proof == CPEG_WTL_PROOF_EXACT) {
      candidate->worlds_exact++;
      candidate->exact_weight += world_weight;
    } else {
      candidate->worlds_bounded++;
      candidate->bounded_weight += world_weight;
    }
  }
  state->outcome_mass = world_mass * draw_mass;
  state->outcome =
      cpeg_weighted_draw_envelope(values, weights, world_count, world_mass);
  candidate->outcome = state->outcome;
  candidate->outcome_den = state->outcome_mass;
  candidate->win_lower_num = state->win_lower_mass;
  candidate->win_upper_num = state->win_upper_mass;
  candidate->tie_lower_num = state->tie_lower_mass;
  candidate->tie_upper_num = state->tie_upper_mass;
  candidate->loss_lower_num = state->loss_lower_mass;
  candidate->loss_upper_num = state->loss_upper_mass;
  cpeg_wtl_set_rational_outcome(candidate);
  state->outcome = candidate->outcome;
}

static bool cpeg_wtl_fraction_less(int64_t lhs_num, int64_t lhs_den,
                                   int64_t rhs_num, int64_t rhs_den,
                                   bool *valid) {
  if (lhs_num < 0 || rhs_num < 0 || lhs_den <= 0 || rhs_den <= 0 ||
      (lhs_num > 0 && rhs_den > INT64_MAX / lhs_num) ||
      (rhs_num > 0 && lhs_den > INT64_MAX / rhs_num)) {
    *valid = false;
    return false;
  }
  return lhs_num * rhs_den < rhs_num * lhs_den;
}

static bool cpeg_wtl_fraction_equal(int64_t lhs_num, int64_t lhs_den,
                                    int64_t rhs_num, int64_t rhs_den,
                                    bool *valid) {
  if (lhs_num < 0 || rhs_num < 0 || lhs_den <= 0 || rhs_den <= 0 ||
      (lhs_num > 0 && rhs_den > INT64_MAX / lhs_num) ||
      (rhs_num > 0 && lhs_den > INT64_MAX / rhs_num)) {
    *valid = false;
    return false;
  }
  return lhs_num * rhs_den == rhs_num * lhs_den;
}

static bool cpeg_wtl_candidate_precedes(const CpegRootCand *candidates, int bag,
                                        int lhs_idx, int rhs_idx) {
  const bool lhs_horizon =
      candidates[lhs_idx].kind == 0 &&
      move_get_tiles_played(&candidates[lhs_idx].move) >= bag;
  const bool rhs_horizon =
      candidates[rhs_idx].kind == 0 &&
      move_get_tiles_played(&candidates[rhs_idx].move) >= bag;
  if (lhs_horizon != rhs_horizon) {
    return lhs_horizon;
  }
  if (candidates[lhs_idx].score != candidates[rhs_idx].score) {
    return candidates[lhs_idx].score > candidates[rhs_idx].score;
  }
  const CpegCandKind lhs_kind = cpeg_root_candidate_kind(&candidates[lhs_idx]);
  const CpegCandKind rhs_kind = cpeg_root_candidate_kind(&candidates[rhs_idx]);
  if (lhs_kind != rhs_kind) {
    return lhs_kind < rhs_kind;
  }
  return lhs_idx < rhs_idx;
}

static int64_t cpeg_wtl_proof_deadline_ns(double budget_seconds) {
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

static bool cpeg_wtl_proof_budget_reached(int64_t deadline_ns, int max_batches,
                                          int batches_completed) {
  return cpeg_defense_deadline_reached(deadline_ns) ||
         (max_batches > 0 && batches_completed >= max_batches);
}

static bool cpeg_wtl_run_defense_batch(
    PegPool *pool, CpegDefenseWorker *workers, int helper_worker_idx,
    CpegDefenseJob *jobs, void **job_ptrs, int first_world, int world_count,
    const Game *source_game, const CpegScheduledWorld *worlds,
    const int *unseen, int ld_size, int opponent_idx, int root_idx,
    const CpegRootCand *candidate, int64_t initial_lead, int64_t deadline_ns,
    CpegInterval margin_prior, bool exhaustive_horizon, bool use_exact_two_ply,
    bool use_threshold_reply_screen, CpegWtlReplyPhase reply_phase,
    int max_defenses, CpegWtlProofWorld *world_evaluations, int *exact_jobs,
    int *bound_jobs, bool *batch_complete, CpegWtlTrace *trace,
    CpegWtlTrace *candidate_trace) {
  *batch_complete = false;
  if (trace != NULL) {
    trace->scheduler_batches++;
  }
  if (candidate_trace != NULL) {
    candidate_trace->scheduler_batches++;
  }
  for (int job_idx = 0; job_idx < world_count; job_idx++) {
    const int world_idx = first_world + job_idx;
    jobs[job_idx] = (CpegDefenseJob){
        .workers = workers,
        .source_game = source_game,
        .world = &worlds[world_idx].multiset,
        .unseen = unseen,
        .ld_size = ld_size,
        .opponent_idx = opponent_idx,
        .root_idx = root_idx,
        .candidate = candidate,
        .initial_lead = initial_lead,
        .deadline_ns = deadline_ns,
        .margin_prior = margin_prior,
        .exhaustive_horizon = exhaustive_horizon,
        .use_exact_two_ply = use_exact_two_ply,
        .use_threshold_reply_screen = use_threshold_reply_screen,
        .reply_phase = reply_phase,
        .max_defenses = max_defenses,
        .collect_trace = trace != NULL,
    };
    job_ptrs[job_idx] = &jobs[job_idx];
  }
  peg_pool_submit_and_wait(pool, cpeg_defense_job_run, job_ptrs, world_count,
                           helper_worker_idx);
  for (int job_idx = 0; job_idx < world_count; job_idx++) {
    cpeg_wtl_trace_add_work(trace, &jobs[job_idx].trace);
    cpeg_wtl_trace_add_work(candidate_trace, &jobs[job_idx].trace);
    if (jobs[job_idx].capacity_exceeded) {
      return false;
    }
    if (!jobs[job_idx].complete) {
      return true;
    }
  }
  for (int job_idx = 0; job_idx < world_count; job_idx++) {
    const int world_idx = first_world + job_idx;
    world_evaluations[world_idx] = jobs[job_idx].result;
    if (jobs[job_idx].result.proof == CPEG_WTL_PROOF_EXACT) {
      (*exact_jobs)++;
    } else {
      (*bound_jobs)++;
    }
  }
  *batch_complete = true;
  return true;
}

typedef struct CpegWtlAtomicCandidateTrace {
  atomic_int_fast64_t defense_world_jobs;
  atomic_int_fast64_t defenses_threshold_tested;
  atomic_int_fast64_t defenses_accepted;
  atomic_int_fast64_t compatible_draws_tested;
  atomic_int_fast64_t final_reply_queries;
  atomic_int_fast64_t final_replies_generated;
  atomic_int_fast64_t final_reply_cache_hits;
  atomic_int_fast64_t final_reply_movegen_work_ns;
} CpegWtlAtomicCandidateTrace;

static void
cpeg_wtl_atomic_candidate_trace_init(CpegWtlAtomicCandidateTrace *trace) {
  atomic_init(&trace->defense_world_jobs, 0);
  atomic_init(&trace->defenses_threshold_tested, 0);
  atomic_init(&trace->defenses_accepted, 0);
  atomic_init(&trace->compatible_draws_tested, 0);
  atomic_init(&trace->final_reply_queries, 0);
  atomic_init(&trace->final_replies_generated, 0);
  atomic_init(&trace->final_reply_cache_hits, 0);
  atomic_init(&trace->final_reply_movegen_work_ns, 0);
}

static void cpeg_wtl_atomic_candidate_trace_add(
    CpegWtlAtomicCandidateTrace *dest, const CpegWtlTrace *source) {
  if (dest == NULL || source == NULL) {
    return;
  }
  atomic_fetch_add_explicit(&dest->defense_world_jobs,
                            source->defense_world_jobs,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->defenses_threshold_tested,
                            source->defenses_threshold_tested,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->defenses_accepted,
                            source->defenses_accepted,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->compatible_draws_tested,
                            source->compatible_draws_tested,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->final_reply_queries,
                            source->final_reply_queries,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->final_replies_generated,
                            source->final_replies_generated,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->final_reply_cache_hits,
                            source->final_reply_cache_hits,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&dest->final_reply_movegen_work_ns,
                            source->final_reply_movegen_work_ns,
                            memory_order_relaxed);
}

static void cpeg_wtl_atomic_candidate_trace_load(
    CpegWtlTrace *dest, const CpegWtlAtomicCandidateTrace *source) {
  if (dest == NULL || source == NULL) {
    return;
  }
  dest->defense_world_jobs +=
      atomic_load_explicit(&source->defense_world_jobs, memory_order_relaxed);
  dest->defenses_threshold_tested += atomic_load_explicit(
      &source->defenses_threshold_tested, memory_order_relaxed);
  dest->defenses_accepted +=
      atomic_load_explicit(&source->defenses_accepted, memory_order_relaxed);
  dest->compatible_draws_tested += atomic_load_explicit(
      &source->compatible_draws_tested, memory_order_relaxed);
  dest->final_reply_queries +=
      atomic_load_explicit(&source->final_reply_queries, memory_order_relaxed);
  dest->final_replies_generated += atomic_load_explicit(
      &source->final_replies_generated, memory_order_relaxed);
  dest->final_reply_cache_hits += atomic_load_explicit(
      &source->final_reply_cache_hits, memory_order_relaxed);
  dest->final_reply_movegen_work_ns += atomic_load_explicit(
      &source->final_reply_movegen_work_ns, memory_order_relaxed);
}

static bool cpeg_wtl_screen_scoreless_candidates(
    const Game *root_game, const CpegRootCand *candidates, int candidate_count,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    const int *unseen, int ld_size, int root_idx, int opponent_idx,
    const CpegWtlCertifiedArgs *args, int64_t deadline_ns,
    CpegInterval margin_prior, CpegWtlCertifiedResult *out,
    CpegWtlProofState *states, bool *stopped) {
  int64_t *win_upper =
      calloc_or_die((size_t)candidate_count, sizeof(*win_upper));
  int64_t *tie_upper =
      calloc_or_die((size_t)candidate_count, sizeof(*tie_upper));
  int64_t *loss_lower =
      calloc_or_die((size_t)candidate_count, sizeof(*loss_lower));
  Game *world_game = game_duplicate(root_game);
  MoveList *opponent_moves = move_list_create_small(CPEG_MOVE_LIST_CAP + 1);
  MoveList *root_moves = move_list_create(1);
  MoveList *root_small_moves = NULL;
  CpegRootReplyCacheEntry *cache = NULL;
  if (args->use_threshold_reply_screen) {
    root_small_moves = move_list_create_small(1);
    cache = calloc_or_die(CPEG_ROOT_REPLY_CACHE_CAPACITY, sizeof(*cache));
  }
  MoveUndo *defense_undo = malloc_or_die(sizeof(*defense_undo));
  int worlds_bounded = 0;
  int64_t bounded_world_mass = 0;
  CpegWtlTrace *trace = args->collect_trace ? &out->trace : NULL;

  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    if (cpeg_wtl_proof_budget_reached(deadline_ns, args->max_batches,
                                      out->batches_completed)) {
      *stopped = true;
      break;
    }
    cpeg_set_world(world_game, root_game, &worlds[world_idx].multiset, unseen,
                   ld_size, opponent_idx);
    game_start_next_player_turn(world_game);
    if (!cpeg_generate_small_moves(world_game, opponent_moves,
                                   MOVE_RECORD_ALL_SMALL, trace)) {
      free(cache);
      free(defense_undo);
      small_move_list_destroy(root_small_moves);
      move_list_destroy(root_moves);
      small_move_list_destroy(opponent_moves);
      game_destroy(world_game);
      free(loss_lower);
      free(tie_upper);
      free(win_upper);
      return false;
    }
    cpeg_sort_small_moves(opponent_moves, trace);
    const SmallMove *defense = NULL;
    for (int move_idx = 0; move_idx < opponent_moves->count; move_idx++) {
      const SmallMove *move = opponent_moves->small_moves[move_idx];
      if (!small_move_is_pass(move) &&
          small_move_get_tiles_played(move) >= args->bag) {
        defense = move;
        break;
      }
    }
    if (defense == NULL) {
      out->batches_completed++;
      continue;
    }
    if (trace != NULL) {
      trace->defenses_threshold_tested++;
      trace->defenses_accepted++;
    }
    small_move_to_move(opponent_moves->spare_move, defense,
                       game_get_board(world_game));
    play_move_incremental(opponent_moves->spare_move, world_game, defense_undo);
    game_set_game_end_reason(world_game, GAME_END_REASON_NONE);
    bag_set_to_tiles(game_get_bag(world_game), NULL, 0);
    const int defense_score = small_move_get_score(defense);
    const int64_t threshold = (int64_t)defense_score - args->initial_lead;
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      const CpegRootCand *candidate = &candidates[candidate_idx];
      if (candidate->kind == 0) {
        continue;
      }
      CpegMultiset draws[CPEG_ENUM_CAP] = {0};
      int draw_count = 1;
      if (candidate->kind == 2) {
        int counts[MAX_ALPHABET_SIZE] = {0};
        for (int tile_idx = 0; tile_idx < worlds[world_idx].multiset.n;
             tile_idx++) {
          counts[worlds[world_idx].multiset.tiles[tile_idx]]++;
        }
        bool overflow = false;
        draw_count = cpeg_enum_submultisets(counts, ld_size, candidate->exch_n,
                                            draws, CPEG_ENUM_CAP, &overflow);
        if (overflow || draw_count < 1) {
          free(cache);
          free(defense_undo);
          small_move_list_destroy(root_small_moves);
          move_list_destroy(root_moves);
          small_move_list_destroy(opponent_moves);
          game_destroy(world_game);
          free(loss_lower);
          free(tie_upper);
          free(win_upper);
          return false;
        }
      } else {
        draws[0].weight = 1;
      }
      if (trace != NULL) {
        trace->compatible_draws_tested += draw_count;
        CpegWtlTrace *candidate_trace =
            &out->candidate_traces[candidate_idx];
        candidate_trace->defense_world_jobs++;
        candidate_trace->defenses_threshold_tested++;
        candidate_trace->defenses_accepted++;
        candidate_trace->compatible_draws_tested += draw_count;
      }
      int64_t candidate_win_upper = 0;
      int64_t candidate_tie_upper = 0;
      int64_t candidate_loss_lower = 0;
      for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
        Rack branch_rack;
        rack_copy(&branch_rack,
                  player_get_rack(game_get_player(root_game, root_idx)));
        if (candidate->kind == 2) {
          for (int tile_idx = 0; tile_idx < candidate->exch_n; tile_idx++) {
            rack_take_letter(&branch_rack, candidate->exch_tiles[tile_idx]);
          }
          for (int tile_idx = 0; tile_idx < draws[draw_idx].n; tile_idx++) {
            rack_add_letter(&branch_rack, draws[draw_idx].tiles[tile_idx]);
          }
        }
        rack_copy(player_get_rack(game_get_player(world_game, root_idx)),
                  &branch_rack);
        CpegWtlTrace reply_trace = {0};
        CpegRootReplyResult reply;
        const bool reply_ok = cpeg_root_reply(
            world_game, root_moves, root_small_moves, cache,
            args->use_threshold_reply_screen, threshold,
            /*require_exact_score=*/false,
            CPEG_WTL_REPLY_PHASE_SCORELESS_SCREEN, &reply,
            trace != NULL ? &reply_trace : NULL);
        cpeg_wtl_trace_add_work(trace, &reply_trace);
        cpeg_wtl_trace_add_work(
            trace != NULL ? &out->candidate_traces[candidate_idx] : NULL,
            &reply_trace);
        if (!reply_ok) {
          free(cache);
          free(defense_undo);
          small_move_list_destroy(root_small_moves);
          move_list_destroy(root_moves);
          small_move_list_destroy(opponent_moves);
          game_destroy(world_game);
          free(loss_lower);
          free(tie_upper);
          free(win_upper);
          return false;
        }
        if (reply.kind == CPEG_ROOT_REPLY_ABOVE_THRESHOLD) {
          candidate_win_upper += draws[draw_idx].weight;
          candidate_tie_upper += draws[draw_idx].weight;
        } else {
          const int64_t margin_upper =
              args->initial_lead - (int64_t)defense_score +
              reply.exact_score;
          if (margin_upper > 0) {
            candidate_win_upper += draws[draw_idx].weight;
          }
          if (margin_upper >= 0) {
            candidate_tie_upper += draws[draw_idx].weight;
          } else {
            candidate_loss_lower += draws[draw_idx].weight;
          }
        }
      }
      const int64_t world_weight = worlds[world_idx].multiset.weight;
      win_upper[candidate_idx] += world_weight * candidate_win_upper;
      tie_upper[candidate_idx] += world_weight * candidate_tie_upper;
      loss_lower[candidate_idx] += world_weight * candidate_loss_lower;
      out->bound_jobs++;
    }
    worlds_bounded++;
    bounded_world_mass += worlds[world_idx].multiset.weight;
    out->batches_completed++;
  }

  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    const CpegRootCand *candidate = &candidates[candidate_idx];
    if (candidate->kind == 0) {
      continue;
    }
    const int64_t draw_mass =
        cpeg_wtl_candidate_draw_mass(candidate, args->bag);
    const int64_t outcome_mass = world_mass * draw_mass;
    const int64_t unresolved_mass =
        (world_mass - bounded_world_mass) * draw_mass;
    CpegWtlCertifiedCand *result_candidate = &out->cands[candidate_idx];
    result_candidate->outcome_den = outcome_mass;
    result_candidate->win_lower_num = 0;
    result_candidate->win_upper_num =
        win_upper[candidate_idx] + unresolved_mass;
    result_candidate->tie_lower_num = 0;
    result_candidate->tie_upper_num =
        tie_upper[candidate_idx] + unresolved_mass;
    result_candidate->loss_lower_num = loss_lower[candidate_idx];
    result_candidate->loss_upper_num = outcome_mass;
    result_candidate->worlds_exact = 0;
    result_candidate->worlds_bounded = worlds_bounded;
    result_candidate->worlds_unresolved = world_count - worlds_bounded;
    result_candidate->exact_weight = 0;
    result_candidate->bounded_weight = bounded_world_mass;
    result_candidate->unresolved_weight = world_mass - bounded_world_mass;
    result_candidate->outcome = (CpegWtlEnvelope){
        .estimate =
            {
                .win = 0.0,
                .tie = 0.0,
                .loss = 1.0,
                .expected_final_margin =
                    margin_prior.lo / 2.0 + margin_prior.hi / 2.0,
            },
        .win = cpeg_wtl_rational_interval(0, outcome_mass),
        .tie = cpeg_wtl_rational_interval(0, outcome_mass),
        .loss =
            cpeg_wtl_rational_interval(loss_lower[candidate_idx], outcome_mass),
        .expected_final_margin = margin_prior,
    };
    result_candidate->outcome.win.hi =
        cpeg_wtl_rational_interval(result_candidate->win_upper_num,
                                   outcome_mass)
            .hi;
    result_candidate->outcome.tie.hi =
        cpeg_wtl_rational_interval(result_candidate->tie_upper_num,
                                   outcome_mass)
            .hi;
    result_candidate->outcome.loss.hi = 1.0;
    cpeg_wtl_set_rational_outcome(result_candidate);
    states[candidate_idx] = (CpegWtlProofState){
        .win_upper_mass = result_candidate->win_upper_num,
        .tie_upper_mass = result_candidate->tie_upper_num,
        .loss_lower_mass = result_candidate->loss_lower_num,
        .loss_upper_mass = outcome_mass,
        .outcome_mass = outcome_mass,
        .outcome = result_candidate->outcome,
    };
  }

  free(cache);
  free(defense_undo);
  small_move_list_destroy(root_small_moves);
  move_list_destroy(root_moves);
  small_move_list_destroy(opponent_moves);
  game_destroy(world_game);
  free(loss_lower);
  free(tie_upper);
  free(win_upper);
  return true;
}

typedef struct CpegWtlScorelessWorldJob {
  CpegDefenseWorker *workers;
  const Game *root_game;
  const CpegRootCand *candidates;
  int candidate_count;
  const CpegScheduledWorld *world;
  const int *unseen;
  int ld_size;
  int root_idx;
  int opponent_idx;
  int bag;
  int64_t initial_lead;
  int64_t deadline_ns;
  int64_t *win_upper;
  int64_t *tie_upper;
  int64_t *loss_lower;
  CpegWtlAtomicCandidateTrace *candidate_traces;
  bool collect_trace;
  bool use_threshold_reply_screen;
  bool bounded;
  bool complete;
  bool proof_valid;
  CpegWtlTrace trace;
} CpegWtlScorelessWorldJob;

static void cpeg_wtl_scoreless_world_job_run(void *arg, int worker_idx) {
  CpegWtlScorelessWorldJob *job = arg;
  CpegDefenseWorker *worker = &job->workers[worker_idx];
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;
  memset(&job->trace, 0, sizeof(job->trace));
  job->complete = true;
  job->proof_valid = true;
  if (cpeg_defense_deadline_reached(job->deadline_ns)) {
    job->complete = false;
    return;
  }
  cpeg_set_world(worker->opponent_game, job->root_game, &job->world->multiset,
                 job->unseen, job->ld_size, job->opponent_idx);
  game_start_next_player_turn(worker->opponent_game);
  if (!cpeg_generate_small_moves(worker->opponent_game, worker->opponent_moves,
                                 MOVE_RECORD_ALL_SMALL, trace)) {
    job->proof_valid = false;
    return;
  }
  cpeg_sort_small_moves(worker->opponent_moves, trace);
  const SmallMove *defense = NULL;
  for (int move_idx = 0; move_idx < worker->opponent_moves->count; move_idx++) {
    const SmallMove *move = worker->opponent_moves->small_moves[move_idx];
    if (!small_move_is_pass(move) &&
        small_move_get_tiles_played(move) >= job->bag) {
      defense = move;
      break;
    }
  }
  if (defense == NULL) {
    return;
  }
  if (trace != NULL) {
    trace->defenses_threshold_tested++;
    trace->defenses_accepted++;
  }
  game_copy(worker->defense_game, worker->opponent_game);
  small_move_to_move(worker->opponent_moves->spare_move, defense,
                     game_get_board(worker->defense_game));
  play_move_incremental(worker->opponent_moves->spare_move,
                        worker->defense_game, worker->defense_undo);
  bag_set_to_tiles(game_get_bag(worker->defense_game), NULL, 0);
  game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
  game_set_consecutive_scoreless_turns(worker->defense_game, 0);
  const int defense_score = small_move_get_score(defense);
  const int64_t threshold = (int64_t)defense_score - job->initial_lead;
  for (int candidate_idx = 0; candidate_idx < job->candidate_count;
       candidate_idx++) {
    const CpegRootCand *candidate = &job->candidates[candidate_idx];
    if (candidate->kind == 0) {
      continue;
    }
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->complete = false;
      return;
    }
    CpegMultiset draws[CPEG_ENUM_CAP] = {0};
    int draw_count = 1;
    if (candidate->kind == 2) {
      int counts[MAX_ALPHABET_SIZE] = {0};
      for (int tile_idx = 0; tile_idx < job->world->multiset.n; tile_idx++) {
        counts[job->world->multiset.tiles[tile_idx]]++;
      }
      bool overflow = false;
      draw_count =
          cpeg_enum_submultisets(counts, job->ld_size, candidate->exch_n, draws,
                                 CPEG_ENUM_CAP, &overflow);
      if (overflow || draw_count < 1) {
        job->proof_valid = false;
        return;
      }
    } else {
      draws[0].weight = 1;
    }
    if (trace != NULL) {
      const CpegWtlTrace candidate_context = {
          .defense_world_jobs = 1,
          .defenses_threshold_tested = 1,
          .defenses_accepted = 1,
          .compatible_draws_tested = draw_count,
      };
      cpeg_wtl_atomic_candidate_trace_add(
          &job->candidate_traces[candidate_idx], &candidate_context);
    }
    if (trace != NULL) {
      trace->compatible_draws_tested += draw_count;
    }
    for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
      Rack branch_rack;
      rack_copy(&branch_rack, player_get_rack(game_get_player(job->root_game,
                                                              job->root_idx)));
      if (candidate->kind == 2) {
        for (int tile_idx = 0; tile_idx < candidate->exch_n; tile_idx++) {
          rack_take_letter(&branch_rack, candidate->exch_tiles[tile_idx]);
        }
        for (int tile_idx = 0; tile_idx < draws[draw_idx].n; tile_idx++) {
          rack_add_letter(&branch_rack, draws[draw_idx].tiles[tile_idx]);
        }
      }
      rack_copy(
          player_get_rack(game_get_player(worker->defense_game, job->root_idx)),
          &branch_rack);
      CpegWtlTrace reply_trace = {0};
      CpegRootReplyResult reply;
      const bool reply_ok = cpeg_root_reply(
          worker->defense_game, worker->root_best, worker->root_best_small,
          worker->root_reply_cache, job->use_threshold_reply_screen, threshold,
          /*require_exact_score=*/false,
          CPEG_WTL_REPLY_PHASE_SCORELESS_SCREEN, &reply,
          trace != NULL ? &reply_trace : NULL);
      cpeg_wtl_trace_add_work(trace, &reply_trace);
      cpeg_wtl_atomic_candidate_trace_add(
          trace != NULL ? &job->candidate_traces[candidate_idx] : NULL,
          &reply_trace);
      if (!reply_ok) {
        job->proof_valid = false;
        return;
      }
      if (reply.kind == CPEG_ROOT_REPLY_ABOVE_THRESHOLD) {
        job->win_upper[candidate_idx] += draws[draw_idx].weight;
        job->tie_upper[candidate_idx] += draws[draw_idx].weight;
      } else {
        int64_t margin_upper;
        if (!cpeg_checked_margin_add(job->initial_lead,
                                     -(int64_t)defense_score, &margin_upper) ||
            !cpeg_checked_margin_add(margin_upper, reply.exact_score,
                                     &margin_upper)) {
          job->proof_valid = false;
          return;
        }
        if (margin_upper > 0) {
          job->win_upper[candidate_idx] += draws[draw_idx].weight;
        }
        if (margin_upper >= 0) {
          job->tie_upper[candidate_idx] += draws[draw_idx].weight;
        } else {
          job->loss_lower[candidate_idx] += draws[draw_idx].weight;
        }
      }
    }
  }
  job->bounded = true;
}

static bool cpeg_wtl_screen_scoreless_candidates_parallel(
    PegPool *pool, CpegDefenseWorker *workers, int helper_worker_idx,
    Game *root_game, const CpegRootCand *candidates, int candidate_count,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    const int *unseen, int ld_size, int root_idx, int opponent_idx,
    const CpegWtlCertifiedArgs *args, int64_t deadline_ns,
    CpegInterval margin_prior, CpegWtlCertifiedResult *out,
    CpegWtlProofState *states, bool *stopped) {
  CpegWtlScorelessWorldJob *jobs =
      calloc_or_die((size_t)world_count, sizeof(*jobs));
  void **job_ptrs = malloc_or_die((size_t)world_count * sizeof(*job_ptrs));
  int64_t *win_upper = calloc_or_die(
      (size_t)world_count * (size_t)candidate_count, sizeof(*win_upper));
  int64_t *tie_upper = calloc_or_die(
      (size_t)world_count * (size_t)candidate_count, sizeof(*tie_upper));
  int64_t *loss_lower = calloc_or_die(
      (size_t)world_count * (size_t)candidate_count, sizeof(*loss_lower));
  CpegWtlAtomicCandidateTrace *candidate_traces = NULL;
  if (args->collect_trace) {
    candidate_traces =
        malloc_or_die((size_t)candidate_count * sizeof(*candidate_traces));
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      cpeg_wtl_atomic_candidate_trace_init(
          &candidate_traces[candidate_idx]);
    }
  }
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    jobs[world_idx] = (CpegWtlScorelessWorldJob){
        .workers = workers,
        .root_game = root_game,
        .candidates = candidates,
        .candidate_count = candidate_count,
        .world = &worlds[world_idx],
        .unseen = unseen,
        .ld_size = ld_size,
        .root_idx = root_idx,
        .opponent_idx = opponent_idx,
        .bag = args->bag,
        .initial_lead = args->initial_lead,
        .deadline_ns = deadline_ns,
        .win_upper = &win_upper[world_idx * candidate_count],
        .tie_upper = &tie_upper[world_idx * candidate_count],
        .loss_lower = &loss_lower[world_idx * candidate_count],
        .candidate_traces = candidate_traces,
        .collect_trace = args->collect_trace,
        .use_threshold_reply_screen = args->use_threshold_reply_screen,
    };
    job_ptrs[world_idx] = &jobs[world_idx];
  }
  peg_pool_submit_and_wait(pool, cpeg_wtl_scoreless_world_job_run, job_ptrs,
                           world_count, helper_worker_idx);

  bool proof_valid = true;
  int worlds_bounded = 0;
  int64_t bounded_world_mass = 0;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    cpeg_wtl_trace_add_work(args->collect_trace ? &out->trace : NULL,
                            &jobs[world_idx].trace);
    if (!jobs[world_idx].proof_valid) {
      proof_valid = false;
    }
    if (!jobs[world_idx].complete) {
      *stopped = true;
    }
    if (jobs[world_idx].bounded) {
      worlds_bounded++;
      bounded_world_mass += worlds[world_idx].multiset.weight;
      out->batches_completed++;
    }
  }
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    cpeg_wtl_atomic_candidate_trace_load(
        args->collect_trace ? &out->candidate_traces[candidate_idx] : NULL,
        candidate_traces != NULL ? &candidate_traces[candidate_idx] : NULL);
    const CpegRootCand *candidate = &candidates[candidate_idx];
    if (candidate->kind == 0) {
      continue;
    }
    const int64_t draw_mass =
        cpeg_wtl_candidate_draw_mass(candidate, args->bag);
    const int64_t outcome_mass = world_mass * draw_mass;
    int64_t candidate_win_upper = (world_mass - bounded_world_mass) * draw_mass;
    int64_t candidate_tie_upper = candidate_win_upper;
    int64_t candidate_loss_lower = 0;
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      if (!jobs[world_idx].bounded) {
        continue;
      }
      const int64_t world_weight = worlds[world_idx].multiset.weight;
      const int offset = world_idx * candidate_count + candidate_idx;
      candidate_win_upper += world_weight * win_upper[offset];
      candidate_tie_upper += world_weight * tie_upper[offset];
      candidate_loss_lower += world_weight * loss_lower[offset];
    }
    CpegWtlCertifiedCand *result_candidate = &out->cands[candidate_idx];
    result_candidate->outcome_den = outcome_mass;
    result_candidate->win_lower_num = 0;
    result_candidate->win_upper_num = candidate_win_upper;
    result_candidate->tie_lower_num = 0;
    result_candidate->tie_upper_num = candidate_tie_upper;
    result_candidate->loss_lower_num = candidate_loss_lower;
    result_candidate->loss_upper_num = outcome_mass;
    result_candidate->worlds_exact = 0;
    result_candidate->worlds_bounded = worlds_bounded;
    result_candidate->worlds_unresolved = world_count - worlds_bounded;
    result_candidate->exact_weight = 0;
    result_candidate->bounded_weight = bounded_world_mass;
    result_candidate->unresolved_weight = world_mass - bounded_world_mass;
    result_candidate->outcome = cpeg_wtl_unresolved_envelope(margin_prior);
    cpeg_wtl_set_rational_outcome(result_candidate);
    states[candidate_idx] = (CpegWtlProofState){
        .win_upper_mass = candidate_win_upper,
        .tie_upper_mass = candidate_tie_upper,
        .loss_lower_mass = candidate_loss_lower,
        .loss_upper_mass = outcome_mass,
        .outcome_mass = outcome_mass,
        .outcome = result_candidate->outcome,
    };
    out->bound_jobs += worlds_bounded;
  }
  free(loss_lower);
  free(tie_upper);
  free(win_upper);
  free(candidate_traces);
  free(job_ptrs);
  free(jobs);
  return proof_valid;
}

typedef struct CpegWtlPlacementScreenJob {
  CpegDefenseWorker *workers;
  const Game *source_game;
  const CpegScheduledWorld *worlds;
  int world_count;
  const int *unseen;
  int ld_size;
  int opponent_idx;
  int root_idx;
  int candidate_idx;
  const CpegRootCand *candidate;
  int64_t initial_lead;
  int64_t deadline_ns;
  CpegInterval margin_prior;
  int64_t world_mass;
  int64_t incumbent_win_lower;
  int64_t incumbent_outcome_mass;
  CpegWtlCertifiedCand *result_candidate;
  CpegWtlProofState *state;
  CpegWtlProofWorld *world_evaluations;
  int exact_jobs;
  int bound_jobs;
  int batches_completed;
  bool collect_trace;
  bool use_best_bag_emptying_screen;
  bool use_threshold_reply_screen;
  bool proof_valid;
  bool deadline_reached;
  CpegWtlTrace trace;
} CpegWtlPlacementScreenJob;

static bool cpeg_wtl_shallow_placement_world(CpegWtlPlacementScreenJob *job,
                                             CpegDefenseWorker *worker,
                                             int world_idx,
                                             CpegWtlProofWorld *result) {
  memset(result, 0, sizeof(*result));
  const CpegMultiset *world = &job->worlds[world_idx].multiset;
  cpeg_set_world(worker->opponent_game, job->source_game, world, job->unseen,
                 job->ld_size, job->opponent_idx);
  const int tiles_played = move_get_tiles_played(&job->candidate->move);
  const int tiles_drawn = tiles_played < world->n ? tiles_played : world->n;
  const int remaining_bag = world->n - tiles_drawn;
  const bool restricted_best_defense =
      job->use_best_bag_emptying_screen && remaining_bag > 1;
  const bool single_best_defense =
      remaining_bag <= 1 || restricted_best_defense;
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;
  if (!cpeg_generate_small_moves_at_least(
          worker->opponent_game, worker->opponent_moves,
          single_best_defense ? MOVE_RECORD_BEST_SMALL : MOVE_RECORD_ALL_SMALL,
          restricted_best_defense ? remaining_bag : 0, trace)) {
    return false;
  }
  cpeg_sort_small_moves(worker->opponent_moves, trace);
  const SmallMove *defense = NULL;
  for (int move_idx = 0; move_idx < worker->opponent_moves->count; move_idx++) {
    const SmallMove *move = worker->opponent_moves->small_moves[move_idx];
    if (!small_move_is_pass(move) &&
        small_move_get_tiles_played(move) >= remaining_bag) {
      defense = move;
      break;
    }
  }
  if (defense == NULL) {
    result->envelope = cpeg_wtl_unresolved_envelope(job->margin_prior);
    result->proof = CPEG_WTL_PROOF_UNRESOLVED;
    return true;
  }
  if (trace != NULL) {
    trace->defenses_threshold_tested++;
    trace->defenses_accepted++;
  }

  Rack leave;
  rack_copy(&leave, player_get_rack(
                        game_get_player(worker->opponent_game, job->root_idx)));
  game_copy(worker->defense_game, worker->opponent_game);
  small_move_to_move(worker->opponent_moves->spare_move, defense,
                     game_get_board(worker->defense_game));
  play_move_incremental(worker->opponent_moves->spare_move,
                        worker->defense_game, worker->defense_undo);
  bag_set_to_tiles(game_get_bag(worker->defense_game), NULL, 0);
  game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
  game_set_consecutive_scoreless_turns(worker->defense_game, 0);

  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < world->n; tile_idx++) {
    bag_counts[world->tiles[tile_idx]]++;
  }
  CpegMultiset draws[CPEG_ENUM_CAP] = {0};
  bool overflow = false;
  const int draw_count = cpeg_enum_submultisets(
      bag_counts, job->ld_size, tiles_drawn, draws, CPEG_ENUM_CAP, &overflow);
  if (overflow || draw_count < 1) {
    return false;
  }
  if (trace != NULL) {
    trace->compatible_draws_tested += draw_count;
  }
  CpegWtlEnvelope draw_values[CPEG_ENUM_CAP] = {0};
  int64_t draw_weights[CPEG_ENUM_CAP] = {0};
  int64_t draw_mass = 0;
  int bounded_draws = 0;
  const int defense_score = small_move_get_score(defense);
  int64_t margin_after_root;
  if (!cpeg_checked_margin_add(job->initial_lead, job->candidate->score,
                               &margin_after_root)) {
    return false;
  }
  const int64_t threshold = (int64_t)defense_score - margin_after_root;
  for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      return true;
    }
    Rack branch_rack;
    rack_copy(&branch_rack, &leave);
    for (int tile_idx = 0; tile_idx < draws[draw_idx].n; tile_idx++) {
      rack_add_letter(&branch_rack, draws[draw_idx].tiles[tile_idx]);
    }
    rack_copy(
        player_get_rack(game_get_player(worker->defense_game, job->root_idx)),
        &branch_rack);
    CpegRootReplyResult reply;
    if (!cpeg_root_reply(
            worker->defense_game, worker->root_best, worker->root_best_small,
            worker->root_reply_cache, job->use_threshold_reply_screen,
            threshold, /*require_exact_score=*/false,
            CPEG_WTL_REPLY_PHASE_PLACEMENT_SCREEN, &reply, trace)) {
      return false;
    }
    if (reply.kind == CPEG_ROOT_REPLY_ABOVE_THRESHOLD) {
      draw_values[draw_idx] =
          cpeg_wtl_unresolved_envelope(job->margin_prior);
      draw_weights[draw_idx] = draws[draw_idx].weight;
      draw_mass += draws[draw_idx].weight;
      result->win_upper_mass += draws[draw_idx].weight;
      result->tie_upper_mass += draws[draw_idx].weight;
      result->loss_upper_mass += draws[draw_idx].weight;
      continue;
    }
    int64_t margin_upper;
    if (!cpeg_checked_margin_add(margin_after_root, -(int64_t)defense_score,
                                 &margin_upper) ||
        !cpeg_checked_margin_add(margin_upper, reply.exact_score,
                                 &margin_upper)) {
      return false;
    }
    draw_values[draw_idx] =
        cpeg_wtl_envelope_from_margin_upper(margin_upper, job->margin_prior);
    bounded_draws++;
    draw_weights[draw_idx] = draws[draw_idx].weight;
    draw_mass += draws[draw_idx].weight;
    if (margin_upper > 0) {
      result->win_upper_mass += draws[draw_idx].weight;
    }
    if (margin_upper >= 0) {
      result->tie_upper_mass += draws[draw_idx].weight;
    }
    if (margin_upper < 0) {
      result->loss_lower_mass += draws[draw_idx].weight;
    }
    result->loss_upper_mass += draws[draw_idx].weight;
  }
  result->envelope = cpeg_weighted_draw_envelope(draw_values, draw_weights,
                                                 draw_count, draw_mass);
  result->proof = bounded_draws > 0 ? CPEG_WTL_PROOF_DEFENSE_BOUND
                                    : CPEG_WTL_PROOF_UNRESOLVED;
  result->draw_mass = draw_mass;
  return true;
}

static void cpeg_wtl_placement_screen_job_run(void *arg, int worker_idx) {
  CpegWtlPlacementScreenJob *job = arg;
  memset(&job->trace, 0, sizeof(job->trace));
  const int64_t draw_mass =
      cpeg_wtl_candidate_draw_mass(job->candidate, job->worlds[0].multiset.n);
  const int64_t outcome_mass = job->world_mass * draw_mass;
  int64_t win_upper_mass = outcome_mass;
  job->proof_valid = true;
  for (int world_idx = 0; world_idx < job->world_count; world_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      break;
    }
    CpegWtlProofWorld evaluation;
    if (!cpeg_wtl_shallow_placement_world(job, &job->workers[worker_idx],
                                          world_idx, &evaluation)) {
      job->proof_valid = false;
      break;
    }
    if (job->deadline_reached) {
      break;
    }
    job->world_evaluations[world_idx] = evaluation;
    const int64_t world_weight = job->worlds[world_idx].multiset.weight;
    win_upper_mass -= world_weight * (draw_mass - evaluation.win_upper_mass);
    if (evaluation.proof == CPEG_WTL_PROOF_EXACT) {
      job->exact_jobs++;
    } else {
      job->bound_jobs++;
    }
    job->batches_completed++;
    bool comparison_valid = true;
    if (cpeg_wtl_fraction_less(
            win_upper_mass, outcome_mass, job->incumbent_win_lower,
            job->incumbent_outcome_mass, &comparison_valid)) {
      job->result_candidate->eliminated = true;
      break;
    }
    if (!comparison_valid) {
      job->proof_valid = false;
      break;
    }
  }
  cpeg_wtl_recompute_candidate(
      job->result_candidate, job->state, job->world_evaluations, job->worlds,
      job->world_count, job->world_mass, draw_mass, job->margin_prior);
}

static bool cpeg_wtl_screen_placements_parallel(
    PegPool *pool, CpegDefenseWorker *workers, int helper_worker_idx,
    const Game *root_game, const CpegRootCand *candidates, int candidate_count,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    const int *unseen, int ld_size, int opponent_idx, int root_idx,
    int incumbent_idx, const CpegWtlCertifiedArgs *args, int64_t deadline_ns,
    CpegInterval margin_prior, CpegWtlCertifiedResult *out,
    CpegWtlProofState *states, bool *stopped) {
  int job_count = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx != incumbent_idx && candidates[candidate_idx].kind == 0) {
      job_count++;
    }
  }
  CpegWtlPlacementScreenJob *screen_jobs =
      calloc_or_die((size_t)job_count, sizeof(*screen_jobs));
  void **screen_job_ptrs =
      malloc_or_die((size_t)job_count * sizeof(*screen_job_ptrs));
  Game **templates = calloc_or_die((size_t)job_count, sizeof(*templates));
  CpegWtlProofWorld *evaluations = calloc_or_die(
      (size_t)job_count * (size_t)world_count, sizeof(*evaluations));
  int job_idx = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx == incumbent_idx || candidates[candidate_idx].kind != 0) {
      continue;
    }
    templates[job_idx] =
        cpeg_build_root_template(root_game, &candidates[candidate_idx].move);
    screen_jobs[job_idx] = (CpegWtlPlacementScreenJob){
        .workers = workers,
        .source_game = templates[job_idx],
        .worlds = worlds,
        .world_count = world_count,
        .unseen = unseen,
        .ld_size = ld_size,
        .opponent_idx = opponent_idx,
        .root_idx = root_idx,
        .candidate_idx = candidate_idx,
        .candidate = &candidates[candidate_idx],
        .initial_lead = args->initial_lead,
        .deadline_ns = deadline_ns,
        .margin_prior = margin_prior,
        .world_mass = world_mass,
        .incumbent_win_lower = states[incumbent_idx].win_lower_mass,
        .incumbent_outcome_mass = states[incumbent_idx].outcome_mass,
        .result_candidate = &out->cands[candidate_idx],
        .state = &states[candidate_idx],
        .world_evaluations = &evaluations[job_idx * world_count],
        .collect_trace = args->collect_trace,
        .use_best_bag_emptying_screen =
            args->use_best_bag_emptying_screen,
        .use_threshold_reply_screen =
            args->use_threshold_reply_screen,
    };
    screen_job_ptrs[job_idx] = &screen_jobs[job_idx];
    job_idx++;
  }
  peg_pool_submit_and_wait(pool, cpeg_wtl_placement_screen_job_run,
                           screen_job_ptrs, job_count, helper_worker_idx);

  bool proof_valid = true;
  for (int screen_idx = 0; screen_idx < job_count; screen_idx++) {
    const CpegWtlPlacementScreenJob *job = &screen_jobs[screen_idx];
    cpeg_wtl_trace_add_work(args->collect_trace ? &out->trace : NULL,
                            &job->trace);
    cpeg_wtl_trace_add_work(
        args->collect_trace ? &out->candidate_traces[job->candidate_idx]
                            : NULL,
        &job->trace);
    out->exact_jobs += job->exact_jobs;
    out->bound_jobs += job->bound_jobs;
    out->batches_completed += job->batches_completed;
    if (!job->proof_valid) {
      proof_valid = false;
    }
    if (job->deadline_reached) {
      *stopped = true;
    }
    game_destroy(templates[screen_idx]);
  }
  free(evaluations);
  free(templates);
  free(screen_job_ptrs);
  free(screen_jobs);
  return proof_valid;
}

typedef struct CpegWtlPlacementRefineJob {
  CpegDefenseWorker *workers;
  const Game *source_game;
  const CpegScheduledWorld *worlds;
  int world_count;
  const int *unseen;
  int ld_size;
  int opponent_idx;
  int root_idx;
  int candidate_idx;
  const CpegRootCand *candidate;
  int64_t initial_lead;
  int64_t deadline_ns;
  CpegInterval margin_prior;
  int64_t world_mass;
  int64_t incumbent_win_lower;
  int64_t incumbent_outcome_mass;
  int max_defenses;
  CpegWtlCertifiedCand *result_candidate;
  CpegWtlProofState *state;
  CpegWtlProofWorld *world_evaluations;
  int exact_jobs;
  int bound_jobs;
  int batches_completed;
  bool collect_trace;
  bool use_threshold_reply_screen;
  bool use_fixed_win_threshold;
  bool proof_valid;
  bool deadline_reached;
  CpegWtlTrace trace;
} CpegWtlPlacementRefineJob;

static bool cpeg_wtl_refine_placement_world(CpegWtlPlacementRefineJob *job,
                                            CpegDefenseWorker *worker,
                                            int world_idx,
                                            CpegWtlProofWorld *result) {
  memset(result, 0, sizeof(*result));
  const CpegMultiset *world = &job->worlds[world_idx].multiset;
  cpeg_set_world(worker->opponent_game, job->source_game, world, job->unseen,
                 job->ld_size, job->opponent_idx);
  const int tiles_played = move_get_tiles_played(&job->candidate->move);
  const int tiles_drawn = tiles_played < world->n ? tiles_played : world->n;
  const int remaining_bag = world->n - tiles_drawn;
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;
  if (!cpeg_generate_small_moves(worker->opponent_game, worker->opponent_moves,
                                 MOVE_RECORD_ALL_SMALL, trace)) {
    return false;
  }
  cpeg_sort_small_moves(worker->opponent_moves, trace);
  Rack leave;
  rack_copy(&leave, player_get_rack(
                        game_get_player(worker->opponent_game, job->root_idx)));
  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < world->n; tile_idx++) {
    bag_counts[world->tiles[tile_idx]]++;
  }
  CpegMultiset draws[CPEG_ENUM_CAP] = {0};
  bool overflow = false;
  const int draw_count = cpeg_enum_submultisets(
      bag_counts, job->ld_size, tiles_drawn, draws, CPEG_ENUM_CAP, &overflow);
  if (overflow || draw_count < 1) {
    return false;
  }
  if (trace != NULL) {
    trace->compatible_draws_tested += draw_count;
  }
  int64_t best_margin_upper[CPEG_ENUM_CAP];
  bool have_bound[CPEG_ENUM_CAP] = {0};
  for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
    best_margin_upper[draw_idx] = INT64_MAX;
  }
  int64_t margin_after_root;
  if (!cpeg_checked_margin_add(job->initial_lead, job->candidate->score,
                               &margin_after_root)) {
    return false;
  }
  int defenses_tried = 0;
  for (int move_idx = 0; move_idx < worker->opponent_moves->count; move_idx++) {
    const SmallMove *defense = worker->opponent_moves->small_moves[move_idx];
    if (small_move_is_pass(defense) ||
        small_move_get_tiles_played(defense) < remaining_bag) {
      continue;
    }
    if (defenses_tried >= job->max_defenses) {
      break;
    }
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      return true;
    }
    defenses_tried++;
    if (trace != NULL) {
      trace->defenses_threshold_tested++;
    }
    game_copy(worker->defense_game, worker->opponent_game);
    small_move_to_move(worker->opponent_moves->spare_move, defense,
                       game_get_board(worker->defense_game));
    play_move_incremental(worker->opponent_moves->spare_move,
                          worker->defense_game, worker->defense_undo);
    bag_set_to_tiles(game_get_bag(worker->defense_game), NULL, 0);
    game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(worker->defense_game, 0);
    const int defense_score = small_move_get_score(defense);
    const int64_t threshold = (int64_t)defense_score - margin_after_root;
    for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
      if (have_bound[draw_idx] && best_margin_upper[draw_idx] <= 0) {
        continue;
      }
      Rack branch_rack;
      rack_copy(&branch_rack, &leave);
      for (int tile_idx = 0; tile_idx < draws[draw_idx].n; tile_idx++) {
        rack_add_letter(&branch_rack, draws[draw_idx].tiles[tile_idx]);
      }
      rack_copy(
          player_get_rack(game_get_player(worker->defense_game, job->root_idx)),
          &branch_rack);
      CpegRootReplyResult reply;
      if (!cpeg_root_reply(
              worker->defense_game, worker->root_best,
              worker->root_best_small, worker->root_reply_cache,
              job->use_threshold_reply_screen, threshold,
              /*require_exact_score=*/false,
              CPEG_WTL_REPLY_PHASE_SURVIVING_REFINE, &reply, trace)) {
        return false;
      }
      if (reply.kind == CPEG_ROOT_REPLY_ABOVE_THRESHOLD) {
        continue;
      }
      int64_t margin_upper;
      if (!cpeg_checked_margin_add(margin_after_root, -(int64_t)defense_score,
                                   &margin_upper) ||
          !cpeg_checked_margin_add(margin_upper, reply.exact_score,
                                   &margin_upper)) {
        return false;
      }
      have_bound[draw_idx] = true;
      if (margin_upper < best_margin_upper[draw_idx]) {
        best_margin_upper[draw_idx] = margin_upper;
      }
    }
  }
  if (trace != NULL) {
    for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
      if (have_bound[draw_idx]) {
        trace->defenses_accepted++;
      }
    }
  }

  CpegWtlEnvelope draw_values[CPEG_ENUM_CAP] = {0};
  int64_t draw_weights[CPEG_ENUM_CAP] = {0};
  int64_t draw_mass = 0;
  CpegWtlProofKind aggregate_proof = CPEG_WTL_PROOF_DEFENSE_BOUND;
  for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
    if (have_bound[draw_idx]) {
      draw_values[draw_idx] = cpeg_wtl_envelope_from_margin_upper(
          best_margin_upper[draw_idx], job->margin_prior);
    } else {
      draw_values[draw_idx] = cpeg_wtl_unresolved_envelope(job->margin_prior);
      aggregate_proof = CPEG_WTL_PROOF_UNRESOLVED;
    }
    const int64_t draw_weight = draws[draw_idx].weight;
    draw_weights[draw_idx] = draw_weight;
    draw_mass += draw_weight;
    if (!have_bound[draw_idx] || best_margin_upper[draw_idx] > 0) {
      result->win_upper_mass += draw_weight;
    }
    if (!have_bound[draw_idx] || best_margin_upper[draw_idx] >= 0) {
      result->tie_upper_mass += draw_weight;
    }
    if (have_bound[draw_idx] && best_margin_upper[draw_idx] < 0) {
      result->loss_lower_mass += draw_weight;
    }
    result->loss_upper_mass += draw_weight;
  }
  result->envelope = cpeg_weighted_draw_envelope(draw_values, draw_weights,
                                                 draw_count, draw_mass);
  result->proof = aggregate_proof;
  result->draw_mass = draw_mass;
  return true;
}

static bool cpeg_wtl_fixed_defense_world(CpegWtlPlacementRefineJob *job,
                                         CpegDefenseWorker *worker,
                                         int world_idx,
                                         CpegWtlProofWorld *result) {
  memset(result, 0, sizeof(*result));
  const CpegMultiset *world = &job->worlds[world_idx].multiset;
  cpeg_set_world(worker->opponent_game, job->source_game, world, job->unseen,
                 job->ld_size, job->opponent_idx);
  const int tiles_played = move_get_tiles_played(&job->candidate->move);
  const int tiles_drawn = tiles_played < world->n ? tiles_played : world->n;
  const int remaining_bag = world->n - tiles_drawn;
  const bool single_best_defense = remaining_bag <= 1;
  CpegWtlTrace *trace = job->collect_trace ? &job->trace : NULL;
  if (!cpeg_generate_small_moves(worker->opponent_game, worker->opponent_moves,
                                 single_best_defense ? MOVE_RECORD_BEST_SMALL
                                                     : MOVE_RECORD_ALL_SMALL,
                                 trace)) {
    return false;
  }
  cpeg_sort_small_moves(worker->opponent_moves, trace);
  const SmallMove *defense = NULL;
  for (int move_idx = 0; move_idx < worker->opponent_moves->count; move_idx++) {
    const SmallMove *move = worker->opponent_moves->small_moves[move_idx];
    if (!small_move_is_pass(move) &&
        small_move_get_tiles_played(move) >= remaining_bag) {
      defense = move;
      break;
    }
  }
  if (defense == NULL) {
    result->envelope = cpeg_wtl_unresolved_envelope(job->margin_prior);
    result->proof = CPEG_WTL_PROOF_UNRESOLVED;
    return true;
  }
  if (trace != NULL) {
    trace->defenses_threshold_tested++;
    trace->defenses_accepted++;
  }

  Rack leave;
  rack_copy(&leave, player_get_rack(
                        game_get_player(worker->opponent_game, job->root_idx)));
  game_copy(worker->defense_game, worker->opponent_game);
  small_move_to_move(worker->opponent_moves->spare_move, defense,
                     game_get_board(worker->defense_game));
  play_move_incremental(worker->opponent_moves->spare_move,
                        worker->defense_game, worker->defense_undo);
  game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
  game_set_consecutive_scoreless_turns(worker->defense_game, 0);
  Rack post_defense_opponent;
  rack_copy(&post_defense_opponent,
            player_get_rack(
                game_get_player(worker->defense_game, job->opponent_idx)));

  int bag_counts[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < world->n; tile_idx++) {
    bag_counts[world->tiles[tile_idx]]++;
  }
  CpegMultiset draws[CPEG_ENUM_CAP] = {0};
  bool overflow = false;
  const int draw_count = cpeg_enum_submultisets(
      bag_counts, job->ld_size, tiles_drawn, draws, CPEG_ENUM_CAP, &overflow);
  if (overflow || draw_count < 1) {
    return false;
  }
  if (trace != NULL) {
    trace->compatible_draws_tested += draw_count;
  }
  CpegWtlEnvelope draw_values[CPEG_ENUM_CAP] = {0};
  int64_t draw_weights[CPEG_ENUM_CAP] = {0};
  int64_t draw_mass = 0;
  const int defense_score = small_move_get_score(defense);
  int64_t margin_after_defense;
  if (!cpeg_checked_margin_add(job->initial_lead, job->candidate->score,
                               &margin_after_defense) ||
      !cpeg_checked_margin_add(margin_after_defense, -(int64_t)defense_score,
                               &margin_after_defense)) {
    return false;
  }
  for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      return true;
    }
    Rack branch_rack;
    rack_copy(&branch_rack, &leave);
    int remaining_counts[MAX_ALPHABET_SIZE];
    memcpy(remaining_counts, bag_counts, sizeof(remaining_counts));
    for (int tile_idx = 0; tile_idx < draws[draw_idx].n; tile_idx++) {
      const MachineLetter ml = draws[draw_idx].tiles[tile_idx];
      rack_add_letter(&branch_rack, ml);
      remaining_counts[ml]--;
    }
    rack_copy(
        player_get_rack(game_get_player(worker->defense_game, job->root_idx)),
        &branch_rack);
    Rack *opponent_rack = player_get_rack(
        game_get_player(worker->defense_game, job->opponent_idx));
    rack_copy(opponent_rack, &post_defense_opponent);
    for (int ml = 0; ml < job->ld_size; ml++) {
      for (int tile_idx = 0; tile_idx < remaining_counts[ml]; tile_idx++) {
        rack_add_letter(opponent_rack, (MachineLetter)ml);
      }
    }
    bag_set_to_tiles(game_get_bag(worker->defense_game), NULL, 0);
    game_set_game_end_reason(worker->defense_game, GAME_END_REASON_NONE);
    game_set_consecutive_scoreless_turns(worker->defense_game, 0);
    bool capacity_exceeded = false;
    bool endgame_complete = true;
    bool endgame_cache_hit = false;
    const int opponent_rack_tiles = rack_get_total_letters(opponent_rack);
    if (opponent_rack_tiles < 0 || opponent_rack_tiles > RACK_SIZE) {
      return false;
    }
    const int64_t start_ns = trace != NULL ? ctimer_monotonic_ns() : 0;
    int root_swing = 0;
    bool above_win_threshold = false;
    if (job->use_fixed_win_threshold) {
      if (!cpeg_endgame_swing_above(
              worker->defense_game, worker->endgame_mover,
              worker->endgame_reply, worker->endgame_undo,
              -margin_after_defense, &above_win_threshold,
              &capacity_exceeded, /*deadline_ns=*/0, &endgame_complete)) {
        return false;
      }
    } else {
      root_swing = cpeg_exact_endgame_swing(
          worker->defense_game, worker->endgame_mover, worker->endgame_reply,
          worker->endgame_undo, worker->exact_endgame_cache,
          &capacity_exceeded, /*deadline_ns=*/0, &endgame_complete,
          &endgame_cache_hit);
    }
    if (trace != NULL) {
      const int64_t work_ns = ctimer_monotonic_ns() - start_ns;
      trace->fixed_endgame_queries++;
      if (job->use_fixed_win_threshold) {
        trace->fixed_endgame_threshold_queries++;
        if (!capacity_exceeded && endgame_complete &&
            !above_win_threshold) {
          trace->fixed_endgame_threshold_proofs++;
        }
        trace->fixed_endgame_threshold_work_ns += work_ns;
      } else {
        trace->exact_endgame_queries++;
        if (endgame_cache_hit) {
          trace->exact_endgame_cache_hits++;
          trace->fixed_endgame_cache_hits++;
        }
        trace->exact_endgame_work_ns += work_ns;
      }
      trace->fixed_endgame_work_ns += work_ns;
      trace->fixed_endgame_queries_by_opponent_rack[opponent_rack_tiles]++;
      if (endgame_cache_hit) {
        trace
            ->fixed_endgame_cache_hits_by_opponent_rack[opponent_rack_tiles]++;
      }
      trace->fixed_endgame_work_ns_by_opponent_rack[opponent_rack_tiles] +=
          work_ns;
    }
    if (capacity_exceeded) {
      draw_values[draw_idx] = cpeg_wtl_unresolved_envelope(job->margin_prior);
      result->win_upper_mass += draws[draw_idx].weight;
      result->tie_upper_mass += draws[draw_idx].weight;
    } else if (job->use_fixed_win_threshold && above_win_threshold) {
      draw_values[draw_idx] =
          cpeg_wtl_unresolved_envelope(job->margin_prior);
      result->win_upper_mass += draws[draw_idx].weight;
      result->tie_upper_mass += draws[draw_idx].weight;
    } else {
      int64_t margin_upper;
      const int64_t swing_upper =
          job->use_fixed_win_threshold ? -margin_after_defense : root_swing;
      if (!cpeg_checked_margin_add(margin_after_defense, swing_upper,
                                   &margin_upper)) {
        return false;
      }
      draw_values[draw_idx] =
          cpeg_wtl_envelope_from_margin_upper(margin_upper, job->margin_prior);
      if (margin_upper > 0) {
        result->win_upper_mass += draws[draw_idx].weight;
      }
      if (margin_upper >= 0) {
        result->tie_upper_mass += draws[draw_idx].weight;
      } else {
        result->loss_lower_mass += draws[draw_idx].weight;
      }
    }
    result->loss_upper_mass += draws[draw_idx].weight;
    draw_weights[draw_idx] = draws[draw_idx].weight;
    draw_mass += draws[draw_idx].weight;
  }
  result->envelope = cpeg_weighted_draw_envelope(draw_values, draw_weights,
                                                 draw_count, draw_mass);
  result->proof = CPEG_WTL_PROOF_DEFENSE_BOUND;
  result->draw_mass = draw_mass;
  return true;
}

static void cpeg_wtl_placement_refine_job_run(void *arg, int worker_idx) {
  CpegWtlPlacementRefineJob *job = arg;
  memset(&job->trace, 0, sizeof(job->trace));
  const int bag = job->worlds[0].multiset.n;
  const int64_t draw_mass = cpeg_wtl_candidate_draw_mass(job->candidate, bag);
  const int64_t outcome_mass = job->world_mass * draw_mass;
  int64_t win_upper_mass = outcome_mass;
  job->proof_valid = true;
  for (int world_idx = 0; world_idx < job->world_count; world_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      break;
    }
    CpegWtlProofWorld evaluation;
    if (!cpeg_wtl_refine_placement_world(job, &job->workers[worker_idx],
                                         world_idx, &evaluation)) {
      job->proof_valid = false;
      break;
    }
    if (job->deadline_reached) {
      break;
    }
    job->world_evaluations[world_idx] = evaluation;
    const int64_t world_weight = job->worlds[world_idx].multiset.weight;
    win_upper_mass -= world_weight * (draw_mass - evaluation.win_upper_mass);
    if (evaluation.proof == CPEG_WTL_PROOF_EXACT) {
      job->exact_jobs++;
    } else {
      job->bound_jobs++;
    }
    job->batches_completed++;
    bool comparison_valid = true;
    if (cpeg_wtl_fraction_less(
            win_upper_mass, outcome_mass, job->incumbent_win_lower,
            job->incumbent_outcome_mass, &comparison_valid)) {
      job->result_candidate->eliminated = true;
      break;
    }
    if (!comparison_valid) {
      job->proof_valid = false;
      break;
    }
  }
  cpeg_wtl_recompute_candidate(
      job->result_candidate, job->state, job->world_evaluations, job->worlds,
      job->world_count, job->world_mass, draw_mass, job->margin_prior);
}

static void cpeg_wtl_fixed_defense_job_run(void *arg, int worker_idx) {
  CpegWtlPlacementRefineJob *job = arg;
  memset(&job->trace, 0, sizeof(job->trace));
  const int bag = job->worlds[0].multiset.n;
  const int64_t draw_mass = cpeg_wtl_candidate_draw_mass(job->candidate, bag);
  const int64_t outcome_mass = job->world_mass * draw_mass;
  int64_t win_upper_mass = outcome_mass;
  job->proof_valid = true;
  for (int world_idx = 0; world_idx < job->world_count; world_idx++) {
    if (cpeg_defense_deadline_reached(job->deadline_ns)) {
      job->deadline_reached = true;
      break;
    }
    CpegWtlProofWorld evaluation;
    if (!cpeg_wtl_fixed_defense_world(job, &job->workers[worker_idx], world_idx,
                                      &evaluation)) {
      job->proof_valid = false;
      break;
    }
    if (job->deadline_reached) {
      break;
    }
    job->world_evaluations[world_idx] = evaluation;
    const int64_t world_weight = job->worlds[world_idx].multiset.weight;
    win_upper_mass -= world_weight * (draw_mass - evaluation.win_upper_mass);
    job->bound_jobs++;
    job->batches_completed++;
    bool comparison_valid = true;
    if (cpeg_wtl_fraction_less(
            win_upper_mass, outcome_mass, job->incumbent_win_lower,
            job->incumbent_outcome_mass, &comparison_valid)) {
      job->result_candidate->eliminated = true;
      break;
    }
    if (!comparison_valid) {
      job->proof_valid = false;
      break;
    }
  }
  cpeg_wtl_recompute_candidate(
      job->result_candidate, job->state, job->world_evaluations, job->worlds,
      job->world_count, job->world_mass, draw_mass, job->margin_prior);
}

static bool cpeg_wtl_refine_surviving_placements(
    PegPool *pool, CpegDefenseWorker *workers, int helper_worker_idx,
    Game *root_game, const CpegRootCand *candidates, int candidate_count,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    const int *unseen, int ld_size, int opponent_idx, int root_idx,
    int incumbent_idx, const CpegWtlCertifiedArgs *args, int64_t deadline_ns,
    CpegInterval margin_prior, int max_defenses, bool fixed_endgame,
    CpegWtlCertifiedResult *out, CpegWtlProofState *states, bool *stopped) {
  int job_count = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    const bool eligible =
        candidates[candidate_idx].kind == 0 &&
        (!fixed_endgame ||
         move_get_tiles_played(&candidates[candidate_idx].move) < args->bag);
    if (candidate_idx != incumbent_idx && eligible &&
        !out->cands[candidate_idx].eliminated) {
      job_count++;
    }
  }
  if (job_count == 0) {
    return true;
  }
  CpegWtlPlacementRefineJob *refine_jobs =
      calloc_or_die((size_t)job_count, sizeof(*refine_jobs));
  void **refine_job_ptrs =
      malloc_or_die((size_t)job_count * sizeof(*refine_job_ptrs));
  Game **templates = calloc_or_die((size_t)job_count, sizeof(*templates));
  CpegWtlProofWorld *evaluations = calloc_or_die(
      (size_t)job_count * (size_t)world_count, sizeof(*evaluations));
  int job_idx = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    const bool eligible =
        candidates[candidate_idx].kind == 0 &&
        (!fixed_endgame ||
         move_get_tiles_played(&candidates[candidate_idx].move) < args->bag);
    if (candidate_idx == incumbent_idx || !eligible ||
        out->cands[candidate_idx].eliminated) {
      continue;
    }
    templates[job_idx] =
        cpeg_build_root_template(root_game, &candidates[candidate_idx].move);
    refine_jobs[job_idx] = (CpegWtlPlacementRefineJob){
        .workers = workers,
        .source_game = templates[job_idx],
        .worlds = worlds,
        .world_count = world_count,
        .unseen = unseen,
        .ld_size = ld_size,
        .opponent_idx = opponent_idx,
        .root_idx = root_idx,
        .candidate_idx = candidate_idx,
        .candidate = &candidates[candidate_idx],
        .initial_lead = args->initial_lead,
        .deadline_ns = deadline_ns,
        .margin_prior = margin_prior,
        .world_mass = world_mass,
        .incumbent_win_lower = states[incumbent_idx].win_lower_mass,
        .incumbent_outcome_mass = states[incumbent_idx].outcome_mass,
        .max_defenses = max_defenses,
        .result_candidate = &out->cands[candidate_idx],
        .state = &states[candidate_idx],
        .world_evaluations = &evaluations[job_idx * world_count],
        .collect_trace = args->collect_trace,
        .use_threshold_reply_screen = args->use_threshold_reply_screen,
        .use_fixed_win_threshold = args->use_fixed_win_threshold,
    };
    refine_job_ptrs[job_idx] = &refine_jobs[job_idx];
    job_idx++;
  }
  peg_pool_submit_and_wait(pool,
                           fixed_endgame ? cpeg_wtl_fixed_defense_job_run
                                         : cpeg_wtl_placement_refine_job_run,
                           refine_job_ptrs, job_count, helper_worker_idx);

  bool proof_valid = true;
  for (int refine_idx = 0; refine_idx < job_count; refine_idx++) {
    const CpegWtlPlacementRefineJob *job = &refine_jobs[refine_idx];
    cpeg_wtl_trace_add_work(args->collect_trace ? &out->trace : NULL,
                            &job->trace);
    cpeg_wtl_trace_add_work(
        args->collect_trace ? &out->candidate_traces[job->candidate_idx]
                            : NULL,
        &job->trace);
    out->exact_jobs += job->exact_jobs;
    out->bound_jobs += job->bound_jobs;
    out->batches_completed += job->batches_completed;
    if (!job->proof_valid) {
      proof_valid = false;
    }
    if (job->deadline_reached) {
      *stopped = true;
    }
    game_destroy(templates[refine_idx]);
  }
  free(evaluations);
  free(templates);
  free(refine_job_ptrs);
  free(refine_jobs);
  return proof_valid;
}

typedef struct CpegWtlFixedWorldJob {
  CpegWtlPlacementRefineJob context;
  int world_idx;
  CpegWtlProofWorld result;
  bool complete;
  bool proof_valid;
} CpegWtlFixedWorldJob;

static void cpeg_wtl_fixed_world_job_run(void *arg, int worker_idx) {
  CpegWtlFixedWorldJob *job = arg;
  job->complete = true;
  job->proof_valid = cpeg_wtl_fixed_defense_world(
      &job->context, &job->context.workers[worker_idx], job->world_idx,
      &job->result);
  if (job->context.deadline_reached) {
    job->complete = false;
  }
}

static bool cpeg_wtl_refine_fixed_worlds_parallel(
    PegPool *pool, CpegDefenseWorker *workers, int helper_worker_idx,
    const Game *root_game, const CpegRootCand *candidates, int candidate_count,
    const CpegScheduledWorld *worlds, int world_count, int64_t world_mass,
    const int *unseen, int ld_size, int opponent_idx, int root_idx,
    int incumbent_idx, const CpegWtlCertifiedArgs *args, int64_t deadline_ns,
    CpegInterval margin_prior, CpegWtlCertifiedResult *out,
    CpegWtlProofState *states, bool *stopped) {
  int refine_count = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx != incumbent_idx && candidates[candidate_idx].kind == 0 &&
        move_get_tiles_played(&candidates[candidate_idx].move) < args->bag &&
        !out->cands[candidate_idx].eliminated) {
      refine_count++;
    }
  }
  if (refine_count == 0) {
    return true;
  }
  int *candidate_indices =
      malloc_or_die((size_t)refine_count * sizeof(*candidate_indices));
  Game **templates = calloc_or_die((size_t)refine_count, sizeof(*templates));
  CpegWtlProofWorld *evaluations = calloc_or_die(
      (size_t)refine_count * (size_t)world_count, sizeof(*evaluations));
  const int job_count = refine_count * world_count;
  CpegWtlFixedWorldJob *jobs = calloc_or_die((size_t)job_count, sizeof(*jobs));
  void **job_ptrs = malloc_or_die((size_t)job_count * sizeof(*job_ptrs));
  int refine_idx = 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx == incumbent_idx || candidates[candidate_idx].kind != 0 ||
        move_get_tiles_played(&candidates[candidate_idx].move) >= args->bag ||
        out->cands[candidate_idx].eliminated) {
      continue;
    }
    candidate_indices[refine_idx] = candidate_idx;
    templates[refine_idx] =
        cpeg_build_root_template(root_game, &candidates[candidate_idx].move);
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      const int job_idx = refine_idx * world_count + world_idx;
      jobs[job_idx] = (CpegWtlFixedWorldJob){
          .context =
              {
                  .workers = workers,
                  .source_game = templates[refine_idx],
                  .worlds = worlds,
                  .world_count = world_count,
                  .unseen = unseen,
                  .ld_size = ld_size,
                  .opponent_idx = opponent_idx,
                  .root_idx = root_idx,
                  .candidate_idx = candidate_idx,
                  .candidate = &candidates[candidate_idx],
                  .initial_lead = args->initial_lead,
                  .deadline_ns = deadline_ns,
                  .margin_prior = margin_prior,
                   .world_mass = world_mass,
                  .collect_trace = args->collect_trace,
                  .use_threshold_reply_screen =
                      args->use_threshold_reply_screen,
                  .use_fixed_win_threshold =
                      args->use_fixed_win_threshold,
               },
          .world_idx = world_idx,
      };
      job_ptrs[job_idx] = &jobs[job_idx];
    }
    refine_idx++;
  }
  peg_pool_submit_and_wait(pool, cpeg_wtl_fixed_world_job_run, job_ptrs,
                           job_count, helper_worker_idx);

  bool proof_valid = true;
  for (int job_idx = 0; job_idx < job_count; job_idx++) {
    cpeg_wtl_trace_add_work(args->collect_trace ? &out->trace : NULL,
                            &jobs[job_idx].context.trace);
    cpeg_wtl_trace_add_work(
        args->collect_trace
            ? &out->candidate_traces[jobs[job_idx].context.candidate_idx]
            : NULL,
        &jobs[job_idx].context.trace);
    if (!jobs[job_idx].proof_valid) {
      proof_valid = false;
    }
    if (!jobs[job_idx].complete) {
      *stopped = true;
      continue;
    }
    const int candidate_slot = job_idx / world_count;
    const int world_idx = job_idx % world_count;
    evaluations[candidate_slot * world_count + world_idx] =
        jobs[job_idx].result;
    out->bound_jobs++;
    out->batches_completed++;
  }
  for (int candidate_slot = 0; candidate_slot < refine_count;
       candidate_slot++) {
    const int candidate_idx = candidate_indices[candidate_slot];
    const int64_t draw_mass =
        cpeg_wtl_candidate_draw_mass(&candidates[candidate_idx], args->bag);
    cpeg_wtl_recompute_candidate(
        &out->cands[candidate_idx], &states[candidate_idx],
        &evaluations[candidate_slot * world_count], worlds, world_count,
        world_mass, draw_mass, margin_prior);
  }
  for (int candidate_slot = 0; candidate_slot < refine_count;
       candidate_slot++) {
    game_destroy(templates[candidate_slot]);
  }
  free(job_ptrs);
  free(jobs);
  free(evaluations);
  free(templates);
  free(candidate_indices);
  return proof_valid;
}

int cpeg_solve_pre_endgame_wtl_certified(const Game *game,
                                         const CpegWtlCertifiedArgs *args,
                                         CpegWtlCertifiedResult *out) {
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->best_index = -1;
  if (game == NULL || args == NULL || args->bag < 1 ||
      args->bag > PEG_MAX_BAG || !isfinite(args->budget_seconds) ||
      args->budget_seconds < 0.0 || args->max_batches < 0 ||
      args->weighted_world_count < 0 ||
      ((args->weighted_worlds == NULL) != (args->weighted_world_count == 0)) ||
      ((args->weighted_unseen_tiles == NULL) !=
       (args->weighted_unseen_count == 0)) ||
      ((args->weighted_worlds == NULL) !=
       (args->weighted_unseen_tiles == NULL))) {
    return -1;
  }

  int result = -1;
  bool proof_valid = true;
  const bool collect_trace = args->collect_trace;
  const int64_t trace_start_ns =
      collect_trace ? ctimer_monotonic_ns() : 0;
  int64_t trace_stage_start_ns = trace_start_ns;
  const int64_t deadline_ns = cpeg_wtl_proof_deadline_ns(args->budget_seconds);
  Game *root_game = game_duplicate(game);
  const LetterDistribution *ld = game_get_ld(root_game);
  const int ld_size = ld_get_size(ld);
  game_gen_all_cross_sets(root_game);
  board_set_cross_sets_valid(game_get_board(root_game), true);
  const int root_idx = game_get_player_on_turn_index(root_game);
  const int opponent_idx = 1 - root_idx;
  int unseen[MAX_ALPHABET_SIZE];
  const int total_unseen = cpeg_compute_unseen(root_game, root_idx, unseen);
  if (total_unseen - args->bag < 0 || total_unseen - args->bag > RACK_SIZE) {
    game_destroy(root_game);
    return -1;
  }
  if (args->weighted_worlds != NULL) {
    if (args->weighted_unseen_count != total_unseen) {
      game_destroy(root_game);
      return -1;
    }
    int supplied_unseen[MAX_ALPHABET_SIZE] = {0};
    for (int tile_idx = 0; tile_idx < args->weighted_unseen_count; tile_idx++) {
      const MachineLetter tile = args->weighted_unseen_tiles[tile_idx];
      if ((int)tile >= ld_size) {
        game_destroy(root_game);
        return -1;
      }
      supplied_unseen[tile]++;
    }
    for (int ml = 0; ml < ld_size; ml++) {
      if (supplied_unseen[ml] != unseen[ml]) {
        game_destroy(root_game);
        return -1;
      }
    }
  }

  CpegRootCollection root_collection = {0};
  CpegScheduledWorld *worlds = NULL;
  CpegDefenseWorker *workers = NULL;
  CpegExactEndgameCache *exact_endgame_cache = NULL;
  PegPool *pool = NULL;
  CpegDefenseJob *jobs = NULL;
  void **job_ptrs = NULL;
  int *candidate_order = NULL;
  bool *candidates_refined = NULL;
  CpegWtlProofState *states = NULL;
  CpegWtlProofWorld *world_evaluations = NULL;
  int scratch_count = 0;
  if (!cpeg_collect_root_candidates(root_game, args->bag, args->allow_exchanges,
                                    &root_collection)) {
    goto cleanup;
  }
  CpegRootCand *candidates = root_collection.candidates;
  const int candidate_count = root_collection.count;
  if (collect_trace) {
    out->trace.root_actions = candidate_count;
    out->trace.challengers = candidate_count > 0 ? candidate_count - 1 : 0;
  }
  out->coverage = root_collection.coverage;
  out->cands = calloc_or_die((size_t)candidate_count, sizeof(*out->cands));
  if (collect_trace) {
    out->candidate_traces =
        calloc_or_die((size_t)candidate_count,
                      sizeof(*out->candidate_traces));
  }
  states = calloc_or_die((size_t)candidate_count, sizeof(*states));
  candidate_order =
      malloc_or_die((size_t)candidate_count * sizeof(*candidate_order));
  candidates_refined =
      calloc_or_die((size_t)candidate_count, sizeof(*candidates_refined));
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    candidate_order[candidate_idx] = candidate_idx;
    cpeg_render_root_candidate(out->cands[candidate_idx].label,
                               &candidates[candidate_idx],
                               game_get_board(root_game), ld);
    out->cands[candidate_idx].score = candidates[candidate_idx].score;
    int insertion_idx = candidate_idx;
    while (insertion_idx > 0 &&
           cpeg_wtl_candidate_precedes(candidates, args->bag,
                                       candidate_order[insertion_idx],
                                       candidate_order[insertion_idx - 1])) {
      const int previous = candidate_order[insertion_idx - 1];
      candidate_order[insertion_idx - 1] = candidate_order[insertion_idx];
      candidate_order[insertion_idx] = previous;
      insertion_idx--;
    }
  }

  int64_t world_mass = 0;
  const int world_count = cpeg_prepare_scheduled_worlds(
      unseen, ld_size, args->bag, args->weighted_worlds,
      args->weighted_world_count, &worlds, &world_mass);
  if (world_count < 1) {
    goto cleanup;
  }
  out->worlds_distinct = world_count;
  out->world_weight_mass = world_mass;
  if (collect_trace) {
    out->trace.worlds = world_count;
    // The current producer schedules one concrete private rack per world.
    out->trace.opponent_information_states = world_count;
  }
  const int root_score_bound =
      cpeg_score_upper_bound(game_get_board(root_game), root_game);
  const CpegInterval root_spread_prior =
      cpeg_scoreless_prior(args->bag, root_score_bound);
  const CpegInterval unresolved_margin_prior = {
      .lo = cpeg_down_add((double)args->initial_lead, root_spread_prior.lo),
      .hi = cpeg_up_add((double)args->initial_lead, root_spread_prior.hi),
  };
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    const int64_t draw_mass =
        cpeg_wtl_candidate_draw_mass(&candidates[candidate_idx], args->bag);
    const int64_t outcome_mass = world_mass * draw_mass;
    states[candidate_idx] = (CpegWtlProofState){
        .win_upper_mass = outcome_mass,
        .tie_upper_mass = outcome_mass,
        .loss_upper_mass = outcome_mass,
        .outcome_mass = outcome_mass,
        .outcome = cpeg_wtl_unresolved_envelope(unresolved_margin_prior),
    };
    CpegWtlCertifiedCand *result_candidate = &out->cands[candidate_idx];
    result_candidate->outcome = states[candidate_idx].outcome;
    result_candidate->outcome_den = outcome_mass;
    result_candidate->win_upper_num = outcome_mass;
    result_candidate->tie_upper_num = outcome_mass;
    result_candidate->loss_upper_num = outcome_mass;
    result_candidate->worlds_unresolved = world_count;
    result_candidate->unresolved_weight = world_mass;
  }

  const int thread_count = args->num_threads < 1 ? 1 : args->num_threads;
  if (collect_trace) {
    // The pool owns `thread_count` workers and the caller may also help.
    out->trace.compute_participant_capacity =
        thread_count > 1 ? thread_count + 1 : 1;
  }
  pool = thread_count > 1 ? peg_pool_create(thread_count, 0) : NULL;
  if (pool != NULL) {
    peg_pool_set_stuck_timeout_seconds(pool, 0);
  }
  scratch_count = pool != NULL ? thread_count + 1 : 1;
  workers = calloc_or_die((size_t)scratch_count, sizeof(*workers));
  if (args->use_exact_endgame_cache) {
    exact_endgame_cache = calloc_or_die(1, sizeof(*exact_endgame_cache));
    atomic_flag_clear(&exact_endgame_cache->lock);
  }
  for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
    workers[worker_idx].opponent_game = game_duplicate(root_game);
    workers[worker_idx].draw_game = game_duplicate(root_game);
    workers[worker_idx].defense_game = game_duplicate(root_game);
    workers[worker_idx].opponent_moves =
        move_list_create_small(CPEG_MOVE_LIST_CAP + 1);
    workers[worker_idx].root_best = move_list_create(1);
    workers[worker_idx].exact_endgame_cache = exact_endgame_cache;
    if (args->use_threshold_reply_screen) {
      workers[worker_idx].root_best_small = move_list_create_small(1);
      workers[worker_idx].root_reply_cache =
          calloc_or_die(CPEG_ROOT_REPLY_CACHE_CAPACITY,
                        sizeof(*workers[worker_idx].root_reply_cache));
    }
    workers[worker_idx].defense_undo = malloc_or_die(sizeof(MoveUndo));
    workers[worker_idx].endgame_mover =
        move_list_create(CPEG_MOVE_LIST_CAP + 1);
    workers[worker_idx].endgame_reply = move_list_create(CPEG_MOVE_LIST_CAP);
    workers[worker_idx].endgame_undo = malloc_or_die(sizeof(MoveUndo));
  }
  int batch_size = args->batch_size > 0 ? args->batch_size : 64;
  if (batch_size > world_count) {
    batch_size = world_count;
  }
  jobs = calloc_or_die((size_t)batch_size, sizeof(*jobs));
  job_ptrs = malloc_or_die((size_t)batch_size * sizeof(*job_ptrs));
  world_evaluations =
      calloc_or_die((size_t)world_count, sizeof(*world_evaluations));
  const int helper_worker_idx = pool != NULL ? thread_count : 0;
  if (collect_trace) {
    out->trace.setup_ns = ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }

  int incumbent_idx = -1;
  bool stopped = false;
  for (int order_idx = 0; order_idx < candidate_count && !stopped;
       order_idx++) {
    if (args->max_batches == 0 && incumbent_idx >= 0) {
      break;
    }
    const int candidate_idx = candidate_order[order_idx];
    const CpegRootCand *candidate = &candidates[candidate_idx];
    if (candidate->kind != 0) {
      continue;
    }
    const bool horizon = move_get_tiles_played(&candidate->move) >= args->bag;
    const bool bootstrap = incumbent_idx < 0 && horizon;
    Game *template_game =
        candidate->kind == 0
            ? cpeg_build_root_template(root_game, &candidate->move)
            : NULL;
    const Game *source_game = template_game != NULL ? template_game : root_game;
    const CpegInterval margin_prior = unresolved_margin_prior;
    const int64_t draw_mass =
        cpeg_wtl_candidate_draw_mass(candidate, args->bag);
    memset(world_evaluations, 0,
           (size_t)world_count * sizeof(*world_evaluations));

    for (int first_world = 0; first_world < world_count;
         first_world += batch_size) {
      if (cpeg_wtl_proof_budget_reached(deadline_ns, args->max_batches,
                                        out->batches_completed)) {
        stopped = true;
        break;
      }
      int batch_count = world_count - first_world;
      if (batch_count > batch_size) {
        batch_count = batch_size;
      }
      bool batch_complete = false;
      proof_valid = cpeg_wtl_run_defense_batch(
          pool, workers, helper_worker_idx, jobs, job_ptrs, first_world,
          batch_count, source_game, worlds, unseen, ld_size, opponent_idx,
          root_idx, candidate, args->initial_lead, deadline_ns, margin_prior,
          bootstrap, bootstrap && args->use_exact_two_ply_incumbent,
          args->use_threshold_reply_screen, CPEG_WTL_REPLY_PHASE_INCUMBENT,
          bootstrap ? 0 : 1, world_evaluations, &out->exact_jobs,
          &out->bound_jobs, &batch_complete,
          collect_trace ? &out->trace : NULL,
          collect_trace ? &out->candidate_traces[candidate_idx] : NULL);
      if (!proof_valid || !batch_complete) {
        stopped = true;
        break;
      }
      out->batches_completed++;
      cpeg_wtl_recompute_candidate(
          &out->cands[candidate_idx], &states[candidate_idx], world_evaluations,
          worlds, world_count, world_mass, draw_mass, margin_prior);
      if (incumbent_idx >= 0 &&
          cpeg_wtl_fraction_less(states[candidate_idx].win_upper_mass,
                                 states[candidate_idx].outcome_mass,
                                 states[incumbent_idx].win_lower_mass,
                                 states[incumbent_idx].outcome_mass,
                                 &proof_valid)) {
        out->cands[candidate_idx].eliminated = true;
        break;
      }
      if (!proof_valid) {
        stopped = true;
        break;
      }
    }

    if (stopped || out->cands[candidate_idx].eliminated) {
      if (template_game != NULL) {
        game_destroy(template_game);
      }
      continue;
    }
    cpeg_wtl_recompute_candidate(
        &out->cands[candidate_idx], &states[candidate_idx], world_evaluations,
        worlds, world_count, world_mass, draw_mass, margin_prior);
    const bool improves_incumbent =
        horizon && (incumbent_idx < 0 ||
                    cpeg_wtl_fraction_less(states[incumbent_idx].win_lower_mass,
                                           states[incumbent_idx].outcome_mass,
                                           states[candidate_idx].win_lower_mass,
                                           states[candidate_idx].outcome_mass,
                                           &proof_valid));
    if (improves_incumbent && !bootstrap) {
      memset(world_evaluations, 0,
             (size_t)world_count * sizeof(*world_evaluations));
      for (int first_world = 0; first_world < world_count;
           first_world += batch_size) {
        if (cpeg_wtl_proof_budget_reached(deadline_ns, args->max_batches,
                                          out->batches_completed)) {
          stopped = true;
          break;
        }
        int batch_count = world_count - first_world;
        if (batch_count > batch_size) {
          batch_count = batch_size;
        }
        bool batch_complete = false;
        proof_valid = cpeg_wtl_run_defense_batch(
            pool, workers, helper_worker_idx, jobs, job_ptrs, first_world,
            batch_count, source_game, worlds, unseen, ld_size, opponent_idx,
            root_idx, candidate, args->initial_lead, deadline_ns, margin_prior,
            /*exhaustive_horizon=*/true,
            /*use_exact_two_ply=*/args->use_exact_two_ply_incumbent,
            args->use_threshold_reply_screen, CPEG_WTL_REPLY_PHASE_INCUMBENT,
            /*max_defenses=*/0, world_evaluations, &out->exact_jobs,
            &out->bound_jobs, &batch_complete,
            collect_trace ? &out->trace : NULL,
            collect_trace ? &out->candidate_traces[candidate_idx] : NULL);
        if (!proof_valid || !batch_complete) {
          stopped = true;
          break;
        }
        out->batches_completed++;
      }
      cpeg_wtl_recompute_candidate(
          &out->cands[candidate_idx], &states[candidate_idx], world_evaluations,
          worlds, world_count, world_mass, draw_mass, margin_prior);
    }
    if (improves_incumbent && !stopped) {
      incumbent_idx = candidate_idx;
    }
    if (template_game != NULL) {
      game_destroy(template_game);
    }
    if (!proof_valid) {
      stopped = true;
    }
  }

  if (collect_trace) {
    out->trace.incumbent_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }
  if (!stopped && args->max_batches == 0 && incumbent_idx >= 0) {
    proof_valid = cpeg_wtl_screen_placements_parallel(
        pool, workers, helper_worker_idx, root_game, candidates,
        candidate_count, worlds, world_count, world_mass, unseen, ld_size,
        opponent_idx, root_idx, incumbent_idx, args, deadline_ns,
        unresolved_margin_prior, out, states, &stopped);
  }
  if (collect_trace) {
    out->trace.placement_screen_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }

  // Stage B: refine horizon-collapsing challengers by their proved strict-win
  // upper mass, not by score or label. A root-empty action has one
  // deterministic draw per world, so exhausting its opponent final actions
  // establishes exact W/T/L integer mass without entering the recursive
  // pre-endgame solver.
  while (!stopped && incumbent_idx >= 0) {
    int challenger_idx = -1;
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      const bool horizon =
          candidates[candidate_idx].kind == 0 &&
          move_get_tiles_played(&candidates[candidate_idx].move) >= args->bag;
      if (!horizon || candidate_idx == incumbent_idx ||
          candidates_refined[candidate_idx] ||
          out->cands[candidate_idx].eliminated) {
        continue;
      }
      if (cpeg_wtl_fraction_less(states[candidate_idx].win_upper_mass,
                                 states[candidate_idx].outcome_mass,
                                 states[incumbent_idx].win_lower_mass,
                                 states[incumbent_idx].outcome_mass,
                                 &proof_valid)) {
        out->cands[candidate_idx].eliminated = true;
        continue;
      }
      if (challenger_idx < 0) {
        challenger_idx = candidate_idx;
        continue;
      }
      const CpegWtlProofState *challenger_state = &states[challenger_idx];
      const CpegWtlProofState *candidate_state = &states[candidate_idx];
      const bool candidate_has_higher_upper = cpeg_wtl_fraction_less(
          challenger_state->win_upper_mass, challenger_state->outcome_mass,
          candidate_state->win_upper_mass, candidate_state->outcome_mass,
          &proof_valid);
      if (!proof_valid) {
        break;
      }
      const bool equal_upper = cpeg_wtl_fraction_equal(
          challenger_state->win_upper_mass, challenger_state->outcome_mass,
          candidate_state->win_upper_mass, candidate_state->outcome_mass,
          &proof_valid);
      if (!proof_valid) {
        stopped = true;
        break;
      }
      if (candidate_has_higher_upper ||
          (equal_upper && candidate_idx < challenger_idx)) {
        challenger_idx = candidate_idx;
      }
    }
    if (!proof_valid || challenger_idx < 0) {
      break;
    }
    candidates_refined[challenger_idx] = true;
    const CpegRootCand *candidate = &candidates[challenger_idx];
    Game *template_game = cpeg_build_root_template(root_game, &candidate->move);
    memset(world_evaluations, 0,
           (size_t)world_count * sizeof(*world_evaluations));
    for (int first_world = 0; first_world < world_count;
         first_world += batch_size) {
      if (cpeg_wtl_proof_budget_reached(deadline_ns, args->max_batches,
                                        out->batches_completed)) {
        stopped = true;
        break;
      }
      int batch_count = world_count - first_world;
      if (batch_count > batch_size) {
        batch_count = batch_size;
      }
      bool batch_complete = false;
      proof_valid = cpeg_wtl_run_defense_batch(
          pool, workers, helper_worker_idx, jobs, job_ptrs, first_world,
          batch_count, template_game, worlds, unseen, ld_size, opponent_idx,
          root_idx, candidate, args->initial_lead, deadline_ns,
          unresolved_margin_prior, /*exhaustive_horizon=*/false,
          /*use_exact_two_ply=*/false, args->use_threshold_reply_screen,
          CPEG_WTL_REPLY_PHASE_HORIZON_REFINE, /*max_defenses=*/0,
          world_evaluations,
          &out->exact_jobs, &out->bound_jobs, &batch_complete,
          collect_trace ? &out->trace : NULL,
          collect_trace ? &out->candidate_traces[challenger_idx] : NULL);
      if (!proof_valid || !batch_complete) {
        stopped = true;
        break;
      }
      out->batches_completed++;
    }
    cpeg_wtl_recompute_candidate(&out->cands[challenger_idx],
                                 &states[challenger_idx], world_evaluations,
                                 worlds, world_count, world_mass,
                                 /*draw_mass=*/1, unresolved_margin_prior);
    const bool improves = cpeg_wtl_fraction_less(
        states[incumbent_idx].win_lower_mass,
        states[incumbent_idx].outcome_mass,
        states[challenger_idx].win_lower_mass,
        states[challenger_idx].outcome_mass, &proof_valid);
    if (improves && !stopped) {
      incumbent_idx = challenger_idx;
    }
    game_destroy(template_game);
    if (improves && !stopped) {
      break;
    }
  }

  if (collect_trace) {
    out->trace.horizon_refine_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }
  if (!stopped) {
    if (args->max_batches == 0) {
      proof_valid = cpeg_wtl_screen_scoreless_candidates_parallel(
          pool, workers, helper_worker_idx, root_game, candidates,
          candidate_count, worlds, world_count, world_mass, unseen, ld_size,
          root_idx, opponent_idx, args, deadline_ns, unresolved_margin_prior,
          out, states, &stopped);
    } else {
      proof_valid = cpeg_wtl_screen_scoreless_candidates(
          root_game, candidates, candidate_count, worlds, world_count,
          world_mass, unseen, ld_size, root_idx, opponent_idx, args,
          deadline_ns, unresolved_margin_prior, out, states, &stopped);
    }
  }
  if (collect_trace) {
    out->trace.scoreless_screen_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }

  if (!stopped && incumbent_idx >= 0) {
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      if (candidate_idx == incumbent_idx) {
        continue;
      }
      out->cands[candidate_idx].eliminated = cpeg_wtl_fraction_less(
          states[candidate_idx].win_upper_mass,
          states[candidate_idx].outcome_mass,
          states[incumbent_idx].win_lower_mass,
          states[incumbent_idx].outcome_mass, &proof_valid);
    }
  }
  if (proof_valid && !stopped && args->max_batches == 0 && incumbent_idx >= 0) {
    proof_valid = cpeg_wtl_refine_surviving_placements(
        pool, workers, helper_worker_idx, root_game, candidates,
        candidate_count, worlds, world_count, world_mass, unseen, ld_size,
        opponent_idx, root_idx, incumbent_idx, args, deadline_ns,
        unresolved_margin_prior, /*max_defenses=*/64,
        /*fixed_endgame=*/false, out, states, &stopped);
  }
  if (collect_trace) {
    out->trace.surviving_refine_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }
  if (proof_valid && !stopped && incumbent_idx >= 0) {
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      if (candidate_idx == incumbent_idx) {
        continue;
      }
      out->cands[candidate_idx].eliminated = cpeg_wtl_fraction_less(
          states[candidate_idx].win_upper_mass,
          states[candidate_idx].outcome_mass,
          states[incumbent_idx].win_lower_mass,
          states[incumbent_idx].outcome_mass, &proof_valid);
    }
  }
  if (proof_valid && !stopped && args->max_batches == 0 && incumbent_idx >= 0) {
    if (args->use_fixed_win_threshold) {
      proof_valid = cpeg_wtl_refine_surviving_placements(
          pool, workers, helper_worker_idx, root_game, candidates,
          candidate_count, worlds, world_count, world_mass, unseen, ld_size,
          opponent_idx, root_idx, incumbent_idx, args, deadline_ns,
          unresolved_margin_prior, /*max_defenses=*/1,
          /*fixed_endgame=*/true, out, states, &stopped);
    } else {
      proof_valid = cpeg_wtl_refine_fixed_worlds_parallel(
          pool, workers, helper_worker_idx, root_game, candidates,
          candidate_count, worlds, world_count, world_mass, unseen, ld_size,
          opponent_idx, root_idx, incumbent_idx, args, deadline_ns,
          unresolved_margin_prior, out, states, &stopped);
    }
  }
  if (collect_trace) {
    out->trace.fixed_refine_ns =
        ctimer_monotonic_ns() - trace_stage_start_ns;
    trace_stage_start_ns = ctimer_monotonic_ns();
  }

  if (!proof_valid) {
    goto cleanup;
  }
  if (incumbent_idx >= 0) {
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      if (candidate_idx == incumbent_idx) {
        continue;
      }
      out->cands[candidate_idx].eliminated = cpeg_wtl_fraction_less(
          states[candidate_idx].win_upper_mass,
          states[candidate_idx].outcome_mass,
          states[incumbent_idx].win_lower_mass,
          states[incumbent_idx].outcome_mass, &proof_valid);
    }
  }
  if (!proof_valid) {
    goto cleanup;
  }
  bool certified = incumbent_idx >= 0;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx != incumbent_idx &&
        !out->cands[candidate_idx].eliminated) {
      certified = false;
      break;
    }
  }
  out->status = certified ? CPEG_PRE_CERTIFIED : CPEG_PRE_BOUNDED;
  out->best_index = incumbent_idx >= 0 ? incumbent_idx : candidate_order[0];
  out->unique_best = certified;
  out->count = candidate_count;
  out->regret_num = 0;
  out->regret_den = 1;
  const CpegWtlCertifiedCand *selected = &out->cands[out->best_index];
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidate_idx == out->best_index) {
      continue;
    }
    const CpegWtlCertifiedCand *other = &out->cands[candidate_idx];
    if (other->win_upper_num > 0 &&
        selected->outcome_den > INT64_MAX / other->win_upper_num) {
      proof_valid = false;
      break;
    }
    if (selected->win_lower_num > 0 &&
        other->outcome_den > INT64_MAX / selected->win_lower_num) {
      proof_valid = false;
      break;
    }
    const int64_t upper_scaled = other->win_upper_num * selected->outcome_den;
    const int64_t lower_scaled = selected->win_lower_num * other->outcome_den;
    if (upper_scaled <= lower_scaled) {
      continue;
    }
    if (other->outcome_den > INT64_MAX / selected->outcome_den) {
      proof_valid = false;
      break;
    }
    const int64_t regret_num = upper_scaled - lower_scaled;
    const int64_t regret_den = other->outcome_den * selected->outcome_den;
    if (cpeg_wtl_fraction_less(out->regret_num, out->regret_den, regret_num,
                               regret_den, &proof_valid)) {
      out->regret_num = regret_num;
      out->regret_den = regret_den;
    }
  }
  if (!proof_valid) {
    goto cleanup;
  }
  out->decision_regret_bound =
      (double)out->regret_num / (double)out->regret_den;
  result = candidate_count;

cleanup:
  if (collect_trace) {
    out->trace.finalize_ns = ctimer_monotonic_ns() - trace_stage_start_ns;
  }
  free(world_evaluations);
  free(candidates_refined);
  free(states);
  free(candidate_order);
  free(job_ptrs);
  free(jobs);
  peg_pool_destroy(pool);
  if (workers != NULL) {
    for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
      free(workers[worker_idx].root_reply_cache);
      small_move_list_destroy(workers[worker_idx].root_best_small);
      move_list_destroy(workers[worker_idx].root_best);
      small_move_list_destroy(workers[worker_idx].opponent_moves);
      free(workers[worker_idx].endgame_undo);
      move_list_destroy(workers[worker_idx].endgame_reply);
      move_list_destroy(workers[worker_idx].endgame_mover);
      free(workers[worker_idx].defense_undo);
      game_destroy(workers[worker_idx].defense_game);
      game_destroy(workers[worker_idx].draw_game);
      game_destroy(workers[worker_idx].opponent_game);
    }
  }
  free(workers);
  free(exact_endgame_cache);
  free(worlds);
  cpeg_root_collection_destroy(&root_collection);
  game_destroy(root_game);
  if (collect_trace) {
    out->trace.wall_ns = ctimer_monotonic_ns() - trace_start_ns;
  }
  if (result < 0) {
    cpeg_wtl_certified_result_destroy(out);
  }
  return result;
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

static bool cpeg_run_statistical_batch(
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
    job->value = 0.0;
    job->complete = false;
    job_ptrs[pair_idx] = job;
  }

  peg_pool_submit_and_wait(pool, cpeg_root_job_run, job_ptrs, pair_count,
                           helper_worker_idx);
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
  for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
    const int candidate_idx = pairs[pair_idx].candidate_idx;
    const int world_idx = pairs[pair_idx].world_idx;
    evaluations[candidate_idx * world_count + world_idx] = (CpegWorldEval){
        .value = {.lo = jobs[pair_idx].value, .hi = jobs[pair_idx].value},
        .resolved = true,
    };
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

  CpegRootCollection root_collection;
  if (!cpeg_collect_root_candidates(game, args->bag, args->allow_exchanges,
                                    &root_collection)) {
    return -1;
  }
  CpegRootCand *candidates = root_collection.candidates;
  const int candidate_count = root_collection.count;
  out->coverage = root_collection.coverage;

  CpegMultiset *enumerated_worlds =
      malloc_or_die(CPEG_WORLD_CAP * sizeof(*enumerated_worlds));
  bool world_overflow = false;
  const int world_count =
      cpeg_enum_submultisets(unseen, ld_size, args->bag, enumerated_worlds,
                             CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow || world_count < 1) {
    free(enumerated_worlds);
    free(candidates);
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
    out->cands = calloc_or_die((size_t)candidate_count, sizeof(*out->cands));
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
  return out->count > 0 ? out->count : -1;
}

static void cpeg_statistical_sample_order(const CpegScheduledWorld *worlds,
                                          int world_count, uint64_t seed,
                                          int *sample_order) {
  bool selected[CPEG_WORLD_CAP] = {false};
  int64_t remaining_weight = 0;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    remaining_weight += worlds[world_idx].multiset.weight;
  }

  XoshiroPRNG *prng = prng_create(seed);
  for (int sample_idx = 0; sample_idx < world_count; sample_idx++) {
    const int64_t draw =
        (int64_t)prng_get_random_number(prng, (uint64_t)remaining_weight);
    int64_t cumulative_weight = 0;
    // Positive remaining_weight guarantees that one unselected interval
    // contains draw. Initialize to a valid index so static analysis can see
    // that the subsequent array access is in bounds as well.
    int selected_world_idx = 0;
    for (int world_idx = 0; world_idx < world_count; world_idx++) {
      if (selected[world_idx]) {
        continue;
      }
      cumulative_weight += worlds[world_idx].multiset.weight;
      if (draw < cumulative_weight) {
        selected_world_idx = world_idx;
        break;
      }
    }
    sample_order[sample_idx] = selected_world_idx;
    selected[selected_world_idx] = true;
    remaining_weight -= worlds[selected_world_idx].multiset.weight;
  }
  prng_destroy(prng);
}

typedef struct CpegStatisticalSummary {
  double mean;
  double variance;
  double lower;
  double upper;
} CpegStatisticalSummary;

// Weighted empirical-Bernstein/Serfling interval used by statistical mode.
// rho=(N-n)/(N-1), and
//   h=sqrt(2*rho*s_w^2*log(3/delta)/n)
//       +3*rho*R_s*log(3/delta)/n.
// Here s_w^2 is the reliability-weighted sample variance and R_s is the
// observed sample range. A census is exact by construction. With only one
// observation there is no empirical scale, so the candidate's certified prior
// is retained until a second world resolves.
static CpegStatisticalSummary
cpeg_statistical_summary(const CpegWorldEval *candidate_evaluations,
                         const int *sample_order, int sample_count,
                         const int64_t *world_weights, int world_count,
                         CpegInterval prior, double delta) {
  CpegStatisticalSummary summary = {
      .mean = cpeg_interval_midpoint(prior),
      .variance = 0.0,
      .lower = prior.lo,
      .upper = prior.hi,
  };
  if (sample_count < 1) {
    return summary;
  }

  double weight_sum = 0.0;
  double squared_weight_sum = 0.0;
  double weighted_value_sum = 0.0;
  double sample_minimum = INFINITY;
  double sample_maximum = -INFINITY;
  for (int sample_idx = 0; sample_idx < sample_count; sample_idx++) {
    const int world_idx = sample_order[sample_idx];
    const double weight = (double)world_weights[world_idx];
    const double value =
        cpeg_interval_midpoint(candidate_evaluations[world_idx].value);
    weight_sum += weight;
    squared_weight_sum += weight * weight;
    weighted_value_sum += weight * value;
    if (value < sample_minimum) {
      sample_minimum = value;
    }
    if (value > sample_maximum) {
      sample_maximum = value;
    }
  }
  summary.mean = weighted_value_sum / weight_sum;
  if (sample_count == 1) {
    return summary;
  }

  double weighted_squared_deviation_sum = 0.0;
  for (int sample_idx = 0; sample_idx < sample_count; sample_idx++) {
    const int world_idx = sample_order[sample_idx];
    const double weight = (double)world_weights[world_idx];
    const double value =
        cpeg_interval_midpoint(candidate_evaluations[world_idx].value);
    const double deviation = value - summary.mean;
    weighted_squared_deviation_sum += weight * deviation * deviation;
  }
  const double variance_denominator =
      weight_sum - squared_weight_sum / weight_sum;
  if (variance_denominator > 0.0) {
    summary.variance = weighted_squared_deviation_sum / variance_denominator;
  }
  if (sample_count == world_count) {
    summary.lower = summary.mean;
    summary.upper = summary.mean;
    return summary;
  }

  const double rho =
      (double)(world_count - sample_count) / (double)(world_count - 1);
  const double log_term = log(3.0 / delta);
  const double sample_range = sample_maximum - sample_minimum;
  const double half_width =
      sqrt(2.0 * rho * summary.variance * log_term / (double)sample_count) +
      3.0 * rho * sample_range * log_term / (double)sample_count;
  summary.lower = summary.mean - half_width;
  summary.upper = summary.mean + half_width;
  return summary;
}

int cpeg_statistical_resample_values(const double *values,
                                     const int64_t *weights, int world_count,
                                     int sample_count, uint64_t seed,
                                     double confidence,
                                     CpegStatisticalCand *out) {
  if (values == NULL || weights == NULL || out == NULL || world_count < 1 ||
      world_count > CPEG_WORLD_CAP || sample_count < 1 ||
      sample_count > world_count || !isfinite(confidence) ||
      confidence <= 0.0 || confidence >= 1.0) {
    return -1;
  }
  CpegScheduledWorld worlds[CPEG_WORLD_CAP] = {0};
  CpegWorldEval evaluations[CPEG_WORLD_CAP] = {0};
  double population_minimum = INFINITY;
  double population_maximum = -INFINITY;
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    if (weights[world_idx] <= 0 || !isfinite(values[world_idx])) {
      return -1;
    }
    worlds[world_idx].multiset.weight = weights[world_idx];
    worlds[world_idx].generation_index = world_idx;
    evaluations[world_idx] = (CpegWorldEval){
        .value = {.lo = values[world_idx], .hi = values[world_idx]},
        .resolved = true,
    };
    if (values[world_idx] < population_minimum) {
      population_minimum = values[world_idx];
    }
    if (values[world_idx] > population_maximum) {
      population_maximum = values[world_idx];
    }
  }
  int sample_order[CPEG_WORLD_CAP];
  cpeg_statistical_sample_order(worlds, world_count, seed, sample_order);
  const CpegStatisticalSummary summary = cpeg_statistical_summary(
      evaluations, sample_order, sample_count, weights, world_count,
      (CpegInterval){.lo = population_minimum, .hi = population_maximum},
      1.0 - confidence);
  memset(out, 0, sizeof(*out));
  out->estimate = summary.mean;
  out->lower = summary.lower;
  out->upper = summary.upper;
  out->sample_variance = summary.variance;
  out->worlds_sampled = sample_count;
  return 1;
}

static CpegStatisticalSummary cpeg_statistical_gap_summary(
    const CpegWorldEval *evaluations, int world_count, int candidate_idx,
    int leader_idx, const int *sample_order, int sample_count,
    const int64_t *world_weights, CpegInterval candidate_prior,
    CpegInterval leader_prior, double delta) {
  CpegWorldEval gap_evaluations[CPEG_WORLD_CAP] = {0};
  for (int sample_idx = 0; sample_idx < sample_count; sample_idx++) {
    const int world_idx = sample_order[sample_idx];
    const CpegInterval candidate_value =
        evaluations[candidate_idx * world_count + world_idx].value;
    const CpegInterval leader_value =
        evaluations[leader_idx * world_count + world_idx].value;
    const double gap = cpeg_interval_midpoint(candidate_value) -
                       cpeg_interval_midpoint(leader_value);
    gap_evaluations[world_idx] =
        (CpegWorldEval){.value = {.lo = gap, .hi = gap}, .resolved = true};
  }
  const CpegInterval gap_prior = {
      .lo = candidate_prior.lo - leader_prior.hi,
      .hi = candidate_prior.hi - leader_prior.lo,
  };
  return cpeg_statistical_summary(gap_evaluations, sample_order, sample_count,
                                  world_weights, world_count, gap_prior, delta);
}

static int cpeg_statistical_best(const CpegCandState *states,
                                 const CpegStatisticalSummary *summaries,
                                 int candidate_count, bool include_eliminated) {
  int best_idx = -1;
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (!include_eliminated && states[candidate_idx].eliminated) {
      continue;
    }
    if (best_idx < 0 ||
        summaries[candidate_idx].mean > summaries[best_idx].mean ||
        (summaries[candidate_idx].mean == summaries[best_idx].mean &&
         cpeg_rank_precedes(&states[candidate_idx], &states[best_idx]))) {
      best_idx = candidate_idx;
    }
  }
  return best_idx;
}

int cpeg_solve_pre_endgame_statistical(Game *game,
                                       const CpegStatisticalArgs *args,
                                       CpegStatisticalResult *out) {
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->best_index = -1;
  if (game == NULL || args == NULL || args->bag < 1 ||
      args->bag > PEG_MAX_BAG || !isfinite(args->budget_seconds) ||
      args->budget_seconds < 0.0 || !isfinite(args->confidence) ||
      args->confidence <= 0.0 || args->confidence >= 1.0 ||
      args->max_worlds < 0) {
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

  CpegRootCollection root_collection;
  if (!cpeg_collect_root_candidates(game, args->bag, args->allow_exchanges,
                                    &root_collection)) {
    return -1;
  }
  CpegRootCand *candidates = root_collection.candidates;
  const int candidate_count = root_collection.count;
  out->coverage = root_collection.coverage;
  out->cands = calloc_or_die((size_t)candidate_count, sizeof(*out->cands));

  CpegMultiset *enumerated_worlds =
      malloc_or_die(CPEG_WORLD_CAP * sizeof(*enumerated_worlds));
  bool world_overflow = false;
  const int world_count =
      cpeg_enum_submultisets(unseen, ld_size, args->bag, enumerated_worlds,
                             CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow || world_count < 1) {
    free(enumerated_worlds);
    free(candidates);
    cpeg_statistical_result_destroy(out);
    return -1;
  }
  CpegScheduledWorld *worlds =
      malloc_or_die((size_t)world_count * sizeof(*worlds));
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    worlds[world_idx] = (CpegScheduledWorld){
        .multiset = enumerated_worlds[world_idx],
        .generation_index = world_idx,
    };
  }
  free(enumerated_worlds);
  cpeg_sort_scheduled_worlds(worlds, world_count);

  Game **templates = calloc_or_die((size_t)candidate_count, sizeof(*templates));
  CpegCandState *states =
      malloc_or_die((size_t)candidate_count * sizeof(*states));
  int *stable_order =
      malloc_or_die((size_t)candidate_count * sizeof(*stable_order));
  const int root_score_bound = cpeg_score_upper_bound(board, game);
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    if (candidates[candidate_idx].kind == 0) {
      templates[candidate_idx] =
          cpeg_build_root_template(game, &candidates[candidate_idx].move);
    }
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
  int *sample_order =
      malloc_or_die((size_t)world_count * sizeof(*sample_order));
  for (int world_idx = 0; world_idx < world_count; world_idx++) {
    world_weights[world_idx] = worlds[world_idx].multiset.weight;
  }
  cpeg_statistical_sample_order(worlds, world_count, args->seed, sample_order);
  CpegWorldEval *evaluations = calloc_or_die(
      (size_t)candidate_count * (size_t)world_count, sizeof(*evaluations));
  CpegStatisticalSummary *summaries =
      malloc_or_die((size_t)candidate_count * sizeof(*summaries));
  int *candidate_sample_counts =
      calloc_or_die((size_t)candidate_count, sizeof(*candidate_sample_counts));

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

  CpegScheduledPair *pairs =
      malloc_or_die((size_t)candidate_count * sizeof(*pairs));
  CpegRootJob *jobs = malloc_or_die((size_t)candidate_count * sizeof(*jobs));
  void **job_ptrs = malloc_or_die((size_t)candidate_count * sizeof(*job_ptrs));
  const int helper_worker_idx = pool != NULL ? thread_count : 0;
  bool search_ok = true;
  const int sample_limit =
      args->max_worlds > 0 && args->max_worlds < world_count ? args->max_worlds
                                                             : world_count;

  for (int sample_idx = 0; sample_idx < sample_limit; sample_idx++) {
    if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
      break;
    }
    int pair_count = 0;
    for (int order_idx = 0; order_idx < candidate_count; order_idx++) {
      const int candidate_idx = stable_order[order_idx];
      if (!states[candidate_idx].eliminated) {
        pairs[pair_count++] = (CpegScheduledPair){
            .candidate_idx = candidate_idx,
            .world_idx = sample_order[sample_idx],
        };
      }
    }
    bool round_complete = false;
    search_ok = cpeg_run_statistical_batch(
        pool, workers, helper_worker_idx, jobs, job_ptrs, pairs, pair_count,
        candidates, templates, game, worlds, unseen, ld_size, opp_idx,
        scratch_count, evaluations, world_count, &round_complete);
    if (!search_ok || !round_complete) {
      break;
    }
    out->jobs_completed += pair_count;
    out->sampled_world_indices[out->worlds_sampled] = sample_order[sample_idx];
    out->sampled_world_weights[out->worlds_sampled] =
        world_weights[sample_order[sample_idx]];
    for (int pair_idx = 0; pair_idx < pair_count; pair_idx++) {
      candidate_sample_counts[pairs[pair_idx].candidate_idx]++;
    }
    out->rounds_completed++;
    out->worlds_sampled++;

    const double failure_probability = 1.0 - args->confidence;
    const double round = (double)out->rounds_completed;
    const double round_delta =
        6.0 * failure_probability / (9.86960440108935861883 * round * round);
    const double interval_delta = round_delta / (2.0 * candidate_count);
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      summaries[candidate_idx] = cpeg_statistical_summary(
          &evaluations[candidate_idx * world_count], sample_order,
          candidate_sample_counts[candidate_idx], world_weights, world_count,
          states[candidate_idx].prior, interval_delta);
    }
    const int leader_idx = cpeg_statistical_best(
        states, summaries, candidate_count, /*include_eliminated=*/false);
    if (out->worlds_sampled < world_count && out->worlds_sampled >= 2) {
      for (int candidate_idx = 0; candidate_idx < candidate_count;
           candidate_idx++) {
        if (candidate_idx == leader_idx || states[candidate_idx].eliminated) {
          continue;
        }
        const CpegStatisticalSummary gap = cpeg_statistical_gap_summary(
            evaluations, world_count, candidate_idx, leader_idx, sample_order,
            out->worlds_sampled, world_weights, states[candidate_idx].prior,
            states[leader_idx].prior, interval_delta);
        if (gap.upper < 0.0) {
          states[candidate_idx].eliminated = true;
          out->cands[candidate_idx].elimination_round = out->rounds_completed;
        }
      }
    }
  }

  if (search_ok && out->worlds_sampled > 0) {
    const double failure_probability = 1.0 - args->confidence;
    const double round = (double)out->rounds_completed;
    const double round_delta =
        6.0 * failure_probability / (9.86960440108935861883 * round * round);
    const double interval_delta = round_delta / (2.0 * candidate_count);
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      summaries[candidate_idx] = cpeg_statistical_summary(
          &evaluations[candidate_idx * world_count], sample_order,
          candidate_sample_counts[candidate_idx], world_weights, world_count,
          states[candidate_idx].prior, interval_delta);
    }
    out->status = CPEG_PRE_STATISTICAL;
    out->count = candidate_count;
    out->best_index = cpeg_statistical_best(states, summaries, candidate_count,
                                            /*include_eliminated=*/false);
    out->worlds_total = world_count;
    out->confidence = args->confidence;
    out->seed = args->seed;
    for (int candidate_idx = 0; candidate_idx < candidate_count;
         candidate_idx++) {
      CpegStatisticalCand *result_candidate = &out->cands[candidate_idx];
      memcpy(result_candidate->label, states[candidate_idx].rank.label,
             sizeof(result_candidate->label));
      result_candidate->score = candidates[candidate_idx].score;
      result_candidate->estimate = summaries[candidate_idx].mean;
      result_candidate->lower = summaries[candidate_idx].lower;
      result_candidate->upper = summaries[candidate_idx].upper;
      result_candidate->sample_variance = summaries[candidate_idx].variance;
      result_candidate->worlds_sampled = candidate_sample_counts[candidate_idx];
      result_candidate->eliminated = states[candidate_idx].eliminated;
    }
  }

  free(job_ptrs);
  free(jobs);
  free(pairs);
  peg_pool_destroy(pool);
  for (int worker_idx = 0; worker_idx < scratch_count; worker_idx++) {
    game_destroy(workers[worker_idx].world_game);
    cpeg_ctx_destroy(&workers[worker_idx].ctx);
  }
  free(workers);
  free(candidate_sample_counts);
  free(summaries);
  free(evaluations);
  free(sample_order);
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
  if (out->count == 0) {
    cpeg_statistical_result_destroy(out);
  }
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

  CpegRootCollection root_collection;
  if (!cpeg_collect_root_candidates(game, bag, allow_exchanges,
                                    &root_collection)) {
    return -1;
  }
  CpegRootCand *cands = root_collection.candidates;
  const int candidate_count = root_collection.count;

  CpegMultiset *worlds = malloc_or_die(CPEG_WORLD_CAP * sizeof(*worlds));
  bool world_overflow = false;
  const int world_count = cpeg_enum_submultisets(
      unseen, ld_size, bag, worlds, CPEG_WORLD_CAP, &world_overflow);
  if (world_overflow) {
    free(worlds);
    free(cands);
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
  return capacity_exceeded ? -1 : stats->pair_count;
}
