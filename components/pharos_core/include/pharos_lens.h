/* Pharos - the lens registry
 *
 * A "lens" is one tool. Adding a tool to Pharos means adding one .c file that
 * ends with PHAROS_LENS_REGISTER(&my_lens); - no menu table to edit, no
 * central switch statement, no header to touch. That is the extensibility
 * lesson taken from Evil-M5Project's app list, made compile-time safe.
 *
 * Registration happens through constructor attributes rather than a custom
 * linker section so that the identical code registers lenses in the host
 * simulator build, where there is no ESP-IDF linker script.
 */
#ifndef PHAROS_LENS_H
#define PHAROS_LENS_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pharos_event;
struct pharos_bus;

typedef enum {
    PHAROS_LENS_OBSERVE = 0, /* passive sensing of the environment */
    PHAROS_LENS_ANALYSE,     /* works on already-captured material */
    PHAROS_LENS_TRAIN,       /* teaching / simulation, no radio      */
    PHAROS_LENS_SYSTEM,      /* settings, self-audit, storage        */
} pharos_lens_kind_t;

typedef struct pharos_lens {
    const char *id;      /* stable slug, used in logs: "wifi.census" */
    const char *name;    /* shown on the dial: "Census"              */
    const char *summary; /* one line shown on the info card          */
    const char *glyph;   /* icon name in the built-in atlas          */

    pharos_lens_kind_t kind;
    pharos_caps_t caps; /* exhaustive; the HAL enforces this */

    /* Worst-case current draw in mA at 100% duty, used by the power budget
     * planner to predict runtime on the dial before you launch anything. */
    uint16_t budget_ma;

    /* Lifecycle. Any may be NULL. mount/unmount bracket allocation;
     * start/stop bracket radio and sensor use; tick runs on the UI core at
     * roughly 20 Hz; on_event runs on the analytics core, once per bus
     * event, and must not touch LVGL. */
    bool (*on_mount)(void);
    void (*on_unmount)(void);
    bool (*on_start)(void);
    void (*on_stop)(void);
    void (*on_tick)(uint32_t dt_ms);
    void (*on_event)(const struct pharos_event *ev);

    /* The ingest ring the analytics loop should drain for this lens, calling
     * on_event once per popped event, or NULL for a lens with no radio (the
     * training and system lenses). Kept as an accessor rather than a pointer
     * so a lens can allocate its bus lazily in on_mount. */
    struct pharos_bus *(*ingest)(void);
} pharos_lens_t;

/* Registration ------------------------------------------------------- */

void pharos_lens_register(const pharos_lens_t *lens);

#define PHAROS__CAT2(a, b) a##b
#define PHAROS__CAT(a, b) PHAROS__CAT2(a, b)

#define PHAROS_LENS_REGISTER(lens_ptr)                                    \
    static void __attribute__((constructor))                              \
    PHAROS__CAT(pharos_lens_autoreg_, __LINE__)(void)                     \
    {                                                                     \
        pharos_lens_register(lens_ptr);                                   \
    }

/* Introspection ------------------------------------------------------ */

unsigned pharos_lens_count(void);
const pharos_lens_t *pharos_lens_at(unsigned index);
const pharos_lens_t *pharos_lens_find(const char *id);

/* Activation. Exactly one lens is active at a time; the previous one is
 * stopped and unmounted before the next mounts, so two lenses can never
 * contend for the radio. */
bool pharos_lens_activate(const char *id);
void pharos_lens_deactivate(void);
const pharos_lens_t *pharos_lens_active(void);

/* The capability set the HAL will honour right now. Zero when idle. */
pharos_caps_t pharos_lens_active_caps(void);

/* Test seam: drop every registration. Host tests only. */
void pharos_lens_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_LENS_H */
