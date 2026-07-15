#include "cpeg_belief.h"

#include "../def/letter_distribution_defs.h"
#include "../util/sha256.h"
#include "../util/string_util.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CPEG_BELIEF_LINE_LEN = 256,
  CPEG_BELIEF_MAX_WORLDS = 1024,
};

static bool cpeg_belief_read_value(FILE *stream, const char *expected_key,
                                   char *value, size_t value_size) {
  char line[CPEG_BELIEF_LINE_LEN];
  char key[64];
  char parsed[128];
  char extra;
  if (fgets(line, sizeof(line), stream) == NULL ||
      sscanf(line, "%63s %127s %c", key, parsed, &extra) != 2 ||
      strcmp(key, expected_key) != 0 || strlen(parsed) >= value_size) {
    return false;
  }
  memcpy(value, parsed, strlen(parsed) + 1);
  return true;
}

static bool cpeg_belief_identifier_valid(const char *identifier) {
  if (identifier[0] == '\0') {
    return false;
  }
  for (const char *cursor = identifier; *cursor != '\0'; cursor++) {
    if (!(islower((unsigned char)*cursor) || isdigit((unsigned char)*cursor) ||
          *cursor == '_' || *cursor == '-')) {
      return false;
    }
  }
  return true;
}

static bool cpeg_belief_digest_valid(const char *digest) {
  if (strlen(digest) != CPEG_BELIEF_DIGEST_LEN - 1) {
    return false;
  }
  for (const char *cursor = digest; *cursor != '\0'; cursor++) {
    if (!(isdigit((unsigned char)*cursor) ||
          (*cursor >= 'a' && *cursor <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool cpeg_belief_parse_positive_int64(const char *text, int64_t *value) {
  char *end = NULL;
  errno = 0;
  const intmax_t parsed = strtoimax(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0' || parsed < 1 ||
      parsed > INT64_MAX) {
    return false;
  }
  *value = (int64_t)parsed;
  return true;
}

static bool cpeg_belief_parse_positive_int(const char *text, int maximum,
                                           int *value) {
  int64_t parsed = 0;
  if (!cpeg_belief_parse_positive_int64(text, &parsed) || parsed > maximum) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool cpeg_belief_tiles_canonical(const char *tiles) {
  for (size_t tile_idx = 1; tiles[tile_idx] != '\0'; tile_idx++) {
    if ((unsigned char)tiles[tile_idx] < (unsigned char)tiles[tile_idx - 1]) {
      return false;
    }
  }
  return true;
}

static bool cpeg_belief_world_duplicate(const CpegBeliefManifest *manifest,
                                        int world_idx) {
  const CpegWeightedWorld *world = &manifest->worlds[world_idx];
  for (int prior_idx = 0; prior_idx < world_idx; prior_idx++) {
    const CpegWeightedWorld *prior = &manifest->worlds[prior_idx];
    if (prior->bag_count != world->bag_count) {
      continue;
    }
    bool same = true;
    for (int tile_idx = 0; tile_idx < world->bag_count; tile_idx++) {
      if (prior->bag_tiles[tile_idx] != world->bag_tiles[tile_idx]) {
        same = false;
        break;
      }
    }
    if (same) {
      return true;
    }
  }
  return false;
}

static bool cpeg_belief_sha256_add_ml(Sha256 *sha,
                                      const LetterDistribution *ld,
                                      MachineLetter ml) {
  if ((int)ml >= ld_get_size(ld)) {
    return false;
  }
  const char *letter = ld->ld_ml_to_hl[ml];
  if (letter[0] == '\0' || letter[1] != '\0') {
    return false;
  }
  sha256_update(sha, letter, 1);
  return true;
}

// Python's BeliefState digest is SHA-256 over canonical rows:
//   opponent_rack<TAB>bag<TAB>weight<LF>
// The manifest omits the redundant opponent rack, so reconstruct it from the
// declared unseen multiset and each bag before accepting the claimed digest.
static bool
cpeg_belief_manifest_digest_matches(const CpegBeliefManifest *manifest,
                                    const LetterDistribution *ld) {
  const int ld_size = ld_get_size(ld);
  int unseen[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < manifest->unseen_count; tile_idx++) {
    const MachineLetter tile = manifest->unseen_mls[tile_idx];
    if ((int)tile >= ld_size) {
      return false;
    }
    unseen[tile]++;
  }

  Sha256 sha;
  sha256_init(&sha);
  for (int world_idx = 0; world_idx < manifest->world_count; world_idx++) {
    int opponent[MAX_ALPHABET_SIZE];
    memcpy(opponent, unseen, sizeof(opponent));
    const CpegWeightedWorld *world = &manifest->worlds[world_idx];
    for (int tile_idx = 0; tile_idx < world->bag_count; tile_idx++) {
      const MachineLetter tile = world->bag_tiles[tile_idx];
      if ((int)tile >= ld_size || --opponent[tile] < 0) {
        return false;
      }
    }
    for (int ml = 0; ml < ld_size; ml++) {
      for (int count = 0; count < opponent[ml]; count++) {
        if (!cpeg_belief_sha256_add_ml(&sha, ld, (MachineLetter)ml)) {
          return false;
        }
      }
    }
    sha256_update(&sha, "\t", 1);
    for (int tile_idx = 0; tile_idx < world->bag_count; tile_idx++) {
      if (!cpeg_belief_sha256_add_ml(&sha, ld, world->bag_tiles[tile_idx])) {
        return false;
      }
    }
    char suffix[64];
    const int suffix_length =
        snprintf(suffix, sizeof(suffix), "\t%" PRId64 "\n", world->weight);
    if (suffix_length < 1 || (size_t)suffix_length >= sizeof(suffix)) {
      return false;
    }
    sha256_update(&sha, suffix, (size_t)suffix_length);
  }

  char hex_digest[CPEG_BELIEF_DIGEST_LEN];
  sha256_final_hex(&sha, hex_digest);
  return strcmp(hex_digest, manifest->digest) == 0;
}

void cpeg_belief_manifest_destroy(CpegBeliefManifest *manifest) {
  if (manifest == NULL) {
    return;
  }
  free(manifest->worlds);
  memset(manifest, 0, sizeof(*manifest));
}

bool cpeg_belief_manifest_load(const char *path, const LetterDistribution *ld,
                               int expected_bag_size,
                               CpegBeliefManifest *manifest) {
  if (path == NULL || ld == NULL || manifest == NULL || expected_bag_size < 1 ||
      expected_bag_size > PEG_MAX_BAG) {
    return false;
  }
  // The API requires a zero-initialized or destroyed destination. Refuse to
  // overwrite a live allocation: silently memset'ing it would leak the prior
  // manifest and make repeated-load failures ownership-dependent.
  if (manifest->worlds != NULL) {
    return false;
  }
  memset(manifest, 0, sizeof(*manifest));
  FILE *stream = fopen(path, "r");
  if (stream == NULL) {
    return false;
  }

  bool valid = false;
  char line[CPEG_BELIEF_LINE_LEN];
  char value[128];
  if (fgets(line, sizeof(line), stream) == NULL ||
      strcmp(line, "cpeg-belief-v1\n") != 0 ||
      !cpeg_belief_read_value(stream, "posterior", manifest->posterior_id,
                              sizeof(manifest->posterior_id)) ||
      !cpeg_belief_identifier_valid(manifest->posterior_id) ||
      !cpeg_belief_read_value(stream, "unseen", manifest->unseen_tiles,
                              sizeof(manifest->unseen_tiles)) ||
      !cpeg_belief_tiles_canonical(manifest->unseen_tiles) ||
      (manifest->unseen_count =
           ld_str_to_mls(ld, manifest->unseen_tiles, false,
                         manifest->unseen_mls, RACK_SIZE + PEG_MAX_BAG)) < 1 ||
      !cpeg_belief_read_value(stream, "bag", value, sizeof(value)) ||
      !cpeg_belief_parse_positive_int(value, PEG_MAX_BAG,
                                      &manifest->bag_size) ||
      manifest->bag_size != expected_bag_size ||
      manifest->unseen_count <= manifest->bag_size ||
      manifest->unseen_count > RACK_SIZE + manifest->bag_size ||
      !cpeg_belief_read_value(stream, "worlds", value, sizeof(value)) ||
      !cpeg_belief_parse_positive_int(value, CPEG_BELIEF_MAX_WORLDS,
                                      &manifest->world_count) ||
      !cpeg_belief_read_value(stream, "mass", value, sizeof(value)) ||
      !cpeg_belief_parse_positive_int64(value, &manifest->weight_mass) ||
      !cpeg_belief_read_value(stream, "digest", manifest->digest,
                              sizeof(manifest->digest)) ||
      !cpeg_belief_digest_valid(manifest->digest)) {
    goto cleanup;
  }

  manifest->worlds =
      calloc((size_t)manifest->world_count, sizeof(*manifest->worlds));
  if (manifest->worlds == NULL) {
    goto cleanup;
  }
  int64_t observed_mass = 0;
  for (int world_idx = 0; world_idx < manifest->world_count; world_idx++) {
    char key[64];
    char bag_tiles[2 * RACK_SIZE + PEG_MAX_BAG + 1];
    char weight_text[64];
    char extra;
    if (fgets(line, sizeof(line), stream) == NULL ||
        sscanf(line, "%63s %18s %63s %c", key, bag_tiles, weight_text,
               &extra) != 3 ||
        strcmp(key, "world") != 0 || !cpeg_belief_tiles_canonical(bag_tiles)) {
      goto cleanup;
    }
    CpegWeightedWorld *world = &manifest->worlds[world_idx];
    world->bag_count =
        ld_str_to_mls(ld, bag_tiles, false, world->bag_tiles, PEG_MAX_BAG);
    if (world->bag_count != expected_bag_size ||
        !cpeg_belief_parse_positive_int64(weight_text, &world->weight) ||
        INT64_MAX - observed_mass < world->weight ||
        cpeg_belief_world_duplicate(manifest, world_idx)) {
      goto cleanup;
    }
    observed_mass += world->weight;
  }
  while (fgets(line, sizeof(line), stream) != NULL) {
    for (const char *cursor = line; *cursor != '\0'; cursor++) {
      if (!isspace((unsigned char)*cursor)) {
        goto cleanup;
      }
    }
  }
  if (observed_mass != manifest->weight_mass ||
      !cpeg_belief_manifest_digest_matches(manifest, ld)) {
    goto cleanup;
  }
  valid = true;

cleanup:
  fclose(stream);
  if (!valid) {
    cpeg_belief_manifest_destroy(manifest);
  }
  return valid;
}
