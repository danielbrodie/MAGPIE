#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t SHA256_K[64] = {
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

static uint32_t sha256_rotate_right(uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

static void sha256_transform(Sha256 *sha) {
  uint32_t words[64];
  for (int word_idx = 0; word_idx < 16; word_idx++) {
    const int offset = word_idx * 4;
    words[word_idx] = ((uint32_t)sha->block[offset] << 24) |
                      ((uint32_t)sha->block[offset + 1] << 16) |
                      ((uint32_t)sha->block[offset + 2] << 8) |
                      (uint32_t)sha->block[offset + 3];
  }
  for (int word_idx = 16; word_idx < 64; word_idx++) {
    const uint32_t first =
        sha256_rotate_right(words[word_idx - 15], 7) ^
        sha256_rotate_right(words[word_idx - 15], 18) ^
        (words[word_idx - 15] >> 3);
    const uint32_t second =
        sha256_rotate_right(words[word_idx - 2], 17) ^
        sha256_rotate_right(words[word_idx - 2], 19) ^
        (words[word_idx - 2] >> 10);
    words[word_idx] =
        words[word_idx - 16] + first + words[word_idx - 7] + second;
  }

  uint32_t a = sha->state[0];
  uint32_t b = sha->state[1];
  uint32_t c = sha->state[2];
  uint32_t d = sha->state[3];
  uint32_t e = sha->state[4];
  uint32_t f = sha->state[5];
  uint32_t g = sha->state[6];
  uint32_t h = sha->state[7];
  for (int round_idx = 0; round_idx < 64; round_idx++) {
    const uint32_t first = sha256_rotate_right(e, 6) ^
                           sha256_rotate_right(e, 11) ^
                           sha256_rotate_right(e, 25);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t temp1 =
        h + first + choose + SHA256_K[round_idx] + words[round_idx];
    const uint32_t second = sha256_rotate_right(a, 2) ^
                            sha256_rotate_right(a, 13) ^
                            sha256_rotate_right(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = second + majority;
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

void sha256_init(Sha256 *sha) {
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

void sha256_update(Sha256 *sha, const void *bytes, size_t length) {
  const uint8_t *input = (const uint8_t *)bytes;
  for (size_t byte_idx = 0; byte_idx < length; byte_idx++) {
    sha->block[sha->block_size++] = input[byte_idx];
    if (sha->block_size == sizeof(sha->block)) {
      sha256_transform(sha);
      sha->bit_count += UINT64_C(512);
      sha->block_size = 0;
    }
  }
}

void sha256_final(Sha256 *sha, uint8_t digest[SHA256_DIGEST_SIZE]) {
  size_t block_idx = sha->block_size;
  sha->block[block_idx++] = UINT8_C(0x80);
  if (block_idx > 56) {
    while (block_idx < 64) {
      sha->block[block_idx++] = 0;
    }
    sha256_transform(sha);
    block_idx = 0;
  }
  while (block_idx < 56) {
    sha->block[block_idx++] = 0;
  }
  sha->bit_count += (uint64_t)sha->block_size * UINT64_C(8);
  for (int byte_idx = 0; byte_idx < 8; byte_idx++) {
    sha->block[63 - byte_idx] =
        (uint8_t)(sha->bit_count >> (unsigned int)(byte_idx * 8));
  }
  sha256_transform(sha);
  for (int word_idx = 0; word_idx < 8; word_idx++) {
    for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
      digest[word_idx * 4 + byte_idx] = (uint8_t)(
          sha->state[word_idx] >> (unsigned int)(24 - byte_idx * 8));
    }
  }
}

void sha256_final_hex(Sha256 *sha, char hex_digest[SHA256_HEX_SIZE]) {
  static const char HEX[] = "0123456789abcdef";
  uint8_t digest[SHA256_DIGEST_SIZE];
  sha256_final(sha, digest);
  for (int byte_idx = 0; byte_idx < SHA256_DIGEST_SIZE; byte_idx++) {
    hex_digest[byte_idx * 2] = HEX[digest[byte_idx] >> 4];
    hex_digest[byte_idx * 2 + 1] = HEX[digest[byte_idx] & UINT8_C(0x0f)];
  }
  hex_digest[SHA256_HEX_SIZE - 1] = '\0';
}

bool sha256_file_hex(const char *path, char hex_digest[SHA256_HEX_SIZE]) {
  if (path == NULL || hex_digest == NULL) {
    return false;
  }
  FILE *stream = fopen(path, "rb");
  if (stream == NULL) {
    return false;
  }
  Sha256 sha;
  sha256_init(&sha);
  uint8_t buffer[8192];
  bool valid = true;
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), stream)) > 0) {
    sha256_update(&sha, buffer, bytes_read);
  }
  if (ferror(stream)) {
    valid = false;
  }
  if (fclose(stream) != 0) {
    valid = false;
  }
  if (!valid) {
    return false;
  }
  sha256_final_hex(&sha, hex_digest);
  return true;
}
