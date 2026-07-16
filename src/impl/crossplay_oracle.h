#ifndef CROSSPLAY_ORACLE_H
#define CROSSPLAY_ORACLE_H

#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../util/sha256.h"
#include <stdbool.h>
#include <stdint.h>

enum {
  CROSSPLAY_ORACLE_ACTION_CAPACITY = 16384,
  CROSSPLAY_ORACLE_EXCHANGE_CAPACITY = 128,
  CROSSPLAY_ORACLE_DRAW_CAPACITY = 128,
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

typedef struct CrossplayOracleDraw {
  MachineLetter tiles[RACK_SIZE];
  int count;
  int64_t weight;
} CrossplayOracleDraw;

typedef struct CrossplayOracleDrawSet {
  CrossplayOracleDraw draws[CROSSPLAY_ORACLE_DRAW_CAPACITY];
  int count;
  int64_t weight_mass;
  bool complete;
} CrossplayOracleDrawSet;

typedef struct CrossplayOracleTransition {
  Game *game;
  CrossplayOracleDraw draw;
  bool bag_emptied;
} CrossplayOracleTransition;

typedef struct CrossplayOracleTransitionSet {
  CrossplayOracleTransition transitions[CROSSPLAY_ORACLE_DRAW_CAPACITY];
  int count;
  int64_t weight_mass;
  bool complete;
} CrossplayOracleTransitionSet;

CrossplayOracleStatus crossplay_oracle_generate_actions(
    const Game *game, int bag_count, bool allow_exchanges,
    CrossplayOracleActionSet *result);
CrossplayOracleStatus crossplay_oracle_bounded_sequence_commitment(
    const CrossplayOracleActionSet *actions, const char *selected_action_id,
    int actor, const char *parent_sequence_digest,
    const char *information_key_digest,
    char commitment[SHA256_HEX_SIZE]);
void crossplay_oracle_action_set_destroy(CrossplayOracleActionSet *result);
const char *crossplay_oracle_status_name(CrossplayOracleStatus status);
CrossplayOracleStatus crossplay_oracle_enumerate_draws(
    const Bag *bag, int draw_count, int ld_size, CrossplayOracleDrawSet *result);
CrossplayOracleStatus crossplay_oracle_apply_action(
    const Game *game, const CrossplayOracleAction *action,
    CrossplayOracleTransitionSet *result);
void crossplay_oracle_transition_set_destroy(
    CrossplayOracleTransitionSet *result);

#endif
