#include "crossplay_oracle_assets.h"

#include <stdio.h>
#include <string.h>

enum {
  CROSSPLAY_ORACLE_ASSET_FIELDS = 10,
};

static bool crossplay_oracle_asset_digest_valid(const char *digest) {
  if (strlen(digest) != SHA256_HEX_SIZE - 1) {
    return false;
  }
  for (int char_idx = 0; char_idx < SHA256_HEX_SIZE - 1; char_idx++) {
    const char value = digest[char_idx];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool crossplay_oracle_asset_copy(char *dest, size_t capacity,
                                        const char *value) {
  const size_t length = strlen(value);
  if (length == 0 || length >= capacity) {
    return false;
  }
  memcpy(dest, value, length + 1);
  return true;
}

static bool crossplay_oracle_asset_assign(CrossplayOracleAssetManifest *manifest,
                                          bool *seen, const char *key,
                                          const char *value) {
  int field_idx = -1;
  char *dest = NULL;
  size_t capacity = 0;
  if (strcmp(key, "rules_id") == 0) {
    field_idx = 0;
    dest = manifest->rules_id;
    capacity = sizeof(manifest->rules_id);
  } else if (strcmp(key, "rules_digest") == 0) {
    field_idx = 1;
    dest = manifest->rules_digest;
    capacity = sizeof(manifest->rules_digest);
  } else if (strcmp(key, "lexicon_id") == 0) {
    field_idx = 2;
    dest = manifest->lexicon_id;
    capacity = sizeof(manifest->lexicon_id);
  } else if (strcmp(key, "lexicon_digest") == 0) {
    field_idx = 3;
    dest = manifest->lexicon_digest;
    capacity = sizeof(manifest->lexicon_digest);
  } else if (strcmp(key, "layout_id") == 0) {
    field_idx = 4;
    dest = manifest->layout_id;
    capacity = sizeof(manifest->layout_id);
  } else if (strcmp(key, "layout_digest") == 0) {
    field_idx = 5;
    dest = manifest->layout_digest;
    capacity = sizeof(manifest->layout_digest);
  } else if (strcmp(key, "distribution_id") == 0) {
    field_idx = 6;
    dest = manifest->distribution_id;
    capacity = sizeof(manifest->distribution_id);
  } else if (strcmp(key, "distribution_digest") == 0) {
    field_idx = 7;
    dest = manifest->distribution_digest;
    capacity = sizeof(manifest->distribution_digest);
  } else if (strcmp(key, "blocklist_path") == 0) {
    field_idx = 8;
    dest = manifest->blocklist_path;
    capacity = sizeof(manifest->blocklist_path);
  } else if (strcmp(key, "blocklist_digest") == 0) {
    field_idx = 9;
    dest = manifest->blocklist_digest;
    capacity = sizeof(manifest->blocklist_digest);
  }
  if (field_idx < 0 || seen[field_idx] ||
      !crossplay_oracle_asset_copy(dest, capacity, value)) {
    return false;
  }
  seen[field_idx] = true;
  return true;
}

bool crossplay_oracle_asset_manifest_load(
    const char *path, CrossplayOracleAssetManifest *manifest) {
  if (path == NULL || manifest == NULL) {
    return false;
  }
  memset(manifest, 0, sizeof(*manifest));
  FILE *stream = fopen(path, "rb");
  if (stream == NULL) {
    return false;
  }
  char line[1400];
  if (fgets(line, sizeof(line), stream) == NULL ||
      strcmp(line, "crossplay-oracle-assets-v1\n") != 0) {
    fclose(stream);
    return false;
  }
  bool seen[CROSSPLAY_ORACLE_ASSET_FIELDS] = {false};
  bool valid = true;
  while (valid && fgets(line, sizeof(line), stream) != NULL) {
    char key[64];
    char value[CROSSPLAY_ORACLE_ASSET_PATH_SIZE];
    char extra;
    if (sscanf(line, "%63s %1023s %c", key, value, &extra) != 2 ||
        !crossplay_oracle_asset_assign(manifest, seen, key, value)) {
      valid = false;
    }
  }
  if (ferror(stream) || fclose(stream) != 0) {
    valid = false;
  }
  for (int field_idx = 0; field_idx < CROSSPLAY_ORACLE_ASSET_FIELDS;
       field_idx++) {
    valid = valid && seen[field_idx];
  }
  return valid && crossplay_oracle_asset_digest_valid(manifest->rules_digest) &&
         crossplay_oracle_asset_digest_valid(manifest->lexicon_digest) &&
         crossplay_oracle_asset_digest_valid(manifest->layout_digest) &&
         crossplay_oracle_asset_digest_valid(manifest->distribution_digest) &&
         crossplay_oracle_asset_digest_valid(manifest->blocklist_digest);
}

static bool crossplay_oracle_asset_file_matches(
    const char *path, const char expected[SHA256_HEX_SIZE]) {
  char actual[SHA256_HEX_SIZE];
  return sha256_file_hex(path, actual) && strcmp(actual, expected) == 0;
}

bool crossplay_oracle_asset_manifest_verify(
    const CrossplayOracleAssetManifest *manifest, const char *lexicon_id,
    const char *lexicon_path, const char *layout_id, const char *layout_path,
    const char *distribution_id, const char *distribution_path,
    int bingo_bonus) {
  if (manifest == NULL || lexicon_id == NULL || lexicon_path == NULL ||
      layout_id == NULL || layout_path == NULL || distribution_id == NULL ||
      distribution_path == NULL) {
    return false;
  }
  if (strcmp(manifest->lexicon_id, "NWL23_crossplay") != 0 ||
      strcmp(lexicon_id, manifest->lexicon_id) != 0 ||
      strcmp(manifest->layout_id, "crossplay") != 0 ||
      strcmp(layout_id, manifest->layout_id) != 0 ||
      strcmp(manifest->distribution_id, "english_crossplay") != 0 ||
      strcmp(distribution_id, manifest->distribution_id) != 0 ||
      bingo_bonus != 40) {
    return false;
  }
  return crossplay_oracle_asset_file_matches(lexicon_path,
                                              manifest->lexicon_digest) &&
         crossplay_oracle_asset_file_matches(layout_path,
                                              manifest->layout_digest) &&
         crossplay_oracle_asset_file_matches(distribution_path,
                                              manifest->distribution_digest) &&
         crossplay_oracle_asset_file_matches(manifest->blocklist_path,
                                              manifest->blocklist_digest);
}

bool crossplay_oracle_asset_session_remember(
    CrossplayOracleAssetSession *session, const char *manifest_path,
    const char *lexicon_id, const char *layout_id,
    const char *distribution_id, const void *lexicon_handle,
    const void *layout_handle, const void *distribution_handle, int bingo_bonus,
    const CrossplayOracleAssetManifest *manifest) {
  if (session == NULL || manifest_path == NULL || lexicon_id == NULL ||
      layout_id == NULL || distribution_id == NULL || lexicon_handle == NULL ||
      layout_handle == NULL || distribution_handle == NULL ||
      manifest == NULL) {
    return false;
  }
  CrossplayOracleAssetSession candidate = {0};
  if (!crossplay_oracle_asset_copy(candidate.manifest_path,
                                   sizeof(candidate.manifest_path),
                                   manifest_path) ||
      !crossplay_oracle_asset_copy(candidate.lexicon_id,
                                   sizeof(candidate.lexicon_id), lexicon_id) ||
      !crossplay_oracle_asset_copy(candidate.layout_id,
                                   sizeof(candidate.layout_id), layout_id) ||
      !crossplay_oracle_asset_copy(candidate.distribution_id,
                                   sizeof(candidate.distribution_id),
                                   distribution_id)) {
    return false;
  }
  candidate.bingo_bonus = bingo_bonus;
  candidate.lexicon_handle = lexicon_handle;
  candidate.layout_handle = layout_handle;
  candidate.distribution_handle = distribution_handle;
  candidate.manifest = *manifest;
  candidate.verified = true;
  *session = candidate;
  return true;
}

bool crossplay_oracle_asset_session_resolve(
    const CrossplayOracleAssetSession *session, const char *manifest_path,
    const char *lexicon_id, const char *layout_id,
    const char *distribution_id, const void *lexicon_handle,
    const void *layout_handle, const void *distribution_handle, int bingo_bonus,
    CrossplayOracleAssetManifest *manifest) {
  if (session == NULL || manifest_path == NULL || lexicon_id == NULL ||
      layout_id == NULL || distribution_id == NULL || lexicon_handle == NULL ||
      layout_handle == NULL || distribution_handle == NULL ||
      manifest == NULL || !session->verified ||
      strcmp(session->manifest_path, manifest_path) != 0 ||
      strcmp(session->lexicon_id, lexicon_id) != 0 ||
      strcmp(session->layout_id, layout_id) != 0 ||
      strcmp(session->distribution_id, distribution_id) != 0 ||
      session->lexicon_handle != lexicon_handle ||
      session->layout_handle != layout_handle ||
      session->distribution_handle != distribution_handle ||
      session->bingo_bonus != bingo_bonus) {
    return false;
  }
  *manifest = session->manifest;
  return true;
}
