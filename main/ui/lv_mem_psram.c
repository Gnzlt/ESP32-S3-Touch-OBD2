/*
 * LVGL's heap, in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). With plain malloc every
 * object, style and label text went to internal RAM first (the allocations are
 * all under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL): about 48 KB for the nine
 * pages, which left the SPI and BLE drivers short. Falls back to internal RAM
 * only if PSRAM is full.
 */
#include "lvgl.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "esp_heap_caps.h"

#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS_PSRAM);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, CAPS_PSRAM);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS_PSRAM);
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->free_cnt = info.free_blocks;
    mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->used_pct = mon_p->total_size ? 100 - (100 * info.total_free_bytes) / mon_p->total_size : 0;
    mon_p->frag_pct = info.total_free_bytes
                      ? 100 - (100 * info.largest_free_block) / info.total_free_bytes : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(CAPS_PSRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif
