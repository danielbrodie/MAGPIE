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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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
  CpegPreResult result;
  cpeg_solve_pre_endgame(game, /*bag=*/1, /*allow_exchanges=*/false,
                         /*num_threads=*/1, &result);
  assert(result.count > 0);
  assert(fabs(cpeg_find_spread(&result, "15J BEET") - 43.875) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "15J BEAT") - 39.125) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "13J TAU") - 44.125) < 1e-6);
  // The blocker outranks the high scorer: TAU is the exact optimum here.
  assert_strings_equal(result.cands[0].label, "13J TAU");
  assert(fabs(result.cands[0].expected_spread - 44.125) < 1e-6);

  // IMG_9653, bag 1, no exchanges: top move and spread match the reference.
  load_and_exec_config_or_die(config, CPEG_PRE_9653_CGP);
  game = config_get_game(config);
  cpeg_solve_pre_endgame(game, /*bag=*/1, /*allow_exchanges=*/false,
                         /*num_threads=*/1, &result);
  assert(result.count > 0);
  assert_strings_equal(result.cands[0].label, "12G I(N)S(I)D(E)");
  assert(fabs(result.cands[0].expected_spread - 23.0) < 1e-6);
  assert(fabs(cpeg_find_spread(&result, "15F PAST(Y)") - 22.25) < 1e-6);

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
  CpegPreResult result;
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
  CpegCertifiedResult result;
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
}

static void cpeg_assert_certified_bag1_early_stop(Config *config) {
  CpegPreResult oracle;
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
  CpegCertifiedResult single_threaded;
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_certified(config_get_game(config), &work_args,
                                          &single_threaded) > 0);
  assert(single_threaded.status == CPEG_PRE_ESTIMATED ||
         single_threaded.status == CPEG_PRE_CERTIFIED);
  assert(single_threaded.batches_completed == work_args.max_batches);
  cpeg_assert_best_interval_contains_oracle(&single_threaded, &oracle);

  CpegCertifiedArgs parallel_args = work_args;
  parallel_args.num_threads = 4;
  CpegCertifiedResult four_threaded;
  load_and_exec_config_or_die(config, CPEG_PRE_9570_CGP);
  assert(cpeg_solve_pre_endgame_certified(config_get_game(config),
                                          &parallel_args, &four_threaded) > 0);
  cpeg_assert_certified_results_equal(&single_threaded, &four_threaded);

  CpegCertifiedArgs wall_args = work_args;
  wall_args.num_threads = 4;
  wall_args.budget_seconds = 0.001;
  wall_args.max_batches = 0;
  CpegCertifiedResult wall_limited;
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
  CpegStatisticalResult full;
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
  CpegStatisticalResult deterministic_one;
  CpegStatisticalResult deterministic_two;
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
    CpegStatisticalResult prefix_result;
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
    CpegStatisticalResult result;
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
  }
  assert(tau_chosen == best_arm_seeds);
  printf("cpegstat coverage=%d/%d (%.1f%%), best=%d/%d, tau_eliminated=0\n",
         covered, coverage_seeds,
         100.0 * (double)covered / (double)coverage_seeds, tau_chosen,
         best_arm_seeds);
  config_destroy(config);
}

void test_cpeg(void) {
  test_cpeg_endgame();
  test_cpeg_interval_contains_scalar();
  test_cpeg_pre_endgame();
  test_cpeg_candidate_legality();
  test_cpeg_score_upper_bound();
  test_cpeg_enumeration_capacity_guard();
  test_cpeg_coordinator();
  test_cpeg_incremental_round_trip();
}
