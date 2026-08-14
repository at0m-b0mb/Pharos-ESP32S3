/* Pharos - the command console
 *
 * A scriptable, Cardputer-style command interface for a receive-only tool.
 * You type `watch camp 6`, `census`, `locate 02:66:6e:00:00:02`, `footprint
 * deauth`, `report`, `fence` - and the console routes each to the same lenses
 * and engines the dial drives. It is the operator's REPL: fast on the serial
 * port, scriptable, and the natural home for the red/blue muscle memory of a
 * command line.
 *
 * The important design decision is what the console *cannot* say. Its command
 * table is fixed and data-driven, and every command carries a category -
 * SCAN / ANALYSE / EVIDENCE / SYSTEM. There is no TRANSMIT category, and no
 * command that emits a frame, because there is nothing in the firmware that
 * can. A host test walks the entire table and asserts it: the console is a
 * receive-only surface by construction, not by convention, exactly like the
 * rest of Pharos. Adding an offensive command would require adding a
 * capability that does not exist.
 *
 * Everything here is pure C: the tokeniser, the dispatch, the help system and
 * the fixed-size output sink are all exercised on a laptop against a mock ops
 * table, so the parser is correct before it ever meets a keyboard.
 */
#ifndef PHAROS_CONSOLE_H
#define PHAROS_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MAX_ARGS   8
#define PC_MAX_LINE 160
#define PC_OUT_CAP  1024 /* bytes of output one command may produce */

typedef enum {
    PC_CAT_SCAN = 0,   /* bring a receive lens up          */
    PC_CAT_ANALYSE,    /* work on what has been heard      */
    PC_CAT_EVIDENCE,   /* reports, the chain               */
    PC_CAT_SYSTEM,     /* fence, region, battery, help     */
    PC_CAT_COUNT,
} pc_cat_t;

/* A bounded output sink a command writes its result into. */
typedef struct {
    char buf[PC_OUT_CAP];
    size_t len;
    bool overflow;
} pc_out_t;

void pc_out_reset(pc_out_t *o);
void pc_print(pc_out_t *o, const char *s);
void pc_printf(pc_out_t *o, const char *fmt, ...);
void pc_println(pc_out_t *o, const char *s);

/* The operations a command may perform. The firmware provides these; the
 * console never touches hardware itself. Any may be NULL - a command whose op
 * is NULL reports "not wired on this build" rather than crashing. Note what is
 * absent: there is no transmit, no inject, no deauth. The vtable cannot
 * express an attack because the firmware cannot perform one. */
typedef struct pc_ops {
    /* Activate a lens by id (e.g. "wifi.watch"). Returns false if unknown. */
    bool (*activate_lens)(const char *lens_id);
    /* Stop the active lens. */
    void (*deactivate)(void);
    /* Ask the active lens to camp on / survey a channel. camp<0 => survey. */
    void (*set_channel)(int channel);
    /* Set the analysis target (for Locate), as 6 raw bytes. */
    void (*set_target)(const uint8_t bssid[6]);
    /* Set the regulatory region 0..3. */
    void (*set_region)(int region);
    /* Write a one-line status of the active lens' current verdict into o. */
    void (*status_line)(pc_out_t *o);
    /* Write the transmit-fence status into o. */
    void (*fence_status)(pc_out_t *o);
    /* Write a session report (redacted) into o. */
    void (*report)(pc_out_t *o);
    /* Select a training scenario by name for the Range/Footprint. */
    bool (*select_scenario)(const char *name);
    /* Adopt whatever the active lens has in view as the trusted site baseline
     * (Sentinel). Returns how many networks were captured. Receive-only: this
     * only remembers what has already been heard - it emits nothing. */
    unsigned (*adopt_baseline)(void);
    /* Clear the Aegis latch. The operator has read the accumulated findings
     * and is starting a fresh watch; the device never clears them itself. */
    void (*acknowledge)(void);
    void *ctx;
} pc_ops_t;

typedef struct pc_console pc_console_t;

/* One command in the table. */
typedef struct {
    const char *name;
    const char *usage;   /* e.g. "watch [camp <ch> | survey]" */
    const char *summary; /* one line for `help`               */
    pc_cat_t category;
    uint8_t min_args;    /* not counting the command word     */
    uint8_t max_args;
    void (*run)(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out);
} pc_cmd_t;

/* The built-in Pharos command table, and its length. Data, so a test can walk
 * it and assert the receive-only invariant. */
const pc_cmd_t *pc_table(unsigned *count);
const pc_cmd_t *pc_find(const char *name);

/* Tokenise a line in place into argv (splitting on runs of spaces/tabs).
 * Returns argc. Does not allocate; argv points into `line`. */
int pc_tokenise(char *line, char **argv, int max);

/* Parse "aa:bb:cc:dd:ee:ff" (or with '-') into 6 bytes. Returns true on a
 * well-formed address. */
bool pc_parse_mac(const char *s, uint8_t out[6]);

/* Execute one command line against ops, writing results into out. Returns
 * false only for an empty line or an unknown command (the message is still
 * written to out). Never transmits - see the header note. */
bool pc_exec(const pc_ops_t *ops, const char *line, pc_out_t *out);

const char *pc_cat_name(pc_cat_t c);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_CONSOLE_H */
