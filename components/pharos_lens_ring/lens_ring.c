/* Pharos lens: Ring - choosing what the home screen watches
 *
 * Thirteen watches ship armed, and that is the right default for somebody who
 * has just switched the device on and does not yet know what any of them do.
 * It is the wrong setting for almost everybody after that: somebody working a
 * Wi-Fi engagement does not need the microphone, somebody sweeping a room for
 * trackers does not need the handshake collector, and there is only one radio,
 * so every watch left on is airtime the ones they care about do not get.
 *
 * That last part is why this page leads with the LAP TIME. Switching a watch
 * off is not tidying a display - it makes the rest come round sooner, and the
 * number at the top says by how much. A settings page that hid that would be
 * asking somebody to make a trade-off without showing them the trade.
 *
 * Reached by holding anywhere on the home ring. That gesture used to toggle
 * the rotation silently, which meant a stray long-press could stop the device
 * watching with nothing to say it had happened - so the pause now lives here,
 * as a row, where it is visible and reversible.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "pharos_lens.h"
#include "pharos_tower.h"
#include "pharos_ui.h"

static const char *TAG = "lens.ring";

/* Row 0 is the rotation itself; the watches follow. */
#define RING_ROW_RUN 0u
#define RING_ROW_FIRST 1u

static bool ring_mount(void)
{
    ESP_LOGI(TAG, "ring: %u watches, %ums a lap", pharos_ui_ring_count(),
             (unsigned)pharos_ui_ring_lap_ms());
    return true;
}

static bool k_ring_display(struct pharos_lens_display *o)
{
    unsigned on = 0;
    const unsigned n = pharos_ui_ring_count();
    for (unsigned i = 0; i < n; i++) {
        bool armed = false;
        if (pharos_ui_ring_at(i, NULL, &armed, NULL, NULL) && armed) {
            on++;
        }
    }
    const uint32_t lap = pharos_ui_ring_lap_ms();

    snprintf(o->big, sizeof(o->big), "%u", on);
    snprintf(o->band, sizeof(o->band), "of %u watching", n);
    /* THE TRADE, ON THE GLASS. One radio: fewer watches means each comes round
     * sooner, and that is the entire reason to switch any of them off. */
    snprintf(o->detail, sizeof(o->detail), "%u.%us a lap", (unsigned)(lap / 1000u),
             (unsigned)((lap % 1000u) / 100u));
    snprintf(o->advice, sizeof(o->advice), "%s",
             pharos_ui_ring_running() ? "Touch a row to switch it on or off."
                                      : "Rotation paused - nothing is watching.");
    if (!pharos_ui_ring_running()) {
        snprintf(o->why, sizeof(o->why), "%s", "top row starts it again");
    }

    o->has_score = false;
    /* Settings, not a finding: the ring must not colour this page as a threat. */
    o->has_alert = true;
    o->alert = pharos_ui_ring_running() ? 0u : 1u;
    return true;
}

static const char *period_word(uint8_t p)
{
    switch (p) {
    case 1:  return "every lap";
    case 2:  return "every 2nd";
    case 3:  return "every 3rd";
    default: return "every 4th";
    }
}

static bool k_ring_row(unsigned index, struct pharos_lens_row *out)
{
    /* Every watch row toggles; the lap-time row is a reading. */
    out->tappable = true;

    if (index == RING_ROW_RUN) {
        snprintf(out->left, sizeof(out->left), "rotation");
        snprintf(out->right, sizeof(out->right), "%s",
                 pharos_ui_ring_running() ? "running" : "PAUSED");
        out->tone = pharos_ui_ring_running() ? PHAROS_TONE_GOOD
                                             : PHAROS_TONE_WARN;
        return true;
    }

    const unsigned i = index - RING_ROW_FIRST;
    const char *name = NULL;
    bool armed = false;
    uint8_t period = 1, state = 0;
    if (!pharos_ui_ring_at(i, &name, &armed, &period, &state)) {
        /* Then the lap time, which is what all of the above adds up to. */
        if (i == pharos_ui_ring_count()) {
            const uint32_t lap = pharos_ui_ring_lap_ms();
            out->tappable = false; /* a reading, not a control */
            snprintf(out->left, sizeof(out->left), "one lap takes");
            snprintf(out->right, sizeof(out->right), "%u.%us",
                     (unsigned)(lap / 1000u), (unsigned)((lap % 1000u) / 100u));
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        return false;
    }

    snprintf(out->left, sizeof(out->left), "%s", name ? name : "?");
    if (!armed) {
        snprintf(out->right, sizeof(out->right), "off");
        out->tone = PHAROS_TONE_DIM;
    } else {
        /* An armed watch shows HOW OFTEN, not just that it is on - the period
         * is the other half of the airtime trade and is editable here too. */
        snprintf(out->right, sizeof(out->right), "%s", period_word(period));
        out->tone = (period == 1u) ? PHAROS_TONE_GOOD : PHAROS_TONE_NEUTRAL;
    }
    return true;
}

/* Touching a row switches that watch on or off. That is all it does.
 *
 * The first version cycled on -> slower -> slower -> off, so that one finger
 * could reach every setting. It also meant switching a watch OFF took up to
 * four presses, and the thing somebody actually came to this page to do was
 * the hardest thing on it. The period is a considered default - events every
 * lap, standing facts less often - and it is shown on the row so nothing is
 * hidden; anybody who genuinely wants to change it has `ring` on the console,
 * where there is room to be precise. */
static bool k_ring_edit(unsigned index)
{
    if (index == RING_ROW_RUN) {
        pharos_ui_ring_set_running(!pharos_ui_ring_running());
        return true;
    }
    const unsigned i = index - RING_ROW_FIRST;
    if (i >= pharos_ui_ring_count()) {
        return false; /* the lap-time row is a reading, not a control */
    }
    if (!pharos_ui_ring_toggle(i)) {
        /* Refused: this is the last armed watch, and a ring watching nothing
         * would be a state reached through the ring with no obvious way back.
         * Say so rather than letting the press look broken. */
        ESP_LOGI(TAG, "ring: %u is the last watch armed; leaving it on", i);
    }
    return true;
}

static bool k_ring_expand(unsigned row, unsigned sub,
                          struct pharos_lens_row *out)
{
    if (row == RING_ROW_RUN) {
        switch (sub) {
        case 0:
            snprintf(out->left, sizeof(out->left), "one radio, taking turns");
            snprintf(out->right, sizeof(out->right), "%u", pharos_ui_ring_count());
            out->tone = PHAROS_TONE_NEUTRAL;
            return true;
        case 1: {
            const uint32_t lap = pharos_ui_ring_lap_ms();
            snprintf(out->left, sizeof(out->left), "a full lap");
            snprintf(out->right, sizeof(out->right), "%u.%us",
                     (unsigned)(lap / 1000u), (unsigned)((lap % 1000u) / 100u));
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        case 2:
            snprintf(out->left, sizeof(out->left), "paused means deaf");
            snprintf(out->right, sizeof(out->right), "%s",
                     pharos_ui_ring_running() ? "not now" : "NOW");
            out->tone = pharos_ui_ring_running() ? PHAROS_TONE_GOOD
                                                 : PHAROS_TONE_BAD;
            return true;
        case 3:
            snprintf(out->left, sizeof(out->left), "choices are remembered");
            snprintf(out->right, sizeof(out->right), "yes");
            out->tone = PHAROS_TONE_DIM;
            return true;
        default:
            return false;
        }
    }

    const unsigned i = row - RING_ROW_FIRST;
    const char *name = NULL;
    bool armed = false;
    uint8_t period = 1, state = 0;
    if (!pharos_ui_ring_at(i, &name, &armed, &period, &state)) {
        return false;
    }
    static const char *k_state[] = { "not yet", "quiet", "noted", "elevated",
                                     "ALARM" };
    switch (sub) {
    case 0:
        snprintf(out->left, sizeof(out->left), "on the ring");
        snprintf(out->right, sizeof(out->right), "%s", armed ? "yes" : "no");
        out->tone = armed ? PHAROS_TONE_GOOD : PHAROS_TONE_DIM;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "takes a turn");
        snprintf(out->right, sizeof(out->right), "%s",
                 armed ? period_word(period) : "never");
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "last said");
        snprintf(out->right, sizeof(out->right), "%s",
                 (state < 5u) ? k_state[state] : "?");
        out->tone = (state >= 4u)   ? PHAROS_TONE_BAD
                    : (state >= 3u) ? PHAROS_TONE_WARN
                    : (state >= 1u) ? PHAROS_TONE_GOOD
                                    : PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "press again to");
        snprintf(out->right, sizeof(out->right), "%s",
                 armed ? "switch off" : "switch on");
        out->tone = PHAROS_TONE_DIM;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_ring = {
    .id = "sys.ring",
    .purpose = "what the ring watches",
    .name = "Ring",
    .summary = "Choose which watches the home screen carries, and how often",
    .glyph = "gear",
    .kind = PHAROS_LENS_SYSTEM,
    .caps = PHAROS_CAP_NONE,
    .budget_ma = 25,
    .on_mount = ring_mount,
    .display = k_ring_display,
    .row = k_ring_row,
    .row_head_left = "WATCH",
    .row_head_right = "HOW OFTEN",
    .row_edit = k_ring_edit,
    .row_expand = k_ring_expand,
};

PHAROS_LENS_REGISTER(&k_ring);
