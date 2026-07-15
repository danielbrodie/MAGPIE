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
