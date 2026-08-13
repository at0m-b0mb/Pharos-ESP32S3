#include "pharos_report.h"

#include <string.h>

static const char k_hex[] = "0123456789abcdef";

static void put(prt_t *w, char c)
{
    if (!w->ok) {
        return;
    }
    if (w->len + 1 >= w->cap) { /* keep one byte for the terminator */
        w->ok = false;
        return;
    }
    w->buf[w->len++] = c;
}

static void puts_raw(prt_t *w, const char *s)
{
    while (s && *s && w->ok) {
        put(w, *s++);
    }
}

/* Escapes to the JSON string grammar. Everything below 0x20, plus the quote
 * and the backslash, becomes an escape; bytes at or above 0x80 are passed
 * through, so a UTF-8 SSID survives and a non-UTF-8 one at least round-trips
 * byte for byte rather than being silently mangled. */
static void put_escaped(prt_t *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n && w->ok; i++) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  puts_raw(w, "\\\""); break;
        case '\\': puts_raw(w, "\\\\"); break;
        case '\b': puts_raw(w, "\\b"); break;
        case '\f': puts_raw(w, "\\f"); break;
        case '\n': puts_raw(w, "\\n"); break;
        case '\r': puts_raw(w, "\\r"); break;
        case '\t': puts_raw(w, "\\t"); break;
        default:
            if (c < 0x20) {
                puts_raw(w, "\\u00");
                put(w, k_hex[(c >> 4) & 0xF]);
                put(w, k_hex[c & 0xF]);
            } else {
                put(w, (char)c);
            }
            break;
        }
    }
}

static void separator(prt_t *w, const char *key)
{
    if (w->depth <= PRT_MAX_DEPTH && w->need_comma[w->depth]) {
        put(w, ',');
    }
    if (w->depth <= PRT_MAX_DEPTH) {
        w->need_comma[w->depth] = true;
    }
    if (key) {
        put(w, '"');
        put_escaped(w, key, strlen(key));
        puts_raw(w, "\":");
    }
}

static void push(prt_t *w, char kind)
{
    if (w->depth >= PRT_MAX_DEPTH) {
        w->ok = false;
        return;
    }
    w->depth++;
    w->need_comma[w->depth] = false;
    w->open[w->depth] = kind;
}

/* Returns the closing bracket for the container being left, or 0 on
 * underflow. A mismatched close is a programming error in the caller, so it
 * fails the write rather than emitting bracket soup. */
static char pop(prt_t *w, char expect)
{
    if (w->depth == 0) {
        w->ok = false;
        return 0;
    }
    const char kind = w->open[w->depth];
    if (expect && kind != expect) {
        w->ok = false;
    }
    w->depth--;
    return (kind == '[') ? ']' : '}';
}

static void put_u32(prt_t *w, uint32_t v)
{
    char tmp[11];
    unsigned n = 0;
    if (v == 0) {
        put(w, '0');
        return;
    }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) {
        put(w, tmp[--n]);
    }
}

/* FNV-1a over the salt and the address. Not a security primitive - it is a
 * correlation key that expires with the session, which is exactly the
 * property wanted: link a device to itself within one report, never across
 * two. */
static uint32_t fnv_mac(uint32_t salt, const uint8_t mac[6])
{
    uint32_t h = 2166136261u ^ salt;
    for (unsigned i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return h;
}

void prt_init(prt_t *w, char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    if (!w) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = (buf != NULL && cap > 1);
    w->redact = redact;
    w->salt = salt;
    w->depth = 0;
    w->need_comma[0] = false;
}

void prt_obj_begin(prt_t *w, const char *key)
{
    if (!w) return;
    separator(w, key);
    put(w, '{');
    push(w, '{');
}

void prt_obj_end(prt_t *w)
{
    if (!w) return;
    const char close = pop(w, '{');
    if (close) put(w, close);
}

void prt_arr_begin(prt_t *w, const char *key)
{
    if (!w) return;
    separator(w, key);
    put(w, '[');
    push(w, '[');
}

void prt_arr_end(prt_t *w)
{
    if (!w) return;
    const char close = pop(w, '[');
    if (close) put(w, close);
}

void prt_strn(prt_t *w, const char *key, const char *value, size_t len)
{
    if (!w) return;
    separator(w, key);
    put(w, '"');
    if (value) {
        put_escaped(w, value, len);
    }
    put(w, '"');
}

void prt_str(prt_t *w, const char *key, const char *value)
{
    prt_strn(w, key, value, value ? strlen(value) : 0u);
}

void prt_u32(prt_t *w, const char *key, uint32_t value)
{
    if (!w) return;
    separator(w, key);
    put_u32(w, value);
}

void prt_i32(prt_t *w, const char *key, int32_t value)
{
    if (!w) return;
    separator(w, key);
    if (value < 0) {
        put(w, '-');
        /* Negate in unsigned space so INT32_MIN does not overflow. */
        put_u32(w, (uint32_t)(-(int64_t)value));
    } else {
        put_u32(w, (uint32_t)value);
    }
}

void prt_bool(prt_t *w, const char *key, bool value)
{
    if (!w) return;
    separator(w, key);
    puts_raw(w, value ? "true" : "false");
}

void prt_null(prt_t *w, const char *key)
{
    if (!w) return;
    separator(w, key);
    puts_raw(w, "null");
}

void prt_mac(prt_t *w, const char *key, const uint8_t mac[6])
{
    if (!w) return;
    if (!mac) {
        prt_null(w, key);
        return;
    }
    separator(w, key);
    put(w, '"');
    switch (w->redact) {
    case PRT_REDACT_HASH: {
        const uint32_t h = fnv_mac(w->salt, mac);
        puts_raw(w, "h:");
        for (int i = 7; i >= 0; i--) {
            put(w, k_hex[(h >> (i * 4)) & 0xF]);
        }
        break;
    }
    case PRT_REDACT_OUI:
        for (unsigned i = 0; i < 3; i++) {
            put(w, k_hex[(mac[i] >> 4) & 0xF]);
            put(w, k_hex[mac[i] & 0xF]);
            put(w, ':');
        }
        puts_raw(w, "xx:xx:xx");
        break;
    case PRT_REDACT_NONE:
    default:
        for (unsigned i = 0; i < 6; i++) {
            if (i) put(w, ':');
            put(w, k_hex[(mac[i] >> 4) & 0xF]);
            put(w, k_hex[mac[i] & 0xF]);
        }
        break;
    }
    put(w, '"');
}

bool prt_finish(prt_t *w)
{
    if (!w || !w->buf || w->cap == 0) {
        return false;
    }
    /* Close anything left open so a caller that returned early still gets
     * either valid JSON or an explicit failure - never a half-written file
     * that parses today and not tomorrow. */
    while (w->depth > 0 && w->ok) {
        const char close = pop(w, 0);
        if (!close) break;
        put(w, close);
    }
    if (w->len < w->cap) {
        w->buf[w->len] = '\0';
    } else {
        w->ok = false;
        w->buf[w->cap - 1] = '\0';
    }
    return w->ok;
}

size_t prt_len(const prt_t *w)
{
    return w ? w->len : 0u;
}
