/* Pharos host tests, part nine: the command console.
 *
 * The parser and dispatch are tested against a mock ops table, and - the point
 * - the whole command table is walked to assert the receive-only invariant:
 * every command is scan/analyse/evidence/system, never transmit, because the
 * ops vtable the console can call has no way to emit a frame.
 */
#include "pharos_console.h"
#include "test_support.h"

/* ---- a mock firmware the console drives ------------------------------ */

typedef struct {
    char last_lens[32];
    int  activations;
    int  last_channel;   /* -2 = untouched, -1 = survey, >=1 = camp */
    uint8_t target[6];
    bool target_set;
    int  region;
    int  deactivations;
    bool report_called;
    bool fence_called;
    char last_scenario[24];
    bool activate_ok;
    bool scenario_ok;
    int  adopts;
    unsigned adopt_ret;
    int  acks;
} mock_t;

static mock_t g;

static bool m_activate(const char *id)
{
    snprintf(g.last_lens, sizeof(g.last_lens), "%s", id ? id : "");
    g.activations++;
    return g.activate_ok;
}
static void m_deactivate(void) { g.deactivations++; }
static void m_set_channel(int ch) { g.last_channel = ch; }
static void m_set_target(const uint8_t b[6]) { memcpy(g.target, b, 6); g.target_set = true; }
static void m_set_region(int r) { g.region = r; }
static void m_status(pc_out_t *o) { pc_println(o, "  band=BACKGROUND score=22/96"); }
static void m_fence(pc_out_t *o) { pc_println(o, "fence: clean (receive-only)"); }
static void m_report(pc_out_t *o) { pc_print(o, "{\"tool\":\"pharos\"}"); }
static bool m_scenario(const char *n) { snprintf(g.last_scenario, sizeof(g.last_scenario), "%s", n); return g.scenario_ok; }
static unsigned m_adopt(void) { g.adopts++; return g.adopt_ret; }
static void m_ack(void) { g.acks++; }

static pc_ops_t mock_ops(void)
{
    pc_ops_t o;
    memset(&o, 0, sizeof(o));
    o.activate_lens = m_activate;
    o.deactivate = m_deactivate;
    o.set_channel = m_set_channel;
    o.set_target = m_set_target;
    o.set_region = m_set_region;
    o.status_line = m_status;
    o.fence_status = m_fence;
    o.report = m_report;
    o.select_scenario = m_scenario;
    o.adopt_baseline = m_adopt;
    o.acknowledge = m_ack;
    return o;
}

static void reset_mock(void)
{
    memset(&g, 0, sizeof(g));
    g.last_channel = -2;
    g.activate_ok = true;
    g.scenario_ok = true;
    g.adopt_ret = 3;
}

/* ---- tokeniser ------------------------------------------------------- */

void test_console_tokenise(void)
{
    banner("console: tokeniser");
    char a[] = "watch camp 6";
    char *argv[8];
    CHECK_EQ(pc_tokenise(a, argv, 8), 3);
    CHECK(strcmp(argv[0], "watch") == 0, "argv0");
    CHECK(strcmp(argv[1], "camp") == 0, "argv1");
    CHECK(strcmp(argv[2], "6") == 0, "argv2");

    char b[] = "   spaced    out\t\ttabs  ";
    CHECK_EQ(pc_tokenise(b, argv, 8), 3);
    CHECK(strcmp(argv[0], "spaced") == 0, "leading/mid spaces collapsed");
    CHECK(strcmp(argv[2], "tabs") == 0, "tabs split");

    char empty[] = "     ";
    CHECK_EQ(pc_tokenise(empty, argv, 8), 0);

    /* argv cap respected. */
    char many[] = "a b c d e f g h i j";
    CHECK_EQ(pc_tokenise(many, argv, 4), 4);

    /* MAC parsing, both separators, rejects malformed. */
    uint8_t mac[6];
    CHECK(pc_parse_mac("02:66:6e:aa:bb:cc", mac), "colon mac");
    CHECK(mac[0] == 0x02 && mac[5] == 0xcc, "mac bytes");
    CHECK(pc_parse_mac("AC-11-22-33-44-55", mac), "dash mac, upper hex");
    CHECK(!pc_parse_mac("02:66:6e:aa:bb", mac), "too short rejected");
    CHECK(!pc_parse_mac("02:66:6e:aa:bb:cc:dd", mac), "too long rejected");
    CHECK(!pc_parse_mac("gg:66:6e:aa:bb:cc", mac), "non-hex rejected");
    CHECK(!pc_parse_mac("0266.6eaa.bbcc", mac), "wrong separator rejected");
}

/* ---- dispatch -------------------------------------------------------- */

void test_console_dispatch(void)
{
    banner("console: dispatch");
    pc_ops_t ops = mock_ops();
    pc_out_t out;

    /* Blank and comment lines do nothing. */
    reset_mock();
    CHECK(!pc_exec(&ops, "", &out), "empty line");
    CHECK(!pc_exec(&ops, "   ", &out), "spaces only");
    CHECK(!pc_exec(&ops, "# just a comment", &out), "comment line");
    CHECK_EQ(g.activations, 0);

    /* An unknown command is refused, not run. */
    reset_mock();
    CHECK(!pc_exec(&ops, "deauth", &out), "unknown command refused");
    CHECK(strstr(out.buf, "unknown command") != NULL, "says unknown: %s", out.buf);
    CHECK_EQ(g.activations, 0);

    /* watch survey. */
    reset_mock();
    CHECK(pc_exec(&ops, "watch survey", &out), "watch survey runs");
    CHECK(strcmp(g.last_lens, "wifi.watch") == 0, "watch activates the watch lens");
    CHECK_EQ(g.last_channel, -1);
    CHECK(strstr(out.buf, "surveying") != NULL, "reports surveying");

    /* watch camp 6. */
    reset_mock();
    CHECK(pc_exec(&ops, "watch camp 6", &out), "watch camp runs");
    CHECK_EQ(g.last_channel, 6);
    CHECK(strstr(out.buf, "camped on channel 6") != NULL, "camp confirmed: %s", out.buf);

    /* camp with a bad channel is rejected. */
    reset_mock();
    pc_exec(&ops, "watch camp 99", &out);
    CHECK(strstr(out.buf, "1..14") != NULL, "bad camp channel rejected");

    /* locate needs a valid mac, and sets the target. */
    reset_mock();
    CHECK(pc_exec(&ops, "locate 02:66:6e:00:00:02", &out), "locate runs");
    CHECK(strcmp(g.last_lens, "wifi.locate") == 0, "locate lens");
    CHECK(g.target_set, "target set");
    CHECK(g.target[0] == 0x02 && g.target[2] == 0x6e, "target bytes");

    reset_mock();
    pc_exec(&ops, "locate not-a-mac", &out);
    CHECK(strstr(out.buf, "needs a BSSID") != NULL, "locate rejects a bad mac");
    CHECK(!g.target_set, "no target set on bad mac");

    /* Each scan command maps to the right lens id. */
    struct { const char *cmd; const char *lens; } map[] = {
        { "scan", "wifi.spectrum" }, { "census", "wifi.census" },
        { "twin", "wifi.twin" }, { "karma", "wifi.karma" },
        { "mirage", "wifi.mirage" }, { "probe", "wifi.probe" },
        { "sentinel", "wifi.sentinel" },
        { "harvest", "wifi.harvest" },
        { "squall", "wifi.squall" },
        { "aegis", "sys.aegis" },
        { "footprint", "train.footprint" }, { "range", "train.range" },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        reset_mock();
        CHECK(pc_exec(&ops, map[i].cmd, &out), "%s runs", map[i].cmd);
        CHECK(strcmp(g.last_lens, map[i].lens) == 0, "%s -> %s (got %s)",
              map[i].cmd, map[i].lens, g.last_lens);
    }

    /* footprint passes a scenario through. */
    reset_mock();
    pc_exec(&ops, "footprint deauth", &out);
    CHECK(strcmp(g.last_scenario, "deauth") == 0, "footprint scenario forwarded");

    /* sentinel (no arg) brings the change-detector up but adopts nothing. */
    reset_mock();
    CHECK(pc_exec(&ops, "sentinel", &out), "sentinel runs");
    CHECK(strcmp(g.last_lens, "wifi.sentinel") == 0, "sentinel activates its lens");
    CHECK_EQ(g.adopts, 0);

    /* sentinel adopt freezes the baseline and reports the count. */
    reset_mock();
    g.adopt_ret = 5;
    CHECK(pc_exec(&ops, "sentinel adopt", &out), "sentinel adopt runs");
    CHECK_EQ(g.adopts, 1);
    CHECK_EQ(g.activations, 0); /* adopting does not re-activate a lens */
    CHECK(strstr(out.buf, "5") != NULL, "reports how many were adopted: %s", out.buf);

    /* adopt with nothing in view says so rather than claiming a baseline. */
    reset_mock();
    g.adopt_ret = 0;
    pc_exec(&ops, "sentinel adopt", &out);
    CHECK(strstr(out.buf, "nothing in view") != NULL, "empty adopt is honest: %s", out.buf);

    /* aegis ack clears the latch, and does not re-activate a lens. */
    reset_mock();
    CHECK(pc_exec(&ops, "aegis ack", &out), "aegis ack runs");
    CHECK_EQ(g.acks, 1);
    CHECK_EQ(g.activations, 0);
    CHECK(strstr(out.buf, "cleared") != NULL, "ack confirmed: %s", out.buf);

    reset_mock();
    pc_ops_t no_ack = mock_ops();
    no_ack.acknowledge = NULL;
    pc_exec(&no_ack, "aegis ack", &out);
    CHECK(strstr(out.buf, "not wired") != NULL, "null ack op reported");

    /* adopt with the op unwired is reported, not crashed into. */
    reset_mock();
    pc_ops_t no_adopt = mock_ops();
    no_adopt.adopt_baseline = NULL;
    pc_exec(&no_adopt, "sentinel adopt", &out);
    CHECK(strstr(out.buf, "not wired") != NULL, "null adopt op reported");

    /* report and fence route to their ops. */
    reset_mock();
    pc_exec(&ops, "report", &out);
    CHECK(strstr(out.buf, "pharos") != NULL, "report ran");
    reset_mock();
    pc_exec(&ops, "fence", &out);
    CHECK(strstr(out.buf, "clean") != NULL, "fence ran");

    /* region validates range. */
    reset_mock();
    pc_exec(&ops, "region 1", &out);
    CHECK_EQ(g.region, 1);
    CHECK(strstr(out.buf, "FCC") != NULL, "region named");
    reset_mock();
    pc_exec(&ops, "region 9", &out);
    CHECK(strstr(out.buf, "0..3") != NULL, "bad region rejected");

    /* stop deactivates. */
    reset_mock();
    pc_exec(&ops, "stop", &out);
    CHECK_EQ(g.deactivations, 1);

    /* Arg-count validation: locate with no arg prints usage, does not run. */
    reset_mock();
    pc_exec(&ops, "locate", &out);
    CHECK(strstr(out.buf, "usage:") != NULL, "missing arg -> usage");
    CHECK_EQ(g.activations, 0);

    /* A NULL op is reported, not crashed into. */
    reset_mock();
    pc_ops_t bare;
    memset(&bare, 0, sizeof(bare));
    pc_exec(&bare, "report", &out);
    CHECK(strstr(out.buf, "not wired") != NULL, "null op reported");

    /* Output sink cannot overrun. */
    pc_out_reset(&out);
    for (int i = 0; i < 2000; i++) pc_print(&out, "0123456789");
    CHECK(out.overflow, "sink overflow flagged");
    CHECK(out.len < PC_OUT_CAP, "sink stayed in bounds");
}

/* ---- help + the receive-only invariant ------------------------------- */

void test_console_help_and_safety(void)
{
    banner("console: help + receive-only invariant");
    pc_ops_t ops = mock_ops();
    pc_out_t out;

    reset_mock();
    pc_exec(&ops, "help", &out);
    CHECK(strstr(out.buf, "receive-only") != NULL, "help states receive-only");
    CHECK(strstr(out.buf, "watch") != NULL, "help lists watch");
    CHECK(strstr(out.buf, "locate") != NULL, "help lists locate");

    pc_exec(&ops, "help watch", &out);
    CHECK(strstr(out.buf, "usage:") != NULL, "help <cmd> shows usage");
    pc_exec(&ops, "help nonesuch", &out);
    CHECK(strstr(out.buf, "no such command") != NULL, "help unknown");

    /* THE invariant. Walk the whole table: every command is a receive-side
     * category, and no command name or summary hints at transmission. The
     * console is receive-only by construction - there is no ops entry that can
     * emit a frame, so no command can. */
    unsigned n = 0;
    const pc_cmd_t *t = pc_table(&n);
    CHECK(n >= 15, "the table is populated (%u)", n);
    static const char *banned[] = {
        "deauth", "inject", "beacon", "spam", "evil", "portal", "attack",
        "jam", "tx", "transmit", "send", "flood",
    };
    for (unsigned i = 0; i < n; i++) {
        const pc_cmd_t *c = &t[i];
        CHECK(c->category >= PC_CAT_SCAN && c->category < PC_CAT_COUNT,
              "%s has a valid category", c->name);
        /* No command lives in a "transmit" category - there isn't one. */
        for (unsigned b = 0; b < sizeof(banned) / sizeof(banned[0]); b++) {
            CHECK(strcmp(c->name, banned[b]) != 0,
                  "no offensive command '%s' in the table", banned[b]);
        }
        /* Every command has a handler and usage. */
        CHECK(c->run != NULL, "%s has a handler", c->name);
        CHECK(c->usage && c->summary, "%s documented", c->name);
        CHECK(c->min_args <= c->max_args, "%s arg bounds sane", c->name);
    }

    /* Category names all resolve. */
    for (int cat = 0; cat < PC_CAT_COUNT; cat++) {
        CHECK(pc_cat_name((pc_cat_t)cat)[0] != '?', "category %d named", cat);
    }
}
