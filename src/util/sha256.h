#ifndef SHA256_H
#define SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

enum {
  SHA256_DIGEST_SIZE = 32,
  SHA256_HEX_SIZE = 65,
};

typedef struct Sha256 {
#ifdef __APPLE__
  CC_SHA256_CTX context;
#else
  uint8_t block[64];
  uint32_t state[8];
  uint64_t bit_count;
  size_t block_size;
#endif
} Sha256;

void sha256_init(Sha256 *sha);
void sha256_update(Sha256 *sha, const void *bytes, size_t length);
void sha256_final(Sha256 *sha, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256_final_hex(Sha256 *sha, char hex_digest[SHA256_HEX_SIZE]);
bool sha256_file_hex(const char *path, char hex_digest[SHA256_HEX_SIZE]);

#endif
