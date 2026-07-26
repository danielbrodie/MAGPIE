#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/move_undo.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/cpeg.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CPEG_UNDO_MOVE_CAP = 16384,
  CPEG_UNDO_MAX_DEPTH = 3,
  CPEG_UNDO_TRIALS = 1000,
  CPEG_COORDINATOR_RANDOM_TRIALS = 1000,
  CPEG_COORDINATOR_RANDOM_WORLDS = 7,
};

// A real Crossplay bag-empty position (the endgame_final_turn.png fixture the
// Python solver pins). Two plies remain: the mover plays C4 EYAS for 32, the
// opponent's best raw-score reply is K4 (B)hUT for 27, so the swing is 5.
static const char *const CPEG_FINAL_TURN_CGP =
    "cgp ZIPs7T2J/I2N6SHAVE/T2aA3C2R2E/H2WE3ONBOARD/E2ER3W2EX2/R2DO3F2S3/"
    "4S3L6/4O2FOB5/4LO1OPA5/5C1A1R3T1/5HMM1QI1GOT/6AE1UNLIKE/6ID1EN1TEG/"
    "6D6R1/7GALENAS1 IIEAYSI/?TUUVY 0/0 0";

// The same nearly full board with the opponent rack hidden. Treating one of
// ?TUUVY as the bag gives a compact bag-one position with five distinct worlds
// and total multiset mass six (the duplicate U carries weight two).
static const char *const CPEG_WTL_BAG1_CGP =
    "cgp ZIPs7T2J/I2N6SHAVE/T2aA3C2R2E/H2WE3ONBOARD/E2ER3W2EX2/"
    "R2DO3F2S3/4S3L6/4O2FOB5/4LO1OPA5/5C1A1R3T1/5HMM1QI1GOT/"
    "6AE1UNLIKE/6ID1EN1TEG/6D6R1/7GALENAS1 IIEAYSI/ 0/0 0";

// Two real Crossplay pre-endgame positions with the bag holding one tile. The
// opponent's rack is left empty in the CGP so the solver enumerates the unseen
// tiles into opponent-rack worlds. Both are cross-checked against the Python
// reference (its listed candidates match cpeg's values exactly).
//   IMG_9570: mover BQEEUAT. 15J BEET scores 39 and is worth +43.875, but the
//   exhaustive search finds 13J TAU (16) worth +44.125 -- a blocker/leave play
//   that strands the opponent vowelless. 13J TAU is a fully legal MAGPIE play
//   (it is play #37 of 410 that `gen`/generate_moves emits here; its down-word
//   LUD is in NWL23), so it is a legitimate candidate -- see
//   test_cpeg_candidate_legality, which proves every cpeg candidate is a legal
//   generate_moves play.
static const char *const CPEG_PRE_9570_CGP =
    "cgp 13V1/13AG/6DIMLY2SH/7N5TE/3p3VAC4R/3H3O6A/T2O3I6O/I2N3COZEN1ES/"
    "BRIEFS1E2ROUX1/I3AIRsOME2I1/A4N7L1/L3GEEK3LEIS/5W7A1/1DOTeS1GUARDANT/"
    "YORE11 BQEEUAT/ 0/0 0";
//   IMG_9653: mover DPIATSS. Top play 12G I(N)S(I)D(E) scores 25 and is worth
//   +23.0 -- matches the Python reference's top move and spread exactly.
static const char *const CPEG_PRE_9653_CGP =
    "cgp 7KiNIN3/8G1TORN1/7ON6/7YO6/8R2H3/7BE1BO3/4F2OR1ET1J1/2REAVOW2HE1US/"
    "4TI1SODALITE/4WE1M2V3G/4AD1A1FED2U/CInQS2N1I1E1AE/A2A5L1T1I1/"
    "R2d2ZEAL1O1R1/P2I5Y1X3 DPIATSS/ 0/0 0";

// The incident position that exceeded the old 1024-candidate result array.
// Complete root accounting is 1215 placements + 98 exchanges + voluntary pass.
static const char *const CPEG_SENATOR_TOSA_CGP =
    "cgp 3Z5F1NAIF/3E5I4I/3S1E3A2H1R/1INTERNaLS1VACS/5G3C2KAT/"
    "4JO3OM1ER1/4E5I2R1/3QUBITS1D2EX/5E1OE2T1LI/5V1PAWPAWs1/"
    "5Y1HM2B3/7E1EULOGY/10HE1O1/11A1O1/11U1D1 SENATOR/ 329/380 0";

typedef struct CpegTestBranchUndo {
  Bag *bag;
  Rack rack;
  int player_idx;
  int player_on_turn_idx;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
} CpegTestBranchUndo;

static uint64_t cpeg_test_random_state = UINT64_C(0x9e3779b97f4a7c15);

static uint32_t cpeg_test_random(void) {
  cpeg_test_random_state ^= cpeg_test_random_state >> 12;
  cpeg_test_random_state ^= cpeg_test_random_state << 25;
  cpeg_test_random_state ^= cpeg_test_random_state >> 27;
  return (uint32_t)((cpeg_test_random_state * UINT64_C(2685821657736338717)) >>
                    32);
}

static void cpeg_test_save_branch(const Game *game, int player_idx,
                                  CpegTestBranchUndo *undo) {
  undo->bag = bag_duplicate(game_get_bag(game));
  rack_copy(&undo->rack, player_get_rack(game_get_player(game, player_idx)));
  undo->player_idx = player_idx;
  undo->player_on_turn_idx = game_get_player_on_turn_index(game);
  undo->consecutive_scoreless_turns =
      game_get_consecutive_scoreless_turns(game);
  undo->game_end_reason = game_get_game_end_reason(game);
}

static void cpeg_test_restore_branch(Game *game,
                                     const CpegTestBranchUndo *undo) {
  bag_copy(game_get_bag(game), undo->bag);
  rack_copy(player_get_rack(game_get_player(game, undo->player_idx)),
            &undo->rack);
  game_set_player_on_turn_index(game, undo->player_on_turn_idx);
  game_set_consecutive_scoreless_turns(game, undo->consecutive_scoreless_turns);
  game_set_game_end_reason(game, undo->game_end_reason);
}

static void cpeg_test_destroy_branch(CpegTestBranchUndo *undo) {
  bag_destroy(undo->bag);
}

static void cpeg_test_assert_bag_state_equal(const Bag *expected,
                                             const Bag *actual) {
  MachineLetter expected_tiles[MAX_BAG_SIZE];
  MachineLetter actual_tiles[MAX_BAG_SIZE];
  const int expected_count = bag_peek_tiles(expected, expected_tiles);
  const int actual_count = bag_peek_tiles(actual, actual_tiles);
  assert(expected_count == actual_count);
  assert(memcmp(expected_tiles, actual_tiles,
                (size_t)expected_count * sizeof(*expected_tiles)) == 0);

  // Probe copies with the PRNG-using inverse operation too. Equal output after
  // several insertions proves the otherwise opaque bag PRNG state was restored.
  Bag *expected_probe = bag_duplicate(expected);
  Bag *actual_probe = bag_duplicate(actual);
  for (int probe_idx = 0; probe_idx < 4; probe_idx++) {
    const MachineLetter ml = (MachineLetter)(probe_idx + 1);
    bag_add_letter(expected_probe, ml, 0);
    bag_add_letter(actual_probe, ml, 0);
  }
  const int expected_probe_count =
      bag_peek_tiles(expected_probe, expected_tiles);
  const int actual_probe_count = bag_peek_tiles(actual_probe, actual_tiles);
  assert(expected_probe_count == actual_probe_count);
  assert(memcmp(expected_tiles, actual_tiles,
                (size_t)expected_probe_count * sizeof(*expected_tiles)) == 0);
  bag_destroy(actual_probe);
  bag_destroy(expected_probe);
}

static void cpeg_test_assert_state_equal(const Game *expected,
                                         const Game *actual) {
  assert(game_get_player_on_turn_index(expected) ==
         game_get_player_on_turn_index(actual));
  assert(game_get_consecutive_scoreless_turns(expected) ==
         game_get_consecutive_scoreless_turns(actual));
  assert(game_get_game_end_reason(expected) ==
         game_get_game_end_reason(actual));
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    const Player *expected_player = game_get_player(expected, player_idx);
    const Player *actual_player = game_get_player(actual, player_idx);
    assert(player_get_score(expected_player) ==
           player_get_score(actual_player));
    assert(memcmp(player_get_rack(expected_player),
                  player_get_rack(actual_player), sizeof(Rack)) == 0);
  }
  assert(memcmp(game_get_board(expected), game_get_board(actual),
                sizeof(Board)) == 0);
  cpeg_test_assert_bag_state_equal(game_get_bag(expected),
                                   game_get_bag(actual));
}

static const Move *cpeg_test_random_placement(const MoveList *moves) {
  const Move *selected = NULL;
  int placements_seen = 0;
  const int count = move_list_get_count(moves);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    placements_seen++;
    if (cpeg_test_random() % (uint32_t)placements_seen == 0) {
      selected = move;
    }
  }
  return selected;
}

static int cpeg_test_rack_to_array(const Rack *rack, MachineLetter *tiles) {
  int count = 0;
  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 0; ml < dist_size; ml++) {
    const int letter_count = rack_get_letter(rack, (MachineLetter)ml);
    for (int letter_idx = 0; letter_idx < letter_count; letter_idx++) {
      tiles[count++] = (MachineLetter)ml;
    }
  }
  return count;
}

static void cpeg_test_nested_round_trip(Game *game, int depth, int operation) {
  if (depth >= CPEG_UNDO_MAX_DEPTH) {
    return;
  }
  Game *before = game_duplicate(game);
  const int on_turn = game_get_player_on_turn_index(game);
  const int bag_count = bag_get_letters(game_get_bag(game));

  MoveList *moves = move_list_create(CPEG_UNDO_MOVE_CAP);
  const MoveGenArgs args = {
      .game = game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  const Move *placement = cpeg_test_random_placement(moves);

  if (operation == 0 && placement != NULL) {
    MoveUndo *move_undo = malloc(sizeof(*move_undo));
    assert(move_undo != NULL);
    play_move_incremental(placement, game, move_undo);
    CpegTestBranchUndo branch_undo;
    cpeg_test_save_branch(game, on_turn, &branch_undo);

    MachineLetter bag_tiles[MAX_BAG_SIZE];
    int available = bag_peek_tiles(game_get_bag(game), bag_tiles);
    const int tiles_played = move_get_tiles_played(placement);
    const int draw_count = tiles_played < available ? tiles_played : available;
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
      const int selected_idx =
          draw_idx +
          (int)(cpeg_test_random() % (uint32_t)(available - draw_idx));
      const MachineLetter ml = bag_tiles[selected_idx];
      bag_tiles[selected_idx] = bag_tiles[draw_idx];
      bag_draw_letter(game_get_bag(game), ml, on_turn);
      rack_add_letter(mover_rack, ml);
    }
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    cpeg_test_nested_round_trip(game, depth + 1, (operation + 1) % 3);

    cpeg_test_restore_branch(game, &branch_undo);
    cpeg_test_destroy_branch(&branch_undo);
    unplay_move_incremental(game, move_undo);
    free(move_undo);
  } else if (operation == 1 && bag_count > 0) {
    Rack *mover_rack = player_get_rack(game_get_player(game, on_turn));
    if (!rack_is_empty(mover_rack)) {
      CpegTestBranchUndo branch_undo;
      cpeg_test_save_branch(game, on_turn, &branch_undo);
      MachineLetter bag_tiles[MAX_BAG_SIZE];
      const int available = bag_peek_tiles(game_get_bag(game), bag_tiles);
      const MachineLetter drawn =
          bag_tiles[cpeg_test_random() % (uint32_t)available];
      MachineLetter rack_tiles[RACK_SIZE];
      const int rack_count = cpeg_test_rack_to_array(mover_rack, rack_tiles);
      const MachineLetter exchanged =
          rack_tiles[cpeg_test_random() % (uint32_t)rack_count];
      bag_draw_letter(game_get_bag(game), drawn, on_turn);
      rack_add_letter(mover_rack, drawn);
      rack_take_letter(mover_rack, exchanged);
      bag_add_letter(game_get_bag(game), exchanged, on_turn);
      game_start_next_player_turn(game);
      game_set_consecutive_scoreless_turns(game, 0);
      game_set_game_end_reason(game, GAME_END_REASON_NONE);
      cpeg_test_nested_round_trip(game, depth + 1, (operation + 1) % 3);
      cpeg_test_restore_branch(game, &branch_undo);
      cpeg_test_destroy_branch(&branch_undo);
    }
  } else {
    CpegTestBranchUndo branch_undo;
    cpeg_test_save_branch(game, on_turn, &branch_undo);
    game_start_next_player_turn(game);
    game_set_consecutive_scoreless_turns(game, 0);
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
    cpeg_test_nested_round_trip(game, depth + 1, (operation + 1) % 3);
    cpeg_test_restore_branch(game, &branch_undo);
    cpeg_test_destroy_branch(&branch_undo);
  }

  move_list_destroy(moves);
  cpeg_test_assert_state_equal(before, game);
  game_destroy(before);
}

static void test_cpeg_incremental_round_trip(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  rack_set_to_string(ld, player_get_rack(game_get_player(game, 1)), "DEINRST");
  const MachineLetter bag_tiles[] = {
      ld_hl_to_ml(ld, "A"),
      ld_hl_to_ml(ld, "E"),
      ld_hl_to_ml(ld, "I"),
      ld_hl_to_ml(ld, "O"),
  };
  bag_set_to_tiles(game_get_bag(game), bag_tiles,
                   (int)(sizeof(bag_tiles) / sizeof(*bag_tiles)));
  game_gen_all_cross_sets(game);
  board_set_cross_sets_valid(game_get_board(game), true);

  for (int trial = 0; trial < CPEG_UNDO_TRIALS; trial++) {
    cpeg_test_nested_round_trip(game, 0, trial % 3);
  }

  config_destroy(config);
}

// Returns the expected spread of the candidate whose label matches, or NAN when
// no such candidate is present.
static double cpeg_find_spread(const CpegPreResult *result, const char *label) {
  for (int cand_idx = 0; cand_idx < result->count; cand_idx++) {
    if (strcmp(result->cands[cand_idx].label, label) == 0) {
      return result->cands[cand_idx].expected_spread;
    }
  }
  return NAN;
}

static void test_cpeg_endgame(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_FINAL_TURN_CGP);

  Game *game = config_get_game(config);
  CpegResult result;
  const int swing = cpeg_solve_endgame(game, &result);

  assert(swing == 5);
  assert(result.swing == 5);
  assert(result.mover_score == 32);
  assert(result.reply_score == 27);
  assert(result.has_reply);
  assert_strings_equal(result.mover_str, "C4 EYAS");
  assert_strings_equal(result.reply_str, "K4 (B)hUT");

  config_destroy(config);
}

static void cpeg_assert_wtl_value(const CpegWtlValue *value, double win,
                                  double tie, double loss, double margin) {
  assert(fabs(value->win - win) < 1e-12);
  assert(fabs(value->tie - tie) < 1e-12);
  assert(fabs(value->loss - loss) < 1e-12);
  assert(fabs(value->expected_final_margin - margin) < 1e-12);
}

static void test_cpeg_wtl_value_core(void) {
  const CpegWtlValue loss = cpeg_wtl_classify_margin(-1);
  const CpegWtlValue tie = cpeg_wtl_classify_margin(0);
  const CpegWtlValue win = cpeg_wtl_classify_margin(1);
  cpeg_assert_wtl_value(&loss, 0.0, 0.0, 1.0, -1.0);
  cpeg_assert_wtl_value(&tie, 0.0, 1.0, 0.0, 0.0);
  cpeg_assert_wtl_value(&win, 1.0, 0.0, 0.0, 1.0);

  const CpegWtlValue higher_win = {
      .win = 0.31, .tie = 0.0, .loss = 0.69, .expected_final_margin = -100.0};
  const CpegWtlValue lower_win = {
      .win = 0.30, .tie = 0.6, .loss = 0.10, .expected_final_margin = 100.0};
  assert(cpeg_wtl_compare(&higher_win, &lower_win) > 0);

  // Equal win probability uses tie probability before expected margin.
  const CpegWtlValue more_ties = {
      .win = 0.3, .tie = 0.2, .loss = 0.5, .expected_final_margin = -12.0};
  const CpegWtlValue fewer_ties = {
      .win = 0.3, .tie = 0.1, .loss = 0.6, .expected_final_margin = -5.0};
  assert(cpeg_wtl_compare(&more_ties, &fewer_ties) > 0);
  const CpegWtlValue better_margin = {
      .win = 0.3, .tie = 0.2, .loss = 0.5, .expected_final_margin = -11.0};
  assert(cpeg_wtl_compare(&better_margin, &more_ties) > 0);
  const CpegWtlValue reduction_noise = {
      .win = nextafter(more_ties.win, 1.0),
      .tie = 0.1,
      .loss = 0.6,
      .expected_final_margin = -5.0,
  };
  assert(cpeg_wtl_compare(&more_ties, &reduction_noise) > 0);
  CpegWtlValue real_win_delta = reduction_noise;
  real_win_delta.win = more_ties.win + 1024.0 * DBL_EPSILON;
  assert(cpeg_wtl_compare(&real_win_delta, &more_ties) > 0);

  const CpegWtlValue values[] = {
      {.win = 1.0, .tie = 0.0, .loss = 0.0, .expected_final_margin = 10.0},
      {.win = 0.0, .tie = 1.0, .loss = 0.0, .expected_final_margin = -2.0},
  };
  const int64_t weights[] = {1, 3};
  CpegWtlValue average;
  assert(cpeg_wtl_weighted_average(values, weights, 2, &average) == 0);
  cpeg_assert_wtl_value(&average, 0.25, 0.75, 0.0, 1.0);
  assert(cpeg_wtl_weighted_average(values, weights, 0, &average) == -1);
}

static void test_cpeg_wtl_endgame_sign(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_FINAL_TURN_CGP);
  Game *game = config_get_game(config);
  const int on_turn = game_get_player_on_turn_index(game);
  Game *before = game_duplicate(game);
  CpegWtlValue value;

  assert(cpeg_solve_endgame_wtl(game, on_turn, -6, &value) == 0);
  cpeg_assert_wtl_value(&value, 0.0, 0.0, 1.0, -1.0);
  assert(cpeg_solve_endgame_wtl(game, on_turn, -5, &value) == 0);
  cpeg_assert_wtl_value(&value, 0.0, 1.0, 0.0, 0.0);
  assert(cpeg_solve_endgame_wtl(game, on_turn, -4, &value) == 0);
  cpeg_assert_wtl_value(&value, 1.0, 0.0, 0.0, 1.0);

  assert(cpeg_solve_endgame_wtl(game, 1 - on_turn, 4, &value) == 0);
  cpeg_assert_wtl_value(&value, 0.0, 0.0, 1.0, -1.0);
  assert(cpeg_solve_endgame_wtl(game, 1 - on_turn, 5, &value) == 0);
  cpeg_assert_wtl_value(&value, 0.0, 1.0, 0.0, 0.0);
  assert(cpeg_solve_endgame_wtl(game, 1 - on_turn, 6, &value) == 0);
  cpeg_assert_wtl_value(&value, 1.0, 0.0, 0.0, 1.0);
  assert(cpeg_solve_endgame_wtl(game, on_turn, INT64_MAX, &value) == -1);
  assert(cpeg_solve_endgame_wtl(game, 1 - on_turn, INT64_MIN, &value) == -1);
  cpeg_test_assert_state_equal(before, game);
  game_destroy(before);
  config_destroy(config);
}

static void test_cpeg_interval_contains_scalar(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");

  load_and_exec_config_or_die(config, CPEG_FINAL_TURN_CGP);
  Game *game = config_get_game(config);
  CpegResult leaf_result;
  const int leaf_swing = cpeg_solve_endgame(game, &leaf_result);
  const CpegInterval leaf_interval = cpeg_solve_endgame_interval(game);
  assert(leaf_interval.lo == leaf_interval.hi);
  assert(leaf_interval.lo == (double)leaf_swing);

  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  game = config_get_game(config);
  CpegIntervalRecursionStats stats;
  const int pair_count = cpeg_measure_interval_recursion(
      game, /*bag=*/1, /*allow_exchanges=*/true, &stats);
  assert(pair_count > 0);
  assert(pair_count == stats.candidate_count * stats.world_count);
  assert(stats.all_contained);
  assert(stats.maximum_width < 1e-8);
  assert(stats.total_width / (double)stats.pair_count < 1e-9);

  config_destroy(config);
}

static void test_cpeg_pre_endgame(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");

  // IMG_9570, bag 1, no exchanges: BEET's known value is reproduced exactly,
  // and the exhaustive search's optimum is the blocker 13J TAU.
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  Game *game = config_get_game(config);
  CpegPreResult result = {0};
  cpeg_solve_pre_endgame(game, /*bag=*/1, /*allow_exchanges=*/false,
                         /*num_threads=*/1, &result);
  assert(result.count > 0);
  assert(fabs(cpeg_find_spread(&result, "15J BEET") - 43.875) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "15J BEAT") - 39.125) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "13J TAU") - 44.125) < 1e-6);
  // The blocker outranks the high scorer: TAU is the exact optimum here.
  assert_strings_equal(result.cands[0].label, "13J TAU");
  assert(fabs(result.cands[0].expected_spread - 44.125) < 1e-6);
  cpeg_pre_result_destroy(&result);

  // IMG_9653, bag 1, no exchanges: top move and spread match the reference.
  load_and_exec_config_or_die(config, CPEG_PRE_9653_CGP);
  game = config_get_game(config);
  cpeg_solve_pre_endgame(game, /*bag=*/1, /*allow_exchanges=*/false,
                         /*num_threads=*/1, &result);
  assert(result.count > 0);
  assert_strings_equal(result.cands[0].label, "12G I(N)S(I)D(E)");
  assert(fabs(result.cands[0].expected_spread - 23.0) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "15F PAST(Y)") - 22.25) < 1e-6);
  cpeg_pre_result_destroy(&result);

  config_destroy(config);
}

static void cpeg_assert_wtl_results_equal(const CpegWtlResult *lhs,
                                          const CpegWtlResult *rhs) {
  assert(lhs->count == rhs->count);
  assert(lhs->worlds_distinct == rhs->worlds_distinct);
  assert(lhs->world_weight_mass == rhs->world_weight_mass);
  assert(lhs->coverage.placements == rhs->coverage.placements);
  assert(lhs->coverage.exchanges == rhs->coverage.exchanges);
  assert(lhs->coverage.passes == rhs->coverage.passes);
  assert(lhs->coverage.total == rhs->coverage.total);
  assert(lhs->coverage.generation_complete ==
         rhs->coverage.generation_complete);
  for (int candidate_idx = 0; candidate_idx < lhs->count; candidate_idx++) {
    const CpegWtlCand *lhs_candidate = &lhs->cands[candidate_idx];
    const CpegWtlCand *rhs_candidate = &rhs->cands[candidate_idx];
    assert_strings_equal(lhs_candidate->label, rhs_candidate->label);
    assert(lhs_candidate->score == rhs_candidate->score);
    assert(lhs_candidate->value.win == rhs_candidate->value.win);
    assert(lhs_candidate->value.tie == rhs_candidate->value.tie);
    assert(lhs_candidate->value.loss == rhs_candidate->value.loss);
    assert(lhs_candidate->value.expected_final_margin ==
           rhs_candidate->value.expected_final_margin);
  }
}

static void test_cpeg_wtl_pre_endgame(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  const Game *game = config_get_game(config);

  const CpegWtlArgs one_thread_args = {
      .bag = 1,
      .allow_exchanges = false,
      .num_threads = 1,
      .initial_lead = -51,
  };
  Game *before = game_duplicate(game);
  CpegWtlResult one_thread = {0};
  assert(cpeg_solve_pre_endgame_wtl(game, &one_thread_args, &one_thread) > 0);
  cpeg_test_assert_state_equal(before, game);
  game_destroy(before);

  assert(one_thread.count == one_thread.coverage.total);
  assert(one_thread.coverage.placements > 0);
  assert(one_thread.coverage.exchanges == 0);
  assert(one_thread.coverage.passes == 1);
  assert(one_thread.coverage.generation_complete);
  assert(one_thread.worlds_distinct == 5);
  assert(one_thread.world_weight_mass == 6);
  for (int candidate_idx = 0; candidate_idx < one_thread.count;
       candidate_idx++) {
    const CpegWtlCand *candidate = &one_thread.cands[candidate_idx];
    assert(fabs(candidate->value.win + candidate->value.tie +
                candidate->value.loss - 1.0) < 1e-12);
    if (candidate_idx > 0) {
      const CpegWtlCand *previous = &one_thread.cands[candidate_idx - 1];
      const int comparison =
          cpeg_wtl_compare(&previous->value, &candidate->value);
      assert(comparison >= 0);
      if (comparison == 0) {
        assert(strcmp(previous->label, candidate->label) <= 0);
      }
    }
  }

  // The explicit lead is the only score input. Changing both Game scores must
  // not affect any value; the fixed world reduction also makes 1 and 4 threads
  // bit-identical.
  player_set_score(game_get_player(game, 0), int_to_equity(1234));
  player_set_score(game_get_player(game, 1), int_to_equity(-987));
  before = game_duplicate(game);
  CpegWtlArgs four_thread_args = one_thread_args;
  four_thread_args.num_threads = 4;
  CpegWtlResult four_threads = {0};
  assert(cpeg_solve_pre_endgame_wtl(game, &four_thread_args, &four_threads) >
         0);
  cpeg_test_assert_state_equal(before, game);
  game_destroy(before);
  cpeg_assert_wtl_results_equal(&one_thread, &four_threads);

  cpeg_wtl_result_destroy(&four_threads);
  cpeg_wtl_result_destroy(&four_threads);
  cpeg_wtl_result_destroy(&one_thread);
  cpeg_wtl_result_destroy(&one_thread);
  config_destroy(config);
}

static void cpeg_assert_command_error(Config *config, ErrorStack *error_stack,
                                      const char *command,
                                      error_code_t expected_error) {
  config_load_command(config, command, error_stack);
  if (error_stack_is_empty(error_stack)) {
    config_execute_command(config, error_stack);
  }
  const error_code_t actual_error = error_stack_top(error_stack);
  if (actual_error != expected_error) {
    fprintf(stderr, "cpeg command error mismatch: %s expected=%d actual=%d\n",
            command, expected_error, actual_error);
  }
  assert(actual_error == expected_error);
  error_stack_reset(error_stack);
}

static void test_cpeg_wtl_command_parsing(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  ErrorStack *error_stack = error_stack_create();

  cpeg_assert_command_error(config, error_stack, "cpeg lead -51",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg 4 lead",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg lead 1 lead 2",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg lead -51 budget 1",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg 4 lead nope",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg 4 trace",
                            ERROR_STATUS_CONFIG_LOAD_MISSING_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg 1 replybest",
                            ERROR_STATUS_CONFIG_LOAD_MISSING_ARG);
  cpeg_assert_command_error(config, error_stack, "cpeg 1 endcache",
                            ERROR_STATUS_CONFIG_LOAD_MISSING_ARG);
  // A negative lead is consumed as the signed lead value, not mistaken for
  // the bag; the subsequent error is specifically the unsupported bag size.
  cpeg_assert_command_error(config, error_stack, "cpeg 5 lead -51",
                            ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY);

  // Exercise the real command adapter, not only the C solver API. With no
  // `belief` argument all four weighted-input fields must be the NULL/zero
  // tuple; an inline zeroed manifest's fixed-size unseen array is non-NULL and
  // used to make this otherwise-valid uniform command fail tuple validation.
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  config_load_command(config, "cpeg 1 noexch lead -51 budget 0.001",
                      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  config_load_command(
      config, "cpeg 1 noexch lead -51 budget 0.001 endcache", error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  config_load_command(
      config, "cpeg 1 noexch lead -51 budget 0.001 replybest", error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  config_load_command(config,
                      "cpeg 1 noexch lead -51 budget 0.001 trace",
                      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));

  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Renders a move to "<coord> <word>" (or "pass"), exactly as the pre-endgame
// solver labels its candidates, so labels are directly comparable.
static void cpeg_test_render(char *dest, size_t dest_size, const Board *board,
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

// The invariant the coordinator asked to prove: every placement cpeg evaluates
// is a move `generate_moves` would produce on the same board. We build the
// authoritative legal set with generate_moves(MOVE_RECORD_ALL) -- the same
// generator `gen`, `endgame`, and `peg` use -- and assert each cpeg placement
// candidate is in it. This also pins that `13J TAU` IS legal (refuting the
// "illegal candidate" hypothesis): it is one of the 410 plays generate_moves
// emits here, so cpeg is right to evaluate it.
static void test_cpeg_candidate_legality(void) {
  enum { LEGAL_CAP = 4096, LABEL_LEN = 64 };
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);

  // Authoritative legal move set (rendered labels) from generate_moves.
  MoveList *legal = move_list_create(LEGAL_CAP * 4);
  const MoveGenArgs args = {
      .game = game,
      .move_list = legal,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  static char legal_labels[LEGAL_CAP][LABEL_LEN];
  int n_legal = 0;
  const int gen_count = move_list_get_count(legal);
  for (int move_idx = 0; move_idx < gen_count && n_legal < LEGAL_CAP;
       move_idx++) {
    const Move *move = move_list_get_move(legal, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    cpeg_test_render(legal_labels[n_legal], LABEL_LEN, board, move, ld);
    n_legal++;
  }

  // 13J TAU is legal (present in the authoritative generate_moves set).
  bool tau_is_legal = false;
  for (int label_idx = 0; label_idx < n_legal; label_idx++) {
    if (strcmp(legal_labels[label_idx], "13J TAU") == 0) {
      tau_is_legal = true;
    }
  }
  assert(tau_is_legal);

  // Every cpeg placement candidate must be in the legal set.
  CpegPreResult result = {0};
  cpeg_solve_pre_endgame(game, /*bag=*/1, /*allow_exchanges=*/false,
                         /*num_threads=*/1, &result);
  assert(result.count > 0);
  for (int cand_idx = 0; cand_idx < result.count; cand_idx++) {
    const char *label = result.cands[cand_idx].label;
    if (strcmp(label, "pass") == 0 || strncmp(label, "exch:", 5) == 0) {
      continue;
    }
    bool found = false;
    for (int label_idx = 0; label_idx < n_legal; label_idx++) {
      if (strcmp(legal_labels[label_idx], label) == 0) {
        found = true;
        break;
      }
    }
    assert(found);
  }

  cpeg_pre_result_destroy(&result);
  move_list_destroy(legal);
  config_destroy(config);
}

static void cpeg_assert_score_bound_sound(Game *game) {
  enum { SCORE_BOUND_MOVE_CAP = 16384 };
  MoveList *moves = move_list_create(SCORE_BOUND_MOVE_CAP);
  const MoveGenArgs args = {
      .game = game,
      .move_list = moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);

  const int score_bound = cpeg_score_upper_bound(game_get_board(game), game);
  const int move_count = move_list_get_count(moves);
  assert(move_count < SCORE_BOUND_MOVE_CAP);
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    const Move *move = move_list_get_move(moves, move_idx);
    const int score = equity_to_int(move_get_score(move));
    assert(score <= score_bound);
  }
  move_list_destroy(moves);
}

static void test_cpeg_score_upper_bound(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  const char *const positions[] = {
      CPEG_FINAL_TURN_CGP,
      CPEG_PRE_9570_CGP,
      CPEG_PRE_9653_CGP,
  };
  for (size_t position_idx = 0;
       position_idx < sizeof(positions) / sizeof(positions[0]);
       position_idx++) {
    load_and_exec_config_or_die(config, positions[position_idx]);
    cpeg_assert_score_bound_sound(config_get_game(config));
  }

  // Exercise the same API on an immutable-style post-placement template: the
  // newly occupied squares can no longer consume their premiums.
  load_and_exec_config_or_die(config, CPEG_FINAL_TURN_CGP);
  Game *root_game = config_get_game(config);
  MoveList *root_moves = move_list_create(CPEG_UNDO_MOVE_CAP);
  const MoveGenArgs args = {
      .game = root_game,
      .move_list = root_moves,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  Move template_move;
  bool found_placement = false;
  const int root_move_count = move_list_get_count(root_moves);
  for (int move_idx = 0; move_idx < root_move_count; move_idx++) {
    const Move *move = move_list_get_move(root_moves, move_idx);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      move_copy(&template_move, move);
      found_placement = true;
      break;
    }
  }
  assert(found_placement);
  Game *template_game = game_duplicate(root_game);
  play_move_without_drawing_tiles(&template_move, template_game);
  game_gen_all_cross_sets(template_game);
  cpeg_assert_score_bound_sound(template_game);
  game_destroy(template_game);
  move_list_destroy(root_moves);

  config_destroy(config);
}

static void test_cpeg_enumeration_capacity_guard(void) {
  int counts[MAX_ALPHABET_SIZE] = {0};
  counts[1] = 2;
  counts[2] = 2;

  // The distinct 2-submultisets are {1,1}, {1,2}, and {2,2}. Exactly three
  // entries fit without overflow; a deliberately tiny capacity of two must
  // report that the third entry would otherwise have been silently dropped.
  assert(cpeg_submultiset_capacity_overflows(counts, 3, 2, 2));
  assert(!cpeg_submultiset_capacity_overflows(counts, 3, 2, 3));
}

void test_cpeg_complete_root_collection(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);

  CpegRootCoverage coverage;
  const int action_count = cpeg_count_root_actions(
      config_get_game(config), /*bag=*/4, /*allow_exchanges=*/true, &coverage);

  assert(action_count == 1314);
  assert(action_count > 1024);
  assert(coverage.placements == 1215);
  assert(coverage.exchanges == 98);
  assert(coverage.passes == 1);
  assert(coverage.total == action_count);
  assert(coverage.generation_complete);

  config_destroy(config);
}

static void cpeg_test_init_states(CpegCandState *states,
                                  const CpegInterval *priors,
                                  const CpegStableRank *ranks,
                                  int candidate_count) {
  for (int candidate_idx = 0; candidate_idx < candidate_count;
       candidate_idx++) {
    cpeg_cand_state_init(&states[candidate_idx], priors[candidate_idx],
                         &ranks[candidate_idx]);
  }
}

static void test_cpeg_coordinator_aggregation(void) {
  const int64_t weights[] = {1, 3};
  const CpegInterval priors[] = {{.lo = 0.0, .hi = 20.0},
                                 {.lo = -10.0, .hi = 10.0}};
  const CpegStableRank ranks[] = {
      {.immediate_score = 20,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "first",
       .generation_index = 0},
      {.immediate_score = 10,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "second",
       .generation_index = 1},
  };
  CpegCandState states[2];
  cpeg_test_init_states(states, priors, ranks, 2);
  CpegWorldEval evaluations[4] = {
      {.value = {.lo = 4.0, .hi = 6.0}, .resolved = true},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
  };
  CpegCoordinatorResult result;
  cpeg_coordinator_recompute(states, 2, evaluations, weights, 2, &result);
  assert(states[0].worlds_resolved == 1);
  assert(states[0].resolved_weight == 1);
  assert(states[0].expectation.lo <= 1.0);
  assert(fabs(states[0].expectation.lo - 1.0) < 1e-12);
  assert(states[0].expectation.hi >= 16.5);
  assert(fabs(states[0].expectation.hi - 16.5) < 1e-12);
  assert(states[1].expectation.lo <= -10.0);
  assert(states[1].expectation.hi >= 10.0);
  assert(result.status == CPEG_COORDINATOR_PENDING);
  assert(result.best_index == 0);
  assert(fabs(result.value_error_bound - 7.75) < 1e-12);
  assert(fabs(result.decision_regret_bound - 15.5) < 1e-12);

  evaluations[0].value = (CpegInterval){.lo = 4.0, .hi = 4.0};
  evaluations[1] =
      (CpegWorldEval){.value = {.lo = 8.0, .hi = 8.0}, .resolved = true};
  evaluations[2] =
      (CpegWorldEval){.value = {.lo = 2.0, .hi = 2.0}, .resolved = true};
  evaluations[3] =
      (CpegWorldEval){.value = {.lo = 2.0, .hi = 2.0}, .resolved = true};
  cpeg_coordinator_recompute(states, 2, evaluations, weights, 2, &result);
  assert(result.status == CPEG_COORDINATOR_EXACT_VALUES);
  assert(states[0].worlds_resolved == 2);
  assert(states[0].resolved_weight == 4);
  assert(states[0].expectation.lo <= 7.0);
  assert(states[0].expectation.hi >= 7.0);
  assert(states[0].expectation.hi - states[0].expectation.lo < 1e-12);
  assert(states[1].expectation.lo <= 2.0);
  assert(states[1].expectation.hi >= 2.0);
  assert(states[1].expectation.hi - states[1].expectation.lo < 1e-12);
  assert(result.best_index == 0);

  const CpegInterval placement =
      cpeg_placement_prior(/*score=*/16, /*bag=*/4, /*tiles_played=*/3,
                           /*score_upper_bound=*/100);
  assert(placement.lo == -284.0);
  assert(placement.hi == 316.0);
  const CpegInterval scoreless =
      cpeg_scoreless_prior(/*bag=*/4, /*score_upper_bound=*/100);
  assert(scoreless.lo == -600.0);
  assert(scoreless.hi == 600.0);
}

static void test_cpeg_coordinator_tau_and_certification(void) {
  const int64_t weights[] = {1, 1};
  const CpegInterval priors[] = {
      {.lo = 5.0, .hi = 15.0},
      {.lo = -100.0, .hi = 100.0},
      {.lo = -100.0, .hi = 5.0},
  };
  const CpegStableRank ranks[] = {
      {.immediate_score = 40,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "high-score",
       .generation_index = 0},
      {.immediate_score = 16,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "tau-shape",
       .generation_index = 1},
      {.immediate_score = 0,
       .kind = CPEG_CAND_PASS,
       .label = "pass",
       .generation_index = 2},
  };
  CpegCandState states[3];
  cpeg_test_init_states(states, priors, ranks, 3);
  CpegWorldEval evaluations[6] = {
      {.value = {.lo = 10.0, .hi = 10.0}, .resolved = true},
      {.value = {.lo = 10.0, .hi = 10.0}, .resolved = true},
      {.value = {.lo = 30.0, .hi = 30.0}, .resolved = true},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = true},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
  };
  CpegCoordinatorResult result;
  cpeg_coordinator_recompute(states, 3, evaluations, weights, 2, &result);
  assert(!states[1].eliminated);
  assert(states[1].expectation.hi >= 65.0);
  assert(result.status == CPEG_COORDINATOR_PENDING);

  evaluations[3] =
      (CpegWorldEval){.value = {.lo = 20.0, .hi = 20.0}, .resolved = true};
  cpeg_coordinator_recompute(states, 3, evaluations, weights, 2, &result);
  assert(result.status == CPEG_COORDINATOR_CERTIFIED);
  assert(result.best_index == 1);
  assert(result.unique_best);

  evaluations[5] =
      (CpegWorldEval){.value = {.lo = 1.0, .hi = 1.0}, .resolved = true};
  cpeg_coordinator_recompute(states, 3, evaluations, weights, 2, &result);
  assert(result.status == CPEG_COORDINATOR_EXACT_VALUES);
  assert(result.best_index == 1);
}

static void test_cpeg_coordinator_elimination(void) {
  const int64_t weights[] = {1, 1};
  const CpegInterval priors[] = {{.lo = 8.0, .hi = 12.0},
                                 {.lo = -5.0, .hi = 5.0}};
  const CpegStableRank ranks[] = {
      {.immediate_score = 20,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "winner",
       .generation_index = 0},
      {.immediate_score = 10,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "loser",
       .generation_index = 1},
  };
  CpegCandState states[2];
  cpeg_test_init_states(states, priors, ranks, 2);
  CpegWorldEval evaluations[4] = {
      {.value = {.lo = 10.0, .hi = 10.0}, .resolved = true},
      {.value = {.lo = 10.0, .hi = 10.0}, .resolved = true},
      {.value = {.lo = 4.0, .hi = 4.0}, .resolved = true},
      {.value = {.lo = 0.0, .hi = 0.0}, .resolved = false},
  };
  CpegCoordinatorResult result;
  cpeg_coordinator_recompute(states, 2, evaluations, weights, 2, &result);
  assert(states[1].expectation.hi < states[0].expectation.lo);
  assert(states[1].eliminated);
  assert(result.status == CPEG_COORDINATOR_CERTIFIED);
  assert(result.best_index == 0);

  evaluations[3] =
      (CpegWorldEval){.value = {.lo = 5.0, .hi = 5.0}, .resolved = true};
  cpeg_coordinator_recompute(states, 2, evaluations, weights, 2, &result);
  assert(states[1].eliminated);
  assert(result.status == CPEG_COORDINATOR_EXACT_VALUES);
  assert(result.best_index == 0);
}

static void test_cpeg_coordinator_tie(void) {
  const int64_t weights[] = {1, 3};
  const CpegInterval priors[] = {{.lo = -10.0, .hi = 10.0},
                                 {.lo = -10.0, .hi = 10.0}};
  const CpegStableRank ranks[] = {
      {.immediate_score = 10,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "stable-first",
       .generation_index = 1},
      {.immediate_score = 10,
       .kind = CPEG_CAND_PLACEMENT,
       .label = "stable-second",
       .generation_index = 0},
  };
  CpegCandState states[2];
  cpeg_test_init_states(states, priors, ranks, 2);
  const CpegWorldEval evaluations[4] = {
      {.value = {.lo = 2.0, .hi = 2.0}, .resolved = true},
      {.value = {.lo = 6.0, .hi = 6.0}, .resolved = true},
      {.value = {.lo = 2.0, .hi = 2.0}, .resolved = true},
      {.value = {.lo = 6.0, .hi = 6.0}, .resolved = true},
  };
  CpegCoordinatorResult result;
  cpeg_coordinator_recompute(states, 2, evaluations, weights, 2, &result);
  assert(result.status == CPEG_COORDINATOR_EXACT_VALUES);
  assert(result.best_index == 0);
  assert(!result.unique_best);
}

static void test_cpeg_coordinator_outward_rounding(void) {
  const int64_t weights[CPEG_COORDINATOR_RANDOM_WORLDS] = {1, 2,  3, 5,
                                                           7, 11, 13};
  int64_t total_weight = 0;
  for (int world_idx = 0; world_idx < CPEG_COORDINATOR_RANDOM_WORLDS;
       world_idx++) {
    total_weight += weights[world_idx];
  }
  const CpegInterval prior = {.lo = -1000.0, .hi = 1000.0};
  const CpegStableRank rank = {
      .immediate_score = 0,
      .kind = CPEG_CAND_PLACEMENT,
      .label = "random",
      .generation_index = 0,
  };
  CpegWorldEval evaluations[CPEG_COORDINATOR_RANDOM_WORLDS];
  CpegCoordinatorResult result;
  for (int trial = 0; trial < CPEG_COORDINATOR_RANDOM_TRIALS; trial++) {
    CpegCandState state;
    cpeg_cand_state_init(&state, prior, &rank);
    long double true_weighted_sum = 0.0L;
    for (int world_idx = 0; world_idx < CPEG_COORDINATOR_RANDOM_WORLDS;
         world_idx++) {
      const double value =
          ((double)(int32_t)cpeg_test_random() / 4294967296.0) * 1000.0;
      evaluations[world_idx] = (CpegWorldEval){
          .value = {.lo = value, .hi = value}, .resolved = true};
      true_weighted_sum += (long double)weights[world_idx] * (long double)value;
    }
    const long double true_mean = true_weighted_sum / (long double)total_weight;
    cpeg_coordinator_recompute(&state, 1, evaluations, weights,
                               CPEG_COORDINATOR_RANDOM_WORLDS, &result);
    assert((long double)state.expectation.lo <= true_mean);
    assert(true_mean <= (long double)state.expectation.hi);
    assert(result.status == CPEG_COORDINATOR_EXACT_VALUES);
  }
}

static void test_cpeg_coordinator(void) {
  test_cpeg_coordinator_aggregation();
  test_cpeg_coordinator_tau_and_certification();
  test_cpeg_coordinator_elimination();
  test_cpeg_coordinator_tie();
  test_cpeg_coordinator_outward_rounding();
}

static int cpeg_find_certified_candidate(const CpegCertifiedResult *result,
                                         const char *label) {
  for (int candidate_idx = 0; candidate_idx < result->count; candidate_idx++) {
    if (strcmp(result->cands[candidate_idx].label, label) == 0) {
      return candidate_idx;
    }
  }
  return -1;
}

static void
cpeg_assert_certified_results_equal(const CpegCertifiedResult *lhs,
                                    const CpegCertifiedResult *rhs) {
  assert(lhs->status == rhs->status);
  assert(lhs->count == rhs->count);
  assert(lhs->best_index == rhs->best_index);
  assert(lhs->worlds_total == rhs->worlds_total);
  assert(lhs->jobs_completed == rhs->jobs_completed);
  assert(lhs->batches_completed == rhs->batches_completed);
  assert(lhs->optimum_lower == rhs->optimum_lower);
  assert(lhs->optimum_upper == rhs->optimum_upper);
  assert(lhs->decision_regret_bound == rhs->decision_regret_bound);
  assert(lhs->unique_best == rhs->unique_best);
  for (int candidate_idx = 0; candidate_idx < lhs->count; candidate_idx++) {
    const CpegCertifiedCand *lhs_candidate = &lhs->cands[candidate_idx];
    const CpegCertifiedCand *rhs_candidate = &rhs->cands[candidate_idx];
    assert_strings_equal(lhs_candidate->label, rhs_candidate->label);
    assert(lhs_candidate->score == rhs_candidate->score);
    assert(lhs_candidate->estimate == rhs_candidate->estimate);
    assert(lhs_candidate->lower == rhs_candidate->lower);
    assert(lhs_candidate->upper == rhs_candidate->upper);
    assert(lhs_candidate->value_error_bound ==
           rhs_candidate->value_error_bound);
    assert(lhs_candidate->worlds_resolved == rhs_candidate->worlds_resolved);
    assert(lhs_candidate->eliminated == rhs_candidate->eliminated);
  }
}

static void
cpeg_assert_best_interval_contains_oracle(const CpegCertifiedResult *result,
                                          const CpegPreResult *oracle) {
  assert(result->best_index >= 0);
  const CpegCertifiedCand *best = &result->cands[result->best_index];
  const double exact = cpeg_find_spread(oracle, best->label);
  assert(isfinite(exact));
  assert(best->lower <= exact);
  assert(best->upper >= exact);
}

static void cpeg_assert_certified_bag1_exact(Config *config,
                                             bool allow_exchanges) {
  CpegCertifiedResult result = {0};
  const CpegCertifiedArgs args = {
      .bag = 1,
      .allow_exchanges = allow_exchanges,
      .num_threads = 4,
      .budget_seconds = 60.0,
      .batch_size = 16,
      .max_batches = 0,
  };

  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_certified(config_get_game(config), &args,
                                          &result) > 0);
  assert(result.status == CPEG_PRE_EXACT_VALUES);
  assert(result.best_index >= 0);
  assert_strings_equal(result.cands[result.best_index].label, "13J TAU");
  const int tau_idx = cpeg_find_certified_candidate(&result, "13J TAU");
  assert(tau_idx >= 0);
  assert(result.cands[tau_idx].lower <= 44.125);
  assert(result.cands[tau_idx].upper >= 44.125);
  cpeg_certified_result_destroy(&result);
}

static void cpeg_assert_certified_bag1_early_stop(Config *config) {
  CpegPreResult oracle = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame(config_get_game(config), /*bag=*/1,
                                /*allow_exchanges=*/false, /*num_threads=*/4,
                                &oracle) > 0);

  const CpegCertifiedArgs work_args = {
      .bag = 1,
      .allow_exchanges = false,
      .num_threads = 1,
      .budget_seconds = 0.0,
      .batch_size = 16,
      .max_batches = 2,
  };
  CpegCertifiedResult single_threaded = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_certified(config_get_game(config), &work_args,
                                          &single_threaded) > 0);
  assert(single_threaded.status == CPEG_PRE_ESTIMATED ||
         single_threaded.status == CPEG_PRE_CERTIFIED);
  assert(single_threaded.batches_completed == work_args.max_batches);
  cpeg_assert_best_interval_contains_oracle(&single_threaded, &oracle);

  CpegCertifiedArgs parallel_args = work_args;
  parallel_args.num_threads = 4;
  CpegCertifiedResult four_threaded = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_certified(config_get_game(config),
                                          &parallel_args, &four_threaded) > 0);
  cpeg_assert_certified_results_equal(&single_threaded, &four_threaded);

  CpegCertifiedArgs wall_args = work_args;
  wall_args.num_threads = 4;
  wall_args.budget_seconds = 0.001;
  wall_args.max_batches = 0;
  CpegCertifiedResult wall_limited = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  Game *wall_game = config_get_game(config);
  game_gen_all_cross_sets(wall_game);
  board_set_cross_sets_valid(game_get_board(wall_game), true);
  Game *before = game_duplicate(wall_game);
  assert(cpeg_solve_pre_endgame_certified(wall_game, &wall_args,
                                          &wall_limited) > 0);
  cpeg_test_assert_state_equal(before, wall_game);
  game_destroy(before);
  assert(wall_limited.status == CPEG_PRE_ESTIMATED ||
         wall_limited.status == CPEG_PRE_CERTIFIED ||
         wall_limited.status == CPEG_PRE_EXACT_VALUES);
  cpeg_assert_best_interval_contains_oracle(&wall_limited, &oracle);
  cpeg_certified_result_destroy(&wall_limited);
  cpeg_certified_result_destroy(&four_threaded);
  cpeg_certified_result_destroy(&single_threaded);
  cpeg_pre_result_destroy(&oracle);
}

void test_cpeg_certified_bag1(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  cpeg_assert_certified_bag1_exact(config, /*allow_exchanges=*/false);
  cpeg_assert_certified_bag1_exact(config, /*allow_exchanges=*/true);
  cpeg_assert_certified_bag1_early_stop(config);
  config_destroy(config);
}

static void
cpeg_assert_statistical_results_equal(const CpegStatisticalResult *lhs,
                                      const CpegStatisticalResult *rhs) {
  assert(lhs->status == rhs->status);
  assert(lhs->count == rhs->count);
  assert(lhs->best_index == rhs->best_index);
  assert(lhs->worlds_sampled == rhs->worlds_sampled);
  assert(lhs->worlds_total == rhs->worlds_total);
  assert(lhs->jobs_completed == rhs->jobs_completed);
  assert(lhs->rounds_completed == rhs->rounds_completed);
  assert(lhs->confidence == rhs->confidence);
  assert(lhs->seed == rhs->seed);
  for (int sample_idx = 0; sample_idx < lhs->worlds_sampled; sample_idx++) {
    assert(lhs->sampled_world_indices[sample_idx] ==
           rhs->sampled_world_indices[sample_idx]);
    assert(lhs->sampled_world_weights[sample_idx] ==
           rhs->sampled_world_weights[sample_idx]);
  }
  for (int candidate_idx = 0; candidate_idx < lhs->count; candidate_idx++) {
    const CpegStatisticalCand *lhs_candidate = &lhs->cands[candidate_idx];
    const CpegStatisticalCand *rhs_candidate = &rhs->cands[candidate_idx];
    assert_strings_equal(lhs_candidate->label, rhs_candidate->label);
    assert(lhs_candidate->score == rhs_candidate->score);
    assert(lhs_candidate->estimate == rhs_candidate->estimate);
    assert(lhs_candidate->lower == rhs_candidate->lower);
    assert(lhs_candidate->upper == rhs_candidate->upper);
    assert(lhs_candidate->sample_variance == rhs_candidate->sample_variance);
    assert(lhs_candidate->worlds_sampled == rhs_candidate->worlds_sampled);
    assert(lhs_candidate->eliminated == rhs_candidate->eliminated);
    assert(lhs_candidate->elimination_round ==
           rhs_candidate->elimination_round);
  }
}

void test_cpeg_statistical_bag1(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");
  const CpegStatisticalArgs full_args = {
      .bag = 1,
      .allow_exchanges = false,
      .num_threads = 4,
      .budget_seconds = 60.0,
      .seed = 42,
      .confidence = 0.95,
      .max_worlds = 0,
  };
  CpegStatisticalResult full = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_statistical(config_get_game(config), &full_args,
                                            &full) > 0);
  assert(full.status == CPEG_PRE_STATISTICAL);
  assert(full.worlds_sampled == 7);
  assert(full.worlds_total == 7);
  assert_strings_equal(full.cands[full.best_index].label, "13J TAU");
  int full_tau_idx = -1;
  for (int candidate_idx = 0; candidate_idx < full.count; candidate_idx++) {
    if (strcmp(full.cands[candidate_idx].label, "13J TAU") == 0) {
      full_tau_idx = candidate_idx;
      break;
    }
  }
  assert(full_tau_idx >= 0);
  assert(fabs(full.cands[full_tau_idx].estimate - 44.125) < 1e-12);
  assert(fabs(full.cands[full_tau_idx].upper - full.cands[full_tau_idx].lower) <
         1e-12);
  assert(!full.cands[full_tau_idx].eliminated);

  CpegStatisticalArgs subset_args = full_args;
  subset_args.max_worlds = 4;
  CpegStatisticalResult deterministic_one = {0};
  CpegStatisticalResult deterministic_two = {0};
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_statistical(
             config_get_game(config), &subset_args, &deterministic_one) > 0);
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_statistical(
             config_get_game(config), &subset_args, &deterministic_two) > 0);
  cpeg_assert_statistical_results_equal(&deterministic_one, &deterministic_two);

  // Recover TAU's seven exact in-world values once from deterministic prefixes.
  // The 500-seed coverage loop below then exercises only the sampling/CI core,
  // rather than re-solving the same perfect-information worlds 500 times.
  double tau_values[7] = {0};
  int64_t tau_weights[7] = {0};
  double prior_weighted_sum = 0.0;
  int64_t prior_weight_sum = 0;
  for (int prefix = 1; prefix <= 7; prefix++) {
    CpegStatisticalArgs prefix_args = full_args;
    prefix_args.max_worlds = prefix;
    CpegStatisticalResult prefix_result = {0};
    load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
    assert(cpeg_solve_pre_endgame_statistical(
               config_get_game(config), &prefix_args, &prefix_result) > 0);
    int tau_idx = -1;
    for (int candidate_idx = 0; candidate_idx < prefix_result.count;
         candidate_idx++) {
      if (strcmp(prefix_result.cands[candidate_idx].label, "13J TAU") == 0) {
        tau_idx = candidate_idx;
        break;
      }
    }
    assert(tau_idx >= 0);
    const int sample_idx = prefix - 1;
    const int world_idx = prefix_result.sampled_world_indices[sample_idx];
    const int64_t weight = prefix_result.sampled_world_weights[sample_idx];
    const int64_t weight_sum = prior_weight_sum + weight;
    const double weighted_sum =
        prefix_result.cands[tau_idx].estimate * (double)weight_sum;
    tau_values[world_idx] =
        (weighted_sum - prior_weighted_sum) / (double)weight;
    tau_weights[world_idx] = weight;
    prior_weighted_sum = weighted_sum;
    prior_weight_sum = weight_sum;
    cpeg_statistical_result_destroy(&prefix_result);
  }

  int covered = 0;
  const int coverage_seeds = 500;
  for (int seed = 0; seed < coverage_seeds; seed++) {
    CpegStatisticalCand estimate;
    assert(cpeg_statistical_resample_values(tau_values, tau_weights, 7, 4,
                                            (uint64_t)seed, 0.95,
                                            &estimate) > 0);
    if (estimate.lower <= 44.125 && 44.125 <= estimate.upper) {
      covered++;
    }
  }
  assert(covered >= 475);

  int tau_chosen = 0;
  const int best_arm_seeds = 8;
  for (int seed = 0; seed < best_arm_seeds; seed++) {
    CpegStatisticalArgs best_args = full_args;
    best_args.seed = (uint64_t)seed;
    best_args.max_worlds = 7;
    CpegStatisticalResult result = {0};
    load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
    assert(cpeg_solve_pre_endgame_statistical(config_get_game(config),
                                              &best_args, &result) > 0);
    int tau_idx = -1;
    for (int candidate_idx = 0; candidate_idx < result.count; candidate_idx++) {
      if (strcmp(result.cands[candidate_idx].label, "13J TAU") == 0) {
        tau_idx = candidate_idx;
        break;
      }
    }
    assert(tau_idx >= 0);
    assert(!result.cands[tau_idx].eliminated);
    if (result.best_index == tau_idx) {
      tau_chosen++;
    }
    cpeg_statistical_result_destroy(&result);
  }
  assert(tau_chosen == best_arm_seeds);
  printf("cpegstat coverage=%d/%d (%.1f%%), best=%d/%d, tau_eliminated=0\n",
         covered, coverage_seeds,
         100.0 * (double)covered / (double)coverage_seeds, tau_chosen,
         best_arm_seeds);
  cpeg_statistical_result_destroy(&deterministic_two);
  cpeg_statistical_result_destroy(&deterministic_one);
  cpeg_statistical_result_destroy(&full);
  config_destroy(config);
}

static const CpegWtlCand *cpeg_find_wtl_candidate(const CpegWtlResult *result,
                                                  const char *label) {
  for (int candidate_idx = 0; candidate_idx < result->count; candidate_idx++) {
    if (strcmp(result->cands[candidate_idx].label, label) == 0) {
      return &result->cands[candidate_idx];
    }
  }
  return NULL;
}

void test_cpeg_wtl_senator(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 4");
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  const Game *game = config_get_game(config);
  Game *before = game_duplicate(game);
  const CpegWtlArgs args = {
      .bag = 4,
      .allow_exchanges = true,
      .num_threads = 4,
      .initial_lead = -51,
  };
  CpegWtlResult result = {0};
  assert(cpeg_solve_pre_endgame_wtl(game, &args, &result) == 1314);
  cpeg_test_assert_state_equal(before, game);
  game_destroy(before);

  assert(result.coverage.placements == 1215);
  assert(result.coverage.exchanges == 98);
  assert(result.coverage.passes == 1);
  assert(result.coverage.total == 1314);
  assert(result.coverage.generation_complete);
  assert(result.worlds_distinct == 246);
  assert(result.world_weight_mass == 330);
  assert_strings_equal(result.cands[0].label, "13F TOSA");

  const CpegWtlCand *tosa = cpeg_find_wtl_candidate(&result, "13F TOSA");
  const CpegWtlCand *atoners = cpeg_find_wtl_candidate(&result, "13B ATONERS");
  assert(tosa != NULL);
  assert(atoners != NULL);
  assert(fabs(tosa->value.win - 42.0 / 330.0) < 1e-12);
  assert(fabs(tosa->value.tie) < 1e-12);
  assert(fabs(tosa->value.loss - 288.0 / 330.0) < 1e-12);
  assert(fabs(tosa->value.expected_final_margin - (-43.5333333333333)) < 1e-5);
  assert(fabs(atoners->value.win - 36.0 / 330.0) < 1e-12);
  assert(fabs(atoners->value.tie - 3.0 / 330.0) < 1e-12);
  assert(fabs(atoners->value.loss - 291.0 / 330.0) < 1e-12);
  assert(fabs(atoners->value.expected_final_margin - (-21.6393939393939)) <
         1e-5);
  printf("cpegwtl best=%s worlds=%d mass=%lld\n", result.cands[0].label,
         result.worlds_distinct, (long long)result.world_weight_mass);

  cpeg_wtl_result_destroy(&result);
  config_destroy(config);
}

static void cpeg_assert_wtl_certified_candidate_valid(
    const CpegWtlCertifiedCand *candidate) {
  assert(candidate->outcome_den > 0);
  assert(candidate->win_lower_num >= 0);
  assert(candidate->win_lower_num <= candidate->win_upper_num);
  assert(candidate->win_upper_num <= candidate->outcome_den);
  assert(candidate->tie_lower_num >= 0);
  assert(candidate->tie_lower_num <= candidate->tie_upper_num);
  assert(candidate->tie_upper_num <= candidate->outcome_den);
  assert(candidate->loss_lower_num >= 0);
  assert(candidate->loss_lower_num <= candidate->loss_upper_num);
  assert(candidate->loss_upper_num <= candidate->outcome_den);
  assert(candidate->outcome.estimate.win >= candidate->outcome.win.lo);
  assert(candidate->outcome.estimate.win <= candidate->outcome.win.hi);
  assert(candidate->outcome.estimate.tie >= candidate->outcome.tie.lo);
  assert(candidate->outcome.estimate.tie <= candidate->outcome.tie.hi);
  assert(candidate->outcome.estimate.loss >= candidate->outcome.loss.lo);
  assert(candidate->outcome.estimate.loss <= candidate->outcome.loss.hi);
  assert(fabs(candidate->outcome.estimate.win +
              candidate->outcome.estimate.tie +
              candidate->outcome.estimate.loss - 1.0) < 1e-15);
}

static void
cpeg_assert_wtl_certified_results_equal(const CpegWtlCertifiedResult *lhs,
                                        const CpegWtlCertifiedResult *rhs) {
  assert(lhs->status == rhs->status);
  assert(lhs->count == rhs->count);
  assert(lhs->best_index == rhs->best_index);
  assert(lhs->worlds_distinct == rhs->worlds_distinct);
  assert(lhs->world_weight_mass == rhs->world_weight_mass);
  assert(lhs->exact_jobs == rhs->exact_jobs);
  assert(lhs->bound_jobs == rhs->bound_jobs);
  assert(lhs->batches_completed == rhs->batches_completed);
  assert(lhs->regret_num == rhs->regret_num);
  assert(lhs->regret_den == rhs->regret_den);
  assert(lhs->unique_best == rhs->unique_best);
  assert(lhs->coverage.total == rhs->coverage.total);
  for (int candidate_idx = 0; candidate_idx < lhs->count; candidate_idx++) {
    const CpegWtlCertifiedCand *lhs_candidate = &lhs->cands[candidate_idx];
    const CpegWtlCertifiedCand *rhs_candidate = &rhs->cands[candidate_idx];
    assert_strings_equal(lhs_candidate->label, rhs_candidate->label);
    assert(lhs_candidate->outcome_den == rhs_candidate->outcome_den);
    assert(lhs_candidate->win_lower_num == rhs_candidate->win_lower_num);
    assert(lhs_candidate->win_upper_num == rhs_candidate->win_upper_num);
    assert(lhs_candidate->tie_lower_num == rhs_candidate->tie_lower_num);
    assert(lhs_candidate->tie_upper_num == rhs_candidate->tie_upper_num);
    assert(lhs_candidate->loss_lower_num == rhs_candidate->loss_lower_num);
    assert(lhs_candidate->loss_upper_num == rhs_candidate->loss_upper_num);
    assert(lhs_candidate->exact_weight == rhs_candidate->exact_weight);
    assert(lhs_candidate->bounded_weight == rhs_candidate->bounded_weight);
    assert(lhs_candidate->unresolved_weight ==
           rhs_candidate->unresolved_weight);
  }
}

static void cpeg_assert_wtl_certified_results_byte_equal(
    const CpegWtlCertifiedResult *lhs,
    const CpegWtlCertifiedResult *rhs) {
  cpeg_assert_wtl_certified_results_equal(lhs, rhs);
  for (int candidate_idx = 0; candidate_idx < lhs->count; candidate_idx++) {
    const CpegWtlCertifiedCand *lhs_candidate = &lhs->cands[candidate_idx];
    const CpegWtlCertifiedCand *rhs_candidate = &rhs->cands[candidate_idx];
    assert(lhs_candidate->score == rhs_candidate->score);
    assert(memcmp(&lhs_candidate->outcome, &rhs_candidate->outcome,
                  sizeof(lhs_candidate->outcome)) == 0);
    assert(lhs_candidate->worlds_exact == rhs_candidate->worlds_exact);
    assert(lhs_candidate->worlds_bounded == rhs_candidate->worlds_bounded);
    assert(lhs_candidate->worlds_unresolved ==
           rhs_candidate->worlds_unresolved);
    assert(lhs_candidate->eliminated == rhs_candidate->eliminated);
  }
}

static void test_cpeg_belief_manifest_parser(const LetterDistribution *ld) {
  char *path = string_duplicate("cpeg_belief_manifest_test.txt");
  FILE *stream = fopen(path, "w");
  assert(stream != NULL);
  int write_result =
      fputs("cpeg-belief-v1\n"
            "posterior test_mid_v1\n"
            "unseen ?TUUVY\n"
            "bag 1\n"
            "worlds 2\n"
            "mass 8\n"
            "digest "
            "8e733a69153f247f23cbd35bb786d6c3cb9720e0920b004ee6f764896d6112b6\n"
            "world T 1\n"
            "world ? 7\n",
            stream);
  assert(write_result >= 0);
  int close_result = fclose(stream);
  assert(close_result == 0);

  CpegBeliefManifest manifest = {0};
  assert(cpeg_belief_manifest_load(path, ld, 1, &manifest));
  assert(manifest.world_count == 2);
  assert(manifest.weight_mass == 8);
  assert_strings_equal(manifest.posterior_id, "test_mid_v1");
  assert_strings_equal(manifest.unseen_tiles, "?TUUVY");
  cpeg_belief_manifest_destroy(&manifest);

  stream = fopen(path, "w");
  assert(stream != NULL);
  // Keep the declared count and mass but alter the weighted support. The
  // canonical world digest must bind the rows the solver will actually use.
  write_result =
      fputs("cpeg-belief-v1\n"
            "posterior test_mid_v1\n"
            "unseen ?TUUVY\n"
            "bag 1\n"
            "worlds 2\n"
            "mass 8\n"
            "digest "
            "8e733a69153f247f23cbd35bb786d6c3cb9720e0920b004ee6f764896d6112b6\n"
            "world T 2\n"
            "world ? 6\n",
            stream);
  assert(write_result >= 0);
  close_result = fclose(stream);
  assert(close_result == 0);
  assert(!cpeg_belief_manifest_load(path, ld, 1, &manifest));

  stream = fopen(path, "w");
  assert(stream != NULL);
  // Upper-case hex is intentionally outside the canonical Python/C wire
  // contract, so a caller cannot spell one digest two different ways.
  write_result =
      fputs("cpeg-belief-v1\n"
            "posterior test_mid_v1\n"
            "unseen ?TUUVY\n"
            "bag 1\n"
            "worlds 1\n"
            "mass 1\n"
            "digest "
            "ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n"
            "world ? 1\n",
            stream);
  assert(write_result >= 0);
  close_result = fclose(stream);
  assert(close_result == 0);
  assert(!cpeg_belief_manifest_load(path, ld, 1, &manifest));

  stream = fopen(path, "w");
  assert(stream != NULL);
  write_result =
      fputs("cpeg-belief-v1\n"
            "posterior test_mid_v1\n"
            "unseen ?TUUVY\n"
            "bag 1\n"
            "worlds 1\n"
            "mass 999999999999999999999999999999999999\n"
            "digest "
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
            "world ? 1\n",
            stream);
  assert(write_result >= 0);
  close_result = fclose(stream);
  assert(close_result == 0);
  assert(!cpeg_belief_manifest_load(path, ld, 1, &manifest));
  delete_file(path);
  free(path);
}

static const CpegWtlCertifiedCand *
cpeg_find_wtl_certified_candidate(const CpegWtlCertifiedResult *result,
                                  const char *label) {
  for (int candidate_idx = 0; candidate_idx < result->count; candidate_idx++) {
    if (strcmp(result->cands[candidate_idx].label, label) == 0) {
      return &result->cands[candidate_idx];
    }
  }
  return NULL;
}

void test_cpeg_wtl_certified_proof(void) {
  const CpegInterval prior = {.lo = -100.0, .hi = 100.0};
  const CpegWtlEnvelope proved_loss =
      cpeg_wtl_envelope_from_margin_upper(-1, prior);
  assert(proved_loss.win.lo == 0.0 && proved_loss.win.hi == 0.0);
  assert(proved_loss.tie.lo == 0.0 && proved_loss.tie.hi == 0.0);
  assert(proved_loss.loss.lo == 1.0 && proved_loss.loss.hi == 1.0);
  const CpegWtlEnvelope no_strict_win =
      cpeg_wtl_envelope_from_margin_upper(0, prior);
  assert(no_strict_win.win.lo == 0.0 && no_strict_win.win.hi == 0.0);
  assert(!cpeg_wtl_envelope_dominates(&no_strict_win, &proved_loss));
  assert(!cpeg_wtl_envelope_dominates(&proved_loss, &no_strict_win));
  const CpegWtlEnvelope exact_tie = {
      .estimate = {.tie = 1.0},
      .win = {.lo = 0.0, .hi = 0.0},
      .tie = {.lo = 1.0, .hi = 1.0},
      .loss = {.lo = 0.0, .hi = 0.0},
      .expected_final_margin = {.lo = 0.0, .hi = 0.0},
  };
  assert(cpeg_wtl_envelope_dominates(&exact_tie, &proved_loss));

  Config *config = config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 1");
  const CpegWtlCertifiedArgs one_thread_args = {
      .bag = 4,
      .allow_exchanges = true,
      .num_threads = 1,
      .initial_lead = -51,
      .budget_seconds = 0.0,
      .batch_size = 1,
      .max_batches = 1,
  };
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  CpegWtlCertifiedResult one_thread = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &one_thread_args, &one_thread) == 1314);
  assert(one_thread.status == CPEG_PRE_BOUNDED);
  assert(one_thread.coverage.placements == 1215);
  assert(one_thread.coverage.exchanges == 98);
  assert(one_thread.coverage.passes == 1);
  assert(one_thread.coverage.total == 1314);
  assert(one_thread.coverage.generation_complete);
  assert(one_thread.worlds_distinct == 246);
  assert(one_thread.world_weight_mass == 330);
  assert(one_thread.candidate_traces == NULL);
  const CpegWtlTrace empty_trace = {0};
  assert(memcmp(&one_thread.trace, &empty_trace, sizeof(empty_trace)) == 0);
  for (int candidate_idx = 0; candidate_idx < one_thread.count;
       candidate_idx++) {
    cpeg_assert_wtl_certified_candidate_valid(&one_thread.cands[candidate_idx]);
    const CpegWtlCertifiedCand *candidate = &one_thread.cands[candidate_idx];
    assert(candidate->exact_weight + candidate->bounded_weight +
               candidate->unresolved_weight ==
           one_thread.world_weight_mass);
  }

  CpegWtlCertifiedArgs four_thread_args = one_thread_args;
  four_thread_args.num_threads = 4;
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  CpegWtlCertifiedResult four_threads = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config),
                                              &four_thread_args,
                                              &four_threads) == 1314);
  cpeg_assert_wtl_certified_results_equal(&one_thread, &four_threads);

  CpegWtlCertifiedArgs two_ply_args = one_thread_args;
  two_ply_args.use_exact_two_ply_incumbent = true;
  two_ply_args.collect_trace = true;
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  CpegWtlCertifiedResult two_ply = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &two_ply_args, &two_ply) == 1314);
  cpeg_assert_wtl_certified_results_byte_equal(&one_thread, &two_ply);
  assert(two_ply.trace.exact_endgame_queries == 1);
  assert(two_ply.trace.final_reply_queries == 0);

  CpegWtlCertifiedArgs trace_args = one_thread_args;
  trace_args.collect_trace = true;
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  CpegWtlCertifiedResult traced = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &trace_args, &traced) == 1314);
  cpeg_assert_wtl_certified_results_equal(&one_thread, &traced);
  assert(traced.trace.wall_ns > 0);
  assert(traced.trace.setup_ns > 0);
  assert(traced.trace.root_actions == 1314);
  assert(traced.trace.challengers == 1313);
  assert(traced.trace.worlds == 246);
  assert(traced.trace.opponent_information_states == 246);
  assert(traced.trace.compute_participant_capacity == 1);
  assert(traced.trace.public_state_batches == 0);
  assert(traced.trace.scheduler_batches > 0);
  assert(traced.trace.defense_world_jobs > 0);
  assert(traced.trace.opponent_movegen_calls > 0);
  assert(traced.trace.opponent_moves_generated > 0);
  assert(traced.trace.opponent_sort_calls > 0);
  assert(traced.trace.final_reply_queries > 0);
  assert(traced.candidate_traces != NULL);
  int traced_candidates = 0;
  int64_t candidate_reply_queries = 0;
  int64_t candidate_exact_endgame_queries = 0;
  int64_t candidate_exact_endgame_cache_hits = 0;
  int64_t candidate_fixed_endgame_queries = 0;
  int64_t candidate_fixed_endgame_cache_hits = 0;
  for (int candidate_idx = 0; candidate_idx < traced.count;
       candidate_idx++) {
    const CpegWtlTrace *candidate_trace =
        &traced.candidate_traces[candidate_idx];
    if (candidate_trace->defense_world_jobs > 0) {
      traced_candidates++;
    }
    candidate_reply_queries += candidate_trace->final_reply_queries;
    candidate_exact_endgame_queries += candidate_trace->exact_endgame_queries;
    candidate_exact_endgame_cache_hits +=
        candidate_trace->exact_endgame_cache_hits;
    candidate_fixed_endgame_queries += candidate_trace->fixed_endgame_queries;
    candidate_fixed_endgame_cache_hits +=
        candidate_trace->fixed_endgame_cache_hits;
  }
  assert(traced_candidates > 0);
  assert(candidate_reply_queries == traced.trace.final_reply_queries);
  assert(candidate_exact_endgame_queries ==
         traced.trace.exact_endgame_queries);
  assert(candidate_exact_endgame_cache_hits ==
         traced.trace.exact_endgame_cache_hits);
  assert(candidate_fixed_endgame_queries ==
         traced.trace.fixed_endgame_queries);
  assert(candidate_fixed_endgame_cache_hits ==
         traced.trace.fixed_endgame_cache_hits);
  cpeg_wtl_certified_result_destroy(&traced);
  cpeg_wtl_certified_result_destroy(&two_ply);
  cpeg_wtl_certified_result_destroy(&four_threads);
  cpeg_wtl_certified_result_destroy(&one_thread);

  // A supplied posterior changes exact world mass without changing the
  // public inventory. This catches implementations that merely echo a model
  // label while continuing to enumerate the neutral prior internally.
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  const LetterDistribution *ld = game_get_ld(config_get_game(config));
  test_cpeg_belief_manifest_parser(ld);
  CpegWeightedWorld weighted_worlds[2] = {
      {
          .bag_tiles = {ld_hl_to_ml(ld, "?")},
          .bag_count = 1,
          .weight = 7,
      },
      {
          .bag_tiles = {ld_hl_to_ml(ld, "T")},
          .bag_count = 1,
          .weight = 1,
      },
  };
  MachineLetter weighted_unseen[RACK_SIZE + PEG_MAX_BAG] = {0};
  const int weighted_unseen_count = ld_str_to_mls(
      ld, "?TUUVY", false, weighted_unseen, RACK_SIZE + PEG_MAX_BAG);
  assert(weighted_unseen_count == 6);
  const CpegWtlCertifiedArgs weighted_args = {
      .bag = 1,
      .allow_exchanges = false,
      .num_threads = 1,
      .initial_lead = -51,
      .budget_seconds = 0.0,
      .batch_size = 1,
      .max_batches = 1,
      .weighted_worlds = weighted_worlds,
      .weighted_world_count = 2,
      .weighted_unseen_tiles = weighted_unseen,
      .weighted_unseen_count = weighted_unseen_count,
  };
  CpegWtlCertifiedResult weighted_result = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &weighted_args, &weighted_result) > 0);
  assert(weighted_result.worlds_distinct == 2);
  assert(weighted_result.world_weight_mass == 8);
  cpeg_wtl_certified_result_destroy(&weighted_result);

  CpegWeightedWorld duplicate_worlds[2] = {
      weighted_worlds[0],
      weighted_worlds[0],
  };
  CpegWtlCertifiedArgs duplicate_args = weighted_args;
  duplicate_args.weighted_worlds = duplicate_worlds;
  CpegWtlCertifiedResult duplicate_result = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &duplicate_args, &duplicate_result) < 0);
  cpeg_wtl_certified_result_destroy(&duplicate_result);

  MachineLetter wrong_unseen[RACK_SIZE + PEG_MAX_BAG] = {0};
  memcpy(wrong_unseen, weighted_unseen, sizeof(weighted_unseen));
  wrong_unseen[0] = ld_hl_to_ml(ld, "Z");
  CpegWtlCertifiedArgs wrong_inventory_args = weighted_args;
  wrong_inventory_args.weighted_unseen_tiles = wrong_unseen;
  CpegWtlCertifiedResult wrong_inventory_result = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config),
                                              &wrong_inventory_args,
                                              &wrong_inventory_result) < 0);
  cpeg_wtl_certified_result_destroy(&wrong_inventory_result);

  const CpegWtlCertifiedArgs compact_args = {
      .bag = 1,
      .allow_exchanges = false,
      .num_threads = 4,
      .initial_lead = -50,
      .budget_seconds = 10.0,
  };
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  CpegWtlCertifiedResult compact = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config),
                                              &compact_args, &compact) > 0);
  assert(compact.status == CPEG_PRE_CERTIFIED);
  assert(compact.unique_best);
  assert_strings_equal(compact.cands[compact.best_index].label, "15J BEET");
  assert(compact.cands[compact.best_index].win_lower_num == 1);
  assert(compact.cands[compact.best_index].win_upper_num == 1);
  assert(compact.cands[compact.best_index].outcome_den == 8);
  assert(compact.regret_num == 0);
  assert(compact.regret_den == 1);
  CpegWtlCertifiedArgs compact_two_ply_args = compact_args;
  compact_two_ply_args.use_exact_two_ply_incumbent = true;
  compact_two_ply_args.collect_trace = true;
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  CpegWtlCertifiedResult compact_two_ply = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &compact_two_ply_args,
             &compact_two_ply) > 0);
  cpeg_assert_wtl_certified_results_byte_equal(&compact, &compact_two_ply);
  assert(compact_two_ply.trace.exact_endgame_queries > 0);
  CpegWtlCertifiedArgs compact_cached_args = compact_two_ply_args;
  compact_cached_args.use_exact_endgame_cache = true;
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  CpegWtlCertifiedResult compact_cached = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &compact_cached_args,
             &compact_cached) > 0);
  cpeg_assert_wtl_certified_results_byte_equal(&compact, &compact_cached);
  assert(compact_cached.trace.exact_endgame_queries > 0);
  assert(compact_cached.trace.exact_endgame_cache_hits <=
         compact_cached.trace.exact_endgame_queries);
  cpeg_wtl_certified_result_destroy(&compact_cached);
  cpeg_wtl_certified_result_destroy(&compact_two_ply);
  cpeg_wtl_certified_result_destroy(&compact);

  // Exchange draws come from the pre-exchange bag: returned tiles are not
  // eligible to be redrawn on the same turn. Check every bag-one exchange
  // enclosure against the exact W/T/L oracle, and require a contracted bound
  // so this is not satisfied by an untouched [0, 1] prior.
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  const CpegWtlArgs exchange_oracle_args = {
      .bag = 1,
      .allow_exchanges = true,
      .num_threads = 4,
      .initial_lead = -51,
  };
  CpegWtlResult exchange_oracle = {0};
  assert(cpeg_solve_pre_endgame_wtl(config_get_game(config),
                                    &exchange_oracle_args,
                                    &exchange_oracle) > 0);
  const CpegWtlCertifiedArgs exchange_bound_args = {
      .bag = 1,
      .allow_exchanges = true,
      .num_threads = 4,
      .initial_lead = -51,
      .budget_seconds = 10.0,
  };
  CpegWtlCertifiedResult exchange_bounds = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config),
                                              &exchange_bound_args,
                                              &exchange_bounds) > 0);
  int exchanges_checked = 0;
  bool saw_contracted_exchange = false;
  for (int candidate_idx = 0; candidate_idx < exchange_oracle.count;
       candidate_idx++) {
    const CpegWtlCand *exact = &exchange_oracle.cands[candidate_idx];
    if (strncmp(exact->label, "exch:", 5) != 0) {
      continue;
    }
    const CpegWtlCertifiedCand *bounded =
        cpeg_find_wtl_certified_candidate(&exchange_bounds, exact->label);
    assert(bounded != NULL);
    const int64_t exact_win =
        (int64_t)llround(exact->value.win * (double)bounded->outcome_den);
    const int64_t exact_tie_mass =
        (int64_t)llround(exact->value.tie * (double)bounded->outcome_den);
    const int64_t exact_loss =
        (int64_t)llround(exact->value.loss * (double)bounded->outcome_den);
    assert(bounded->win_lower_num <= exact_win);
    assert(exact_win <= bounded->win_upper_num);
    assert(bounded->tie_lower_num <= exact_tie_mass);
    assert(exact_tie_mass <= bounded->tie_upper_num);
    assert(bounded->loss_lower_num <= exact_loss);
    assert(exact_loss <= bounded->loss_upper_num);
    if (bounded->win_upper_num < bounded->outcome_den) {
      saw_contracted_exchange = true;
    }
    exchanges_checked++;
  }
  assert(exchanges_checked > 0);
  assert(saw_contracted_exchange);
  cpeg_wtl_certified_result_destroy(&exchange_bounds);
  cpeg_wtl_result_destroy(&exchange_oracle);
  config_destroy(config);
}

void test_cpeg_replybest_screen(void) {
  // A threshold witness proves only that the exact reply is above the
  // threshold. This complete bag-one control pins the selected action and
  // exact incumbent W/T/L mass while exercising the optimized cache path.
  Config *config = config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 4");
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  const CpegWtlCertifiedArgs baseline_args = {
      .bag = 1,
      .allow_exchanges = true,
      .num_threads = 4,
      .initial_lead = 0,
      .budget_seconds = 5.0,
      .collect_trace = true,
  };
  CpegWtlCertifiedResult baseline = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &baseline_args, &baseline) == 204);
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  CpegWtlCertifiedArgs args = baseline_args;
  args.use_threshold_reply_screen = true;
  CpegWtlCertifiedResult result = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config), &args,
                                              &result) == 204);
  cpeg_assert_wtl_certified_results_byte_equal(&baseline, &result);
  assert_strings_equal(result.cands[result.best_index].label, "C4 EYAS");
  assert(result.cands[result.best_index].win_lower_num == 6);
  assert(result.cands[result.best_index].win_upper_num == 6);
  assert(result.cands[result.best_index].outcome_den == 6);
  assert(result.trace.threshold_short_circuits > 0);
  int64_t phase_queries = 0;
  int64_t phase_replies_generated = 0;
  int64_t phase_cache_hits = 0;
  int64_t phase_movegen_work_ns = 0;
  int64_t phase_threshold_short_circuits = 0;
  for (int phase = 0; phase < CPEG_WTL_REPLY_PHASE_COUNT; phase++) {
    const CpegWtlReplyPhaseTrace *phase_trace =
        &result.trace.reply_phases[phase];
    phase_queries += phase_trace->queries;
    phase_replies_generated += phase_trace->replies_generated;
    phase_cache_hits += phase_trace->cache_hits;
    phase_movegen_work_ns += phase_trace->movegen_work_ns;
    phase_threshold_short_circuits +=
        phase_trace->threshold_short_circuits;
  }
  assert(phase_queries == result.trace.final_reply_queries);
  assert(phase_replies_generated == result.trace.final_replies_generated);
  assert(phase_cache_hits == result.trace.final_reply_cache_hits);
  assert(phase_movegen_work_ns == result.trace.final_reply_movegen_work_ns);
  assert(phase_threshold_short_circuits ==
         result.trace.threshold_short_circuits);
  CpegWtlCertifiedArgs endcache_args = args;
  endcache_args.use_exact_endgame_cache = true;
  load_and_exec_config_or_die(config, CPEG_WTL_BAG1_CGP);
  CpegWtlCertifiedResult endcache = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(
             config_get_game(config), &endcache_args, &endcache) == 204);
  cpeg_assert_wtl_certified_results_byte_equal(&result, &endcache);
  assert(endcache.trace.exact_endgame_cache_hits == 0);
  cpeg_wtl_certified_result_destroy(&baseline);
  cpeg_wtl_certified_result_destroy(&result);
  cpeg_wtl_certified_result_destroy(&endcache);
  config_destroy(config);
}

void test_cpeg_wtl_certified_senator_acceptance(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23_crossplay -ld english_crossplay -bdn crossplay -bb 40 "
      "-wmp false -leaves NWL23_crossplay -s1 score -s2 score -threads 3");
  load_and_exec_config_or_die(config, CPEG_SENATOR_TOSA_CGP);
  const CpegWtlCertifiedArgs args = {
      .bag = 4,
      .allow_exchanges = true,
      .num_threads = 3,
      .initial_lead = -51,
      .budget_seconds = 180.0,
      .use_exact_two_ply_incumbent = true,
      .use_best_bag_emptying_screen = true,
      .use_threshold_reply_screen = true,
      .use_exact_endgame_cache = true,
      .collect_trace = true,
  };
  CpegWtlCertifiedResult result = {0};
  assert(cpeg_solve_pre_endgame_wtl_certified(config_get_game(config), &args,
                                              &result) == 1314);
  assert(result.status == CPEG_PRE_CERTIFIED);
  assert(result.unique_best);
  assert_strings_equal(result.cands[result.best_index].label, "13F TOSA");
  assert(result.cands[result.best_index].win_lower_num == 42);
  assert(result.cands[result.best_index].win_upper_num == 42);
  assert(result.cands[result.best_index].outcome_den == 330);
  assert(result.coverage.placements == 1215);
  assert(result.coverage.exchanges == 98);
  assert(result.coverage.passes == 1);
  assert(result.coverage.total == 1314);
  assert(result.coverage.generation_complete);
  assert(result.regret_num == 0);
  assert(result.regret_den == 1);
  assert(result.trace.fixed_endgame_cache_hits > 0);
  cpeg_wtl_certified_result_destroy(&result);
  config_destroy(config);
}

void test_cpeg(void) {
  test_cpeg_endgame();
  test_cpeg_wtl_value_core();
  test_cpeg_wtl_endgame_sign();
  test_cpeg_wtl_command_parsing();
  test_cpeg_interval_contains_scalar();
  test_cpeg_pre_endgame();
  test_cpeg_wtl_pre_endgame();
  test_cpeg_candidate_legality();
  test_cpeg_score_upper_bound();
  test_cpeg_enumeration_capacity_guard();
  test_cpeg_complete_root_collection();
  test_cpeg_coordinator();
  test_cpeg_incremental_round_trip();
}
