/* Pharos - the palette, as data. See pharos_theme.h for what a theme may and
 * may not change; the short version is chrome yes, verdict never. */
#include "pharos_theme.h"

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "pharos_bsp.h"
#endif

/* Five, and each one has a reason to exist. A sixth that was only another hue
 * would be a longer list to scroll and nothing else.
 *
 * The dark values are deliberately not "the accent, darker". On an AMOLED the
 * low end of a channel has very few levels - 0x081C29 was (1,7,5) out of
 * (31,63,31) in RGB565 and banded visibly across a 462 px disc - so the
 * grooves are mixed towards neutral rather than left as thin colour. */
static const pharos_theme_t k_themes[] = {
    {
        .name = "Beacon",
        .note = "cyan on true black",
        .accent = 0x21B6C6, .rim = 0x296984,
        .track = 0x18384A, .track2 = 0x081C21,
        .pip_on = 0x184152, .pip_up = 0x081421,
        .text = 0xE7F7F7, .dim = 0x94BECE, .dimmer = 0x6392A5,
        .denied = 0x39596B, .ghost = 0x294152,
    },
    {
        .name = "Abyss",
        .note = "colder than cyan",
        .accent = 0x4A7DF7, .rim = 0x29498C,
        .track = 0x182852, .track2 = 0x081021,
        .pip_on = 0x21346B, .pip_up = 0x081021,
        .text = 0xE7EFFF, .dim = 0xADBEE7, .dimmer = 0x738ABD,
        .denied = 0x42557B, .ghost = 0x29385A,
    },
    {
        .name = "Violet",
        .note = "furthest from a warning",
        .accent = 0x9C69F7, .rim = 0x5A3C8C,
        .track = 0x291C52, .track2 = 0x100C21,
        .pip_on = 0x39286B, .pip_up = 0x100821,
        .text = 0xF7EBFF, .dim = 0xC6B6E7, .dimmer = 0x9C86BD,
        .denied = 0x63517B, .ghost = 0x42305A,
    },
    {
        .name = "Mono",
        .note = "grey: verdicts stand out",
        .accent = 0xD6D7D6, .rim = 0x6B6D6B,
        .track = 0x393839, .track2 = 0x181818,
        .pip_on = 0x525152, .pip_up = 0x181818,
        .text = 0xFFFFFF, .dim = 0xBDBEBD, .dimmer = 0x8C8E8C,
        .denied = 0x5A595A, .ghost = 0x4A494A,
    },
    {
        /* A defensive tool held up in a dark corridor: the brightest thing in
         * the room is the reason somebody looks at you. */
        .name = "Nightwatch",
        .note = "dim, for dark rooms",
        .accent = 0x086D7B, .rim = 0x083442,
        .track = 0x081C21, .track2 = 0x000C10,
        .pip_on = 0x082431, .pip_up = 0x000808,
        .text = 0x8CB6BD, .dim = 0x5A828C, .dimmer = 0x395D63,
        .denied = 0x29414A, .ghost = 0x183039,
    },
};

#define THEME_N ((unsigned)(sizeof(k_themes) / sizeof(k_themes[0])))

static unsigned s_index;
static uint8_t s_brightness = 100;

unsigned pharos_theme_count(void) { return THEME_N; }

const pharos_theme_t *pharos_theme_at(unsigned index)
{
    return (index < THEME_N) ? &k_themes[index] : (const pharos_theme_t *)0;
}

const pharos_theme_t *pharos_theme(void)
{
    return &k_themes[s_index < THEME_N ? s_index : 0];
}

unsigned pharos_theme_index(void) { return s_index; }

bool pharos_theme_set(unsigned index)
{
    if (index >= THEME_N || index == s_index) {
        return false;
    }
    s_index = index;
    pharos_theme_save();
    return true;
}

bool pharos_theme_next(void)
{
    return pharos_theme_set((s_index + 1u) % THEME_N);
}

uint8_t pharos_theme_brightness(void) { return s_brightness; }

bool pharos_theme_set_brightness(uint8_t pct)
{
    if (pct > 100u) {
        pct = 100u;
    }
    /* Never all the way off. A screen you cannot turn back on without the
     * console is a bricked screen as far as anybody holding it is concerned. */
    if (pct < 10u) {
        pct = 10u;
    }
    if (pct == s_brightness) {
        return false;
    }
    s_brightness = pct;
#ifdef ESP_PLATFORM
    pharos_bsp_brightness((uint8_t)((pct * 255u) / 100u));
#endif
    pharos_theme_save();
    return true;
}

#ifdef ESP_PLATFORM
void pharos_theme_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, "ui_theme", &v) == ESP_OK && v < THEME_N) {
        s_index = v;
    }
    if (nvs_get_u8(h, "ui_bright", &v) == ESP_OK && v >= 10u && v <= 100u) {
        s_brightness = v;
        pharos_bsp_brightness((uint8_t)((v * 255u) / 100u));
    }
    nvs_close(h);
}

void pharos_theme_save(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "ui_theme", (uint8_t)s_index);
    nvs_set_u8(h, "ui_bright", s_brightness);
    nvs_commit(h);
    nvs_close(h);
}
#else
void pharos_theme_load(void) {}
void pharos_theme_save(void) {}
#endif
