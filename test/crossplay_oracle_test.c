#include "crossplay_oracle_test.h"

#include "../src/ent/game.h"
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

void test_crossplay_oracle(void) {
  test_sha256_known_vector();
  test_asset_manifest_verification();
  test_complete_canonical_action_set();
  test_blank_encoding_and_exchange_policy();
}
