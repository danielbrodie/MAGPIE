#include "cpeg.h"

#include "../def/equity_defs.h"
#include "../def/move_defs.h"
#include "../ent/board.h"
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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Capacity for the exhaustive move lists. A single 7-tile endgame rack can
  // enumerate a few thousand plays at most; this cap has generous headroom so
  // MOVE_RECORD_ALL never silently drops a play.
  CPEG_MOVE_LIST_CAP = 16384,
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

// Scans a fully-enumerated move list for the highest raw-score move, returning
// it via *best_out. Ties are broken deterministically by
// compare_moves_without_equity so the same reply is chosen every run.
static int cpeg_best_score_move(const MoveList *move_list, Move *best_out) {
  Equity best_score = EQUITY_MIN_VALUE;
  const int count = move_list_get_count(move_list);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *candidate = move_list_get_move(move_list, move_idx);
    const Equity candidate_score = move_get_score(candidate);
    bool take = false;
    if (candidate_score > best_score) {
      take = true;
    } else if (candidate_score == best_score &&
               compare_moves_without_equity(candidate, best_out,
                                            /*allow_duplicates=*/true) == 1) {
      take = true;
    }
    if (take) {
      best_score = candidate_score;
      move_copy(best_out, candidate);
    }
  }
  return equity_to_int(best_score);
}

int cpeg_solve_endgame(Game *game, CpegResult *result) {
  memset(result, 0, sizeof(*result));

  const LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);

  // Cross-sets must be valid before enumerating the mover's plays.
  if (!board_get_cross_sets_valid(board)) {
    game_gen_all_cross_sets(game);
    board_set_cross_sets_valid(board, true);
  }

  MoveList *mover_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveList *reply_moves = move_list_create(CPEG_MOVE_LIST_CAP);
  MoveUndo *undo = malloc_or_die(sizeof(MoveUndo));

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

  int best_swing = 0;
  int best_mover_score = 0;
  bool have_best = false;

  const int mover_count = move_list_get_count(mover_moves);
  for (int mover_idx = 0; mover_idx < mover_count; mover_idx++) {
    const Move *mover_move = move_list_get_move(mover_moves, mover_idx);
    const int mover_score = equity_to_int(move_get_score(mover_move));

    // Apply the mover's play. play_move_incremental switches the turn to the
    // opponent and (harmlessly, for us) may add the go-out bonus to the mover's
    // Game score; we never read that score -- the swing is computed purely from
    // move scores below.
    play_move_incremental(mover_move, game, undo);

    // The opponent is now on turn; enumerate their plays and take the best by
    // raw score (a leave is worth nothing on the final turn). A pass (score 0)
    // is always among them.
    const MoveGenArgs reply_args = {
        .game = game,
        .move_list = reply_moves,
        .move_record_type = MOVE_RECORD_ALL,
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

    Move reply_move;
    const int reply_score = cpeg_best_score_move(reply_moves, &reply_move);
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
      move_copy(&result->best_reply, &reply_move);
      result->mover_score = mover_score;
      result->reply_score = reply_score;
      result->swing = swing;
      result->has_reply = opponent_can_reply;
      // Both renders happen while the mover's play is on the board: the mover
      // move's own placed tiles come from the move (not the board), and every
      // played-through marker in either move resolves to a tile that is present
      // in this post-mover-move board -- so both strings render correctly here.
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
    // No mover play at all (mover rack empty): value is zero, no moves.
    result->mover_str[0] = '-';
    result->mover_str[1] = '\0';
    result->reply_str[0] = '-';
    result->reply_str[1] = '\0';
  }

  free(undo);
  move_list_destroy(reply_moves);
  move_list_destroy(mover_moves);
  return result->swing;
}
