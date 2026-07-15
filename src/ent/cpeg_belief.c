#include "cpeg_belief.h"

#include "../def/letter_distribution_defs.h"
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

typedef struct CpegBeliefSha256 {
  uint8_t block[64];
  uint32_t state[8];
  uint64_t bit_count;
  size_t block_size;
} CpegBeliefSha256;

static const uint32_t CPEG_BELIEF_SHA256_K[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2),
};

static uint32_t cpeg_belief_rotr32(uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

static void cpeg_belief_sha256_transform(CpegBeliefSha256 *sha) {
  uint32_t words[64];
  for (int index = 0; index < 16; index++) {
    const int offset = index * 4;
    words[index] = ((uint32_t)sha->block[offset] << 24) |
                   ((uint32_t)sha->block[offset + 1] << 16) |
                   ((uint32_t)sha->block[offset + 2] << 8) |
                   (uint32_t)sha->block[offset + 3];
  }
  for (int index = 16; index < 64; index++) {
    const uint32_t s0 = cpeg_belief_rotr32(words[index - 15], 7) ^
                        cpeg_belief_rotr32(words[index - 15], 18) ^
                        (words[index - 15] >> 3);
    const uint32_t s1 = cpeg_belief_rotr32(words[index - 2], 17) ^
                        cpeg_belief_rotr32(words[index - 2], 19) ^
                        (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  uint32_t a = sha->state[0];
  uint32_t b = sha->state[1];
  uint32_t c = sha->state[2];
  uint32_t d = sha->state[3];
  uint32_t e = sha->state[4];
  uint32_t f = sha->state[5];
  uint32_t g = sha->state[6];
  uint32_t h = sha->state[7];
  for (int index = 0; index < 64; index++) {
    const uint32_t s1 = cpeg_belief_rotr32(e, 6) ^ cpeg_belief_rotr32(e, 11) ^
                        cpeg_belief_rotr32(e, 25);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t temp1 =
        h + s1 + choose + CPEG_BELIEF_SHA256_K[index] + words[index];
    const uint32_t s0 = cpeg_belief_rotr32(a, 2) ^ cpeg_belief_rotr32(a, 13) ^
                        cpeg_belief_rotr32(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  sha->state[0] += a;
  sha->state[1] += b;
  sha->state[2] += c;
  sha->state[3] += d;
  sha->state[4] += e;
  sha->state[5] += f;
  sha->state[6] += g;
  sha->state[7] += h;
}

static void cpeg_belief_sha256_init(CpegBeliefSha256 *sha) {
  memset(sha, 0, sizeof(*sha));
  sha->state[0] = UINT32_C(0x6a09e667);
  sha->state[1] = UINT32_C(0xbb67ae85);
  sha->state[2] = UINT32_C(0x3c6ef372);
  sha->state[3] = UINT32_C(0xa54ff53a);
  sha->state[4] = UINT32_C(0x510e527f);
  sha->state[5] = UINT32_C(0x9b05688c);
  sha->state[6] = UINT32_C(0x1f83d9ab);
  sha->state[7] = UINT32_C(0x5be0cd19);
}

static void cpeg_belief_sha256_update(CpegBeliefSha256 *sha, const char *bytes,
                                      size_t length) {
  for (size_t index = 0; index < length; index++) {
    sha->block[sha->block_size++] = (uint8_t)bytes[index];
    if (sha->block_size == sizeof(sha->block)) {
      cpeg_belief_sha256_transform(sha);
      sha->bit_count += UINT64_C(512);
      sha->block_size = 0;
    }
  }
}

static void cpeg_belief_sha256_final(CpegBeliefSha256 *sha,
                                     uint8_t digest[32]) {
  size_t index = sha->block_size;
  sha->block[index++] = UINT8_C(0x80);
  if (index > 56) {
    while (index < 64) {
      sha->block[index++] = 0;
    }
    cpeg_belief_sha256_transform(sha);
    index = 0;
  }
  while (index < 56) {
    sha->block[index++] = 0;
  }
  sha->bit_count += (uint64_t)sha->block_size * UINT64_C(8);
  for (int byte = 0; byte < 8; byte++) {
    sha->block[63 - byte] =
        (uint8_t)(sha->bit_count >> (unsigned int)(byte * 8));
  }
  cpeg_belief_sha256_transform(sha);
  for (int word = 0; word < 8; word++) {
    for (int byte = 0; byte < 4; byte++) {
      digest[word * 4 + byte] =
          (uint8_t)(sha->state[word] >> (unsigned int)(24 - byte * 8));
    }
  }
}

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

static bool cpeg_belief_sha256_add_ml(CpegBeliefSha256 *sha,
                                      const LetterDistribution *ld,
                                      MachineLetter ml) {
  if ((int)ml >= ld_get_size(ld)) {
    return false;
  }
  const char *letter = ld->ld_ml_to_hl[ml];
  if (letter[0] == '\0' || letter[1] != '\0') {
    return false;
  }
  cpeg_belief_sha256_update(sha, letter, 1);
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

  CpegBeliefSha256 sha;
  cpeg_belief_sha256_init(&sha);
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
    cpeg_belief_sha256_update(&sha, "\t", 1);
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
    cpeg_belief_sha256_update(&sha, suffix, (size_t)suffix_length);
  }

  uint8_t digest[32];
  char hex_digest[CPEG_BELIEF_DIGEST_LEN];
  static const char HEX[] = "0123456789abcdef";
  cpeg_belief_sha256_final(&sha, digest);
  for (int index = 0; index < 32; index++) {
    hex_digest[index * 2] = HEX[digest[index] >> 4];
    hex_digest[index * 2 + 1] = HEX[digest[index] & UINT8_C(0x0f)];
  }
  hex_digest[64] = '\0';
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
