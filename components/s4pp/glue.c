#include "s4pp.h"
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <string.h>

typedef struct {
  uint8_t iv[16];
  mbedtls_aes_context mbedtls_ctx;
} aes_ctx_t;


static void init_aes(void *ctx)
{
  aes_ctx_t *c = (aes_ctx_t *)ctx;
  memset(c->iv, 0, sizeof(c->iv));
  mbedtls_aes_init(&c->mbedtls_ctx);
}


static void setkey_aes(void *ctx, const void *key, size_t keylen)
{
  aes_ctx_t *c = (aes_ctx_t *)ctx;
  mbedtls_aes_setkey_enc(&c->mbedtls_ctx, key, keylen * 8);
  mbedtls_aes_setkey_dec(&c->mbedtls_ctx, key, keylen * 8);
}


static void run_aes(void *ctx, const void *in, void *out, size_t len, bool dir_is_encrypt)
{
  aes_ctx_t *c = (aes_ctx_t *)ctx;
  if (in != out)
    memcpy(out, in, len);

  mbedtls_aes_crypt_cbc(
    &c->mbedtls_ctx,
    dir_is_encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
    len,
    c->iv,
    (const uint8_t *)in,
    (uint8_t *)out);
}


void destroy_aes(void *ctx)
{
  aes_ctx_t *c = (aes_ctx_t *)ctx;
  mbedtls_aes_free(&c->mbedtls_ctx);
}


static const crypto_mech_info_t cryptos[] =
{
  {
    .name = "AES-128-CBC",
    .init = init_aes,
    .setkey = setkey_aes,
    .run = run_aes,
    .destroy = destroy_aes,
    .ctx_size = sizeof(aes_ctx_t),
    .block_size = 16
  },
  { .name = NULL }
};


static void init_sha256(void *ctx_in)
{
  mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctx_in;
  mbedtls_sha256_init(ctx);
  mbedtls_sha256_starts_ret(ctx, 0);
}


static void update_sha256(void *ctx_in, const void *msg, int len)
{
  mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctx_in;
  mbedtls_sha256_update_ret(ctx, (const uint8_t *)msg, len);
}


static void finalize_sha256(void *digest, void *ctx_in)
{
  mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctx_in;
  mbedtls_sha256_finish_ret(ctx, digest);
}


static const digest_mech_info_t digests[] =
{
  {
    .name = "SHA256",
    .create = init_sha256,
    .update = update_sha256,
    .finalize = finalize_sha256,
    .ctx_size = sizeof(mbedtls_sha256_context),
    .digest_size = 32,
    .block_size = 64,
  },
  { .name = NULL }
};


int crypto_hash(const digest_mech_info_t *mi, const void *data, size_t data_len, uint8_t *digest)
{
  uint8_t ctx[mi->ctx_size];
  mi->create(ctx);
  mi->update(ctx, data, data_len);
  mi->finalize(digest, ctx);
  return 0;
}


// Aaah - needs to support in-place encoding/expansion, hence reverse order
void crypto_encode_asciihex(const char *bin, size_t bin_len, char *outbuf)
{
  for (int i = (int)bin_len -1; i >= 0; --i)
  {
    static const char x[] = "0123456789abcdef";
    uint8_t c = (uint8_t)bin[i];
    outbuf[i*2 + 0] = x[c >> 4];
    outbuf[i*2 + 1] = x[c & 0xf];
  }
}


s4pp_ctx_t *s4pp_create_glued(const s4pp_io_t *io, const s4pp_auth_t *auth, const s4pp_server_t *server, s4pp_hide_mode_t hide_mode, int data_format, void *user_arg)
{
  return s4pp_create(io, digests, cryptos, (s4pp_rnd_fn)esp_fill_random,
    auth, server, hide_mode, data_format, user_arg);
}
