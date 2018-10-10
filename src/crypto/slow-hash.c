// Copyright (c) 2012-2017, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2014-2018, The Monero Project
// Copyright (c) 2014-2018, The Aeon Project
// Copyright (c) 2018, The TurtleCoin Developers
//
// Please see the included LICENSE file for more information.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "Common/int-util.h"
#include "hash-ops.h"
#include "oaes_lib.h"

#define PAGE_SIZE              2097152

// Standard Crypto Definitions
#define AES_BLOCK_SIZE         16
#define AES_KEY_SIZE           32
#define INIT_SIZE_BLK          8
#define INIT_SIZE_BYTE         (INIT_SIZE_BLK * AES_BLOCK_SIZE)

static void (*const extra_hashes[4])(const void *, size_t, char *) = {
  hash_extra_blake, hash_extra_groestl, hash_extra_jh, hash_extra_skein
};

extern int aesb_single_round(const uint8_t *in, uint8_t*out, const uint8_t *expandedKey);
extern int aesb_pseudo_round(const uint8_t *in, uint8_t *out, const uint8_t *expandedKey);

static size_t e2i(const uint8_t* a, size_t count) { return (*((uint64_t*)a) / AES_BLOCK_SIZE) & (count - 1); }

static void mul(const uint8_t* a, const uint8_t* b, uint8_t* res) {
  uint64_t a0, b0;
  uint64_t hi, lo;

  a0 = SWAP64LE(((uint64_t*)a)[0]);
  b0 = SWAP64LE(((uint64_t*)b)[0]);
  lo = mul128(a0, b0, &hi);
  ((uint64_t*)res)[0] = SWAP64LE(hi);
  ((uint64_t*)res)[1] = SWAP64LE(lo);
}

static void sum_half_blocks(uint8_t* a, const uint8_t* b) {
  uint64_t a0, a1, b0, b1;

  a0 = SWAP64LE(((uint64_t*)a)[0]);
  a1 = SWAP64LE(((uint64_t*)a)[1]);
  b0 = SWAP64LE(((uint64_t*)b)[0]);
  b1 = SWAP64LE(((uint64_t*)b)[1]);
  a0 += b0;
  a1 += b1;
  ((uint64_t*)a)[0] = SWAP64LE(a0);
  ((uint64_t*)a)[1] = SWAP64LE(a1);
}
#define U64(x) ((uint64_t *) (x))

static void copy_block(uint8_t* dst, const uint8_t* src) {
  memcpy(dst, src, AES_BLOCK_SIZE);
}

static void swap_blocks(uint8_t *a, uint8_t *b){
  uint64_t t[2];
  U64(t)[0] = U64(a)[0];
  U64(t)[1] = U64(a)[1];
  U64(a)[0] = U64(b)[0];
  U64(a)[1] = U64(b)[1];
  U64(b)[0] = U64(t)[0];
  U64(b)[1] = U64(t)[1];
}

static void xor_blocks(uint8_t* a, const uint8_t* b) {
  size_t i;
  for (i = 0; i < AES_BLOCK_SIZE; i++) {
    a[i] ^= b[i];
  }
}

static void xor64(uint8_t* left, const uint8_t* right)
{
  size_t i;
  for (i = 0; i < 8; ++i)
  {
    left[i] ^= right[i];
  }
}

#pragma pack(push, 1)
union cn_slow_hash_state {
  union hash_state hs;
  struct {
    uint8_t k[64];
    uint8_t init[INIT_SIZE_BYTE];
  };
};
#pragma pack(pop)

void cn_slow_hash(const void *data, size_t length, char *hash, int variant, size_t scratchpad, size_t iterations) {
  size_t init_rounds = (scratchpad / INIT_SIZE_BYTE);
  size_t aes_rounds = (iterations / 2);
  size_t aes_init = (scratchpad / AES_BLOCK_SIZE);

  uint8_t long_state[PAGE_SIZE];
  union cn_slow_hash_state state;
  uint8_t text[INIT_SIZE_BYTE];
  uint8_t a[AES_BLOCK_SIZE];
  uint8_t b[AES_BLOCK_SIZE];
  uint8_t c[AES_BLOCK_SIZE];
  uint8_t d[AES_BLOCK_SIZE];
  size_t i, j;
  uint8_t aes_key[AES_KEY_SIZE];
  oaes_ctx *aes_ctx;

  hash_process(&state.hs, data, length);
  memcpy(text, state.init, INIT_SIZE_BYTE);
  memcpy(aes_key, state.hs.b, AES_KEY_SIZE);
  aes_ctx = (oaes_ctx *) oaes_alloc();

  // Initialize Variant 1
  uint8_t tweak1_2[8];
  do if (variant == 1)
  {
    do if (length < 43)
    {
      fprintf(stderr, "Cryptonight variants need at least 43 bytes of data");
      abort();
    } while(0);

    memcpy(&tweak1_2, &state.hs.b[192], sizeof(tweak1_2));
    xor64(tweak1_2, (((const uint8_t*)data)+35));
  } while(0);

  oaes_key_import_data(aes_ctx, aes_key, AES_KEY_SIZE);
  for (i = 0; i < init_rounds; i++)
  {
    for (j = 0; j < INIT_SIZE_BLK; j++)
    {
      aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);
    }
    memcpy(&long_state[i * INIT_SIZE_BYTE], text, INIT_SIZE_BYTE);
  }

  for (i = 0; i < 16; i++)
  {
    a[i] = state.k[     i] ^ state.k[32 + i];
    b[i] = state.k[16 + i] ^ state.k[48 + i];
  }

  for (i = 0; i < aes_rounds; i++)
  {
    /* Dependency chain: address -> read value ------+
     * written value <-+ hard function (AES or MUL) <+
     * next address  <-+
     */

    /* Iteration 1 */
    j = e2i(a, aes_init);
    copy_block(c, &long_state[j * AES_BLOCK_SIZE]);
    aesb_single_round(c, c, a);
    xor_blocks(b, c);
    swap_blocks(b, c);
    copy_block(&long_state[j * AES_BLOCK_SIZE], c);
    assert(j == e2i(a, aes_init));
    swap_blocks(a, b);

    // This supports variant 1
    do if (variant == 1)
    {
      const uint8_t tmp = ((const uint8_t*)(&long_state[j * AES_BLOCK_SIZE]))[11];
      static const uint32_t table = 0x75310;
      const uint8_t index = (((tmp >> 3) & 6) | (tmp & 1)) << 1;
      ((uint8_t*)(&long_state[j * AES_BLOCK_SIZE]))[11] = tmp ^ ((table >> index) & 0x30);
    } while(0);

    /* Iteration 2 */
    j = e2i(a, aes_init);
    copy_block(c, &long_state[j * AES_BLOCK_SIZE]);
    mul(a, c, d);
    sum_half_blocks(b, d);
    swap_blocks(b, c);
    xor_blocks(b, c);

    // This supports variant 1
    do if (variant == 1)
    {
      xor64(c + 8, tweak1_2);
    } while(0);

    copy_block(&long_state[j * AES_BLOCK_SIZE], c);
    assert(j == e2i(a, aes_init));
    swap_blocks(a, b);
  }

  memcpy(text, state.init, INIT_SIZE_BYTE);
  oaes_key_import_data(aes_ctx, &state.hs.b[32], AES_KEY_SIZE);

  for (i = 0; i < init_rounds; i++)
  {
    for (j = 0; j < INIT_SIZE_BLK; j++)
    {
      xor_blocks(&text[j * AES_BLOCK_SIZE], &long_state[i * INIT_SIZE_BYTE + j * AES_BLOCK_SIZE]);
      aesb_pseudo_round(&text[AES_BLOCK_SIZE * j], &text[AES_BLOCK_SIZE * j], aes_ctx->key->exp_data);
    }
  }

  memcpy(state.init, text, INIT_SIZE_BYTE);
  hash_permutation(&state.hs);
  /*memcpy(hash, &state, 32);*/
  extra_hashes[state.hs.b[0] & 3](&state, 200, hash);
  oaes_free((OAES_CTX **) &aes_ctx);
}
