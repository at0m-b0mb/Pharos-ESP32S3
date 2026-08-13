/* Pharos - SHA-256
 *
 * A compact, dependency-free implementation (FIPS 180-4). Vendored rather
 * than pulled from mbedTLS for one reason: the evidence chain that uses it
 * must run in the host test suite, on a laptop, with a plain C compiler and
 * no ESP-IDF. A hash the tests cannot exercise is a hash nobody has checked.
 *
 * Verified in test_chain.c against the standard NIST vectors.
 */
#ifndef PHAROS_SHA256_H
#define PHAROS_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHAROS_SHA256_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} pharos_sha256_t;

void pharos_sha256_init(pharos_sha256_t *c);
void pharos_sha256_update(pharos_sha256_t *c, const void *data, size_t len);
void pharos_sha256_final(pharos_sha256_t *c, uint8_t out[PHAROS_SHA256_SIZE]);

/* One-shot convenience. */
void pharos_sha256(const void *data, size_t len, uint8_t out[PHAROS_SHA256_SIZE]);

/* Lowercase hex, writes 65 bytes (64 + NUL). */
void pharos_sha256_hex(const uint8_t digest[PHAROS_SHA256_SIZE], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SHA256_H */
