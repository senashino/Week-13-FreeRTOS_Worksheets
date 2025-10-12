#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"     // สำคัญมากสำหรับ ESP-IDF v5.5+
#include "esp_heap_caps.h"
#include "driver/gpio.h"

static const char *TAG = "MEM_POOLS_EXP4";

// ===== GPIO (ปรับถ้าบอร์ดต่างจากนี้) =====
#define LED_SMALL_POOL  GPIO_NUM_2
#define LED_MEDIUM_POOL GPIO_NUM_4
#define LED_LARGE_POOL  GPIO_NUM_5
#define LED_POOL_FULL   GPIO_NUM_18
#define LED_POOL_ERROR  GPIO_NUM_19

// ===== Config พูล =====
#define SMALL_POOL_BLOCK_SIZE   64
#define SMALL_POOL_BLOCK_COUNT  32

#define MEDIUM_POOL_BLOCK_SIZE  256
#define MEDIUM_POOL_BLOCK_COUNT 16

#define LARGE_POOL_BLOCK_SIZE   1024
#define LARGE_POOL_BLOCK_COUNT  8

#define HUGE_POOL_BLOCK_SIZE    4096
#define HUGE_POOL_BLOCK_COUNT   4

typedef struct memory_block {
    struct memory_block* next;
    uint32_t magic;
    uint32_t pool_id;
    uint64_t alloc_time;
} memory_block_t;

typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    size_t alignment;
    uint32_t caps;
    void* pool_memory;
    memory_block_t* free_list;
    uint8_t* usage_bitmap; // 1bit/blk
    // stats
    size_t allocated_blocks;
    size_t peak_usage;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint32_t allocation_failures;
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
    // sync
    SemaphoreHandle_t mutex;
    // id
    uint32_t pool_id;
} memory_pool_t;

typedef enum {
    POOL_SMALL = 0,
    POOL_MEDIUM,
    POOL_LARGE,
    POOL_HUGE,
    POOL_COUNT
} pool_type_t;

typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    uint32_t caps;
    gpio_num_t led_pin;
} pool_config_t;

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM MALLOC_CAP_DEFAULT
#endif

static const pool_config_t pool_configs[POOL_COUNT] = {
    {"Small",  SMALL_POOL_BLOCK_SIZE,  SMALL_POOL_BLOCK_COUNT,  MALLOC_CAP_INTERNAL, LED_SMALL_POOL},
    {"Medium", MEDIUM_POOL_BLOCK_SIZE, MEDIUM_POOL_BLOCK_COUNT, MALLOC_CAP_INTERNAL, LED_MEDIUM_POOL},
    {"Large",  LARGE_POOL_BLOCK_SIZE,  LARGE_POOL_BLOCK_COUNT,  MALLOC_CAP_DEFAULT,  LED_LARGE_POOL},
    {"Huge",   HUGE_POOL_BLOCK_SIZE,   HUGE_POOL_BLOCK_COUNT,   MALLOC_CAP_SPIRAM,   LED_POOL_FULL}, // ใช้ LED18 แสดงกิจกรรม Huge ด้วย
};

static memory_pool_t pools[POOL_COUNT];

// ===== Magic =====
#define POOL_MAGIC_FREE  0xDEADBEEF
#define POOL_MAGIC_ALLOC 0xCAFEBABE

// ===== Utils =====
static inline size_t pool_total_block_size(const memory_pool_t* p) {
    size_t header = sizeof(memory_block_t);
    size_t aligned = (p->block_size + p->alignment - 1) & ~(p->alignment - 1);
    return header + aligned;
}

static void led_pulse(gpio_num_t pin, int ms) {
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, 0);
}

// ===== พูลพื้นฐาน =====
static bool init_memory_pool(memory_pool_t* pool, const pool_config_t* cfg, uint32_t pool_id) {
    memset(pool, 0, sizeof(*pool));
    pool->name = cfg->name;
    pool->block_size = cfg->block_size;
    pool->block_count = cfg->block_count;
    pool->alignment = 4;
    pool->caps = cfg->caps;
    pool->pool_id = pool_id;

    size_t total_block = pool_total_block_size(pool);
    size_t total_mem   = total_block * pool->block_count;

    pool->pool_memory = heap_caps_malloc(total_mem, pool->caps);
    if (!pool->pool_memory) {
        if (pool->caps == MALLOC_CAP_SPIRAM) {
            ESP_LOGW(TAG, "%s: SPIRAM alloc failed, fallback DEFAULT", pool->name);
            pool->caps = MALLOC_CAP_DEFAULT;
            pool->pool_memory = heap_caps_malloc(total_mem, pool->caps);
        }
    }
    if (!pool->pool_memory) {
        ESP_LOGE(TAG, "Failed to alloc %s pool memory", pool->name);
        return false;
    }

    size_t bitmap_bytes = (pool->block_count + 7) / 8;
    pool->usage_bitmap = (uint8_t*)heap_caps_calloc(bitmap_bytes, 1, MALLOC_CAP_INTERNAL);
    if (!pool->usage_bitmap) {
        heap_caps_free(pool->pool_memory);
        ESP_LOGE(TAG, "Failed to alloc %s bitmap", pool->name);
        return false;
    }

    pool->free_list = NULL;
    uint8_t* base = (uint8_t*)pool->pool_memory;
    for (size_t i = 0; i < pool->block_count; i++) {
        memory_block_t* blk = (memory_block_t*)(base + i * total_block);
        blk->magic = POOL_MAGIC_FREE;
        blk->pool_id = pool->pool_id;
        blk->alloc_time = 0;
        blk->next = pool->free_list;
        pool->free_list = blk;
    }

    pool->mutex = xSemaphoreCreateMutex();
    if (!pool->mutex) {
        heap_caps_free(pool->usage_bitmap);
        heap_caps_free(pool->pool_memory);
        ESP_LOGE(TAG, "Failed to create %s mutex", pool->name);
        return false;
    }

    ESP_LOGI(TAG, "Init %s: %d blocks x %d bytes (total %u bytes)",
             pool->name, (int)pool->block_count, (int)pool->block_size, (unsigned)total_mem);
    return true;
}

static void* pool_malloc(memory_pool_t* pool) {
    uint64_t t0 = esp_timer_get_time();
    void* out = NULL;

    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (pool->free_list) {
            memory_block_t* blk = pool->free_list;
            pool->free_list = blk->next;

            if (blk->magic != POOL_MAGIC_FREE || blk->pool_id != pool->pool_id) {
                ESP_LOGE(TAG, "%s: corruption on allocate", pool->name);
                gpio_set_level(LED_POOL_ERROR, 1);
            } else {
                size_t idx = ((uint8_t*)blk - (uint8_t*)pool->pool_memory) / pool_total_block_size(pool);
                if (idx < pool->block_count) pool->usage_bitmap[idx >> 3] |= (1u << (idx & 7));
                blk->magic = POOL_MAGIC_ALLOC;
                blk->alloc_time = esp_timer_get_time();
                pool->allocated_blocks++;
                if (pool->allocated_blocks > pool->peak_usage) pool->peak_usage = pool->allocated_blocks;
                pool->total_allocations++;

                out = (uint8_t*)blk + sizeof(memory_block_t);
            }
        } else {
            pool->allocation_failures++;
            gpio_set_level(LED_POOL_FULL, 1);
        }
        xSemaphoreGive(pool->mutex);
    }

    pool->allocation_time_total += (esp_timer_get_time() - t0);
    return out;
}

static bool pool_free(memory_pool_t* pool, void* ptr) {
    if (!ptr) return false;
    uint64_t t0 = esp_timer_get_time();
    bool ok = false;

    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memory_block_t* blk = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
        if (blk->magic == POOL_MAGIC_ALLOC && blk->pool_id == pool->pool_id) {
            size_t idx = ((uint8_t*)blk - (uint8_t*)pool->pool_memory) / pool_total_block_size(pool);
            if (idx < pool->block_count) pool->usage_bitmap[idx >> 3] &= ~(1u << (idx & 7));
            blk->magic = POOL_MAGIC_FREE;
            blk->next = pool->free_list;
            pool->free_list = blk;
            pool->allocated_blocks--;
            pool->total_deallocations++;
            ok = true;
        } else {
            // double free หรือ free ผิดพูล
            ESP_LOGE(TAG, "%s: invalid free! magic=0x%08x pool_id=%lu", pool->name,
                     blk->magic, blk->pool_id);
            gpio_set_level(LED_POOL_ERROR, 1);
        }
        xSemaphoreGive(pool->mutex);
    }

    pool->deallocation_time_total += (esp_timer_get_time() - t0);
    return ok;
}

// ===== Smart allocator =====
static void* smart_pool_malloc(size_t size, int* chosen_pool_index) {
    size_t req = size + 16; // margin
    for (int i = 0; i < POOL_COUNT; i++) {
        if (req <= pools[i].block_size) {
            void* p = pool_malloc(&pools[i]);
            if (p) {
                if (chosen_pool_index) *chosen_pool_index = i;
                led_pulse(pool_configs[i].led_pin, 20);
                return p;
            }
        }
    }
    void* hp = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    if (hp) {
        if (chosen_pool_index) *chosen_pool_index = -1;
        ESP_LOGW(TAG, "Fallback to HEAP for %u bytes", (unsigned)size);
    }
    return hp;
}

static bool smart_pool_free(void* ptr) {
    if (!ptr) return false;
    for (int i = 0; i < POOL_COUNT; i++) {
        if (pool_free(&pools[i], ptr)) return true;
    }
    // ไม่ใช่ของพูล → ปล่อยไป heap
    heap_caps_free(ptr);
    return true;
}

// ===== Integrity Check =====
static bool check_pool_integrity_one(memory_pool_t* pool) {
    bool ok = true;
    if (!pool->mutex) return true;

    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // ตรวจ free list
        memory_block_t* cur = pool->free_list;
        int free_seen = 0;
        while (cur && free_seen <= (int)pool->block_count) {
            if (cur->magic != POOL_MAGIC_FREE || cur->pool_id != pool->pool_id) {
                ESP_LOGE(TAG, "❌ %s: corrupted free block %p (magic=0x%08x id=%lu)",
                         pool->name, cur, cur->magic, cur->pool_id);
                ok = false; break;
            }
            cur = cur->next;
            free_seen++;
        }

        // ตรวจ bitmap vs allocated_blocks (คร่าว ๆ)
        int setbits = 0;
        for (size_t i = 0; i < pool->block_count; i++) {
            int b = (pool->usage_bitmap[i >> 3] >> (i & 7)) & 1;
            setbits += b;
        }
        if (setbits != (int)pool->allocated_blocks) {
            ESP_LOGE(TAG, "❌ %s: bitmap mismatch set=%d allocated=%d",
                     pool->name, setbits, (int)pool->allocated_blocks);
            ok = false;
        }

        xSemaphoreGive(pool->mutex);
    }
    return ok;
}

static bool check_all_pools_integrity(void) {
    bool all_ok = true;
    for (int i = 0; i < POOL_COUNT; i++) {
        if (!check_pool_integrity_one(&pools[i])) all_ok = false;
    }
    if (!all_ok) gpio_set_level(LED_POOL_ERROR, 1);
    return all_ok;
}

// ===== Corruption Scenarios =====
typedef struct {
    void* ptr;
    size_t size;
    uint32_t pattern;
    int pool_idx; // -1 = heap
} tracked_t;

#define MAX_TRACKED 64
static tracked_t tracked[MAX_TRACKED];
static int tracked_n = 0;

static void clear_tracked(void) {
    for (int i = 0; i < tracked_n; i++) {
        if (tracked[i].ptr) smart_pool_free(tracked[i].ptr);
    }
    tracked_n = 0;
}

static void allocate_with_pattern(int count, size_t min_sz, size_t max_sz) {
    for (int i = 0; i < count && tracked_n < MAX_TRACKED; i++) {
        size_t sz = min_sz + (esp_random() % (max_sz - min_sz + 1));
        int chosen = -2;
        void* p = smart_pool_malloc(sz, &chosen);
        if (!p) continue;

        tracked[tracked_n].ptr = p;
        tracked[tracked_n].size = sz;
        tracked[tracked_n].pattern = esp_random();
        tracked[tracked_n].pool_idx = chosen;

        // fill pattern
        uint32_t* w = (uint32_t*)p;
        size_t wc = sz / sizeof(uint32_t);
        for (size_t k = 0; k < wc; k++) w[k] = tracked[tracked_n].pattern;

        tracked_n++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int verify_patterns(void) {
    int corrupt = 0;
    for (int i = 0; i < tracked_n; i++) {
        if (!tracked[i].ptr) continue;
        uint32_t* w = (uint32_t*)tracked[i].ptr;
        size_t wc = tracked[i].size / sizeof(uint32_t);
        for (size_t k = 0; k < wc; k++) {
            if (w[k] != tracked[i].pattern) {
                corrupt++;
                ESP_LOGE(TAG, "🚨 Pattern corruption at alloc #%d (pool=%d size=%u)",
                         i, tracked[i].pool_idx, (unsigned)tracked[i].size);
                gpio_set_level(LED_POOL_ERROR, 1);
                break;
            }
        }
    }
    return corrupt;
}

static void scenario_buffer_overrun(void) {
    if (tracked_n == 0) return;
    int idx = esp_random() % tracked_n;
    if (!tracked[idx].ptr) return;

    ESP_LOGW(TAG, "⚠️ Simulate buffer overrun at #%d: write +8 bytes out of bounds", idx);
    uint8_t* p = (uint8_t*)tracked[idx].ptr;
    // เขียนเกินปลาย buffer 8 ไบต์ (จำลองบั๊ก)
    for (int i = 0; i < 8; i++) {
        p[tracked[idx].size + i] = 0x5A; // อาจชน header ของบล็อกถัดไป
    }
}

static void scenario_double_free(void) {
    if (tracked_n == 0) return;
    int idx = esp_random() % tracked_n;
    if (!tracked[idx].ptr) return;

    ESP_LOGW(TAG, "⚠️ Simulate double free at #%d", idx);
    // free ครั้งที่ 1 (ถูก)
    smart_pool_free(tracked[idx].ptr);
    // free ครั้งที่ 2 (ซ้ำ) → pool_free จะขึ้น error
    bool ok = smart_pool_free(tracked[idx].ptr);
    (void)ok;
    tracked[idx].ptr = NULL;
}

static void scenario_wrong_pool_free(void) {
    // จองบล็อกเล็กจาก Small แล้วแกล้ง free กับ Large โดยตรง
    int chosen = -2;
    void* p = smart_pool_malloc(32, &chosen);
    if (!p || chosen < 0) return;
    ESP_LOGW(TAG, "⚠️ Simulate wrong-pool free (allocated from %s, free as Large)",
             pools[chosen].name);
    // เรียกปล่อยผ่านฟังก์ชันภายในพูล Large (ผิดเจตนา) เพื่อให้ขึ้น error
    (void)pool_free(&pools[POOL_LARGE], p);
    // ปล่อยจริงให้ถูกต้อง เพื่อไม่ให้รั่ว
    smart_pool_free(p);
}

// ===== TASK: Corruption Demo =====
static void corruption_demo_task(void* arg) {
    while (1) {
        gpio_set_level(LED_POOL_ERROR, 0);
        ESP_LOGI(TAG, "\n=== Round: Pattern Fill & Verify ===");
        clear_tracked();

        // 1) จองและเติม pattern
        allocate_with_pattern(24, 24, 1200); // ผสม Small/Medium/Large
        int c0 = verify_patterns();
        ESP_LOGI(TAG, "Initial verify: corrupt=%d", c0);

        // 2) จำลองความเสียหายรูปแบบต่าง ๆ
        scenario_buffer_overrun();
        vTaskDelay(pdMS_TO_TICKS(100));
        int c1 = verify_patterns();
        ESP_LOGI(TAG, "After overrun verify: corrupt=%d", c1);

        scenario_double_free();
        vTaskDelay(pdMS_TO_TICKS(100));
        (void)verify_patterns();

        scenario_wrong_pool_free();
        vTaskDelay(pdMS_TO_TICKS(100));
        (void)verify_patterns();

        // 3) Integrity check เชิงโครงสร้าง
        ESP_LOGI(TAG, "🔍 Running integrity checks...");
        bool ok = check_all_pools_integrity();
        ESP_LOGI(TAG, "Integrity: %s", ok ? "OK" : "BROKEN");
        if (!ok) gpio_set_level(LED_POOL_ERROR, 1);

        // 4) เคลียร์ทั้งหมด แล้วตรวจว่ากลับสู่สภาพปกติ
        ESP_LOGI(TAG, "Cleanup: free all tracked allocations");
        clear_tracked();
        vTaskDelay(pdMS_TO_TICKS(50));
        bool ok2 = check_all_pools_integrity();
        ESP_LOGI(TAG, "Post-clean Integrity: %s", ok2 ? "OK" : "BROKEN");
        if (ok2) gpio_set_level(LED_POOL_ERROR, 0);

        // เตือนพูลเต็มถ้าเกิดขึ้นในรอบนี้
        bool any_full = false;
        for (int i = 0; i < POOL_COUNT; i++) {
            if (pools[i].allocated_blocks >= pools[i].block_count) { any_full = true; break; }
        }
        gpio_set_level(LED_POOL_FULL, any_full ? 1 : 0);

        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "=== End Round. Next in 8s ===\n");
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 Experiment 4: Corruption Detection & Integrity Check");

    // GPIO init
    gpio_set_direction(LED_SMALL_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEDIUM_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LARGE_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_FULL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SMALL_POOL, 0);
    gpio_set_level(LED_MEDIUM_POOL, 0);
    gpio_set_level(LED_LARGE_POOL, 0);
    gpio_set_level(LED_POOL_FULL, 0);
    gpio_set_level(LED_POOL_ERROR, 0);

    // Init pools
    for (int i = 0; i < POOL_COUNT; i++) {
        if (!init_memory_pool(&pools[i], &pool_configs[i], i+1)) {
            ESP_LOGE(TAG, "Pool init failed: %s", pool_configs[i].name);
            return;
        }
    }

    // สร้าง task เดโม corruption
    xTaskCreate(corruption_demo_task, "CorruptDemo", 4096, NULL, 5, NULL);
}
