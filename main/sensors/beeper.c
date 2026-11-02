/*
 * Alert tones on the board's speaker (ES8311 codec + amplifier, through the
 * BSP). Tones are generated, not stored. A small task plays them so the
 * caller never waits on I2S.
 */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "beeper.h"

static const char *TAG = "beeper";

#define RATE   22050
#define VOLUME 75

static QueueHandle_t s_queue;
static esp_codec_dev_handle_t s_spk;

typedef struct {
    uint16_t hz;      // 0 = silence
    uint16_t ms;
} note_t;

static const note_t P_URGENT[] = {{1760, 110}, {0, 70}, {1760, 110}, {0, 70}, {1760, 160}, {0, 0}};
static const note_t P_WARN[] = {{1175, 180}, {0, 90}, {1175, 180}, {0, 0}};
static const note_t P_INFO[] = {{880, 120}, {1320, 180}, {0, 0}};
static const note_t P_TICK[] = {{2400, 12}, {0, 0}};

static void play(const note_t *notes)
{
    static int16_t buf[RATE / 50];   // 20 ms
    for (const note_t *n = notes; n->ms; n++) {
        int total = RATE * n->ms / 1000;
        float phase = 0, step = 2.0f * (float)M_PI * n->hz / RATE;
        for (int done = 0; done < total;) {
            int len = total - done < (int)(sizeof(buf) / 2) ? total - done : (int)(sizeof(buf) / 2);
            for (int i = 0; i < len; i++) {
                // A 4 ms fade at both ends keeps the speaker from clicking.
                int at = done + i;
                float env = fminf(1.0f, fminf(at, total - at) / (RATE * 0.004f));
                buf[i] = n->hz ? (int16_t)(sinf(phase) * 9000 * env) : 0;
                phase += step;
            }
            esp_codec_dev_write(s_spk, buf, len * 2);
            done += len;
        }
    }
}

static void beeper_task(void *arg)
{
    beep_t b;
    for (;;) {
        if (xQueueReceive(s_queue, &b, portMAX_DELAY) == pdTRUE) {
            play(b == BEEP_URGENT ? P_URGENT : b == BEEP_WARN ? P_WARN : b == BEEP_INFO ? P_INFO : P_TICK);
        }
    }
}

esp_err_t beeper_init(void)
{
    if (bsp_audio_init(NULL) != ESP_OK) {
        return ESP_FAIL;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGW(TAG, "no speaker codec");
        return ESP_FAIL;
    }
    // Opened once and left open: closing disables the shared I2S channel, and
    // the next open then logs "i2s_channel_disable: not enabled yet".
    esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = RATE};
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, VOLUME);
    s_queue = xQueueCreate(4, sizeof(beep_t));
    xTaskCreatePinnedToCore(beeper_task, "beeper", 3072, NULL, 3, NULL, 0);
    return ESP_OK;
}

void beeper_play(beep_t b)
{
    if (s_queue) {
        xQueueSend(s_queue, &b, 0);
    }
}
