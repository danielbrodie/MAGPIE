#include "crossplay_oracle.h"

#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/peg_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include <stdlib.h>
#include <string.h>

typedef struct CrossplayOracleExchange {
  MachineLetter tiles[RACK_SIZE];
  int count;
} CrossplayOracleExchange;

typedef struct CrossplayOracleExchangeCollector {
  CrossplayOracleExchange *exchanges;
  int count;
  bool overflow;
} CrossplayOracleExchangeCollector;

static bool crossplay_oracle_add_human_letter(
    StringBuilder *builder, const LetterDistribution *ld, MachineLetter ml) {
  const MachineLetter unblanked = get_unblanked_machine_letter(ml);
  if ((int)unblanked >= ld_get_size(ld)) {
    return false;
  }
  const char *letter = ld->ld_ml_to_hl[unblanked];
  if (letter[0] == '\0' || letter[1] != '\0') {
    return false;
  }
  string_builder_add_string(builder, letter);
  return true;
}

static bool crossplay_oracle_add_word(StringBuilder *builder,
                                      const CrossplayOracleAction *action,
                                      const Board *board,
                                      const LetterDistribution *ld) {
  const Move *move = &action->move;
  const int row_increment =
      move_get_dir(move) == BOARD_VERTICAL_DIRECTION ? 1 : 0;
  const int col_increment =
      move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      const int row = move_get_row_start(move) + row_increment * tile_idx;
      const int col = move_get_col_start(move) + col_increment * tile_idx;
      ml = board_get_letter(board, row, col);
    }
    if (!crossplay_oracle_add_human_letter(builder, ld, ml)) {
      return false;
    }
  }
  return true;
}

static bool crossplay_oracle_add_new_tiles(
    StringBuilder *builder, const CrossplayOracleAction *action,
    const LetterDistribution *ld) {
  const Move *move = &action->move;
  const int row_increment =
      move_get_dir(move) == BOARD_VERTICAL_DIRECTION ? 1 : 0;
  const int col_increment =
      move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  bool first = true;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (!first) {
      string_builder_add_string(builder, ",");
    }
    first = false;
    const int row = move_get_row_start(move) + row_increment * tile_idx;
    const int col = move_get_col_start(move) + col_increment * tile_idx;
    string_builder_add_formatted_string(
        builder, "{\"blank\":%s,\"col\":%d,\"letter\":\"",
        get_is_blanked(ml) ? "true" : "false", col);
    if (!crossplay_oracle_add_human_letter(builder, ld, ml)) {
      return false;
    }
    string_builder_add_formatted_string(builder, "\",\"row\":%d}", row);
  }
  return !first;
}

static char *crossplay_oracle_placement_json(
    const CrossplayOracleAction *action, const Board *board,
    const LetterDistribution *ld) {
  StringBuilder *builder = string_builder_create();
  string_builder_add_string(builder,
                            "{\"kind\":\"placement\",\"new_tiles\":[");
  if (!crossplay_oracle_add_new_tiles(builder, action, ld)) {
    string_builder_destroy(builder);
    return NULL;
  }
  string_builder_add_formatted_string(
      builder,
      "],\"orientation\":\"%s\",\"start_col\":%d,\"start_row\":%d,"
      "\"word\":\"",
      move_get_dir(&action->move) == BOARD_HORIZONTAL_DIRECTION ? "horizontal"
                                                               : "vertical",
      move_get_col_start(&action->move), move_get_row_start(&action->move));
  if (!crossplay_oracle_add_word(builder, action, board, ld)) {
    string_builder_destroy(builder);
    return NULL;
  }
  string_builder_add_string(builder, "\"}");
  char *json = string_builder_dump(builder, NULL);
  string_builder_destroy(builder);
  return json;
}

static char *crossplay_oracle_exchange_json(
    const CrossplayOracleAction *action, const LetterDistribution *ld) {
  StringBuilder *builder = string_builder_create();
  string_builder_add_string(builder, "{\"kind\":\"exchange\",\"tiles\":\"");
  for (int tile_idx = 0; tile_idx < action->exchange_count; tile_idx++) {
    if (!crossplay_oracle_add_human_letter(
            builder, ld, action->exchange_tiles[tile_idx])) {
      string_builder_destroy(builder);
      return NULL;
    }
  }
  string_builder_add_string(builder, "\"}");
  char *json = string_builder_dump(builder, NULL);
  string_builder_destroy(builder);
  return json;
}

static char *crossplay_oracle_action_json(
    const CrossplayOracleAction *action, const Board *board,
    const LetterDistribution *ld) {
  if (action->kind == CROSSPLAY_ORACLE_PLACEMENT) {
    return crossplay_oracle_placement_json(action, board, ld);
  }
  if (action->kind == CROSSPLAY_ORACLE_EXCHANGE) {
    return crossplay_oracle_exchange_json(action, ld);
  }
  return string_duplicate("{\"kind\":\"pass\"}");
}

static void crossplay_oracle_action_id(CrossplayOracleAction *action) {
  static const char ACTION_DOMAIN[] = "crossplay-action-v1";
  Sha256 sha;
  sha256_init(&sha);
  sha256_update(&sha, ACTION_DOMAIN, sizeof(ACTION_DOMAIN));
  sha256_update(&sha, action->canonical_json,
                string_length(action->canonical_json));
  sha256_final_hex(&sha, action->id);
}

static void crossplay_oracle_exchange_rec(
    const int *counts, int ld_size, int start_ml, int tiles_left,
    MachineLetter *chosen, int chosen_count,
    CrossplayOracleExchangeCollector *collector) {
  if (collector->overflow) {
    return;
  }
  if (tiles_left == 0) {
    if (collector->count >= CROSSPLAY_ORACLE_EXCHANGE_CAPACITY) {
      collector->overflow = true;
      return;
    }
    CrossplayOracleExchange *exchange =
        &collector->exchanges[collector->count++];
    exchange->count = chosen_count;
    memcpy(exchange->tiles, chosen,
           (size_t)chosen_count * sizeof(*chosen));
    return;
  }
  for (int ml = start_ml; ml < ld_size; ml++) {
    const int available = counts[ml];
    const int maximum_take =
        available < tiles_left ? available : tiles_left;
    for (int take = 1; take <= maximum_take; take++) {
      for (int tile_idx = 0; tile_idx < take; tile_idx++) {
        chosen[chosen_count + tile_idx] = (MachineLetter)ml;
      }
      crossplay_oracle_exchange_rec(
          counts, ld_size, ml + 1, tiles_left - take, chosen,
          chosen_count + take, collector);
    }
  }
}

static int crossplay_oracle_collect_exchanges(
    const Rack *rack, int bag_count, int ld_size,
    CrossplayOracleExchange exchanges[CROSSPLAY_ORACLE_EXCHANGE_CAPACITY],
    bool *overflow) {
  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    counts[ml] = rack_get_letter(rack, (MachineLetter)ml);
  }
  CrossplayOracleExchangeCollector collector = {
      .exchanges = exchanges,
  };
  MachineLetter chosen[RACK_SIZE];
  const int rack_count = rack_get_total_letters(rack);
  const int maximum_exchange =
      rack_count < bag_count ? rack_count : bag_count;
  for (int exchange_count = 1; exchange_count <= maximum_exchange;
       exchange_count++) {
    crossplay_oracle_exchange_rec(counts, ld_size, 0, exchange_count, chosen,
                                  0, &collector);
  }
  *overflow = collector.overflow;
  return collector.count;
}

static int crossplay_oracle_action_compare(const void *lhs, const void *rhs) {
  const CrossplayOracleAction *lhs_action =
      (const CrossplayOracleAction *)lhs;
  const CrossplayOracleAction *rhs_action =
      (const CrossplayOracleAction *)rhs;
  return strcmp(lhs_action->id, rhs_action->id);
}

static void crossplay_oracle_action_space_digest(
    CrossplayOracleActionSet *result) {
  static const char ACTION_SPACE_DOMAIN[] = "crossplay-action-space-v1";
  Sha256 sha;
  sha256_init(&sha);
  sha256_update(&sha, ACTION_SPACE_DOMAIN, sizeof(ACTION_SPACE_DOMAIN));
  for (int action_idx = 0; action_idx < result->count; action_idx++) {
    sha256_update(&sha, result->actions[action_idx].id,
                  string_length(result->actions[action_idx].id));
    sha256_update(&sha, "\n", 1);
  }
  sha256_final_hex(&sha, result->digest);
}

CrossplayOracleStatus crossplay_oracle_generate_actions(
    const Game *game, int bag_count, bool allow_exchanges,
    CrossplayOracleActionSet *result) {
  if (result == NULL) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  memset(result, 0, sizeof(*result));
  if (game == NULL || bag_count < 0 || bag_count > PEG_MAX_BAG) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }

  Game *working_game = game_duplicate(game);
  Board *board = game_get_board(working_game);
  game_gen_all_cross_sets(working_game);
  board_set_cross_sets_valid(board, true);
  MoveList *moves =
      move_list_create(CROSSPLAY_ORACLE_ACTION_CAPACITY + 1);
  const MoveGenArgs move_args = {
      .game = working_game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&move_args);
  if (move_list_get_count(moves) > CROSSPLAY_ORACLE_ACTION_CAPACITY) {
    move_list_destroy(moves);
    game_destroy(working_game);
    return CROSSPLAY_ORACLE_CAPACITY_EXCEEDED;
  }

  int placement_count = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    if (move_get_type(move_list_get_move(moves, move_idx)) ==
        GAME_EVENT_TILE_PLACEMENT_MOVE) {
      placement_count++;
    }
  }

  CrossplayOracleExchange
      exchanges[CROSSPLAY_ORACLE_EXCHANGE_CAPACITY] = {0};
  int exchange_count = 0;
  if (allow_exchanges && bag_count > 0) {
    const int actor = game_get_player_on_turn_index(working_game);
    const Rack *rack =
        player_get_rack(game_get_player(working_game, actor));
    bool exchange_overflow = false;
    exchange_count = crossplay_oracle_collect_exchanges(
        rack, bag_count, ld_get_size(game_get_ld(working_game)), exchanges,
        &exchange_overflow);
    if (exchange_overflow) {
      move_list_destroy(moves);
      game_destroy(working_game);
      return CROSSPLAY_ORACLE_CAPACITY_EXCEEDED;
    }
  }

  const int total = placement_count + exchange_count + 1;
  if (total > CROSSPLAY_ORACLE_ACTION_CAPACITY) {
    move_list_destroy(moves);
    game_destroy(working_game);
    return CROSSPLAY_ORACLE_CAPACITY_EXCEEDED;
  }
  result->actions = calloc_or_die((size_t)total, sizeof(*result->actions));
  int action_count = 0;
  for (int move_idx = 0; move_idx < move_list_get_count(moves); move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    CrossplayOracleAction *action = &result->actions[action_count++];
    action->kind = CROSSPLAY_ORACLE_PLACEMENT;
    move_copy(&action->move, move);
    action->score = equity_to_int(move_get_score(move));
  }
  for (int exchange_idx = 0; exchange_idx < exchange_count; exchange_idx++) {
    CrossplayOracleAction *action = &result->actions[action_count++];
    action->kind = CROSSPLAY_ORACLE_EXCHANGE;
    action->exchange_count = exchanges[exchange_idx].count;
    memcpy(action->exchange_tiles, exchanges[exchange_idx].tiles,
           (size_t)action->exchange_count * sizeof(*action->exchange_tiles));
  }
  result->actions[action_count++].kind = CROSSPLAY_ORACLE_PASS;

  const LetterDistribution *ld = game_get_ld(working_game);
  for (int action_idx = 0; action_idx < action_count; action_idx++) {
    CrossplayOracleAction *action = &result->actions[action_idx];
    action->canonical_json = crossplay_oracle_action_json(action, board, ld);
    if (action->canonical_json == NULL) {
      result->count = action_count;
      crossplay_oracle_action_set_destroy(result);
      move_list_destroy(moves);
      game_destroy(working_game);
      return CROSSPLAY_ORACLE_ENCODING_ERROR;
    }
    crossplay_oracle_action_id(action);
  }
  result->count = action_count;
  qsort(result->actions, (size_t)result->count, sizeof(*result->actions),
        crossplay_oracle_action_compare);
  result->coverage = (CrossplayOracleCoverage){
      .placements = placement_count,
      .exchanges = exchange_count,
      .passes = 1,
      .total = total,
      .complete = true,
  };
  crossplay_oracle_action_space_digest(result);
  move_list_destroy(moves);
  game_destroy(working_game);
  return CROSSPLAY_ORACLE_OK;
}

void crossplay_oracle_action_set_destroy(CrossplayOracleActionSet *result) {
  if (result == NULL) {
    return;
  }
  for (int action_idx = 0; action_idx < result->count; action_idx++) {
    free(result->actions[action_idx].canonical_json);
  }
  free(result->actions);
  memset(result, 0, sizeof(*result));
}

const char *crossplay_oracle_status_name(CrossplayOracleStatus status) {
  switch (status) {
  case CROSSPLAY_ORACLE_OK:
    return "OK";
  case CROSSPLAY_ORACLE_INVALID_INPUT:
    return "INVALID_INPUT";
  case CROSSPLAY_ORACLE_CAPACITY_EXCEEDED:
    return "CAPACITY_EXCEEDED";
  case CROSSPLAY_ORACLE_ENCODING_ERROR:
    return "ENCODING_ERROR";
  }
  return "UNKNOWN";
}
