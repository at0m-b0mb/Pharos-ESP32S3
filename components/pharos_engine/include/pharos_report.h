/* Pharos - the evidence writer
 *
 * A bounded streaming JSON emitter with redaction applied at write time.
 *
 * Two design decisions carry this file.
 *
 * First, **redaction happens here, not at export**. A profile enforced only
 * on the way out is one forgotten flag away from being no redaction at all;
 * a MAC address that was never written in full cannot leak from a partition
 * image, a crash dump, or a support bundle.
 *
 * Second, **every string that crosses this writer is escaped**, because the
 * most interesting strings Pharos handles are chosen by somebody else. An
 * SSID is 32 bytes of attacker-controlled data. A network named
 *   ", "admin": true, "x": "
 * must produce a JSON string, not a JSON structure, and must certainly not
 * be able to forge fields in an evidence file that somebody later relies on.
 * There is a host test for exactly that network name.
 *
 * The writer never overruns its buffer and never emits a partial token: on
 * truncation it stops, sets ok = false, and says so in the result rather
 * than handing back malformed JSON that looks fine until it is parsed.
 */
#ifndef PHAROS_REPORT_H
#define PHAROS_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRT_REDACT_NONE = 0, /* full addresses: authorised assessment, own estate */
    PRT_REDACT_OUI,      /* vendor prefix only: aa:bb:cc:xx:xx:xx             */
    PRT_REDACT_HASH,     /* salted digest: correlate within a session, never  */
                         /* across one                                        */
} prt_redact_t;

#define PRT_MAX_DEPTH 8

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
    prt_redact_t redact;
    uint32_t salt;
    unsigned depth;
    bool need_comma[PRT_MAX_DEPTH + 1];
    /* '{' or '[' per open container, so prt_finish closes each with the
     * right bracket rather than assuming objects all the way down. */
    char open[PRT_MAX_DEPTH + 1];
} prt_t;

/* salt should be drawn fresh per session when redact is PRT_REDACT_HASH:
 * it is what stops two reports from different days being joined together. */
void prt_init(prt_t *w, char *buf, size_t cap, prt_redact_t redact, uint32_t salt);

void prt_obj_begin(prt_t *w, const char *key);
void prt_obj_end(prt_t *w);
void prt_arr_begin(prt_t *w, const char *key);
void prt_arr_end(prt_t *w);

void prt_str(prt_t *w, const char *key, const char *value);
/* Length-delimited: SSIDs are not NUL-terminated on the wire and may contain
 * embedded zero bytes. */
void prt_strn(prt_t *w, const char *key, const char *value, size_t len);
void prt_u32(prt_t *w, const char *key, uint32_t value);
void prt_i32(prt_t *w, const char *key, int32_t value);
void prt_bool(prt_t *w, const char *key, bool value);
void prt_null(prt_t *w, const char *key);
void prt_mac(prt_t *w, const char *key, const uint8_t mac[6]);

/* Closes any open containers and NUL-terminates. Returns false if the buffer
 * was too small at any point - in which case the contents are not valid JSON
 * and must not be written to the evidence partition. */
bool prt_finish(prt_t *w);

/* Bytes written so far, excluding the terminator. */
size_t prt_len(const prt_t *w);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_REPORT_H */
