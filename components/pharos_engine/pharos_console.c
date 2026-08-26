#include "pharos_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- output sink ----------------------------------------------------- */

void pc_out_reset(pc_out_t *o)
{
    if (o) {
        o->len = 0;
        o->overflow = false;
        o->buf[0] = '\0';
    }
}

void pc_print(pc_out_t *o, const char *s)
{
    if (!o || !s) {
        return;
    }
    const size_t n = strlen(s);
    if (o->len + n + 1 > PC_OUT_CAP) {
        o->overflow = true;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

void pc_println(pc_out_t *o, const char *s)
{
    pc_print(o, s);
    pc_print(o, "\n");
}

void pc_printf(pc_out_t *o, const char *fmt, ...)
{
    if (!o) {
        return;
    }
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    pc_print(o, tmp);
}

/* ---- tokeniser ------------------------------------------------------- */

int pc_tokenise(char *line, char **argv, int max)
{
    if (!line || !argv || max <= 0) {
        return 0;
    }
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return argc;
}

/* ---- mac parse ------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pc_parse_mac(const char *s, uint8_t out[6])
{
    if (!s || !out) {
        return false;
    }
    unsigned bytes = 0;
    while (bytes < 6) {
        const int hi = hexval(*s++);
        if (hi < 0) return false;
        const int lo = hexval(*s++);
        if (lo < 0) return false;
        out[bytes++] = (uint8_t)((hi << 4) | lo);
        if (bytes < 6) {
            if (*s != ':' && *s != '-') return false;
            s++;
        }
    }
    return *s == '\0';
}

/* ---- command handlers ------------------------------------------------ */

static void need_ops(pc_out_t *o, const void *fn, const char *what)
{
    if (!fn) {
        pc_printf(o, "%s is not wired on this build\n", what);
    }
}

/* Bring up a receive lens, optionally camp/survey. Shared by the scan cmds. */
static void run_scan(const pc_ops_t *ops, const char *lens_id, int argc,
                     char **argv, pc_out_t *out)
{
    if (!ops->activate_lens) {
        need_ops(out, (const void *)ops->activate_lens, "lens control");
        return;
    }
    if (!ops->activate_lens(lens_id)) {
        pc_printf(out, "could not start %s\n", lens_id);
        return;
    }
    /* Optional: camp <ch> | survey */
    if (argc >= 1 && strcmp(argv[0], "camp") == 0 && ops->set_channel) {
        const int ch = (argc >= 2) ? atoi(argv[1]) : 0;
        if (ch < 1 || ch > 14) {
            pc_println(out, "camp needs a channel 1..14");
            return;
        }
        ops->set_channel(ch);
        pc_printf(out, "%s: camped on channel %d\n", lens_id, ch);
    } else if (argc >= 1 && strcmp(argv[0], "survey") == 0 && ops->set_channel) {
        ops->set_channel(-1);
        pc_printf(out, "%s: surveying 1..13\n", lens_id);
    } else {
        pc_printf(out, "%s: started\n", lens_id);
    }
    if (ops->status_line) {
        ops->status_line(out);
    }
}

static void cmd_scan(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.spectrum", 0, NULL, out);
}
static void cmd_watch(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    run_scan(ops, "wifi.watch", argc - 1, argv + 1, out);
}
static void cmd_census(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.census", 0, NULL, out);
}
static void cmd_twin(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.twin", 0, NULL, out);
}
static void cmd_karma(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.karma", 0, NULL, out);
}
static void cmd_mirage(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.mirage", 0, NULL, out);
}
static void cmd_probe(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    run_scan(ops, "wifi.probe", 0, NULL, out);
}

static void cmd_harvest(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    /* Passes its arguments on, unlike the survey lenses: Harvest can camp,
     * and a handshake is over in a hundred milliseconds. */
    run_scan(ops, "wifi.harvest", argc - 1, argv + 1, out);
}

static void cmd_squall(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    run_scan(ops, "wifi.squall", argc - 1, argv + 1, out);
}

/* Vigil: the BLE tracker watch. `vigil mine` marks the current tag as your
 * own - your earbuds are supposed to follow you. */
static void cmd_vigil(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    if (argc >= 2 && strcmp(argv[1], "mine") == 0) {
        if (!ops->mark_known) {
            need_ops(out, (const void *)ops->mark_known, "mine");
            return;
        }
        pc_println(out, ops->mark_known()
                            ? "marked as yours - excluded from now on"
                            : "nothing to mark yet");
        return;
    }
    if (argc >= 2) {
        pc_printf(out, "usage: %s\n", pc_find("vigil")->usage);
        return;
    }
    if (!ops->activate_lens || !ops->activate_lens("ble.vigil")) {
        pc_println(out, "could not start Vigil");
        return;
    }
    pc_println(out, "vigil: watching for trackers. Carry it as you move - a tag");
    pc_println(out, "       that follows you between places is the finding.");
    if (ops->status_line) {
        ops->status_line(out);
    }
}

/* Aegis: the accumulated picture. `aegis ack` clears the latch once the
 * operator has seen it - the device never forgets a finding on its own. */
static void cmd_aegis(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    if (argc >= 2 && strcmp(argv[1], "ack") == 0) {
        if (!ops->acknowledge) {
            need_ops(out, (const void *)ops->acknowledge, "ack");
            return;
        }
        ops->acknowledge();
        pc_println(out, "aegis: latch cleared - a fresh watch starts now");
        return;
    }
    if (argc >= 2) {
        pc_printf(out, "usage: %s\n", pc_find("aegis")->usage);
        return;
    }
    if (!ops->activate_lens || !ops->activate_lens("sys.aegis")) {
        pc_println(out, "could not start Aegis");
        return;
    }
    pc_println(out, "aegis: every finding so far, and what it adds up to");
    if (ops->status_line) {
        ops->status_line(out);
    }
}

/* Sentinel: bring up the change-detector, or - with `adopt` - freeze the
 * current view as the trusted baseline. Adopting only remembers what has
 * already been heard; like everything here it transmits nothing. */
static void cmd_sentinel(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    if (argc >= 2 && strcmp(argv[1], "adopt") == 0) {
        if (!ops->adopt_baseline) {
            need_ops(out, (const void *)ops->adopt_baseline, "adopt");
            return;
        }
        const unsigned n = ops->adopt_baseline();
        if (n == 0) {
            pc_println(out, "nothing in view yet - let Sentinel survey, then adopt");
        } else {
            pc_printf(out, "baseline adopted: %u network%s now trusted\n",
                      n, n == 1 ? "" : "s");
        }
        return;
    }
    if (argc >= 2) {
        pc_printf(out, "usage: %s\n", pc_find("sentinel")->usage);
        return;
    }
    if (!ops->activate_lens || !ops->activate_lens("wifi.sentinel")) {
        pc_println(out, "could not start Sentinel");
        return;
    }
    pc_println(out, "sentinel: watching for change. 'sentinel adopt' when clean, 'status' to diff");
    if (ops->status_line) {
        ops->status_line(out);
    }
}

static void cmd_locate(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    uint8_t bssid[6];
    if (!pc_parse_mac(argv[1], bssid)) {
        pc_println(out, "locate needs a BSSID, e.g. locate 02:66:6e:00:00:02");
        return;
    }
    if (!ops->activate_lens || !ops->activate_lens("wifi.locate")) {
        pc_println(out, "could not start Locate");
        return;
    }
    if (ops->set_target) {
        ops->set_target(bssid);
    }
    pc_printf(out, "locate: hunting %02x:%02x:%02x:%02x:%02x:%02x\n",
              bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    if (ops->status_line) {
        ops->status_line(out);
    }
}

static void cmd_footprint(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    if (!ops->activate_lens || !ops->activate_lens("train.footprint")) {
        pc_println(out, "could not start Footprint");
        return;
    }
    if (argc >= 2 && ops->select_scenario) {
        if (!ops->select_scenario(argv[1])) {
            pc_printf(out, "unknown scenario '%s' (try: deauth twin flood probe)\n", argv[1]);
        }
    }
    pc_println(out, "footprint: how loud is this attack, camped vs hopping?");
    if (ops->status_line) {
        ops->status_line(out);
    }
}

static void cmd_ward(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (!ops->activate_lens || !ops->activate_lens("wifi.ward")) {
        pc_println(out, "could not start Ward");
        return;
    }
    pc_println(out, "ward: guarding one network - tap centre to adopt the nearest");
    if (ops->status_line) {
        ops->status_line(out);
    }
}

static void cmd_report(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (!ops->report) {
        need_ops(out, (const void *)ops->report, "report");
        return;
    }
    ops->report(out);
}

static void cmd_fence(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (ops->fence_status) {
        ops->fence_status(out);
    } else {
        pc_println(out, "fence: receive-only (status op not wired)");
    }
}

static void cmd_region(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    if (argc < 2) {
        pc_println(out, "region <0=World 1=FCC 2=ETSI 3=Japan>");
        return;
    }
    const int r = atoi(argv[1]);
    if (r < 0 || r > 3) {
        pc_println(out, "region must be 0..3");
        return;
    }
    if (ops->set_region) {
        ops->set_region(r);
    }
    static const char *names[] = { "World (1-13)", "FCC (1-11)", "ETSI (1-13)", "Japan (1-14)" };
    pc_printf(out, "region set: %s\n", names[r]);
}

static void cmd_stop(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (ops->deactivate) {
        ops->deactivate();
    }
    pc_println(out, "stopped");
}

static void cmd_status(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)argc; (void)argv;
    if (ops->status_line) {
        ops->status_line(out);
    } else {
        pc_println(out, "no active lens");
    }
}

/* help is defined after the table so it can walk it. */
static void cmd_help(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out);

/* ---- the command table ----------------------------------------------- */

static const pc_cmd_t k_cmds[] = {
    /* name         usage                              summary                                   category         min max  run */
    { "help",     "help [command]",                  "list commands, or explain one",          PC_CAT_SYSTEM,   0, 1, cmd_help },
    { "scan",     "scan",                            "2.4 GHz airtime waterfall (Spectrum)",   PC_CAT_SCAN,     0, 0, cmd_scan },
    { "watch",    "watch [camp <ch>|survey]",        "deauth-flood watch",                     PC_CAT_SCAN,     0, 2, cmd_watch },
    { "census",   "census",                          "grade nearby networks A+..F",            PC_CAT_SCAN,     0, 0, cmd_census },
    { "twin",     "twin",                            "evil-twin / rogue-AP detector",          PC_CAT_SCAN,     0, 0, cmd_twin },
    { "karma",    "karma",                           "KARMA/MANA responder detector",          PC_CAT_SCAN,     0, 0, cmd_karma },
    { "mirage",   "mirage",                          "beacon / SSID-spam flood detector",      PC_CAT_SCAN,     0, 0, cmd_mirage },
    { "probe",    "probe",                           "what devices leak in probe requests",    PC_CAT_SCAN,     0, 0, cmd_probe },
    { "sentinel", "sentinel [adopt]",                "what changed since your site baseline",  PC_CAT_SCAN,     0, 1, cmd_sentinel },
    { "harvest",  "harvest [camp <ch>|survey]",      "catch handshake/PMKID collection",       PC_CAT_SCAN,     0, 2, cmd_harvest },
    { "squall",   "squall [camp <ch>|survey]",       "busy, broken, or jammed?",               PC_CAT_SCAN,     0, 2, cmd_squall },
    { "vigil",    "vigil [mine]",                    "is a tracker travelling with you?",      PC_CAT_SCAN,     0, 1, cmd_vigil },
    { "aegis",    "aegis [ack]",                     "the whole picture: every finding so far",PC_CAT_ANALYSE,  0, 1, cmd_aegis },
    { "locate",   "locate <bssid>",                  "walk to a transmitter (hotter/colder)",  PC_CAT_ANALYSE,  1, 1, cmd_locate },
    { "footprint","footprint [scenario]",            "how detectable is an attack? (OPSEC)",   PC_CAT_ANALYSE,  0, 1, cmd_footprint },
    { "ward",     "ward",                            "guard one network and watch only it",    PC_CAT_SCAN,     0, 0, cmd_ward },
    { "status",   "status",                          "the active lens' current verdict",       PC_CAT_ANALYSE,  0, 0, cmd_status },
    { "report",   "report",                          "write a redacted session report",        PC_CAT_EVIDENCE, 0, 0, cmd_report },
    { "fence",    "fence",                           "prove the transmit fence is clean",      PC_CAT_SYSTEM,   0, 0, cmd_fence },
    { "region",   "region <0..3>",                   "set the regulatory channel plan",        PC_CAT_SYSTEM,   1, 1, cmd_region },
    { "stop",     "stop",                            "stop the active lens",                   PC_CAT_SYSTEM,   0, 0, cmd_stop },
};

const pc_cmd_t *pc_table(unsigned *count)
{
    if (count) {
        *count = (unsigned)(sizeof(k_cmds) / sizeof(k_cmds[0]));
    }
    return k_cmds;
}

const pc_cmd_t *pc_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (unsigned i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        if (strcmp(k_cmds[i].name, name) == 0) {
            return &k_cmds[i];
        }
    }
    return NULL;
}

const char *pc_cat_name(pc_cat_t c)
{
    switch (c) {
    case PC_CAT_SCAN:     return "scan";
    case PC_CAT_ANALYSE:  return "analyse";
    case PC_CAT_EVIDENCE: return "evidence";
    case PC_CAT_SYSTEM:   return "system";
    default:              return "?";
    }
}

static void cmd_help(const pc_ops_t *ops, int argc, char **argv, pc_out_t *out)
{
    (void)ops;
    if (argc >= 2) {
        const pc_cmd_t *c = pc_find(argv[1]);
        if (!c) {
            pc_printf(out, "no such command: %s\n", argv[1]);
            return;
        }
        pc_printf(out, "%s\n  %s\n  usage: %s  [%s]\n", c->name, c->summary,
                  c->usage, pc_cat_name(c->category));
        return;
    }
    pc_println(out, "Pharos console - receive-only. No command transmits.");
    for (int cat = 0; cat < PC_CAT_COUNT; cat++) {
        pc_printf(out, "\n[%s]\n", pc_cat_name((pc_cat_t)cat));
        for (unsigned i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
            if ((int)k_cmds[i].category == cat) {
                pc_printf(out, "  %-10s %s\n", k_cmds[i].name, k_cmds[i].summary);
            }
        }
    }
}

/* ---- exec ------------------------------------------------------------ */

bool pc_exec(const pc_ops_t *ops, const char *line, pc_out_t *out)
{
    static const pc_ops_t k_empty;
    if (!ops) {
        ops = &k_empty;
    }
    if (out) {
        pc_out_reset(out);
    }
    if (!line) {
        return false;
    }

    char work[PC_MAX_LINE];
    size_t n = strlen(line);
    if (n >= sizeof(work)) {
        n = sizeof(work) - 1;
    }
    memcpy(work, line, n);
    work[n] = '\0';

    /* Strip a trailing comment and CR. */
    for (size_t i = 0; i < n; i++) {
        if (work[i] == '#' || work[i] == '\r' || work[i] == '\n') {
            work[i] = '\0';
            break;
        }
    }

    char *argv[PC_MAX_ARGS];
    const int argc = pc_tokenise(work, argv, PC_MAX_ARGS);
    if (argc == 0) {
        return false; /* blank line */
    }

    const pc_cmd_t *c = pc_find(argv[0]);
    if (!c) {
        pc_printf(out, "unknown command: %s  (try 'help')\n", argv[0]);
        return false;
    }

    const int cmd_args = argc - 1;
    if (cmd_args < c->min_args || cmd_args > c->max_args) {
        pc_printf(out, "usage: %s\n", c->usage);
        return true;
    }

    c->run(ops, argc, argv, out);
    return true;
}
