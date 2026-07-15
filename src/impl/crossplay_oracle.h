#ifndef CROSSPLAY_ORACLE_H
#define CROSSPLAY_ORACLE_H

#include "../def/letter_distribution_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../util/sha256.h"
#include <stdbool.h>

enum {
  CROSSPLAY_ORACLE_ACTION_CAPACITY = 16384,
  CROSSPLAY_ORACLE_EXCHANGE_CAPACITY = 128,
};

typedef enum CrossplayOracleStatus {
  CROSSPLAY_ORACLE_OK,
  CROSSPLAY_ORACLE_INVALID_INPUT,
  CROSSPLAY_ORACLE_CAPACITY_EXCEEDED,
  CROSSPLAY_ORACLE_ENCODING_ERROR,
} CrossplayOracleStatus;

typedef enum CrossplayOracleActionKind {
  CROSSPLAY_ORACLE_PLACEMENT,
  CROSSPLAY_ORACLE_EXCHANGE,
  CROSSPLAY_ORACLE_PASS,
} CrossplayOracleActionKind;

typedef struct CrossplayOracleAction {
  CrossplayOracleActionKind kind;
  Move move;
  MachineLetter exchange_tiles[RACK_SIZE];
  int exchange_count;
  int score;
  char *canonical_json;
  char id[SHA256_HEX_SIZE];
} CrossplayOracleAction;

typedef struct CrossplayOracleCoverage {
  int placements;
  int exchanges;
  int passes;
  int total;
  bool complete;
} CrossplayOracleCoverage;

typedef struct CrossplayOracleActionSet {
  CrossplayOracleAction *actions;
  int count;
  CrossplayOracleCoverage coverage;
  char digest[SHA256_HEX_SIZE];
} CrossplayOracleActionSet;

CrossplayOracleStatus crossplay_oracle_generate_actions(
    const Game *game, int bag_count, bool allow_exchanges,
    CrossplayOracleActionSet *result);
void crossplay_oracle_action_set_destroy(CrossplayOracleActionSet *result);
const char *crossplay_oracle_status_name(CrossplayOracleStatus status);

#endif
