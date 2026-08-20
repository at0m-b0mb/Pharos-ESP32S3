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
        .accent = 0x21B6C6, .rim = 0x2A6B80,
        .track = 0x18384A, .track2 = 0x0A1C26,
        .pip_on = 0x1B4257, .pip_up = 0x0A1620,
        .text = 0xE7F7F7, .dim = 0x94BECC, .dimmer = 0x6693A6,
        .denied = 0x3A5A6B, .ghost = 0x2A4257,
    },
    {
        .name = "Abyss",
        .note = "colder than cyan",
        .accent = 0x4C7DF0, .rim = 0x2B4A8C,
        .track = 0x1A2A52, .track2 = 0x0B1226,
        .pip_on = 0x22376B, .pip_up = 0x0A1020,
        .text = 0xE6ECFA, .dim = 0xA9BCE4, .dimmer = 0x7488BE,
        .denied = 0x44547E, .ghost = 0x2C3A5E,
    },
    {
        .name = "Violet",
        .note = "furthest from a warning",
        .accent = 0x9B6BF0, .rim = 0x5A3E8C,
        .track = 0x2E1F52, .track2 = 0x150C26,
        .pip_on = 0x3E2A6B, .pip_up = 0x120A20,
        .text = 0xF0E9FA, .dim = 0xC7B4E4, .dimmer = 0x9A85BE,
        .denied = 0x63527E, .ghost = 0x40325E,
    },
    {
        .name = "Mono",
        .note = "grey: verdicts stand out",
        .accent = 0xD8D8D8, .rim = 0x6E6E6E,
        .track = 0x3A3A3A, .track2 = 0x1A1A1A,
        .pip_on = 0x4E4E4E, .pip_up = 0x161616,
        .text = 0xFFFFFF, .dim = 0xC0C0C0, .dimmer = 0x8E8E8E,
        .denied = 0x5A5A5A, .ghost = 0x484848,
    },
    {
        /* A defensive tool held up in a dark corridor: the brightest thing in
         * the room is the reason somebody looks at you. */
        .name = "Nightwatch",
        .note = "dim, for dark rooms",
        .accent = 0x0E6E7A, .rim = 0x0E3540,
        .track = 0x0A1C24, .track2 = 0x050E12,
        .pip_on = 0x0D2430, .pip_up = 0x050A0E,
        .text = 0x8FB6BE, .dim = 0x5A8089, .dimmer = 0x3E5C64,
        .denied = 0x28434B, .ghost = 0x1C3038,
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
