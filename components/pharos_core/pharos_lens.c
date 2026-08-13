#include "pharos_lens.h"

#include <stddef.h>
#include <string.h>

#ifndef PHAROS_MAX_LENSES
#define PHAROS_MAX_LENSES 32
#endif

static const pharos_lens_t *s_lenses[PHAROS_MAX_LENSES];
static unsigned s_count;
static const pharos_lens_t *s_active;
static bool s_started;

void pharos_lens_register(const pharos_lens_t *lens)
{
    if (!lens || !lens->id || s_count >= PHAROS_MAX_LENSES) {
        return;
    }
    /* Duplicate ids would make log lines ambiguous; first registration wins. */
    for (unsigned i = 0; i < s_count; i++) {
        if (strcmp(s_lenses[i]->id, lens->id) == 0) {
            return;
        }
    }
    s_lenses[s_count++] = lens;
}

unsigned pharos_lens_count(void)
{
    return s_count;
}

const pharos_lens_t *pharos_lens_at(unsigned index)
{
    return (index < s_count) ? s_lenses[index] : NULL;
}

const pharos_lens_t *pharos_lens_find(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (unsigned i = 0; i < s_count; i++) {
        if (strcmp(s_lenses[i]->id, id) == 0) {
            return s_lenses[i];
        }
    }
    return NULL;
}

void pharos_lens_deactivate(void)
{
    if (!s_active) {
        return;
    }
    if (s_started && s_active->on_stop) {
        s_active->on_stop();
    }
    s_started = false;
    if (s_active->on_unmount) {
        s_active->on_unmount();
    }
    s_active = NULL;
}

bool pharos_lens_activate(const char *id)
{
    const pharos_lens_t *next = pharos_lens_find(id);
    if (!next) {
        return false;
    }

    pharos_lens_deactivate();

    if (next->on_mount && !next->on_mount()) {
        return false;
    }
    s_active = next;

    if (next->on_start && !next->on_start()) {
        /* Mounted but could not start - unwind so the radio is definitely
         * released and the dial does not show a half-live lens. */
        pharos_lens_deactivate();
        return false;
    }
    s_started = true;
    return true;
}

const pharos_lens_t *pharos_lens_active(void)
{
    return s_active;
}

pharos_caps_t pharos_lens_active_caps(void)
{
    return s_active ? s_active->caps : PHAROS_CAP_NONE;
}

void pharos_lens_reset_for_test(void)
{
    pharos_lens_deactivate();
    s_count = 0;
}
