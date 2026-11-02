#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mem_report.h"

static const char *TAG = "mem";

void mem_checkpoint(const char *where)
{
    ESP_LOGI(TAG, "ram %s: %u KB free, %u KB lowest, %u KB largest block (PSRAM %u KB free)", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

void mem_report_tasks(void)
{
    mem_checkpoint("now");
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = heap_caps_malloc(n * sizeof(*t), MALLOC_CAP_SPIRAM);
    if (!t) return;
    n = uxTaskGetSystemState(t, n, NULL);
    // One line: "name:unused" in bytes, the smallest headroom first is what matters.
    char line[512];
    int len = snprintf(line, sizeof(line), "stack unused:");
    for (UBaseType_t i = 0; i < n && len < (int)sizeof(line) - 24; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %s=%u", t[i].pcTaskName,
                        (unsigned)t[i].usStackHighWaterMark);
    }
    ESP_LOGI(TAG, "%s", line);
    free(t);
}
