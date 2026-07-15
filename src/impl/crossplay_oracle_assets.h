#ifndef CROSSPLAY_ORACLE_ASSETS_H
#define CROSSPLAY_ORACLE_ASSETS_H

#include "../util/sha256.h"
#include <stdbool.h>

enum {
  CROSSPLAY_ORACLE_ASSET_ID_SIZE = 128,
  CROSSPLAY_ORACLE_ASSET_PATH_SIZE = 1024,
};

typedef struct CrossplayOracleAssetManifest {
  char rules_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char rules_digest[SHA256_HEX_SIZE];
  char lexicon_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char lexicon_digest[SHA256_HEX_SIZE];
  char layout_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char layout_digest[SHA256_HEX_SIZE];
  char distribution_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char distribution_digest[SHA256_HEX_SIZE];
  char blocklist_path[CROSSPLAY_ORACLE_ASSET_PATH_SIZE];
  char blocklist_digest[SHA256_HEX_SIZE];
} CrossplayOracleAssetManifest;

// A persistent oracle process may reuse one verified in-memory asset snapshot.
// Reuse is explicit at the command boundary and remains valid only while the
// loaded asset identities and bingo bonus are unchanged.
typedef struct CrossplayOracleAssetSession {
  bool verified;
  char manifest_path[CROSSPLAY_ORACLE_ASSET_PATH_SIZE];
  char lexicon_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char layout_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  char distribution_id[CROSSPLAY_ORACLE_ASSET_ID_SIZE];
  const void *lexicon_handle;
  const void *layout_handle;
  const void *distribution_handle;
  int bingo_bonus;
  CrossplayOracleAssetManifest manifest;
} CrossplayOracleAssetSession;

bool crossplay_oracle_asset_manifest_load(
    const char *path, CrossplayOracleAssetManifest *manifest);
bool crossplay_oracle_asset_manifest_verify(
    const CrossplayOracleAssetManifest *manifest, const char *lexicon_id,
    const char *lexicon_path, const char *layout_id, const char *layout_path,
    const char *distribution_id, const char *distribution_path,
    int bingo_bonus);
bool crossplay_oracle_asset_session_remember(
    CrossplayOracleAssetSession *session, const char *manifest_path,
    const char *lexicon_id, const char *layout_id,
    const char *distribution_id, const void *lexicon_handle,
    const void *layout_handle, const void *distribution_handle, int bingo_bonus,
    const CrossplayOracleAssetManifest *manifest);
bool crossplay_oracle_asset_session_resolve(
    const CrossplayOracleAssetSession *session, const char *manifest_path,
    const char *lexicon_id, const char *layout_id,
    const char *distribution_id, const void *lexicon_handle,
    const void *layout_handle, const void *distribution_handle, int bingo_bonus,
    CrossplayOracleAssetManifest *manifest);

#endif
