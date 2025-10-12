#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"   // ต้องมีสำหรับ esp_random()

static const char *TAG = "LAB1_LEAK_DET";

// ===== LED pins (ตามแลบก่อนหน้า) =====
#define LED_MEMORY_OK       GPIO_NUM_2
#define LED_LOW_MEMORY      GPIO_NUM_4
#define LED_MEMORY_ERROR    GPIO_NUM_5
#define LED_FRAGMENTATION   GPIO_NUM_18
#define LED_SPIRAM_ACTIVE   GPIO_NUM_19

// ===== Thresholds / Params =====
#define LOW_MEMORY_THRESHOLD        50000   // 50KB
#define CRITICAL_MEMORY_THRESHOLD   20000   // 20KB
#define MAX_TRACKED_ALLOC           160
#define LEAK_AGE_MS                 30000   // ถือเกิน 30s ถือว่า "น่าสงสัย"
#define DETECT_INTERVAL_MS          5000    // ตรวจทุก 5s
#define REPORT_INTERVAL_MS          7000    // รายงานสรุปทุก 7s

// ===== Tracking structs =====
typedef struct {
    void*     ptr;
    size_t    size;
    uint32_t  caps;
    const char* desc;
    uint64_t  ts_us;     // เวลาจอง (μs)
    bool      active;
} alloc_rec_t;

typedef struct {
    uint32_t total_allocs;
    uint32_t total_frees;
    uint32_t failures;
    uint32_t leaks_found;        // จำนวนที่ตรวจพบน่าสงสัย
    size_t   suspected_leaked;   // ไบต์รวมของที่น่าสงสัย
    uint64_t bytes_in_use_peak;  // peak in-use
    uint64_t bytes_allocd;       // สะสม
    uint64_t bytes_freed;        // สะสม
} mem_stats_t;

// ===== Globals =====
static alloc_rec_t  g_rec[MAX_TRACKED_ALLOC];
static mem_stats_t  g_stats = {0};
static SemaphoreHandle_t g_mutex;

// ===== LED utils =====
static void leds_init(void) {
    gpio_set_direction(LED_MEMORY_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LOW_MEMORY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_FRAGMENTATION, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SPIRAM_ACTIVE, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_MEMORY_OK, 0);
    gpio_set_level(LED_LOW_MEMORY, 0);
    gpio_set_level(LED_MEMORY_ERROR, 0);
    gpio_set_level(LED_FRAGMENTATION, 0);
    gpio_set_level(LED_SPIRAM_ACTIVE, 0);
}

static void update_leds_by_heap(void) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < CRITICAL_MEMORY_THRESHOLD) {
        gpio_set_level(LED_MEMORY_OK, 0);
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_ERROR, 1);
    } else if (free_int < LOW_MEMORY_THRESHOLD) {
        gpio_set_level(LED_MEMORY_OK, 0);
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_ERROR, 0);
    } else {
        gpio_set_level(LED_MEMORY_OK, 1);
        gpio_set_level(LED_LOW_MEMORY, 0);
        gpio_set_level(LED_MEMORY_ERROR, 0);
    }
    gpio_set_level(LED_SPIRAM_ACTIVE, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0);
}

// ===== Tracking helpers =====
static int find_free_slot(void) {
    for (int i = 0; i < MAX_TRACKED_ALLOC; i++) if (!g_rec[i].active) return i;
    return -1;
}
static int find_slot_by_ptr(void* p) {
    for (int i = 0; i < MAX_TRACKED_ALLOC; i++) if (g_rec[i].active && g_rec[i].ptr == p) return i;
    return -1;
}

static void* tracked_malloc(size_t sz, uint32_t caps, const char* desc) {
    void* p = heap_caps_malloc(sz, caps);
    if (!g_mutex) return p;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (p) {
            int idx = find_free_slot();
            if (idx >= 0) {
                g_rec[idx].ptr  = p;
                g_rec[idx].size = sz;
                g_rec[idx].caps = caps;
                g_rec[idx].desc = desc;
                g_rec[idx].ts_us= esp_timer_get_time();
                g_rec[idx].active = true;

                g_stats.total_allocs++;
                g_stats.bytes_allocd += sz;
                uint64_t in_use = g_stats.bytes_allocd - g_stats.bytes_freed;
                if (in_use > g_stats.bytes_in_use_peak) g_stats.bytes_in_use_peak = in_use;

                ESP_LOGI(TAG, "alloc %uB @%p (%s)", (unsigned)sz, p, desc);
            } else {
                ESP_LOGW(TAG, "tracking full; possible leak risk for %p (%s)", p, desc);
            }
        } else {
            g_stats.failures++;
            ESP_LOGW(TAG, "alloc FAIL (%uB caps=0x%lx) %s",
                     (unsigned)sz, (unsigned long)caps, desc ? desc : "");
        }
        xSemaphoreGive(g_mutex);
    }
    return p;
}

static void tracked_free(void* p, const char* desc) {
    if (!p || !g_mutex) return;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int idx = find_slot_by_ptr(p);
        if (idx >= 0) {
            g_rec[idx].active = false;
            g_stats.total_frees++;
            g_stats.bytes_freed += g_rec[idx].size;
            ESP_LOGI(TAG, "free  %uB @%p (%s)", (unsigned)g_rec[idx].size, p, desc ? desc : "");
        } else {
            ESP_LOGW(TAG, "free untracked %p (%s)", p, desc ? desc : "");
        }
        xSemaphoreGive(g_mutex);
    }
    heap_caps_free(p);
}

// ===== Leak detection =====
static void detect_leaks_and_report(void) {
    if (!g_mutex) return;

    uint64_t now = esp_timer_get_time();
    uint32_t leak_cnt = 0;
    size_t   leak_bytes = 0;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        ESP_LOGI(TAG, "🔍 Leak scan start (age > %ums)", (unsigned)LEAK_AGE_MS);
        for (int i = 0; i < MAX_TRACKED_ALLOC; i++) {
            if (g_rec[i].active) {
                uint64_t age_ms = (now - g_rec[i].ts_us) / 1000;
                if (age_ms > LEAK_AGE_MS) {
                    leak_cnt++;
                    leak_bytes += g_rec[i].size;
                    ESP_LOGW(TAG, "POTENTIAL LEAK: %uB @%p (%s) age=%llu ms caps=0x%lx",
                             (unsigned)g_rec[i].size, g_rec[i].ptr,
                             g_rec[i].desc ? g_rec[i].desc : "-",
                             (unsigned long long)age_ms,
                             (unsigned long)g_rec[i].caps);
                }
            }
        }
        g_stats.leaks_found      = leak_cnt;
        g_stats.suspected_leaked = leak_bytes;
        xSemaphoreGive(g_mutex);
    }

    if (leak_cnt > 0) {
        gpio_set_level(LED_MEMORY_ERROR, 1);
        ESP_LOGW(TAG, "SUMMARY: potential leaks=%u, total suspected=%u bytes",
                 (unsigned)leak_cnt, (unsigned)leak_bytes);
    } else {
        // ไม่มีรั่วสงสัย ดับไฟแดง
        gpio_set_level(LED_MEMORY_ERROR, 0);
        ESP_LOGI(TAG, "No potential leaks detected");
    }
}

// ===== Monitoring =====
static void log_heap_brief(const char* tag) {
    size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t fs = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "[%s] Int free=%u | SPIRAM free=%u | total=%u | minEver=%u",
             tag,
             (unsigned)fi, (unsigned)fs,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void log_stats_summary(void) {
    if (!g_mutex) return;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        ESP_LOGI(TAG, "STATS: allocs=%u frees=%u in-use=%lluB peak=%lluB fails=%u leaks=%u(%uB)",
                 g_stats.total_allocs, g_stats.total_frees,
                 (unsigned long long)(g_stats.bytes_allocd - g_stats.bytes_freed),
                 (unsigned long long)g_stats.bytes_in_use_peak,
                 g_stats.failures,
                 g_stats.leaks_found, (unsigned)g_stats.suspected_leaked);
        xSemaphoreGive(g_mutex);
    }
}

// ===== Workloads =====
// 1) ปกติ: จอง/ใช้งาน/คืน (ไม่รั่ว)
static void normal_workload_task(void *pv) {
    ESP_LOGI(TAG, "normal workload start");
    const size_t sizes[] = { 256, 512, 1024, 2048, 4096 };
    const int N = sizeof(sizes)/sizeof(sizes[0]);
    while (1) {
        // เลือก internal / spiram (ถ้ามี)
        uint32_t caps = (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 && (esp_random() & 1))
                        ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
        size_t sz = sizes[ esp_random() % N ];
        void* p = tracked_malloc(sz, caps, "normal");
        if (p) {
            memset(p, 0x5A, sz);
            vTaskDelay(pdMS_TO_TICKS(50 + (esp_random() % 100)));
            tracked_free(p, "normal");
        }
        vTaskDelay(pdMS_TO_TICKS(80 + (esp_random() % 120)));
    }
}

// 2) สร้าง “รั่ว”: จองแล้ว “อาจ” ไม่คืนตามโอกาสที่กำหนด
#define LEAK_PROB_PERCENT   35  // โอกาสรั่ว 35%
#define LEAK_MIN_SIZE       1024
#define LEAK_MAX_SIZE       8192

// เก็บ pointer ที่ “ตั้งใจ” ไม่ free (simulate leak)
#define LEAK_BUCKET_MAX     64
static void* leak_bucket[LEAK_BUCKET_MAX] = {0};
static int   leak_bucket_n = 0;

static void leak_generator_task(void *pv) {
    ESP_LOGI(TAG, "leak generator start (p=%d%%)", LEAK_PROB_PERCENT);
    while (1) {
        size_t sz = LEAK_MIN_SIZE + (esp_random() % (LEAK_MAX_SIZE - LEAK_MIN_SIZE + 1));
        uint32_t caps = MALLOC_CAP_INTERNAL; // เน้นกระทบ internal heap ก่อน
        bool will_leak = ((esp_random() % 100) < LEAK_PROB_PERCENT);

        void* p = tracked_malloc(sz, caps, will_leak ? "leaky" : "temp");
        if (p) {
            memset(p, 0xA5, sz);
            if (will_leak && leak_bucket_n < LEAK_BUCKET_MAX) {
                leak_bucket[leak_bucket_n++] = p;   // ไม่ free → “จำลองรั่ว”
                ESP_LOGW(TAG, "INTENTIONAL LEAK: %uB @%p (bucket=%d/%d)",
                         (unsigned)sz, p, leak_bucket_n, LEAK_BUCKET_MAX);
            } else {
                // ใช้งานครู่หนึ่งแล้วคืน
                vTaskDelay(pdMS_TO_TICKS(150 + (esp_random() % 200)));
                tracked_free(p, "temp");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300 + (esp_random() % 400)));
    }
}

// 3) Detector: สแกนหา leak เป็นระยะ
static void leak_detector_task(void *pv) {
    ESP_LOGI(TAG, "leak detector start (interval=%ums)", (unsigned)DETECT_INTERVAL_MS);
    while (1) {
        detect_leaks_and_report();
        update_leds_by_heap();
        vTaskDelay(pdMS_TO_TICKS(DETECT_INTERVAL_MS));
    }
}

// 4) Reporter: รายงานภาพรวมเป็นระยะ และทดลอง “recovery” โดยคืนบาง leak
static void reporter_task(void *pv) {
    ESP_LOGI(TAG, "reporter start (interval=%ums)", (unsigned)REPORT_INTERVAL_MS);
    uint32_t tick = 0;
    while (1) {
        log_heap_brief("report");
        log_stats_summary();

        // ทุกๆ ~4 รอบ ลอง “กู้” หน่วยความจำรั่วบางส่วน เพื่อดูผล leak ลดลง
        if ((tick++ % 4) == 3 && leak_bucket_n > 0) {
            int to_recover = 1 + (esp_random() % (leak_bucket_n)); // คืนแบบสุ่มบางส่วน
            ESP_LOGI(TAG, "attempt recovery: free %d leaked blocks", to_recover);
            for (int i = 0; i < to_recover && leak_bucket_n > 0; i++) {
                void* p = leak_bucket[leak_bucket_n - 1];
                leak_bucket[--leak_bucket_n] = NULL;
                tracked_free(p, "recovery");
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(REPORT_INTERVAL_MS));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 Experiment 4: Memory Leak Detection");
    leds_init();
    update_leds_by_heap();

    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }
    memset(g_rec, 0, sizeof(g_rec));

    ESP_LOGI(TAG, "LEDs: GPIO2 OK | GPIO4 LOW | GPIO5 ERROR(leak) | GPIO19 SPIRAM");

    // Tasks
    xTaskCreate(normal_workload_task, "normal",   4096, NULL, 5, NULL);
    xTaskCreate(leak_generator_task,  "leaker",   4096, NULL, 5, NULL);
    xTaskCreate(leak_detector_task,   "detector", 3072, NULL, 6, NULL);
    xTaskCreate(reporter_task,        "reporter", 3072, NULL, 4, NULL);
}
