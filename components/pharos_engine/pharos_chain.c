#include "pharos_chain.h"

#include <string.h>

/* Everything that goes into a hash is serialised big-endian and
 * fixed-width. The chain must be verifiable by a tool written in another
 * language on another machine, so "however this compiler lays out a struct"
 * is not an acceptable definition. */
static void put_u32(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}

static void put_u64(uint8_t b[8], uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (56 - i * 8));
    }
}

void phc_link(const uint8_t prev_head[PHAROS_SHA256_SIZE], const phc_record_t *rec,
              uint8_t out_head[PHAROS_SHA256_SIZE])
{
    if (!prev_head || !rec || !out_head) {
        return;
    }
    uint8_t seq_be[4], t_be[8];
    put_u32(seq_be, rec->seq);
    put_u64(t_be, rec->t_us);

    pharos_sha256_t h;
    pharos_sha256_init(&h);
    /* Domain separator: stops a record hash ever being confused with a
     * payload hash or a root. */
    pharos_sha256_update(&h, "pharos.chain.v1", 15);
    pharos_sha256_update(&h, prev_head, PHAROS_SHA256_SIZE);
    pharos_sha256_update(&h, seq_be, 4);
    pharos_sha256_update(&h, t_be, 8);
    pharos_sha256_update(&h, &rec->kind, 1);
    pharos_sha256_update(&h, rec->payload_hash, PHAROS_SHA256_SIZE);
    pharos_sha256_final(&h, out_head);
}

void phc_open(phc_chain_t *c, const void *device_id, size_t id_len, uint64_t nonce)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));

    uint8_t nonce_be[8];
    put_u64(nonce_be, nonce);

    pharos_sha256_t h;
    pharos_sha256_init(&h);
    pharos_sha256_update(&h, "pharos.chain.root.v1", 20);
    pharos_sha256_update(&h, nonce_be, 8);
    if (device_id && id_len) {
        pharos_sha256_update(&h, device_id, id_len);
    }
    pharos_sha256_final(&h, c->root);

    memcpy(c->head, c->root, PHAROS_SHA256_SIZE);
    c->seq = 0;
    c->open = true;
}

bool phc_commit(phc_chain_t *c, phc_kind_t kind, uint64_t t_us,
                const void *payload, size_t payload_len, phc_record_t *out)
{
    if (!c || !c->open || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->seq = c->seq;
    out->t_us = t_us;
    out->kind = (uint8_t)kind;
    /* Commit to the payload's hash, not the payload: the chain stays a fixed
     * size per record whether the payload is a verdict or a 12 KB report. */
    pharos_sha256(payload ? payload : "", payload ? payload_len : 0, out->payload_hash);

    phc_link(c->head, out, out->head);
    memcpy(c->head, out->head, PHAROS_SHA256_SIZE);
    c->seq++;
    return true;
}

bool phc_close(phc_chain_t *c, uint64_t t_us, phc_record_t *out)
{
    if (!c || !c->open) {
        return false;
    }
    const bool ok = phc_commit(c, PHC_KIND_SESSION_CLOSE, t_us, NULL, 0, out);
    c->open = false;
    return ok;
}

bool phc_verify(const uint8_t root[PHAROS_SHA256_SIZE], const phc_record_t *records,
                unsigned n, unsigned *bad_index)
{
    if (!root || (!records && n)) {
        if (bad_index) *bad_index = 0;
        return false;
    }
    uint8_t head[PHAROS_SHA256_SIZE];
    memcpy(head, root, PHAROS_SHA256_SIZE);

    for (unsigned i = 0; i < n; i++) {
        /* Sequence numbers must be dense and ascending from zero: this is
         * what catches a record having been lifted out of the middle. */
        if (records[i].seq != i) {
            if (bad_index) *bad_index = i;
            return false;
        }
        uint8_t expect[PHAROS_SHA256_SIZE];
        phc_link(head, &records[i], expect);
        if (memcmp(expect, records[i].head, PHAROS_SHA256_SIZE) != 0) {
            if (bad_index) *bad_index = i;
            return false;
        }
        memcpy(head, records[i].head, PHAROS_SHA256_SIZE);
    }
    if (bad_index) *bad_index = n;
    return true;
}

const char *phc_kind_name(phc_kind_t kind)
{
    switch (kind) {
    case PHC_KIND_SESSION_OPEN:  return "session.open";
    case PHC_KIND_LENS_START:    return "lens.start";
    case PHC_KIND_LENS_STOP:     return "lens.stop";
    case PHC_KIND_VERDICT:       return "verdict";
    case PHC_KIND_REPORT:        return "report";
    case PHC_KIND_OPERATOR_NOTE: return "operator.note";
    case PHC_KIND_FENCE_STATUS:  return "fence.status";
    case PHC_KIND_SESSION_CLOSE: return "session.close";
    default:                     return "?";
    }
}
