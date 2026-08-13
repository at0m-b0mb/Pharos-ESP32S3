#include "pharos_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void transform(pharos_sha256_t *c, const uint8_t block[64])
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (unsigned i = 0; i < 64; i++) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void pharos_sha256_init(pharos_sha256_t *c)
{
    if (!c) return;
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

void pharos_sha256_update(pharos_sha256_t *c, const void *data, size_t len)
{
    if (!c || (!data && len)) return;
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        c->buf[c->buflen++] = *p++;
        if (c->buflen == 64) {
            transform(c, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

void pharos_sha256_final(pharos_sha256_t *c, uint8_t out[PHAROS_SHA256_SIZE])
{
    if (!c || !out) return;
    size_t i = c->buflen;
    c->bitlen += (uint64_t)c->buflen * 8u;

    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buf[i++] = 0;
        transform(c, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    for (int b = 7; b >= 0; b--) {
        c->buf[i++] = (uint8_t)((c->bitlen >> (b * 8)) & 0xFF);
    }
    transform(c, c->buf);

    for (unsigned k = 0; k < 8; k++) {
        out[k * 4]     = (uint8_t)((c->state[k] >> 24) & 0xFF);
        out[k * 4 + 1] = (uint8_t)((c->state[k] >> 16) & 0xFF);
        out[k * 4 + 2] = (uint8_t)((c->state[k] >> 8) & 0xFF);
        out[k * 4 + 3] = (uint8_t)(c->state[k] & 0xFF);
    }
}

void pharos_sha256(const void *data, size_t len, uint8_t out[PHAROS_SHA256_SIZE])
{
    pharos_sha256_t c;
    pharos_sha256_init(&c);
    pharos_sha256_update(&c, data, len);
    pharos_sha256_final(&c, out);
}

void pharos_sha256_hex(const uint8_t digest[PHAROS_SHA256_SIZE], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    if (!digest || !out) return;
    for (unsigned i = 0; i < PHAROS_SHA256_SIZE; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[64] = '\0';
}
