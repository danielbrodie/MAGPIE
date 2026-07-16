#include "crossplay_oracle_test.h"

#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/crossplay_oracle.h"
#include "../src/impl/crossplay_oracle_assets.h"
#include "../src/util/io_util.h"
#include "../src/util/sha256.h"
#include "../include/magpie/crossplay_oracle.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char *const SENATOR_TOSA_CGP =
    "cgp 3Z5F1NAIF/3E5I4I/3S1E3A2H1R/1INTERNaLS1VACS/5G3C2KAT/"
    "4JO3OM1ER1/4E5I2R1/3QUBITS1D2EX/5E1OE2T1LI/5V1PAWPAWs1/"
    "5Y1HM2B3/7E1EULOGY/10HE1O1/11A1O1/11U1D1 SENATOR/ 329/380 0";

static const char *const BLANK_OPENING_CGP =
    "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
    "?AEINRT/ 0/0 0";

static const char *const CONCRETE_SENATOR_CGP =
    "cgp 3Z5F1NAIF/3E5I4I/3S1E3A2H1R/1INTERNaLS1VACS/5G3C2KAT/"
    "4JO3OM1ER1/4E5I2R1/3QUBITS1D2EX/5E1OE2T1LI/5V1PAWPAWs1/"
    "5Y1HM2B3/7E1EULOGY/10HE1O1/11A1O1/11U1D1 SENATOR/ADGLNRT "
    "329/380 0";

static Config *crossplay_oracle_test_config(void) {
  return config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 1");
}

static uint8_t crossplay_oracle_test_hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return (uint8_t)(value - '0');
  }
  assert(value >= 'a' && value <= 'f');
  return (uint8_t)(value - 'a' + 10);
}

static void crossplay_oracle_test_assert_raw_digest(
    const uint8_t actual[32], const char *expected) {
  assert(strlen(expected) == 64);
  for (int byte_idx = 0; byte_idx < 32; byte_idx++) {
    const uint8_t high =
        crossplay_oracle_test_hex_nibble(expected[byte_idx * 2]);
    const uint8_t low =
        crossplay_oracle_test_hex_nibble(expected[byte_idx * 2 + 1]);
    assert(actual[byte_idx] == (uint8_t)((high << 4) | low));
  }
}

static int crossplay_oracle_test_export_rack(const Rack *rack,
                                             uint8_t tiles[RACK_SIZE]) {
  int count = 0;
  for (int tile = 0; tile < 27; tile++) {
    for (int copy = 0; copy < rack_get_letter(rack, (MachineLetter)tile);
         copy++) {
      assert(count < RACK_SIZE);
      tiles[count++] = (uint8_t)tile;
    }
  }
  return count;
}

static MagpieCrossplayPosition crossplay_oracle_test_export_position(
    const Game *game,
    MagpieCrossplayBoardCell board_cells[MAGPIE_CROSSPLAY_BOARD_CELLS],
    uint8_t player0_rack[RACK_SIZE], uint8_t player1_rack[RACK_SIZE],
    uint8_t bag_tiles[MAX_BAG_SIZE]) {
  memset(board_cells, 0,
         sizeof(*board_cells) * MAGPIE_CROSSPLAY_BOARD_CELLS);
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      MagpieCrossplayBoardCell *cell =
          &board_cells[row * BOARD_DIM + col];
      cell->letter = get_unblanked_machine_letter(letter);
      cell->is_blank = get_is_blanked(letter) ? 1 : 0;
    }
  }
  const int player0_count = crossplay_oracle_test_export_rack(
      player_get_rack(game_get_player(game, 0)), player0_rack);
  const int player1_count = crossplay_oracle_test_export_rack(
      player_get_rack(game_get_player(game, 1)), player1_rack);
  const int bag_count = bag_peek_tiles(game_get_bag(game), bag_tiles);
  MagpieCrossplayPosition position = {
      .board_cells = board_cells,
      .board_cell_count = MAGPIE_CROSSPLAY_BOARD_CELLS,
      .player_racks =
          {
              {.data = player0_rack, .length = (uint64_t)player0_count},
              {.data = player1_rack, .length = (uint64_t)player1_count},
          },
      .bag = {.data = bag_tiles, .length = (uint64_t)bag_count},
      .scores =
          {
              equity_to_int(player_get_score(game_get_player(game, 0))),
              equity_to_int(player_get_score(game_get_player(game, 1))),
          },
      .player_on_turn = (uint8_t)game_get_player_on_turn_index(game),
      .starting_player = (uint8_t)game_get_starting_player_index(game),
      .consecutive_scoreless_turns =
          (uint8_t)game_get_consecutive_scoreless_turns(game),
  };
  return position;
}

static void crossplay_oracle_test_public_abi(const char *manifest_path) {
  assert(magpie_crossplay_abi_version() == MAGPIE_CROSSPLAY_ABI_VERSION);
  const char *data_paths = DEFAULT_TEST_DATA_PATH;
  const MagpieCrossplayAssets assets = {
      .data_paths =
          {.data = (const uint8_t *)data_paths, .length = strlen(data_paths)},
      .manifest_path =
          {.data = (const uint8_t *)manifest_path,
           .length = strlen(manifest_path)},
  };
  MagpieCrossplayOracle *oracle = NULL;
  MagpieCrossplayError error;
  assert(magpie_crossplay_oracle_create(&assets, &oracle, &error) ==
         MAGPIE_CROSSPLAY_STATUS_OK);
  assert(oracle != NULL);
  assert(error.status == MAGPIE_CROSSPLAY_STATUS_OK);

  Config *config = crossplay_oracle_test_config();
  load_and_exec_config_or_die(config, CONCRETE_SENATOR_CGP);
  MagpieCrossplayBoardCell board_cells[MAGPIE_CROSSPLAY_BOARD_CELLS];
  uint8_t player0_rack[RACK_SIZE];
  uint8_t player1_rack[RACK_SIZE];
  uint8_t bag_tiles[MAX_BAG_SIZE];
  MagpieCrossplayPosition position = crossplay_oracle_test_export_position(
      config_get_game(config), board_cells, player0_rack, player1_rack,
      bag_tiles);

  MagpieCrossplayActionSet actions;
  assert(magpie_crossplay_generate_actions(oracle, &position, 1, &actions,
                                            &error) ==
         MAGPIE_CROSSPLAY_STATUS_OK);
  assert(actions.complete == 1);
  assert(actions.count == 1314);
  assert(actions.placements == 1215);
  assert(actions.exchanges == 98);
  assert(actions.passes == 1);
  crossplay_oracle_test_assert_raw_digest(
      actions.digest,
      "f72a97fe20a27d5b498528f238f8a3bd9ea88106dab96a14984bc9f3657eaa34");
  bool found_tosa = false;
  bool found_pass = false;
  uint64_t tosa_generation = 0;
  uint64_t tosa_index = 0;
  const uint8_t tosa[] = {20, 15, 19, 1};
  for (uint64_t action_idx = 0; action_idx < actions.count; action_idx++) {
    const MagpieCrossplayAction *action = &actions.actions[action_idx];
    if (action->kind == MAGPIE_CROSSPLAY_ACTION_PASS) {
      found_pass = true;
      crossplay_oracle_test_assert_raw_digest(
          action->id,
          "d04d289dc4be5bbe028e20372d31aefe6c3a5fa1361b778a99c522ec4b484e8e");
    }
    if (action->kind == MAGPIE_CROSSPLAY_ACTION_PLACEMENT &&
        action->orientation == MAGPIE_CROSSPLAY_ORIENTATION_HORIZONTAL &&
        action->start_row == 12 && action->start_col == 5 &&
        action->score == 14 &&
        action->word_length == sizeof(tosa) &&
        memcmp(action->word, tosa, sizeof(tosa)) == 0) {
      found_tosa = true;
      tosa_generation = action->native_generation;
      tosa_index = action->native_index;
    }
  }
  assert(found_pass);
  assert(found_tosa);

  MagpieCrossplayTransitionSet transitions;
  assert(magpie_crossplay_apply_action(oracle, tosa_generation, tosa_index,
                                        &transitions, &error) ==
         MAGPIE_CROSSPLAY_STATUS_OK);
  assert(transitions.complete == 1);
  assert(transitions.count == 1);
  assert(transitions.weight_mass == 1);
  assert(transitions.transitions[0].weight == 1);
  assert(transitions.transitions[0].draw_count == 4);
  assert(transitions.transitions[0].bag_emptied == 1);
  assert(transitions.transitions[0].position.bag_length == 0);
  assert(transitions.transitions[0].position.scores[0] == 343);
  assert(transitions.transitions[0].position.scores[1] == 380);
  assert(transitions.transitions[0].position.player_on_turn == 1);
  magpie_crossplay_transition_set_destroy(&transitions);
  assert(transitions.transitions == NULL);

  int opponent_swap_idx = -1;
  int bag_swap_idx = -1;
  for (uint64_t opponent_idx = 0;
       opponent_idx < position.player_racks[1].length &&
       opponent_swap_idx < 0;
       opponent_idx++) {
    for (uint64_t bag_idx = 0; bag_idx < position.bag.length; bag_idx++) {
      if (player1_rack[opponent_idx] != bag_tiles[bag_idx]) {
        opponent_swap_idx = (int)opponent_idx;
        bag_swap_idx = (int)bag_idx;
        break;
      }
    }
  }
  assert(opponent_swap_idx >= 0);
  assert(bag_swap_idx >= 0);
  const uint8_t saved_opponent_tile = player1_rack[opponent_swap_idx];
  const uint8_t saved_hidden_bag_tile = bag_tiles[bag_swap_idx];
  player1_rack[opponent_swap_idx] = saved_hidden_bag_tile;
  bag_tiles[bag_swap_idx] = saved_opponent_tile;
  assert(magpie_crossplay_validate_action_space_position(
             oracle, tosa_generation, &position, &error) ==
         MAGPIE_CROSSPLAY_STATUS_OK);
  assert(magpie_crossplay_apply_action_to_position(
             oracle, tosa_generation, tosa_index, &position, &transitions,
             &error) == MAGPIE_CROSSPLAY_STATUS_OK);
  assert(transitions.complete == 1);
  assert(transitions.count == 1);
  assert(transitions.transitions[0].position.scores[0] == 343);
  assert(transitions.transitions[0].position.bag_length == 0);
  magpie_crossplay_transition_set_destroy(&transitions);
  player1_rack[opponent_swap_idx] = saved_opponent_tile;
  bag_tiles[bag_swap_idx] = saved_hidden_bag_tile;

  int actor_swap_idx = -1;
  opponent_swap_idx = -1;
  for (uint64_t actor_idx = 0;
       actor_idx < position.player_racks[0].length && actor_swap_idx < 0;
       actor_idx++) {
    for (uint64_t opponent_idx = 0;
         opponent_idx < position.player_racks[1].length; opponent_idx++) {
      if (player0_rack[actor_idx] != player1_rack[opponent_idx]) {
        actor_swap_idx = (int)actor_idx;
        opponent_swap_idx = (int)opponent_idx;
        break;
      }
    }
  }
  assert(actor_swap_idx >= 0);
  assert(opponent_swap_idx >= 0);
  const uint8_t saved_actor_tile = player0_rack[actor_swap_idx];
  const uint8_t saved_other_tile = player1_rack[opponent_swap_idx];
  player0_rack[actor_swap_idx] = saved_other_tile;
  player1_rack[opponent_swap_idx] = saved_actor_tile;
  assert(magpie_crossplay_validate_action_space_position(
             oracle, tosa_generation, &position, &error) ==
         MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT);
  assert(magpie_crossplay_apply_action_to_position(
             oracle, tosa_generation, tosa_index, &position, &transitions,
             &error) == MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT);
  assert(error.status == MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT);
  player0_rack[actor_swap_idx] = saved_actor_tile;
  player1_rack[opponent_swap_idx] = saved_other_tile;

  assert(magpie_crossplay_apply_action(oracle, tosa_generation + 1,
                                        tosa_index, &transitions, &error) ==
         MAGPIE_CROSSPLAY_STATUS_INVALID_ARGUMENT);
  magpie_crossplay_action_set_destroy(&actions);
  assert(actions.actions == NULL);
  assert(actions.count == 0);

  const uint8_t saved_bag_tile = bag_tiles[0];
  bag_tiles[0] = bag_tiles[0] == 1 ? 2 : 1;
  assert(magpie_crossplay_generate_actions(oracle, &position, 1, &actions,
                                            &error) ==
         MAGPIE_CROSSPLAY_STATUS_POSITION_INVALID);
  assert(error.status == MAGPIE_CROSSPLAY_STATUS_POSITION_INVALID);
  bag_tiles[0] = saved_bag_tile;

  config_destroy(config);
  magpie_crossplay_oracle_destroy(oracle);
}

static void test_sha256_known_vector(void) {
  Sha256 sha;
  char digest[SHA256_HEX_SIZE];
  sha256_init(&sha);
  sha256_update(&sha, "abc", 3);
  sha256_final_hex(&sha, digest);
  assert(strcmp(digest,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f"
                "20015ad") == 0);
}

static void test_complete_canonical_action_set(void) {
  Config *config = crossplay_oracle_test_config();
  load_and_exec_config_or_die(config, SENATOR_TOSA_CGP);
  const Game *game = config_get_game(config);
  Game *before = game_duplicate(game);

  CrossplayOracleActionSet actions;
  assert(crossplay_oracle_generate_actions(game, 4, true, &actions) ==
         CROSSPLAY_ORACLE_OK);
  assert(actions.coverage.complete);
  assert(actions.coverage.placements == 1215);
  assert(actions.coverage.exchanges == 98);
  assert(actions.coverage.passes == 1);
  assert(actions.coverage.total == 1314);
  assert(actions.count == 1314);
  assert(strcmp(actions.digest,
                "f72a97fe20a27d5b498528f238f8a3bd9ea88106dab96a14984bc9f3"
                "657eaa34") == 0);

  int pass_count = 0;
  bool found_tosa = false;
  for (int action_idx = 0; action_idx < actions.count; action_idx++) {
    const CrossplayOracleAction *action = &actions.actions[action_idx];
    assert(strlen(action->id) == SHA256_HEX_SIZE - 1);
    assert(action->canonical_json != NULL);
    if (action_idx > 0) {
      assert(strcmp(actions.actions[action_idx - 1].id, action->id) < 0);
    }
    if (action->kind == CROSSPLAY_ORACLE_PASS) {
      pass_count++;
      assert(strcmp(action->canonical_json, "{\"kind\":\"pass\"}") == 0);
      assert(strcmp(action->id,
                    "d04d289dc4be5bbe028e20372d31aefe6c3a5fa1361b778a99c522ec"
                    "4b484e8e") == 0);
    }
    if (strstr(action->canonical_json, "\"word\":\"TOSA\"") != NULL) {
      found_tosa = true;
    }
  }
  assert(pass_count == 1);
  assert(found_tosa);
  assert_games_are_equal(before, game, true);

  const CrossplayOracleAction *selected = &actions.actions[0];
  for (int action_idx = 1; action_idx < actions.count; action_idx++) {
    if (actions.actions[action_idx].score > selected->score) {
      selected = &actions.actions[action_idx];
    }
  }
  char bounded_commitment[SHA256_HEX_SIZE];
  assert(crossplay_oracle_bounded_sequence_commitment(
             &actions, selected->id, 0,
             "0000000000000000000000000000000000000000000000000000000000000000",
             "1111111111111111111111111111111111111111111111111111111111111111",
             bounded_commitment) == CROSSPLAY_ORACLE_OK);
  assert(strcmp(bounded_commitment,
                "8eb61c5047103f59e2c5c9a0433d16c27a53b46a70f2c363fbf5ae"
                "244eeca7ff") == 0);
  CrossplayOracleActionSet singleton = {
      .actions = (CrossplayOracleAction *)selected,
      .count = 1,
      .coverage = {.passes = 1, .total = 1, .complete = true},
  };
  assert(crossplay_oracle_bounded_sequence_commitment(
             &singleton, selected->id, 0,
             "0000000000000000000000000000000000000000000000000000000000000000",
             "1111111111111111111111111111111111111111111111111111111111111111",
             bounded_commitment) == CROSSPLAY_ORACLE_OK);
  assert(strcmp(bounded_commitment,
                "f880884dff07a041e745d8ef0c9334f3ce881819e5487eeb89cf9764"
                "fda10979") == 0);
  assert(crossplay_oracle_bounded_sequence_commitment(
             &actions,
             "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             0,
             "0000000000000000000000000000000000000000000000000000000000000000",
             "1111111111111111111111111111111111111111111111111111111111111111",
             bounded_commitment) == CROSSPLAY_ORACLE_INVALID_INPUT);

  crossplay_oracle_action_set_destroy(&actions);
  game_destroy(before);
  config_destroy(config);
}

static void test_asset_manifest_verification(void) {
  const char *manifest_path = "crossplay_oracle_assets_test.txt";
  const char *blocklist_path = "crossplay_oracle_blocklist_test.txt";
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(blocklist_path, "w", "NOTAWORD\n", error_stack);
  assert(error_stack_is_empty(error_stack));

  char lexicon_digest[SHA256_HEX_SIZE];
  char layout_digest[SHA256_HEX_SIZE];
  char distribution_digest[SHA256_HEX_SIZE];
  char blocklist_digest[SHA256_HEX_SIZE];
  assert(sha256_file_hex("data/lexica/NWL23_crossplay.kwg",
                         lexicon_digest));
  assert(sha256_file_hex("data/layouts/crossplay.txt", layout_digest));
  assert(sha256_file_hex("data/letterdistributions/english_crossplay.csv",
                         distribution_digest));
  assert(sha256_file_hex(blocklist_path, blocklist_digest));
  char *manifest_text = get_formatted_string(
      "crossplay-oracle-assets-v1\n"
      "rules_id test-rules\n"
      "rules_digest "
      "0000000000000000000000000000000000000000000000000000000000000000\n"
      "lexicon_id NWL23_crossplay\n"
      "lexicon_digest %s\n"
      "layout_id crossplay\n"
      "layout_digest %s\n"
      "distribution_id english_crossplay\n"
      "distribution_digest %s\n"
      "blocklist_path %s\n"
      "blocklist_digest %s\n",
      lexicon_digest, layout_digest, distribution_digest, blocklist_path,
      blocklist_digest);
  write_string_to_file(manifest_path, "w", manifest_text, error_stack);
  free(manifest_text);
  assert(error_stack_is_empty(error_stack));

  CrossplayOracleAssetManifest manifest;
  assert(crossplay_oracle_asset_manifest_load(manifest_path, &manifest));
  assert(crossplay_oracle_asset_manifest_verify(
      &manifest, "NWL23_crossplay",
      "data/lexica/NWL23_crossplay.kwg", "crossplay",
      "data/layouts/crossplay.txt", "english_crossplay",
      "data/letterdistributions/english_crossplay.csv", 40));

  CrossplayOracleAssetSession session = {0};
  CrossplayOracleAssetManifest resolved;
  const void *lexicon_handle = &manifest;
  const void *layout_handle = &resolved;
  const void *distribution_handle = &session;
  assert(!crossplay_oracle_asset_session_resolve(
      &session, manifest_path, "NWL23_crossplay", "crossplay",
      "english_crossplay", lexicon_handle, layout_handle, distribution_handle,
      40, &resolved));
  assert(crossplay_oracle_asset_session_remember(
      &session, manifest_path, "NWL23_crossplay", "crossplay",
      "english_crossplay", lexicon_handle, layout_handle, distribution_handle,
      40, &manifest));
  assert(crossplay_oracle_asset_session_resolve(
      &session, manifest_path, "NWL23_crossplay", "crossplay",
      "english_crossplay", lexicon_handle, layout_handle, distribution_handle,
      40, &resolved));
  assert(memcmp(&resolved, &manifest, sizeof(manifest)) == 0);
  assert(!crossplay_oracle_asset_session_resolve(
      &session, "different-manifest.txt", "NWL23_crossplay", "crossplay",
      "english_crossplay", lexicon_handle, layout_handle, distribution_handle,
      40, &resolved));
  assert(!crossplay_oracle_asset_session_resolve(
      &session, manifest_path, "NWL23_crossplay", "crossplay",
      "english_crossplay", lexicon_handle, layout_handle, distribution_handle,
      50, &resolved));
  assert(!crossplay_oracle_asset_session_resolve(
      &session, manifest_path, "NWL23_crossplay", "crossplay",
      "english_crossplay", &resolved, layout_handle, distribution_handle, 40,
      &resolved));

  Config *config = crossplay_oracle_test_config();
  char *six_arg_command = get_formatted_string(
      "crossplayoracle 0 %s noexch trustedassets apply "
      "d04d289dc4be5bbe028e20372d31aefe6c3a5fa1361b778a99c522ec4b484e8e",
      manifest_path);
  config_load_command(config, six_arg_command, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(six_arg_command);
  config_destroy(config);

  crossplay_oracle_test_public_abi(manifest_path);

  manifest.lexicon_digest[0] = manifest.lexicon_digest[0] == '0' ? '1' : '0';
  assert(!crossplay_oracle_asset_manifest_verify(
      &manifest, "NWL23_crossplay",
      "data/lexica/NWL23_crossplay.kwg", "crossplay",
      "data/layouts/crossplay.txt", "english_crossplay",
      "data/letterdistributions/english_crossplay.csv", 40));
  assert(!crossplay_oracle_asset_manifest_load(
      "testdata/does_not_exist.txt", &manifest));
  remove_or_die(manifest_path);
  remove_or_die(blocklist_path);
  error_stack_destroy(error_stack);
}

static void test_blank_encoding_and_exchange_policy(void) {
  Config *config = crossplay_oracle_test_config();
  load_and_exec_config_or_die(config, BLANK_OPENING_CGP);
  const Game *game = config_get_game(config);

  CrossplayOracleActionSet actions;
  assert(crossplay_oracle_generate_actions(game, 4, true, &actions) ==
         CROSSPLAY_ORACLE_OK);
  bool found_blank_placement = false;
  bool found_blank_exchange = false;
  for (int action_idx = 0; action_idx < actions.count; action_idx++) {
    const CrossplayOracleAction *action = &actions.actions[action_idx];
    if (strstr(action->canonical_json, "\"blank\":true") != NULL) {
      found_blank_placement = true;
    }
    if (strcmp(action->canonical_json,
               "{\"kind\":\"exchange\",\"tiles\":\"?\"}") == 0) {
      found_blank_exchange = true;
    }
  }
  assert(found_blank_placement);
  assert(found_blank_exchange);
  assert(actions.coverage.exchanges == 98);
  crossplay_oracle_action_set_destroy(&actions);

  assert(crossplay_oracle_generate_actions(game, 0, true, &actions) ==
         CROSSPLAY_ORACLE_OK);
  assert(actions.coverage.exchanges == 0);
  assert(actions.coverage.passes == 1);
  crossplay_oracle_action_set_destroy(&actions);

  assert(crossplay_oracle_generate_actions(game, 4, false, &actions) ==
         CROSSPLAY_ORACLE_OK);
  assert(actions.coverage.exchanges == 0);
  crossplay_oracle_action_set_destroy(&actions);
  config_destroy(config);
}

static void test_duplicate_draw_weights(void) {
  Config *config = crossplay_oracle_test_config();
  load_and_exec_config_or_die(config, BLANK_OPENING_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const MachineLetter tiles[] = {
      ld_hl_to_ml(ld, "A"),
      ld_hl_to_ml(ld, "A"),
      ld_hl_to_ml(ld, "B"),
  };
  bag_set_to_tiles(game_get_bag(game), tiles, 3);

  CrossplayOracleDrawSet draws;
  assert(crossplay_oracle_enumerate_draws(game_get_bag(game), 2,
                                          ld_get_size(ld), &draws) ==
         CROSSPLAY_ORACLE_OK);
  assert(draws.complete);
  assert(draws.count == 2);
  assert(draws.weight_mass == 3);
  bool found_aa = false;
  bool found_ab = false;
  for (int draw_idx = 0; draw_idx < draws.count; draw_idx++) {
    const CrossplayOracleDraw *draw = &draws.draws[draw_idx];
    if (draw->tiles[0] == ld_hl_to_ml(ld, "A") &&
        draw->tiles[1] == ld_hl_to_ml(ld, "A")) {
      assert(draw->weight == 1);
      found_aa = true;
    } else if (draw->tiles[0] == ld_hl_to_ml(ld, "A") &&
               draw->tiles[1] == ld_hl_to_ml(ld, "B")) {
      assert(draw->weight == 2);
      found_ab = true;
    }
  }
  assert(found_aa);
  assert(found_ab);
  config_destroy(config);
}

static void test_clone_based_action_transitions(void) {
  Config *config = crossplay_oracle_test_config();
  load_and_exec_config_or_die(config, CONCRETE_SENATOR_CGP);
  const Game *game = config_get_game(config);
  Game *before = game_duplicate(game);
  const LetterDistribution *ld = game_get_ld(game);

  CrossplayOracleActionSet actions;
  assert(crossplay_oracle_generate_actions(game, 4, true, &actions) ==
         CROSSPLAY_ORACLE_OK);
  const CrossplayOracleAction *tosa = NULL;
  const CrossplayOracleAction *exchange_a = NULL;
  const CrossplayOracleAction *pass = NULL;
  for (int action_idx = 0; action_idx < actions.count; action_idx++) {
    const CrossplayOracleAction *action = &actions.actions[action_idx];
    if (strstr(action->canonical_json,
               "\"start_col\":5,\"start_row\":12,\"word\":\"TOSA\"") !=
        NULL) {
      tosa = action;
    } else if (strcmp(action->canonical_json,
                      "{\"kind\":\"exchange\",\"tiles\":\"A\"}") == 0) {
      exchange_a = action;
    } else if (action->kind == CROSSPLAY_ORACLE_PASS) {
      pass = action;
    }
  }
  assert(tosa != NULL);
  assert(exchange_a != NULL);
  assert(pass != NULL);

  CrossplayOracleTransitionSet transitions;
  assert(crossplay_oracle_apply_action(game, tosa, &transitions) ==
         CROSSPLAY_ORACLE_OK);
  assert(transitions.complete);
  assert(transitions.count == 1);
  assert(transitions.weight_mass == 1);
  assert(transitions.transitions[0].bag_emptied);
  const Game *after_tosa = transitions.transitions[0].game;
  assert(bag_is_empty(game_get_bag(after_tosa)));
  assert(game_get_player_on_turn_index(after_tosa) == 1);
  assert(game_get_game_end_reason(after_tosa) == GAME_END_REASON_NONE);
  assert(player_get_score(game_get_player(after_tosa, 0)) ==
         int_to_equity(343));
  assert(player_get_score(game_get_player(after_tosa, 1)) ==
         int_to_equity(380));
  assert_rack_equals_string(
      ld, player_get_rack(game_get_player(after_tosa, 0)), "DEINOR?");
  assert_rack_equals_string(
      ld, player_get_rack(game_get_player(after_tosa, 1)), "ADGLNRT");
  crossplay_oracle_transition_set_destroy(&transitions);
  assert_games_are_equal(before, game, true);

  assert(crossplay_oracle_apply_action(game, exchange_a, &transitions) ==
         CROSSPLAY_ORACLE_OK);
  assert(transitions.count == 4);
  assert(transitions.weight_mass == 4);
  CrossplayOracleTransitionSet repeated_transitions;
  assert(crossplay_oracle_apply_action(game, exchange_a,
                                       &repeated_transitions) ==
         CROSSPLAY_ORACLE_OK);
  assert(repeated_transitions.count == transitions.count);
  for (int transition_idx = 0; transition_idx < transitions.count;
       transition_idx++) {
    Game *child = transitions.transitions[transition_idx].game;
    Game *repeated = repeated_transitions.transitions[transition_idx].game;
    assert(bag_get_letters(game_get_bag(child)) == 4);
    assert(game_get_player_on_turn_index(child) == 1);
    assert(rack_get_total_letters(
               player_get_rack(game_get_player(child, 0))) == RACK_SIZE);
    assert(!transitions.transitions[transition_idx].bag_emptied);
    assert_games_are_equal(child, repeated, true);
    MachineLetter child_order[MAX_BAG_SIZE];
    MachineLetter repeated_order[MAX_BAG_SIZE];
    int child_count = bag_peek_tiles(game_get_bag(child), child_order);
    int repeated_count =
        bag_peek_tiles(game_get_bag(repeated), repeated_order);
    assert(child_count == repeated_count);
    assert(memcmp(child_order, repeated_order,
                  (size_t)child_count * sizeof(*child_order)) == 0);
    const MachineLetter probe = ld_hl_to_ml(ld, "E");
    bag_add_letter(game_get_bag(child), probe, 0);
    bag_add_letter(game_get_bag(repeated), probe, 0);
    child_count = bag_peek_tiles(game_get_bag(child), child_order);
    repeated_count = bag_peek_tiles(game_get_bag(repeated), repeated_order);
    assert(child_count == repeated_count);
    assert(memcmp(child_order, repeated_order,
                  (size_t)child_count * sizeof(*child_order)) == 0);
  }
  crossplay_oracle_transition_set_destroy(&repeated_transitions);
  crossplay_oracle_transition_set_destroy(&transitions);
  assert_games_are_equal(before, game, true);

  assert(crossplay_oracle_apply_action(game, pass, &transitions) ==
         CROSSPLAY_ORACLE_OK);
  assert(transitions.count == 1);
  assert(game_get_player_on_turn_index(transitions.transitions[0].game) == 1);
  assert(game_get_consecutive_scoreless_turns(
             transitions.transitions[0].game) == 1);
  crossplay_oracle_transition_set_destroy(&transitions);
  assert_games_are_equal(before, game, true);

  crossplay_oracle_action_set_destroy(&actions);
  game_destroy(before);
  config_destroy(config);
}

void test_crossplay_oracle(void) {
  test_sha256_known_vector();
  test_asset_manifest_verification();
  test_complete_canonical_action_set();
  test_blank_encoding_and_exchange_policy();
  test_duplicate_draw_weights();
  test_clone_based_action_transitions();
}
