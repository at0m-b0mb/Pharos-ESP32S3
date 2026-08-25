/* Pharos - the motion service. See pharos_sense.h. */
#include "pharos_sense.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pharos_bsp.h"

static const char *TAG = "sense";

/* 50 Hz. Fast enough to resolve a footfall - gait tops out near 2.6 steps a
 * second and a step needs several samples to have a shape - and slow enough
 * that the I2C traffic is negligible beside the touch controller's. */
#define SENSE_HZ 50
#define SENSE_PERIOD_MS (1000 / SENSE_HZ)

static pm_engine_t s_engine;
static pm_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;
static bool s_started;

static void sense_task(void *arg)
{
    (void)arg;
    /* Let the board finish coming up before asking. The probe configures the
     * chip and waits for it to produce a real sample, and a task that asks
     * mid-probe used to be told there was no sensor and delete itself. The
     * ordering bug is fixed in the driver; this is the belt to its braces,
     * and it costs one second at boot. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    const bool have = pharos_bsp_imu_present();
    pm_set_present(&s_engine, have);
    if (!have) {
        /* Publish the honest verdict once and stop. A task spinning on a
         * sensor that is not there would burn a core to produce UNKNOWN. */
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            pm_evaluate(&s_engine, (uint64_t)esp_timer_get_time(), &s_verdict);
            xSemaphoreGive(s_lock);
        }
        ESP_LOGW(TAG, "no IMU; motion will read UNKNOWN, never STILL");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "motion sampler running at %d Hz", SENSE_HZ);
    TickType_t last = xTaskGetTickCount();
    unsigned since_eval = 0;
    for (;;) {
        int32_t x = 0, y = 0, z = 0;
        if (pharos_bsp_imu_read(&x, &y, &z)) {
            const uint64_t now = (uint64_t)esp_timer_get_time();
            pm_observe(&s_engine, x, y, z, now);
            /* Re-evaluate a few times a second rather than every sample: the
             * verdict is a window statistic and nothing reads it faster. */
            if (++since_eval >= SENSE_HZ / 5u) {
                since_eval = 0;
                if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
                    pm_evaluate(&s_engine, now, &s_verdict);
                    xSemaphoreGive(s_lock);
                }
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

void pharos_sense_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    pm_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    /* Small stack and low priority: it is a 50 Hz I2C read, and it must never
     * compete with the analytics core or the display flush. */
    xTaskCreatePinnedToCore(sense_task, "pharos_sense", 3072, NULL, 2, NULL, 0);
}

void pharos_sense_motion(pm_verdict_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        memset(out, 0, sizeof(*out));
        out->state = PM_UNKNOWN;
        out->headline = "motion unavailable";
        return;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
}

uint32_t pharos_sense_steps(void)
{
    pm_verdict_t v;
    pharos_sense_motion(&v);
    return v.steps;
}

bool pharos_sense_travelled(uint32_t mark)
{
    return pm_has_travelled(&s_engine, mark);
}
