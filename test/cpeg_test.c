#include "../src/ent/game.h"
#include "../src/impl/config.h"
#include "../src/impl/cpeg.h"
#include "test_util.h"
#include <assert.h>
#include <string.h>

// A real Crossplay bag-empty position (the endgame_final_turn.png fixture the
// Python solver pins). Two plies remain: the mover plays C4 EYAS for 32, the
// opponent's best raw-score reply is K4 (B)hUT for 27, so the swing is 5.
static const char *const CPEG_FINAL_TURN_CGP =
    "cgp ZIPs7T2J/I2N6SHAVE/T2aA3C2R2E/H2WE3ONBOARD/E2ER3W2EX2/R2DO3F2S3/"
    "4S3L6/4O2FOB5/4LO1OPA5/5C1A1R3T1/5HMM1QI1GOT/6AE1UNLIKE/6ID1EN1TEG/"
    "6D6R1/7GALENAS1 IIEAYSI/?TUUVY 0/0 0";

void test_cpeg(void) {
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
