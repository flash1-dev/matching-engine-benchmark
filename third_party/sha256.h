/*
 * sha256.h — public-domain SHA-256 (FIPS 180-4).
 *
 * This file is in the public domain. It is vendored so the benchmark harness
 * has zero external cryptographic dependencies and builds anywhere.
 */
#ifndef ME_SHA256_H
#define ME_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, size_t len);
void sha256_final(sha256_ctx* c, uint8_t out[32]);

/* Convenience: hash `len` bytes and write a 64-char lowercase hex digest plus a
 * NUL terminator into `hex` (which must be at least 65 bytes). */
void sha256_hex(const void* data, size_t len, char hex[65]);

#ifdef __cplusplus
}
#endif

#endif /* ME_SHA256_H */
