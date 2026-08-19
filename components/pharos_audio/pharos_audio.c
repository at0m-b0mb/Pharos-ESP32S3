/* Pharos - the alarm. See pharos_audio.h for what this deliberately is not. */

#include "sdkconfig.h"

#include "pharos_audio.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

/* 16 kHz mono is plenty for notes. The buffer below is sized from it, and a
 * higher rate would only cost internal RAM the display already needs. */
#define AUD_RATE   16000
#define AUD_CHUNK  512 /* samples rendered per write */

static esp_codec_dev_handle_t s_spk;
static bool s_present;
static bool s_enabled = true;
static uint8_t s_volume = 70;
static volatile pharos_alert_t s_pending = PHAROS_ALERT_NONE;
static TaskHandle_t s_task;

/* ---- the vocabulary -------------------------------------------------
 *
 * A note is a frequency and a duration; an alert is a short list of them.
 * Kept as data rather than code so the whole sound vocabulary of the device
 * can be read in one screen and argued with.
 *
 * The shapes are chosen to be distinguishable without being looked up: notice
 * is one flat note, suspect RISES (something is developing), clear FALLS
 * (something ended), and alarm is a fast triple that repeats - the pattern
 * every smoke detector taught everyone to interpret without a manual. */
typedef struct { uint16_t hz; uint16_t ms; } note_t;

static const note_t k_notice[]  = { { 880, 90 }, { 0, 0 } };
static const note_t k_suspect[] = { { 740, 90 }, { 988, 130 }, { 0, 0 } };
static const note_t k_alarm[]   = { { 1245, 80 }, { 0, 60 }, { 1245, 80 },
                                    { 0, 60 }, { 1245, 130 }, { 0, 0 } };
static const note_t k_clear[]   = { { 988, 90 }, { 660, 130 }, { 0, 0 } };
static const note_t k_ack[]     = { { 1320, 35 }, { 0, 0 } };

static const note_t *sequence_for(pharos_alert_t a)
{
    switch (a) {
    case PHAROS_ALERT_NOTICE:  return k_notice;
    case PHAROS_ALERT_SUSPECT: return k_suspect;
    case PHAROS_ALERT_ALARM:   return k_alarm;
    case PHAROS_ALERT_CLEAR:   return k_clear;
    case PHAROS_ALERT_ACK:     return k_ack;
    case PHAROS_ALERT_NONE:
    default:                   return NULL;
    }
}

/* ---- rendering ------------------------------------------------------- */

/* One note, written in chunks so a long note does not need a long buffer.
 *
 * The envelope matters more than it looks: a sine that starts and stops at
 * full amplitude produces a click at each edge - a step change the cone has
 * to follow - and a string of clicks reads as a fault rather than as a
 * signal. A few milliseconds of ramp at each end removes it entirely. */
static void play_note(uint16_t hz, uint16_t ms)
{
    static int16_t buf[AUD_CHUNK];
    const uint32_t total = ((uint32_t)AUD_RATE * ms) / 1000u;
    const uint32_t ramp = (total > 200u) ? 100u : (total / 4u ? total / 4u : 1u);
    const float step = (hz ? (2.0f * (float)M_PI * (float)hz / (float)AUD_RATE) : 0.0f);
    float phase = 0.0f;

    for (uint32_t done = 0; done < total; ) {
        const uint32_t n = (total - done > AUD_CHUNK) ? AUD_CHUNK : (total - done);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t idx = done + i;
            int32_t amp = 9000;
            if (idx < ramp) {
                amp = (int32_t)((int64_t)amp * idx / ramp);
            } else if (idx > total - ramp) {
                amp = (int32_t)((int64_t)amp * (total - idx) / ramp);
            }
            buf[i] = hz ? (int16_t)(sinf(phase) * (float)amp) : 0;
            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        esp_codec_dev_write(s_spk, buf, (int)(n * sizeof(int16_t)));
        done += n;
    }
}

/* The codec is opened on demand and closed after a few seconds of quiet.
 *
 * Opening per ALERT was the obvious first shape and it was wrong twice. It
 * powered the class-D amplifier up and down for every single note sequence,
 * which is a relay-click of a transient into the cone on each one, and it
 * asked the vendor's I2S wrapper to tear the channel down so often that it
 * logged an ERROR every time:
 *
 *   i2s_common: i2s_channel_disable(1218): the channel has not been enabled yet
 *
 * - the wrapper had already disabled it. Nothing leaked and the sound was
 * correct, but an ERROR line per beep trains an operator to scroll past error
 * lines, which is the one habit a security tool must never teach.
 *
 * Holding it open forever is the other wrong answer: this device is meant to
 * sit on a shelf for hours, and an idling amplifier is current draw and a
 * faint hiss somebody will eventually go hunting for.
 *
 * So: open on the first alert, stay open through a burst, and power down after
 * AUD_IDLE_MS of silence. An alarm that repeats every few seconds now opens
 * once instead of once per repetition. */
#define AUD_IDLE_MS 4000

static void audio_task(void *arg)
{
    (void)arg;
    bool open = false;
    uint32_t idle_ms = 0;

    for (;;) {
        const pharos_alert_t want = s_pending;
        if (want == PHAROS_ALERT_NONE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (open) {
                idle_ms += 50;
                if (idle_ms >= AUD_IDLE_MS) {
                    esp_codec_dev_close(s_spk);
                    open = false;
                    idle_ms = 0;
                }
            }
            continue;
        }

        const note_t *seq = sequence_for(want);
        if (seq && s_enabled && s_spk) {
            if (!open) {
                esp_codec_dev_sample_info_t fs = {
                    .bits_per_sample = 16,
                    .channel = 1,
                    .channel_mask = 1,
                    .sample_rate = AUD_RATE,
                };
                if (esp_codec_dev_open(s_spk, &fs) == 0) {
                    open = true;
                    esp_codec_dev_set_out_vol(s_spk, (int)s_volume);
                }
            }
            if (open) {
                for (const note_t *n = seq; n->hz || n->ms; n++) {
                    play_note(n->hz, n->ms);
                }
            }
        }
        idle_ms = 0;
        s_pending = PHAROS_ALERT_NONE;
    }
}

/* ---- lifecycle -------------------------------------------------------- */

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, "aud_on", &v) == ESP_OK) {
        s_enabled = (v != 0);
    }
    if (nvs_get_u8(h, "aud_vol", &v) == ESP_OK && v <= 100) {
        s_volume = v;
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "aud_on", s_enabled ? 1 : 0);
    nvs_set_u8(h, "aud_vol", s_volume);
    nvs_commit(h);
    nvs_close(h);
}

bool pharos_audio_init(void)
{
    if (s_present) {
        return true;
    }
    settings_load();

    if (bsp_audio_init(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "no I2S; the device will run silent");
        return false;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGW(TAG, "no speaker codec; the device will run silent");
        return false;
    }
    /* Small stack: this task renders sine into a static buffer and blocks in
     * the codec write. It does no allocation and touches no LVGL. */
    if (xTaskCreatePinnedToCore(audio_task, "pharos_aud", 3072, NULL, 4,
                                &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "could not create the audio task");
        return false;
    }
    s_present = true;
    ESP_LOGI(TAG, "alarm ready: ES8311, %u Hz mono, %s at %u%%", AUD_RATE,
             s_enabled ? "enabled" : "MUTED", s_volume);
    return true;
}

bool pharos_audio_present(void) { return s_present; }

void pharos_audio_alert(pharos_alert_t which)
{
    if (!s_present || !s_enabled || which == PHAROS_ALERT_NONE) {
        return;
    }
    /* Dropped, not queued: an alarm backlog is only a longer alarm, and the
     * caller is often the UI task, which must never wait on a sound. */
    if (s_pending == PHAROS_ALERT_NONE) {
        s_pending = which;
    }
}

void pharos_audio_set_enabled(bool on)
{
    s_enabled = on;
    settings_save();
    ESP_LOGI(TAG, "alarm %s", on ? "enabled" : "muted");
}

bool pharos_audio_enabled(void) { return s_enabled; }

void pharos_audio_set_volume(uint8_t pct)
{
    s_volume = (pct > 100) ? 100 : pct;
    settings_save();
}

uint8_t pharos_audio_volume(void) { return s_volume; }

#else /* headless or no vendor BSP: the firmware still builds and runs mute */

bool pharos_audio_init(void) { return false; }
bool pharos_audio_present(void) { return false; }
void pharos_audio_alert(pharos_alert_t which) { (void)which; }
void pharos_audio_set_enabled(bool on) { (void)on; }
bool pharos_audio_enabled(void) { return false; }
void pharos_audio_set_volume(uint8_t pct) { (void)pct; }
uint8_t pharos_audio_volume(void) { return 0; }

#endif

/* Band -> alert, in one place so that every lens sounds the same for the same
 * severity. The bands are the five every engine grades in; taking the number
 * rather than a per-engine enum keeps this component free of any dependency
 * on the engines. */
pharos_alert_t pharos_audio_alert_for_band(uint8_t band)
{
    switch (band) {
    case 2:  return PHAROS_ALERT_NOTICE;  /* ELEVATED   */
    case 3:  return PHAROS_ALERT_SUSPECT; /* SUSPICIOUS */
    case 4:  return PHAROS_ALERT_ALARM;   /* LIKELY     */
    default: return PHAROS_ALERT_NONE;    /* quiet, background: say nothing */
    }
}
