#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/cpeg.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

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

static void test_cpeg_pre_endgame(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -ld english_crossplay -bdn crossplay -bb 40 -leaves "
      "NWL23_crossplay -s1 score -s2 score -threads 1");

  // IMG_9570, bag 1, no exchanges: BEET's known value is reproduced exactly, and
  // the exhaustive search's optimum is the blocker 13J TAU.
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

void test_cpeg(void) {
  test_cpeg_endgame();
  test_cpeg_pre_endgame();
  test_cpeg_candidate_legality();
}
