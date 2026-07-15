#ifndef CPEG_BELIEF_H
#define CPEG_BELIEF_H

#include "../def/peg_defs.h"
#include "letter_distribution.h"
#include <stdbool.h>
#include <stdint.h>

enum {
  CPEG_BELIEF_POSTERIOR_ID_LEN = 64,
  CPEG_BELIEF_DIGEST_LEN = 65,
  CPEG_BELIEF_UNSEEN_LEN = 32,
};

// One externally supplied hidden world. The bag determines the opponent rack
// as (public unseen inventory - bag); the root rack is already loaded in the
// Game. Positive integer weights are exact posterior mass, never rounded
// probabilities.
typedef struct CpegWeightedWorld {
  MachineLetter bag_tiles[PEG_MAX_BAG];
  int bag_count;
  int64_t weight;
} CpegWeightedWorld;

// Version-one file boundary used by the isolated pre-endgame worker. Parsing
// validates shape, exact mass, and the canonical SHA-256 world digest; the
// search kernel independently binds the declared unseen inventory to the
// loaded Game. Callers must pass a zero-initialized or destroyed destination.
typedef struct CpegBeliefManifest {
  CpegWeightedWorld *worlds;
  int world_count;
  int bag_size;
  int64_t weight_mass;
  MachineLetter unseen_mls[RACK_SIZE + PEG_MAX_BAG];
  int unseen_count;
  char posterior_id[CPEG_BELIEF_POSTERIOR_ID_LEN];
  char digest[CPEG_BELIEF_DIGEST_LEN];
  char unseen_tiles[CPEG_BELIEF_UNSEEN_LEN];
} CpegBeliefManifest;

bool cpeg_belief_manifest_load(const char *path, const LetterDistribution *ld,
                               int expected_bag_size,
                               CpegBeliefManifest *manifest);
void cpeg_belief_manifest_destroy(CpegBeliefManifest *manifest);

#endif
