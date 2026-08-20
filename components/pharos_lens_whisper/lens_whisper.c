/* Pharos lens: Whisper - the tone you were never meant to hear
 *
 * There is a real, deployed advertising technique in which a shop, a
 * television advert or a web page emits a short tone just above the top of
 * human hearing - typically 18 to 20 kHz - carrying an identifier. An app on
 * a phone in the room hears it through the microphone and reports that this
 * device was in that place, or saw that advert. It is cross-device tracking
 * that travels through the air, it is completely inaudible, and the person
 * being tracked has no way to notice it.
 *
 * That is the same shape as everything else here: a signal aimed at you, that
 * you cannot perceive, that you never agreed to. Whisper makes it visible.
 *
 * ---------------------------------------------------------------------------
 * IT NEVER RECORDS ANYTHING, AND THAT IS STRUCTURAL
 *
 * The capture buffer below is 480 samples - ten milliseconds - and it is
 * overwritten on every read. It is handed to pac_observe(), which converts it
 * to five band energies and returns; nothing retains it. There is no ring, no
 * file, no accumulation, and no API anywhere in this firmware that returns
 * audio. Ten milliseconds is not speech, and five energy figures per window
 * are not recoverable into it.
 *
 * The audible band is measured for exactly one reason: to know the microphone
 * is alive, so "no beacon" can be told apart from "no signal at all". Its
 * LEVEL is used. Its content is not examined, and there is nothing here that
 * could examine it.
 *
 * A microphone on a security device is a serious thing to add. It is added
 * this way, or not at all - see pharos_acoustic.h.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pharos_acoustic.h"
#include "pharos_lens.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#endif

static const char *TAG = "lens.whisper";

/* 48 kHz so that Nyquist is 24 kHz and the 18-21 kHz probes are real. At the
 * default 22.05 kHz every band this lens cares about would be above Nyquist
 * and the whole thing would be measuring aliases. */
#define WHISPER_RATE 48000u
#define WHISPER_WIN  480u /* 10 ms - the entire audio footprint of this lens */

EXT_RAM_BSS_ATTR static pac_engine_t s_engine;
static pac_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
static esp_codec_dev_handle_t s_mic;
static volatile bool s_run;
static TaskHandle_t s_task;

/* The one and only place audio exists in this firmware, and it exists for
 * about ten milliseconds at a time. */
static int16_t s_win[WHISPER_WIN];

static void whisper_task(void *arg)
{
    (void)arg;
    unsigned reads = 0, fails = 0;
    while (s_run) {
        /* esp_codec_dev_read RETURNS A STATUS, NOT A BYTE COUNT.
         *
         * Its header says so plainly - "ESP_CODEC_DEV_OK: Read success" - and
         * ESP_CODEC_DEV_OK is zero. This loop was written as
         *
         *     if (got <= 0) { skip }
         *
         * which reads the success code as failure and discards every window.
         * The screen said "MIC SILENT - cannot hear" and the detail page said
         * "windows heard: 0" while the ES7210 was up, unmuted and delivering
         * audio the whole time. Nothing was wrong with the hardware; the lens
         * was throwing away everything it asked for.
         *
         * On success the driver has filled the whole buffer, so the count is
         * what was requested. */
        const int rc = esp_codec_dev_read(s_mic, s_win, (int)sizeof(s_win));
        if (rc != ESP_CODEC_DEV_OK) {
            fails++;
            if ((fails % 50u) == 1u) {
                ESP_LOGW(TAG, "microphone read failed: %d (%u ok, %u failed)",
                         rc, reads, fails);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (reads == 0) {
            ESP_LOGI(TAG, "microphone delivering %u samples a window",
                     (unsigned)(sizeof(s_win) / sizeof(int16_t)));
        }
        reads++;
        const int got = (int)sizeof(s_win);
        const unsigned n = (unsigned)got / sizeof(int16_t);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            pac_observe(&s_engine, s_win, n, WHISPER_RATE);
            xSemaphoreGive(s_lock);
        }
        /* Overwritten on the next read; nothing here keeps it. */
    }
    /* Wipe on the way out rather than leaving the last window in .bss. It
     * costs nothing and it means a core dump cannot contain even 10 ms. */
    memset(s_win, 0, sizeof(s_win));
    s_task = NULL;
    vTaskDelete(NULL);
}
#endif

static bool whisper_mount(void)
{
    pac_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL;
}

static bool whisper_start(void)
{
#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
    if (!s_mic) {
        s_mic = bsp_audio_codec_microphone_init();
    }
    if (!s_mic) {
        ESP_LOGE(TAG, "no microphone codec on this board");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 1,
        .sample_rate = WHISPER_RATE,
    };
    if (esp_codec_dev_open(s_mic, &fs) != 0) {
        ESP_LOGE(TAG, "could not open the microphone at %u Hz", WHISPER_RATE);
        return false;
    }
    esp_codec_dev_set_in_gain(s_mic, 30.0f);
    s_run = true;
    if (xTaskCreatePinnedToCore(whisper_task, "pharos_whis", 4096, NULL, 5,
                                &s_task, 1) != pdPASS) {
        s_run = false;
        esp_codec_dev_close(s_mic);
        return false;
    }
    ESP_LOGI(TAG, "listening at %u Hz for inaudible tones; no audio is kept",
             WHISPER_RATE);
    return true;
#else
    return false;
#endif
}

static void whisper_stop(void)
{
#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)
    s_run = false;
    for (int i = 0; i < 100 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    ESP_LOGI(TAG, "microphone released");
#endif
}

static void whisper_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pac_verdict_t v;
    pac_evaluate(&s_engine, &v);
    if (v.band != s_verdict.band) {
        ESP_LOGI(TAG, "%s %u/%u  %s  duty=%u%%  fams=0x%02x  notes=0x%02x",
                 pac_band_name(v.band), v.score, v.ceiling,
                 pac_probe_name(v.strongest), v.duty_pct, v.families, v.notes);
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

static bool k_whisper_display(struct pharos_lens_display *o)
{
    pac_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pac_band_name(v.band));
    if (v.notes & PAC_NOTE_DEAF) {
        snprintf(o->detail, sizeof(o->detail), "MIC SILENT - cannot hear");
    } else {
        snprintf(o->detail, sizeof(o->detail), "%s  %u%% of windows",
                 pac_probe_name(v.strongest), v.duty_pct);
    }
    snprintf(o->advice, sizeof(o->advice), "%s", pac_band_hint(v.band));
    if (v.notes & PAC_NOTE_EDGE_OF_HEARING) {
        snprintf(o->why, sizeof(o->why), "at the edge of what this mic hears");
    } else if (v.notes & PAC_NOTE_LOUD_ROOM) {
        snprintf(o->why, sizeof(o->why), "loud room - harmonics possible");
    }
    o->families = v.families;
    o->fam_label[0] = "LEVEL";
    o->fam_label[1] = "TONE";
    o->fam_label[2] = "AGAIN";
    o->fam_label[3] = NULL;
    o->score = v.score;
    o->raw_score = v.score;
    o->ceiling = v.ceiling;
    o->has_score = true;

    /* THERE IS ALMOST ALWAYS SOMETHING AT 19 kHz.
     *
     * Switching power supplies, monitors and rodent deterrents all live up
     * there, and this lens sat at ELEVATED in every ordinary room because of
     * it - which is also what let it monopolise the rotation before the hold
     * was capped. A tone being present is worth a dot; a tone that PERSISTS
     * with the structure of a beacon is the thing worth interrupting somebody
     * for. */
    o->has_alert = true;
    o->alert = (v.band >= PAC_BAND_BEACON)     ? 3u
             : (v.band >= PAC_BAND_PERSISTENT) ? 2u
             : (v.band >= PAC_BAND_PRESENT)    ? 1u
                                               : 0u;
    return true;
}

/* The detail page: what every probe is actually hearing. A single score for
 * "is there a beacon" is not enough to act on - the operator wants to know
 * WHICH frequency, and whether the room itself is loud enough to explain it. */
static bool k_whisper_row(unsigned index, struct pharos_lens_row *out)
{
    pac_verdict_t v = s_verdict;
    if (index < PAC_BAND_COUNT) {
        const pac_band_t b = (pac_band_t)index;
        snprintf(out->left, sizeof(out->left), "%-8s %s", pac_probe_name(b),
                 (b == PAC_BAND_AUDIBLE) ? "(room)" : "");
        /* Levels are "dB below full scale", so a SMALLER number is louder.
         * Printed negative so it reads the way a level meter does. */
        snprintf(out->right, sizeof(out->right), "%d dB", -(int)v.level[b]);
        if (b == PAC_BAND_AUDIBLE) {
            out->tone = (v.notes & PAC_NOTE_DEAF) ? PHAROS_TONE_BAD
                                                  : PHAROS_TONE_DIM;
        } else {
            out->tone = (b == v.strongest && v.score >= 40) ? PHAROS_TONE_BAD
                                                            : PHAROS_TONE_NEUTRAL;
        }
        return true;
    }
    switch (index - PAC_BAND_COUNT) {
    case 0:
        snprintf(out->left, sizeof(out->left), "loud enough");
        snprintf(out->right, sizeof(out->right), "%u/30", v.c_level);
        out->tone = (v.families & PAC_FAM_LEVEL) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "a tone, not noise");
        snprintf(out->right, sizeof(out->right), "%u/24", v.c_narrow);
        out->tone = (v.families & PAC_FAM_NARROW) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "keeps coming back");
        snprintf(out->right, sizeof(out->right), "%u/26", v.c_persist);
        out->tone = (v.families & PAC_FAM_PERSISTENT) ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "windows heard");
        snprintf(out->right, sizeof(out->right), "%u", v.windows);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "audio kept");
        snprintf(out->right, sizeof(out->right), "none");
        out->tone = PHAROS_TONE_GOOD;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_whisper = {
    .id = "mic.whisper",
    .name = "Whisper",
    .summary = "Finds inaudible tracking tones. Measures levels, keeps no audio",
    .glyph = "ear",
    .kind = PHAROS_LENS_OBSERVE,
    /* MIC only. No radio, no storage: this lens cannot write anything down,
     * which is the point. PHAROS_CAP_MIC is already inside
     * PHAROS_CAP_SENSITIVE, so the UI marks it accordingly. */
    .caps = PHAROS_CAP_MIC,
    .budget_ma = 45,
    .on_mount = whisper_mount,
    .on_start = whisper_start,
    .on_stop = whisper_stop,
    .on_tick = whisper_tick,
    .display = k_whisper_display,
    .row = k_whisper_row,
    .row_head_left = "BAND",
    .row_head_right = "LEVEL",
};

PHAROS_LENS_REGISTER(&k_whisper);
