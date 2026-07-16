#include "../../include/magpie/crossplay_oracle.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/data_filepaths.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "crossplay_oracle.h"
#include "crossplay_oracle_assets.h"
#include "config.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct MagpieCrossplayOracle {
  Config *config;
  Game *game;
  CrossplayOracleAssetManifest manifest;
  CrossplayOracleActionSet generated_actions;
  MagpieCrossplayBoardCell
      generated_board[MAGPIE_CROSSPLAY_BOARD_CELLS];
  uint16_t generated_actor_rack_counts[27];
  int32_t generated_scores[2];
  uint64_t generation;
  uint64_t generated_bag_length;
  uint8_t generated_player_on_turn;
  uint8_t generated_starting_player;
  uint8_t generated_consecutive_scoreless_turns;
  uint8_t generated_opponent_rack_length;
  bool has_generated_actions;
};

static void magpie_crossplay_error_reset(MagpieCrossplayError *error) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
  }
}

static MagpieCrossplayStatus magpie_crossplay_error_set(
    MagpieCrossplayError *error, MagpieCrossplayStatus status, uint32_t detail,
    const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->status = status;
    error->detail = detail;
    if (message != NULL) {
      size_t length = strlen(message);
      if (length >= MAGPIE_CROSSPLAY_ERROR_MESSAGE_CAPACITY) {
        length = MAGPIE_CROSSPLAY_ERROR_MESSAGE_CAPACITY - 1;
      }
      memcpy(error->message, message, length);
      error->message_length = (uint64_t)length;
    }
  }
  return status;
}

static MagpieCrossplayStatus magpie_crossplay_error_from_stack(
    MagpieCrossplayError *error, MagpieCrossplayStatus status,
    ErrorStack *error_stack) {
  const error_code_t detail = error_stack_top(error_stack);
  char *message = error_stack_get_string_and_reset(error_stack);
  const MagpieCrossplayStatus result = magpie_crossplay_error_set(
      error, status, (uint32_t)detail, message);
  free(message);
  return result;
}

static char *magpie_crossplay_copy_text(MagpieCrossplayBytes bytes) {
  if (bytes.data == NULL || bytes.length == 0 || bytes.length > SIZE_MAX - 1 ||
      memchr(bytes.data, '\0', (size_t)bytes.length) != NULL) {
    return NULL;
  }
  char *copy = malloc((size_t)bytes.length + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, bytes.data, (size_t)bytes.length);
  copy[bytes.length] = '\0';
  return copy;
}

static bool magpie_crossplay_execute(Config *config, const char *command,
                                    ErrorStack *error_stack) {
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, command, error_stack);
  if (error_stack_is_empty(error_stack)) {
    config_execute_command(config, error_stack);
  }
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  return error_stack_is_empty(error_stack);
}

static bool magpie_crossplay_verify_assets(
    Config *config, const char *manifest_path,
    CrossplayOracleAssetManifest *manifest, ErrorStack *error_stack) {
  if (!crossplay_oracle_asset_manifest_load(manifest_path, manifest)) {
    return false;
  }
  PlayersData *players_data = config_get_players_data(config);
  const char *p1_lexicon =
      players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0);
  const char *p2_lexicon =
      players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 1);
  const BoardLayout *layout = config_get_board_layout(config);
  const LetterDistribution *ld = config_get_ld(config);
  if (p1_lexicon == NULL || p2_lexicon == NULL ||
      strcmp(p1_lexicon, p2_lexicon) != 0 || layout == NULL || ld == NULL) {
    return false;
  }
  char *lexicon_path = data_filepaths_get_readable_filename(
      config_get_data_paths(config), p1_lexicon, DATA_FILEPATH_TYPE_KWG,
      error_stack);
  char *layout_path = data_filepaths_get_readable_filename(
      config_get_data_paths(config), board_layout_get_name(layout),
      DATA_FILEPATH_TYPE_LAYOUT, error_stack);
  char *distribution_path = data_filepaths_get_readable_filename(
      config_get_data_paths(config), ld_get_name(ld), DATA_FILEPATH_TYPE_LD,
      error_stack);
  bool verified = false;
  if (error_stack_is_empty(error_stack)) {
    verified = crossplay_oracle_asset_manifest_verify(
        manifest, p1_lexicon, lexicon_path, board_layout_get_name(layout),
        layout_path, ld_get_name(ld), distribution_path,
        config_get_bingo_bonus(config));
  }
  free(lexicon_path);
  free(layout_path);
  free(distribution_path);
  return verified;
}

uint32_t magpie_crossplay_abi_version(void) {
  return MAGPIE_CROSSPLAY_ABI_VERSION;
}

MagpieCrossplayStatus magpie_crossplay_oracle_create(
    const MagpieCrossplayAssets *assets, MagpieCrossplayOracle **out_oracle,
    MagpieCrossplayError *error) {
  magpie_crossplay_error_reset(error);
  if (assets == NULL || out_oracle == NULL) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "assets and out_oracle are required");
  }
  *out_oracle = NULL;
  char *data_paths = magpie_crossplay_copy_text(assets->data_paths);
  char *manifest_path = magpie_crossplay_copy_text(assets->manifest_path);
  if (data_paths == NULL || manifest_path == NULL) {
    free(data_paths);
    free(manifest_path);
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "asset paths must be nonempty NUL-free byte spans");
  }

  ErrorStack *error_stack = error_stack_create();
  const ConfigArgs config_args = {
      .data_paths = data_paths,
      .settings_filename = "crossplay-native-settings-disabled.txt",
      .use_wmp = false,
  };
  Config *config = config_create(&config_args, error_stack);
  if (config == NULL || !error_stack_is_empty(error_stack)) {
    const MagpieCrossplayStatus status = magpie_crossplay_error_from_stack(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, error_stack);
    config_destroy(config);
    error_stack_destroy(error_stack);
    free(data_paths);
    free(manifest_path);
    return status;
  }
  static const char PRESET[] =
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 1 "
      "-savesettings false -autosave false -fgrequired false -hr false";
  if (!magpie_crossplay_execute(config, PRESET, error_stack)) {
    const MagpieCrossplayStatus status = magpie_crossplay_error_from_stack(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, error_stack);
    config_destroy(config);
    error_stack_destroy(error_stack);
    free(data_paths);
    free(manifest_path);
    return status;
  }

  MagpieCrossplayOracle *oracle = calloc(1, sizeof(*oracle));
  if (oracle == NULL) {
    config_destroy(config);
    error_stack_destroy(error_stack);
    free(data_paths);
    free(manifest_path);
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, 0,
        "could not allocate oracle session");
  }
  if (!magpie_crossplay_verify_assets(config, manifest_path,
                                      &oracle->manifest, error_stack)) {
    MagpieCrossplayStatus status;
    if (error_stack_is_empty(error_stack)) {
      status = magpie_crossplay_error_set(
          error, MAGPIE_CROSSPLAY_STATUS_ASSET_MISMATCH, 0,
          "loaded rules assets do not match the pinned manifest");
    } else {
      status = magpie_crossplay_error_from_stack(
          error, MAGPIE_CROSSPLAY_STATUS_ASSET_MISMATCH, error_stack);
    }
    free(oracle);
    config_destroy(config);
    error_stack_destroy(error_stack);
    free(data_paths);
    free(manifest_path);
    return status;
  }
  oracle->config = config;
  oracle->game = config_game_create(config);
  *out_oracle = oracle;
  error_stack_destroy(error_stack);
  free(data_paths);
  free(manifest_path);
  return MAGPIE_CROSSPLAY_STATUS_OK;
}

void magpie_crossplay_oracle_destroy(MagpieCrossplayOracle *oracle) {
  if (oracle != NULL) {
    crossplay_oracle_action_set_destroy(&oracle->generated_actions);
    game_destroy(oracle->game);
    config_destroy(oracle->config);
    free(oracle);
  }
}

static bool magpie_crossplay_tile_valid(uint8_t tile) { return tile <= 26; }

static bool magpie_crossplay_add_tiles_to_counts(const MagpieCrossplayBytes tiles,
                                                 uint16_t counts[27]) {
  if ((tiles.length > 0 && tiles.data == NULL) || tiles.length > SIZE_MAX) {
    return false;
  }
  for (uint64_t tile_idx = 0; tile_idx < tiles.length; tile_idx++) {
    const uint8_t tile = tiles.data[tile_idx];
    if (!magpie_crossplay_tile_valid(tile) || counts[tile] == UINT16_MAX) {
      return false;
    }
    counts[tile]++;
  }
  return true;
}

static bool magpie_crossplay_position_counts_valid(
    const LetterDistribution *ld, const MagpieCrossplayPosition *position) {
  uint16_t counts[27] = {0};
  for (uint64_t cell_idx = 0; cell_idx < position->board_cell_count;
       cell_idx++) {
    const MagpieCrossplayBoardCell cell = position->board_cells[cell_idx];
    if (cell.letter > 26 || cell.is_blank > 1 ||
        (cell.letter == 0 && cell.is_blank != 0)) {
      return false;
    }
    if (cell.letter != 0) {
      const uint8_t tile = cell.is_blank != 0 ? 0 : cell.letter;
      counts[tile]++;
    }
  }
  if (!magpie_crossplay_add_tiles_to_counts(position->player_racks[0], counts) ||
      !magpie_crossplay_add_tiles_to_counts(position->player_racks[1], counts) ||
      !magpie_crossplay_add_tiles_to_counts(position->bag, counts) ||
      ld_get_size(ld) != 27) {
    return false;
  }
  for (int tile = 0; tile < 27; tile++) {
    if (counts[tile] != (uint16_t)ld_get_dist(ld, (MachineLetter)tile)) {
      return false;
    }
  }
  return true;
}

static void magpie_crossplay_set_rack(Rack *rack,
                                      MagpieCrossplayBytes tiles) {
  rack_reset(rack);
  for (uint64_t tile_idx = 0; tile_idx < tiles.length; tile_idx++) {
    rack_add_letter(rack, tiles.data[tile_idx]);
  }
}

static bool magpie_crossplay_load_position(
    MagpieCrossplayOracle *oracle, const MagpieCrossplayPosition *position) {
  if (position == NULL || position->board_cells == NULL ||
      position->board_cell_count != MAGPIE_CROSSPLAY_BOARD_CELLS ||
      position->player_racks[0].length > MAGPIE_CROSSPLAY_RACK_CAPACITY ||
      position->player_racks[1].length > MAGPIE_CROSSPLAY_RACK_CAPACITY ||
      position->bag.length > INT_MAX || position->player_on_turn > 1 ||
      position->starting_player > 1 ||
      position->scores[0] > EQUITY_MAX_VALUE / EQUITY_RESOLUTION ||
      position->scores[0] < EQUITY_MIN_VALUE / EQUITY_RESOLUTION ||
      position->scores[1] > EQUITY_MAX_VALUE / EQUITY_RESOLUTION ||
      position->scores[1] < EQUITY_MIN_VALUE / EQUITY_RESOLUTION ||
      !magpie_crossplay_position_counts_valid(config_get_ld(oracle->config),
                                              position)) {
    return false;
  }

  Game *game = oracle->game;
  game_reset(game);
  Board *board = game_get_board(game);
  int tiles_played = 0;
  for (uint64_t cell_idx = 0; cell_idx < position->board_cell_count;
       cell_idx++) {
    const MagpieCrossplayBoardCell cell = position->board_cells[cell_idx];
    if (cell.letter == 0) {
      continue;
    }
    const int row = (int)(cell_idx / 15);
    const int col = (int)(cell_idx % 15);
    MachineLetter letter = cell.letter;
    if (cell.is_blank != 0) {
      letter = get_blanked_machine_letter(letter);
    }
    board_set_letter(board, row, col, letter);
    tiles_played++;
  }
  board_set_tiles_played(board, tiles_played);
  magpie_crossplay_set_rack(
      player_get_rack(game_get_player(game, 0)), position->player_racks[0]);
  magpie_crossplay_set_rack(
      player_get_rack(game_get_player(game, 1)), position->player_racks[1]);
  bag_set_to_tiles(game_get_bag(game), position->bag.data,
                   (int)position->bag.length);
  player_set_score(game_get_player(game, 0), int_to_equity(position->scores[0]));
  player_set_score(game_get_player(game, 1), int_to_equity(position->scores[1]));
  game_set_starting_player_index(game, position->starting_player);
  game_set_player_on_turn_index(game, position->player_on_turn);
  game_set_consecutive_scoreless_turns(
      game, position->consecutive_scoreless_turns);
  game_gen_all_cross_sets(game);
  board_update_all_anchors(board);
  if (game_reached_max_scoreless_turns(game)) {
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  } else if (bag_is_empty(game_get_bag(game)) &&
             (rack_is_empty(player_get_rack(game_get_player(game, 0))) ||
              rack_is_empty(player_get_rack(game_get_player(game, 1))))) {
    game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
  } else {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
  return true;
}

static void magpie_crossplay_bind_action_basis(
    MagpieCrossplayOracle *oracle, const MagpieCrossplayPosition *position) {
  memcpy(oracle->generated_board, position->board_cells,
         sizeof(oracle->generated_board));
  memset(oracle->generated_actor_rack_counts, 0,
         sizeof(oracle->generated_actor_rack_counts));
  const uint8_t actor = position->player_on_turn;
  for (uint64_t tile_idx = 0;
       tile_idx < position->player_racks[actor].length; tile_idx++) {
    oracle->generated_actor_rack_counts[
        position->player_racks[actor].data[tile_idx]]++;
  }
  oracle->generated_scores[0] = position->scores[0];
  oracle->generated_scores[1] = position->scores[1];
  oracle->generated_bag_length = position->bag.length;
  oracle->generated_player_on_turn = actor;
  oracle->generated_starting_player = position->starting_player;
  oracle->generated_consecutive_scoreless_turns =
      position->consecutive_scoreless_turns;
  oracle->generated_opponent_rack_length =
      (uint8_t)position->player_racks[1 - actor].length;
}

static bool magpie_crossplay_action_basis_matches(
    const MagpieCrossplayOracle *oracle,
    const MagpieCrossplayPosition *position) {
  if (position->board_cells == NULL ||
      position->board_cell_count != MAGPIE_CROSSPLAY_BOARD_CELLS ||
      position->player_on_turn != oracle->generated_player_on_turn ||
      position->starting_player != oracle->generated_starting_player ||
      position->consecutive_scoreless_turns !=
          oracle->generated_consecutive_scoreless_turns ||
      position->scores[0] != oracle->generated_scores[0] ||
      position->scores[1] != oracle->generated_scores[1] ||
      position->bag.length != oracle->generated_bag_length ||
      position->player_racks[position->player_on_turn].length >
          MAGPIE_CROSSPLAY_RACK_CAPACITY ||
      position->player_racks[1 - position->player_on_turn].length !=
          oracle->generated_opponent_rack_length) {
    return false;
  }
  for (uint64_t cell_idx = 0;
       cell_idx < MAGPIE_CROSSPLAY_BOARD_CELLS; cell_idx++) {
    if (position->board_cells[cell_idx].letter !=
            oracle->generated_board[cell_idx].letter ||
        position->board_cells[cell_idx].is_blank !=
            oracle->generated_board[cell_idx].is_blank) {
      return false;
    }
  }
  uint16_t actor_rack_counts[27] = {0};
  const MagpieCrossplayBytes rack =
      position->player_racks[position->player_on_turn];
  for (uint64_t tile_idx = 0; tile_idx < rack.length; tile_idx++) {
    if (rack.data == NULL || rack.data[tile_idx] > 26) {
      return false;
    }
    actor_rack_counts[rack.data[tile_idx]]++;
  }
  return memcmp(actor_rack_counts, oracle->generated_actor_rack_counts,
                sizeof(actor_rack_counts)) == 0;
}

static bool magpie_crossplay_hex_digest(const char *hex, uint8_t digest[32]) {
  for (int byte_idx = 0; byte_idx < 32; byte_idx++) {
    uint8_t value = 0;
    for (int nibble_idx = 0; nibble_idx < 2; nibble_idx++) {
      const char character = hex[byte_idx * 2 + nibble_idx];
      uint8_t nibble;
      if (character >= '0' && character <= '9') {
        nibble = (uint8_t)(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        nibble = (uint8_t)(character - 'a' + 10);
      } else {
        return false;
      }
      value = (uint8_t)((value << 4) | nibble);
    }
    digest[byte_idx] = value;
  }
  return hex[64] == '\0';
}

static bool magpie_crossplay_export_action(const Game *game,
                                           const CrossplayOracleAction *source,
                                           uint64_t generation,
                                           uint64_t native_index,
                                           MagpieCrossplayAction *dest) {
  memset(dest, 0, sizeof(*dest));
  dest->kind = (uint8_t)source->kind;
  dest->orientation = MAGPIE_CROSSPLAY_ORIENTATION_NONE;
  dest->score = source->score;
  dest->native_generation = generation;
  dest->native_index = native_index;
  if (!magpie_crossplay_hex_digest(source->id, dest->id)) {
    return false;
  }
  if (source->kind == CROSSPLAY_ORACLE_PASS) {
    return true;
  }
  if (source->kind == CROSSPLAY_ORACLE_EXCHANGE) {
    if (source->exchange_count < 1 ||
        source->exchange_count > (int)MAGPIE_CROSSPLAY_RACK_CAPACITY) {
      return false;
    }
    dest->exchange_count = (uint8_t)source->exchange_count;
    for (int tile_idx = 0; tile_idx < source->exchange_count; tile_idx++) {
      dest->exchange_tiles[tile_idx] = source->exchange_tiles[tile_idx];
    }
    return true;
  }
  if (source->kind != CROSSPLAY_ORACLE_PLACEMENT) {
    return false;
  }

  const Move *move = &source->move;
  const int move_length = move_get_tiles_length(move);
  if (move_length < 1 || move_length > (int)MAGPIE_CROSSPLAY_WORD_CAPACITY) {
    return false;
  }
  const int direction = move_get_dir(move);
  if (direction != BOARD_HORIZONTAL_DIRECTION &&
      direction != BOARD_VERTICAL_DIRECTION) {
    return false;
  }
  dest->orientation = direction == BOARD_HORIZONTAL_DIRECTION
                          ? MAGPIE_CROSSPLAY_ORIENTATION_HORIZONTAL
                          : MAGPIE_CROSSPLAY_ORIENTATION_VERTICAL;
  dest->start_row = (uint8_t)move_get_row_start(move);
  dest->start_col = (uint8_t)move_get_col_start(move);
  dest->word_length = (uint8_t)move_length;
  const Board *board = game_get_board(game);
  const int row_increment = direction == BOARD_VERTICAL_DIRECTION ? 1 : 0;
  const int col_increment = direction == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  for (int tile_idx = 0; tile_idx < move_length; tile_idx++) {
    const int row = move_get_row_start(move) + row_increment * tile_idx;
    const int col = move_get_col_start(move) + col_increment * tile_idx;
    MachineLetter letter = move_get_tile(move, tile_idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      letter = board_get_letter(board, row, col);
    } else {
      if (dest->new_tile_count >= MAGPIE_CROSSPLAY_RACK_CAPACITY) {
        return false;
      }
      MagpieCrossplayPlacementTile *tile =
          &dest->new_tiles[dest->new_tile_count++];
      tile->row = (uint8_t)row;
      tile->col = (uint8_t)col;
      tile->letter = get_unblanked_machine_letter(letter);
      tile->is_blank = get_is_blanked(letter) ? 1 : 0;
    }
    dest->word[tile_idx] = get_unblanked_machine_letter(letter);
  }
  return dest->new_tile_count > 0;
}

MagpieCrossplayStatus magpie_crossplay_generate_actions(
    MagpieCrossplayOracle *oracle, const MagpieCrossplayPosition *position,
    uint8_t allow_exchanges, MagpieCrossplayActionSet *out_actions,
    MagpieCrossplayError *error) {
  magpie_crossplay_error_reset(error);
  if (oracle == NULL || position == NULL || out_actions == NULL ||
      allow_exchanges > 1) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "oracle, position, output, and boolean exchange policy are required");
  }
  memset(out_actions, 0, sizeof(*out_actions));
  crossplay_oracle_action_set_destroy(&oracle->generated_actions);
  oracle->has_generated_actions = false;
  if (!magpie_crossplay_load_position(oracle, position)) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_POSITION_INVALID, 0,
        "position is malformed or violates exact tile conservation");
  }

  const CrossplayOracleStatus internal_status =
      crossplay_oracle_generate_actions(oracle->game, (int)position->bag.length,
                                        allow_exchanges != 0,
                                        &oracle->generated_actions);
  CrossplayOracleActionSet *internal = &oracle->generated_actions;
  if (internal_status != CROSSPLAY_ORACLE_OK || !internal->coverage.complete) {
    crossplay_oracle_action_set_destroy(internal);
    const MagpieCrossplayStatus status =
        internal_status == CROSSPLAY_ORACLE_CAPACITY_EXCEEDED
            ? MAGPIE_CROSSPLAY_STATUS_CAPACITY_EXCEEDED
            : MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR;
    return magpie_crossplay_error_set(error, status, (uint32_t)internal_status,
                                      crossplay_oracle_status_name(
                                          internal_status));
  }

  MagpieCrossplayAction *actions =
      calloc((size_t)internal->count, sizeof(*actions));
  if (actions == NULL || !magpie_crossplay_hex_digest(internal->digest,
                                                       out_actions->digest)) {
    free(actions);
    crossplay_oracle_action_set_destroy(internal);
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, 0,
        "could not allocate or decode the complete action set");
  }
  oracle->generation++;
  if (oracle->generation == 0) {
    oracle->generation++;
  }
  for (int action_idx = 0; action_idx < internal->count; action_idx++) {
    if (!magpie_crossplay_export_action(oracle->game,
                                        &internal->actions[action_idx],
                                        oracle->generation,
                                        (uint64_t)action_idx,
                                        &actions[action_idx])) {
      free(actions);
      memset(out_actions, 0, sizeof(*out_actions));
      crossplay_oracle_action_set_destroy(internal);
      return magpie_crossplay_error_set(
          error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, 0,
          "native action cannot be represented by ABI version 1");
    }
  }
  out_actions->actions = actions;
  out_actions->count = (uint64_t)internal->count;
  out_actions->placements = (uint64_t)internal->coverage.placements;
  out_actions->exchanges = (uint64_t)internal->coverage.exchanges;
  out_actions->passes = (uint64_t)internal->coverage.passes;
  out_actions->complete = 1;
  magpie_crossplay_bind_action_basis(oracle, position);
  oracle->has_generated_actions = true;
  return MAGPIE_CROSSPLAY_STATUS_OK;
}

void magpie_crossplay_action_set_destroy(MagpieCrossplayActionSet *actions) {
  if (actions != NULL) {
    free(actions->actions);
    memset(actions, 0, sizeof(*actions));
  }
}

static bool magpie_crossplay_export_owned_position(
    const Game *game, MagpieCrossplayOwnedPosition *position) {
  memset(position, 0, sizeof(*position));
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MagpieCrossplayBoardCell *cell =
          &position->board_cells[row * BOARD_DIM + col];
      cell->letter = get_unblanked_machine_letter(letter);
      cell->is_blank = get_is_blanked(letter) ? 1 : 0;
    }
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const Rack *rack = player_get_rack(game_get_player(game, player_idx));
    int count = 0;
    for (int tile = 0; tile < 27; tile++) {
      for (int copy = 0;
           copy < rack_get_letter(rack, (MachineLetter)tile); copy++) {
        if (count >= (int)MAGPIE_CROSSPLAY_RACK_CAPACITY) {
          return false;
        }
        position->player_racks[player_idx][count++] = (uint8_t)tile;
      }
    }
    position->player_rack_lengths[player_idx] = (uint8_t)count;
  }
  const int bag_count = bag_get_letters(game_get_bag(game));
  if (bag_count < 0 ||
      bag_count > (int)MAGPIE_CROSSPLAY_POSITION_TILE_CAPACITY) {
    return false;
  }
  position->bag_length = (uint64_t)bag_count;
  bag_peek_tiles(game_get_bag(game), position->bag);
  position->scores[0] =
      equity_to_int(player_get_score(game_get_player(game, 0)));
  position->scores[1] =
      equity_to_int(player_get_score(game_get_player(game, 1)));
  position->player_on_turn = (uint8_t)game_get_player_on_turn_index(game);
  position->starting_player = (uint8_t)game_get_starting_player_index(game);
  position->consecutive_scoreless_turns =
      (uint8_t)game_get_consecutive_scoreless_turns(game);
  return true;
}

static MagpieCrossplayStatus magpie_crossplay_apply_retained_action(
    MagpieCrossplayOracle *oracle, uint64_t native_index,
    MagpieCrossplayTransitionSet *out_transitions,
    MagpieCrossplayError *error) {
  CrossplayOracleTransitionSet internal = {0};
  const CrossplayOracleStatus internal_status = crossplay_oracle_apply_action(
      oracle->game,
      &oracle->generated_actions.actions[(size_t)native_index],
      &internal);
  if (internal_status != CROSSPLAY_ORACLE_OK || !internal.complete ||
      internal.count <= 0 ||
      internal.count > (int)MAGPIE_CROSSPLAY_TRANSITION_CAPACITY ||
      internal.weight_mass <= 0) {
    crossplay_oracle_transition_set_destroy(&internal);
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR,
        (uint32_t)internal_status, crossplay_oracle_status_name(internal_status));
  }
  MagpieCrossplayTransition *transitions =
      calloc((size_t)internal.count, sizeof(*transitions));
  if (transitions == NULL) {
    crossplay_oracle_transition_set_destroy(&internal);
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_ENGINE_ERROR, 0,
        "could not allocate transition set");
  }
  for (int transition_idx = 0; transition_idx < internal.count;
       transition_idx++) {
    const CrossplayOracleTransition *source =
        &internal.transitions[transition_idx];
    MagpieCrossplayTransition *dest = &transitions[transition_idx];
    if (source->game == NULL || source->draw.count < 0 ||
        source->draw.count > (int)MAGPIE_CROSSPLAY_RACK_CAPACITY ||
        source->draw.weight <= 0 ||
        !magpie_crossplay_export_owned_position(source->game,
                                                &dest->position)) {
      free(transitions);
      crossplay_oracle_transition_set_destroy(&internal);
      return magpie_crossplay_error_set(
          error, MAGPIE_CROSSPLAY_STATUS_CAPACITY_EXCEEDED, 0,
          "transition position exceeds ABI capacity");
    }
    dest->weight = source->draw.weight;
    dest->draw_count = (uint8_t)source->draw.count;
    memcpy(dest->draw, source->draw.tiles,
           (size_t)source->draw.count * sizeof(*dest->draw));
    dest->bag_emptied = source->bag_emptied ? 1 : 0;
  }
  out_transitions->transitions = transitions;
  out_transitions->count = (uint64_t)internal.count;
  out_transitions->weight_mass = internal.weight_mass;
  out_transitions->complete = 1;
  crossplay_oracle_transition_set_destroy(&internal);
  return MAGPIE_CROSSPLAY_STATUS_OK;
}

static MagpieCrossplayStatus magpie_crossplay_validate_action_handle(
    MagpieCrossplayOracle *oracle, uint64_t native_generation,
    uint64_t native_index, MagpieCrossplayTransitionSet *out_transitions,
    MagpieCrossplayError *error) {
  magpie_crossplay_error_reset(error);
  if (oracle == NULL || out_transitions == NULL) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "oracle and transition output are required");
  }
  memset(out_transitions, 0, sizeof(*out_transitions));
  if (!oracle->has_generated_actions ||
      native_generation != oracle->generation ||
      native_index >= (uint64_t)oracle->generated_actions.count) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "action handle is stale or does not belong to this session");
  }
  return MAGPIE_CROSSPLAY_STATUS_OK;
}

MagpieCrossplayStatus magpie_crossplay_apply_action(
    MagpieCrossplayOracle *oracle, uint64_t native_generation,
    uint64_t native_index, MagpieCrossplayTransitionSet *out_transitions,
    MagpieCrossplayError *error) {
  const MagpieCrossplayStatus validation =
      magpie_crossplay_validate_action_handle(
          oracle, native_generation, native_index, out_transitions, error);
  if (validation != MAGPIE_CROSSPLAY_STATUS_OK) {
    return validation;
  }
  return magpie_crossplay_apply_retained_action(
      oracle, native_index, out_transitions, error);
}

MagpieCrossplayStatus magpie_crossplay_apply_action_to_position(
    MagpieCrossplayOracle *oracle, uint64_t native_generation,
    uint64_t native_index, const MagpieCrossplayPosition *position,
    MagpieCrossplayTransitionSet *out_transitions,
    MagpieCrossplayError *error) {
  const MagpieCrossplayStatus validation =
      magpie_crossplay_validate_action_handle(
          oracle, native_generation, native_index, out_transitions, error);
  if (validation != MAGPIE_CROSSPLAY_STATUS_OK) {
    return validation;
  }
  if (position == NULL ||
      !magpie_crossplay_action_basis_matches(oracle, position)) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT, 0,
        "position does not match the retained action-space basis");
  }
  if (!magpie_crossplay_load_position(oracle, position)) {
    return magpie_crossplay_error_set(
        error, MAGPIE_CROSSPLAY_STATUS_POSITION_INVALID, 0,
        "position is malformed or violates exact tile conservation");
  }
  return magpie_crossplay_apply_retained_action(
      oracle, native_index, out_transitions, error);
}

void magpie_crossplay_transition_set_destroy(
    MagpieCrossplayTransitionSet *transitions) {
  if (transitions != NULL) {
    free(transitions->transitions);
    memset(transitions, 0, sizeof(*transitions));
  }
}
