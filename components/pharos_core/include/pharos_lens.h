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

/* A display-ready verdict. Fixed-size and self-contained: the UI must never
 * hold a pointer into a lens' state, which changes on another core. */
/* How many evidence families the face can draw, and how many seconds of
 * activity history the ribbon holds. Both are UI capacities, not engine ones:
 * a lens with more families than this shows its strongest four. */
#define PHAROS_DISP_FAMILIES 4
#define PHAROS_DISP_HISTORY 16

struct pharos_lens_display {
    char big[16];     /* the headline: a score, a grade, a word     */
    char band[24];    /* what that headline MEANS                   */
    char detail[48];  /* the supporting numbers                     */
    char advice[96];  /* what to do about it                        */
    uint8_t score;    /* 0..100, drives the gauge and its colour    */
    uint8_t raw_score;/* what the evidence earned BEFORE the caps   */
    uint8_t ceiling;  /* the most this observation could have earned*/
    bool has_score;   /* false when the headline is not a score     */

    /* WHY it thinks so, drawn as labelled pips rather than as anonymous dots.
     * Bit i of `families` lights pip i; fam_label[i] names it and must point
     * at storage that outlives the call - a string literal, in practice, never
     * a buffer on the lens' stack. Leave the labels NULL to draw no pips. */
    uint8_t families;
    const char *fam_label[PHAROS_DISP_FAMILIES];

    /* The last PHAROS_DISP_HISTORY seconds of activity, oldest first, each
     * normalised to 0..255 by the lens against its own peak. A score says
     * whether something is happening; this says what shape it is, which is
     * the question an operator asks next. All zero draws a quiet timeline,
     * which is itself information. */
    uint8_t history[PHAROS_DISP_HISTORY];
    bool has_history;

    /* The one specific finding worth a line of glass - "sequence counter went
     * backwards" rather than "the shape looks wrong". Empty when the lens has
     * nothing more specific than its band advice to offer. */
    char why[48];

    /* THIS READING IS A SIMULATION, NOT THE ROOM.
     *
     * The training lenses play synthesised attacks through the real engines,
     * which is the point of them - a learner sees the same arithmetic the
     * field uses. But they therefore produce the same words: Range shows
     * "FLOOD LIKELY 77" and Footprint "BLARING 77", and an operator who
     * glances at one has no way to know the building is fine. That happened,
     * and it is the worst failure this project can have: a drill mistaken for
     * an incident.
     *
     * A lens that is not measuring the actual air sets this, and the HUD marks
     * it unmistakably on every frame. */
    bool simulated;
};

/* ---- the detail page -------------------------------------------------
 *
 * The live face answers "is something wrong, and how sure are you". That is
 * the right thing to lead with, and it is deliberately one number.
 *
 * It is not the whole answer. A Census that grades the air around you and
 * cannot then tell you WHICH networks it graded is asking to be taken on
 * trust, which is the one thing this project refuses to do anywhere else. So
 * a lens may also offer rows: its own evidence, in its own words, reachable
 * from the live view.
 *
 * A row has two columns because nearly every useful line is a thing and a
 * judgement about it - a network and its grade, a family and its score, a
 * device and how many names it leaked.
 *
 * The lens does not choose colours. It states a TONE and the HUD maps it onto
 * the one palette, so that red means the same thing on every page. */
typedef enum {
    PHAROS_TONE_NEUTRAL = 0, /* ordinary text                    */
    PHAROS_TONE_DIM,         /* context, units, secondary detail */
    PHAROS_TONE_GOOD,        /* nothing to do here               */
    PHAROS_TONE_WARN,        /* worth knowing                    */
    PHAROS_TONE_BAD,         /* act on this                      */
} pharos_tone_t;

struct pharos_lens_row {
    char left[26];  /* the thing          */
    char right[12]; /* the judgement      */
    pharos_tone_t tone;
};

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

    /* Optional: report this lens' current finding to the Aegis correlator.
     *
     * Only one lens runs at a time, so no lens can see another's verdict. This
     * is how the picture is assembled anyway: while a lens is active the UI
     * loop asks it, roughly once a second, "what are you seeing, and how sure
     * are you?" and forwards the answer to the latch. Findings therefore
     * accumulate as the operator moves between lenses, and survive long after
     * the lens that saw them has been swapped out.
     *
     * `stage` is a pa_stage_t value, carried as a plain integer so that this
     * header - which every component includes - does not have to depend on the
     * engine's. Return false when there is nothing worth reporting. */
    bool (*stage_report)(uint8_t *stage, uint8_t *score, uint8_t *ceiling);

    /* What this lens wants on the glass, right now.
     *
     * Until v1.11.0 the screen showed a running frame counter and the LOGARITHM
     * of that counter as a "score" - for every lens, identically. It went up
     * forever and meant nothing, which is exactly how it was reported: "the
     * number just increases, how do I know the data is correct?" It wasn't.
     * The engines were computing real verdicts and the UI was ignoring them.
     *
     * A lens fills this from its own verdict, so the panel shows the same
     * numbers the reports and the console do. Returning false means "I have
     * nothing to say yet", and the UI says THAT rather than inventing a value. */
    bool (*display)(struct pharos_lens_display *out);

    /* The centre tap, while this lens is RUNNING.
     *
     * In BROWSE the centre starts a lens; in LIVE it used to do nothing at
     * all, and that hole had a real cost. The Watch engine's confidence
     * ceiling is set by how much of the channel the receiver hears, so the
     * single most useful thing an operator can do about a suspicious reading
     * is stop hopping and stand still - and there was no control on the glass
     * that did it. The only way to camp was a console command over USB, which
     * is not a thing you do while holding the device up in a corridor.
     *
     * Called on the UI task, never on LVGL's, so it may do real work. NULL
     * means the centre stays inert for this lens. */
    void (*on_select)(void);

    /* The detail page: this lens' own evidence, one row at a time.
     *
     * Fill row `index` (0-based across the whole list; the HUD does its own
     * paging) and return true, or return false when there are no more. A lens
     * with nothing to list leaves this NULL and the detail page is not offered
     * for it.
     *
     * Runs on the UI task and must not block: take the snapshot lock with a
     * zero or tiny timeout and give up rather than stalling the repaint. */
    bool (*row)(unsigned index, struct pharos_lens_row *out);

    /* Column headings for that page, e.g. "NETWORK" and "GRADE". Either may
     * be NULL. */
    const char *row_head_left;
    const char *row_head_right;
} pharos_lens_t;

/* Upper bound on registered lenses. Defined here (not just in the .c) because
 * the UI sizes its dial-order array with it. */
#ifndef PHAROS_MAX_LENSES
#define PHAROS_MAX_LENSES 32
#endif

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
