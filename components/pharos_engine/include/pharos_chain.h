/* Pharos - the tamper-evident evidence chain
 *
 * Chain of custody for a wireless assessment, on a £40 board.
 *
 * The problem this solves is real and usually ignored by hobby tooling: an
 * evidence file that anyone can edit afterwards is not evidence, it is a
 * note. If a Pharos report is going to sit in an engagement record - or, at
 * the sharp end, be handed to somebody making a decision about a person -
 * then a reader needs to be able to tell whether it is the same file the
 * device produced.
 *
 * So every record the device commits is hash-linked to the one before it:
 *
 *     H(n) = SHA-256( H(n-1) || seq || t_us || kind || payload )
 *
 * The chain starts from a per-session root that mixes a session nonce with
 * the device identity. That gives three properties worth having, all
 * checkable by `pharos_chain_verify` and all tested:
 *
 *   - **Modification** is caught: change any byte of any record and every
 *     head after it fails to reproduce.
 *   - **Deletion / truncation** is caught: the sequence numbers and the link
 *     stop agreeing.
 *   - **Insertion** is caught for the same reason.
 *
 * What it deliberately does *not* claim: this is integrity, not authorship.
 * A hash chain proves the file has not been edited since it was written; it
 * does not prove which device wrote it, because a device that can be
 * disassembled cannot keep a signing key secret from its owner. Claiming
 * otherwise would be exactly the kind of over-promise the rest of this
 * firmware refuses to make. Publish the head digest somewhere you do not
 * control - a ticket, a chat message, an email to the client at the end of
 * the walk - and you have a timestamp somebody else witnessed, which is the
 * honest version of the guarantee.
 *
 * Pure C, no allocation, host-tested.
 */
#ifndef PHAROS_CHAIN_H
#define PHAROS_CHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pharos_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record kinds. Kept explicit so a verifier can reason about a chain it did
 * not produce, without parsing the payload. */
typedef enum {
    PHC_KIND_SESSION_OPEN = 1,
    PHC_KIND_LENS_START,
    PHC_KIND_LENS_STOP,
    PHC_KIND_VERDICT,
    PHC_KIND_REPORT,
    PHC_KIND_OPERATOR_NOTE,
    PHC_KIND_FENCE_STATUS,
    PHC_KIND_SESSION_CLOSE,
} phc_kind_t;

typedef struct {
    uint8_t head[PHAROS_SHA256_SIZE]; /* current chain head            */
    uint8_t root[PHAROS_SHA256_SIZE]; /* the session's starting point  */
    uint32_t seq;                     /* records committed so far      */
    bool open;
} phc_chain_t;

/* One record as it appears in an exported evidence file. The payload is not
 * stored here - the chain commits to its *hash*, so a caller may keep the
 * payload wherever it likes (the evidence partition, a JSON report) and still
 * be able to prove it belongs. */
typedef struct {
    uint32_t seq;
    uint64_t t_us;
    uint8_t kind;
    uint8_t payload_hash[PHAROS_SHA256_SIZE];
    uint8_t head[PHAROS_SHA256_SIZE]; /* chain head after this record */
} phc_record_t;

/* Open a session. `nonce` should be fresh per session (RNG on target); the
 * device_id may be the eFuse MAC or any stable string, and is mixed in only
 * so two devices in the same room produce different roots. */
void phc_open(phc_chain_t *c, const void *device_id, size_t id_len, uint64_t nonce);

/* Commit a record. Fills `out` with the record as it should be exported.
 * Returns false if the chain is not open. */
bool phc_commit(phc_chain_t *c, phc_kind_t kind, uint64_t t_us,
                const void *payload, size_t payload_len, phc_record_t *out);

/* Close the session, committing a final record. After this, `head` is the
 * digest to publish somewhere you do not control. */
bool phc_close(phc_chain_t *c, uint64_t t_us, phc_record_t *out);

/* Verify a chain of exported records against a known root. Returns true only
 * if every link reproduces. On failure, *bad_index is the first record that
 * does not agree (or the count, if the chain simply ended early). */
bool phc_verify(const uint8_t root[PHAROS_SHA256_SIZE], const phc_record_t *records,
                unsigned n, unsigned *bad_index);

/* Recompute the head a record should produce, given the previous head. The
 * building block of phc_verify, exposed so an external tool can reimplement
 * verification in any language from this one definition. */
void phc_link(const uint8_t prev_head[PHAROS_SHA256_SIZE], const phc_record_t *rec,
              uint8_t out_head[PHAROS_SHA256_SIZE]);

const char *phc_kind_name(phc_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_CHAIN_H */
