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
#include "gameplay.h"
#include "move_gen.h"
#include "peg_combinatorics.h"
#include <ctype.h>
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

static bool crossplay_oracle_is_sha256(const char *value) {
  if (value == NULL || strlen(value) != SHA256_HEX_SIZE - 1) {
    return false;
  }
  for (int char_idx = 0; char_idx < SHA256_HEX_SIZE - 1; char_idx++) {
    const unsigned char character = (unsigned char)value[char_idx];
    if (!isdigit(character) && (character < 'a' || character > 'f')) {
      return false;
    }
  }
  return true;
}

static int crossplay_oracle_digest_compare(const void *lhs, const void *rhs) {
  return strcmp((const char *)lhs, (const char *)rhs);
}

CrossplayOracleStatus crossplay_oracle_bounded_sequence_commitment(
    const CrossplayOracleActionSet *actions, const char *selected_action_id,
    int actor, const char *parent_sequence_digest,
    const char *information_key_digest,
    char commitment[SHA256_HEX_SIZE]) {
  static const char SEQUENCE_DOMAIN[] = "crossplay-player-sequence-edge-v1";
  static const char COMMITMENT_DOMAIN[] =
      "crossplay-bounded-sequence-commitment-v1";
  static const char ACTION_PREFIX[] = "{\"action_id\":\"";
  static const char ACTOR_PREFIX[] = "\",\"actor\":";
  static const char INFORMATION_PREFIX[] =
      ",\"information_key_digest\":\"";
  static const char PARENT_PREFIX[] = "\",\"parent_digest\":\"";
  static const char OBJECT_SUFFIX[] = "\"}";
  static const char COMMITMENT_PREFIX[] =
      "{\"child_sequence_digests\":[";
  static const char COMMITMENT_SEPARATOR[] = ",";
  static const char COMMITMENT_QUOTE[] = "\"";
  static const char COMMITMENT_SUFFIX[] = "]}";
  if (actions == NULL || commitment == NULL || actions->count < 1 ||
      !actions->coverage.complete ||
      actions->coverage.total != actions->count ||
      (actor != 0 && actor != 1) ||
      !crossplay_oracle_is_sha256(selected_action_id) ||
      !crossplay_oracle_is_sha256(parent_sequence_digest) ||
      !crossplay_oracle_is_sha256(information_key_digest)) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  bool found_selected = false;
  char(*child_digests)[SHA256_HEX_SIZE] = NULL;
  if (actions->count > 1) {
    child_digests =
        calloc_or_die((size_t)(actions->count - 1), sizeof(*child_digests));
  }
  int child_count = 0;
  for (int action_idx = 0; action_idx < actions->count; action_idx++) {
    const char *action_id = actions->actions[action_idx].id;
    if (strings_equal(action_id, selected_action_id)) {
      found_selected = true;
      continue;
    }
    if (child_count >= actions->count - 1) {
      free(child_digests);
      return CROSSPLAY_ORACLE_INVALID_INPUT;
    }
    Sha256 sha;
    sha256_init(&sha);
    sha256_update(&sha, SEQUENCE_DOMAIN, sizeof(SEQUENCE_DOMAIN));
    sha256_update(&sha, ACTION_PREFIX, sizeof(ACTION_PREFIX) - 1);
    sha256_update(&sha, action_id, SHA256_HEX_SIZE - 1);
    sha256_update(&sha, ACTOR_PREFIX, sizeof(ACTOR_PREFIX) - 1);
    const char actor_character = (char)('0' + actor);
    sha256_update(&sha, &actor_character, 1);
    sha256_update(&sha, INFORMATION_PREFIX, sizeof(INFORMATION_PREFIX) - 1);
    sha256_update(&sha, information_key_digest, SHA256_HEX_SIZE - 1);
    sha256_update(&sha, PARENT_PREFIX, sizeof(PARENT_PREFIX) - 1);
    sha256_update(&sha, parent_sequence_digest, SHA256_HEX_SIZE - 1);
    sha256_update(&sha, OBJECT_SUFFIX, sizeof(OBJECT_SUFFIX) - 1);
    sha256_final_hex(&sha, child_digests[child_count++]);
  }
  if (!found_selected || child_count != actions->count - 1) {
    free(child_digests);
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  qsort(child_digests, (size_t)child_count, sizeof(*child_digests),
        crossplay_oracle_digest_compare);
  Sha256 sha;
  sha256_init(&sha);
  sha256_update(&sha, COMMITMENT_DOMAIN, sizeof(COMMITMENT_DOMAIN));
  sha256_update(&sha, COMMITMENT_PREFIX, sizeof(COMMITMENT_PREFIX) - 1);
  for (int child_idx = 0; child_idx < child_count; child_idx++) {
    if (child_idx > 0) {
      sha256_update(&sha, COMMITMENT_SEPARATOR,
                    sizeof(COMMITMENT_SEPARATOR) - 1);
    }
    sha256_update(&sha, COMMITMENT_QUOTE, sizeof(COMMITMENT_QUOTE) - 1);
    sha256_update(&sha, child_digests[child_idx], SHA256_HEX_SIZE - 1);
    sha256_update(&sha, COMMITMENT_QUOTE, sizeof(COMMITMENT_QUOTE) - 1);
  }
  sha256_update(&sha, COMMITMENT_SUFFIX, sizeof(COMMITMENT_SUFFIX) - 1);
  sha256_final_hex(&sha, commitment);
  free(child_digests);
  return CROSSPLAY_ORACLE_OK;
}

typedef struct CrossplayOracleDrawCollector {
  CrossplayOracleDrawSet *result;
  bool overflow;
} CrossplayOracleDrawCollector;

static void crossplay_oracle_draw_rec(
    const int *counts, int ld_size, int start_ml, int tiles_left,
    int64_t weight, MachineLetter *chosen, int chosen_count,
    CrossplayOracleDrawCollector *collector) {
  if (collector->overflow) {
    return;
  }
  if (tiles_left == 0) {
    if (collector->result->count >= CROSSPLAY_ORACLE_DRAW_CAPACITY) {
      collector->overflow = true;
      return;
    }
    CrossplayOracleDraw *draw =
        &collector->result->draws[collector->result->count++];
    memcpy(draw->tiles, chosen, (size_t)chosen_count * sizeof(*chosen));
    draw->count = chosen_count;
    draw->weight = weight;
    collector->result->weight_mass += weight;
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
      crossplay_oracle_draw_rec(
          counts, ld_size, ml + 1, tiles_left - take,
          weight * peg_binomial(available, take), chosen, chosen_count + take,
          collector);
    }
  }
}

CrossplayOracleStatus crossplay_oracle_enumerate_draws(
    const Bag *bag, int draw_count, int ld_size,
    CrossplayOracleDrawSet *result) {
  if (result == NULL) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  memset(result, 0, sizeof(*result));
  if (bag == NULL || draw_count < 0 || draw_count > RACK_SIZE || ld_size < 1 ||
      ld_size > MAX_ALPHABET_SIZE || draw_count > bag_get_letters(bag)) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  int counts[MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    counts[ml] = bag_get_letter(bag, (MachineLetter)ml);
  }
  MachineLetter chosen[RACK_SIZE];
  CrossplayOracleDrawCollector collector = {.result = result};
  crossplay_oracle_draw_rec(counts, ld_size, 0, draw_count, 1, chosen, 0,
                            &collector);
  if (collector.overflow) {
    memset(result, 0, sizeof(*result));
    return CROSSPLAY_ORACLE_CAPACITY_EXCEEDED;
  }
  result->complete = true;
  return CROSSPLAY_ORACLE_OK;
}

static bool crossplay_oracle_exchange_available(
    const Game *game, const CrossplayOracleAction *action) {
  const int actor = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, actor));
  int needed[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < action->exchange_count; tile_idx++) {
    needed[action->exchange_tiles[tile_idx]]++;
  }
  for (int ml = 0; ml < ld_get_size(game_get_ld(game)); ml++) {
    if (needed[ml] > rack_get_letter(rack, (MachineLetter)ml)) {
      return false;
    }
  }
  return true;
}

static void crossplay_oracle_apply_draw(Game *game, int actor,
                                        const CrossplayOracleDraw *draw) {
  Bag *bag = game_get_bag(game);
  Rack *rack = player_get_rack(game_get_player(game, actor));
  for (int tile_idx = 0; tile_idx < draw->count; tile_idx++) {
    const MachineLetter tile = draw->tiles[tile_idx];
    bag_draw_letter(bag, tile, actor);
    rack_add_letter(rack, tile);
  }
}

CrossplayOracleStatus crossplay_oracle_apply_action(
    const Game *game, const CrossplayOracleAction *action,
    CrossplayOracleTransitionSet *result) {
  if (result == NULL) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  memset(result, 0, sizeof(*result));
  if (game == NULL || action == NULL ||
      bag_get_letters(game_get_bag(game)) > PEG_MAX_BAG) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }
  const int actor = game_get_player_on_turn_index(game);
  const int bag_before = bag_get_letters(game_get_bag(game));
  if (action->kind == CROSSPLAY_ORACLE_EXCHANGE &&
      (action->exchange_count < 1 || action->exchange_count > bag_before ||
       !crossplay_oracle_exchange_available(game, action))) {
    return CROSSPLAY_ORACLE_INVALID_INPUT;
  }

  int draw_count = 0;
  if (action->kind == CROSSPLAY_ORACLE_PLACEMENT) {
    draw_count = move_get_tiles_played(&action->move);
    if (draw_count > bag_before) {
      draw_count = bag_before;
    }
  } else if (action->kind == CROSSPLAY_ORACLE_EXCHANGE) {
    draw_count = action->exchange_count;
  }
  CrossplayOracleDrawSet draws;
  const CrossplayOracleStatus draw_status = crossplay_oracle_enumerate_draws(
      game_get_bag(game), draw_count, ld_get_size(game_get_ld(game)), &draws);
  if (draw_status != CROSSPLAY_ORACLE_OK) {
    return draw_status;
  }

  for (int draw_idx = 0; draw_idx < draws.count; draw_idx++) {
    CrossplayOracleTransition *transition =
        &result->transitions[result->count++];
    transition->game = game_duplicate(game);
    transition->draw = draws.draws[draw_idx];
    if (action->kind == CROSSPLAY_ORACLE_PLACEMENT) {
      play_move_without_drawing_tiles(&action->move, transition->game);
      crossplay_oracle_apply_draw(transition->game, actor, &transition->draw);
      game_set_consecutive_scoreless_turns(transition->game, 0);
    } else if (action->kind == CROSSPLAY_ORACLE_EXCHANGE) {
      crossplay_oracle_apply_draw(transition->game, actor, &transition->draw);
      Bag *bag = game_get_bag(transition->game);
      Rack *rack =
          player_get_rack(game_get_player(transition->game, actor));
      for (int tile_idx = 0; tile_idx < action->exchange_count; tile_idx++) {
        const MachineLetter tile = action->exchange_tiles[tile_idx];
        rack_take_letter(rack, tile);
        bag_add_letter(bag, tile, actor);
      }
      game_start_next_player_turn(transition->game);
      game_increment_consecutive_scoreless_turns(transition->game);
    } else {
      game_start_next_player_turn(transition->game);
      game_increment_consecutive_scoreless_turns(transition->game);
    }
    game_set_game_end_reason(transition->game, GAME_END_REASON_NONE);
    transition->bag_emptied =
        bag_before > 0 && bag_is_empty(game_get_bag(transition->game));
  }
  result->weight_mass = draws.weight_mass;
  result->complete = true;
  return CROSSPLAY_ORACLE_OK;
}

void crossplay_oracle_transition_set_destroy(
    CrossplayOracleTransitionSet *result) {
  if (result == NULL) {
    return;
  }
  for (int transition_idx = 0; transition_idx < result->count;
       transition_idx++) {
    game_destroy(result->transitions[transition_idx].game);
  }
  memset(result, 0, sizeof(*result));
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
