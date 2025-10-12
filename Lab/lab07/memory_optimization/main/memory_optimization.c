#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "MEM_REGION_EXP4";

// LEDs
#define LED_OPTIMIZATION  GPIO_NUM_19   // กำลังวิเคราะห์
#define LED_ALERT         GPIO_NUM_18   // เตือน utilization/fragmentation สูง

// เกณฑ์เตือน
#define UTIL_WARN_PCT         85.0f
#define FRAG_WARN_PCT         60.0f

typedef struct {
    const char *name;
    uint32_t caps;
    bool is_exec;
    bool is_dma;
} region_desc_t;

static const region_desc_t REGIONS[] = {
    { "Internal RAM",     MALLOC_CAP_INTERNAL,    false, true  },
    { "SPIRAM",           MALLOC_CAP_SPIRAM,      false, false },
    { "DMA Capable",      MALLOC_CAP_DMA,         false, true  },
    { "8-bit Accessible", MALLOC_CAP_8BIT,        false, true  },
    { "32-bit Aligned",   MALLOC_CAP_32BIT,       false, true  },
#ifdef MALLOC_CAP_EXEC
    { "Executable",       MALLOC_CAP_EXEC,        true,  false },
#endif
};

static inline float pct(size_t num, size_t den) {
    if (den == 0) return 0.0f;
    return (float)num * 100.0f / (float)den;
}

// ทดสอบจองหน่วยความจำใน region นั้น ๆ (best-effort) แล้วคืนกลับทันที
static bool probe_region_allocation(uint32_t caps, size_t bytes) {
    void *p = heap_caps_malloc(bytes, caps);
    if (!p) return false;
    memset(p, 0xAB, bytes < 64 ? bytes : 64); // แตะเล็กน้อยยืนยันการเข้าถึง
    heap_caps_free(p);
    return true;
}

static void print_region_report(void) {
    bool any_alert = false;

    ESP_LOGI(TAG, "\n===== MEMORY REGION ANALYSIS =====");
    for (size_t i = 0; i < sizeof(REGIONS)/sizeof(REGIONS[0]); ++i) {
        const region_desc_t *R = &REGIONS[i];

        size_t total   = heap_caps_get_total_size(R->caps);
        size_t free_sz = heap_caps_get_free_size(R->caps);
        size_t largest = heap_caps_get_largest_free_block(R->caps);

        if (total == 0) {
            // ไม่มี region นี้บนบอร์ด/คอนฟิกปัจจุบัน
            ESP_LOGI(TAG, "%s: (not present)", R->name);
            continue;
        }

        float used_pct = 100.0f - pct(free_sz, total);
        float frag_pct = 0.0f;
        if (free_sz > 0 && largest > 0) {
            // นิยาม fragmentation อย่างง่าย: 1 - (largest_free / total_free)
            frag_pct = (1.0f - ((float)largest / (float)free_sz)) * 100.0f;
        }

        ESP_LOGI(TAG, "%s:", R->name);
        ESP_LOGI(TAG, "  Total:         %u bytes (%.1f KB)", (unsigned)total,   total/1024.0f);
        ESP_LOGI(TAG, "  Free:          %u bytes (%.1f KB)", (unsigned)free_sz, free_sz/1024.0f);
        ESP_LOGI(TAG, "  Largest Block: %u bytes", (unsigned)largest);
        ESP_LOGI(TAG, "  Utilization:   %.1f%%", used_pct);
        ESP_LOGI(TAG, "  Fragmentation: %.1f%%", frag_pct);
        ESP_LOGI(TAG, "  Exec: %s | DMA: %s",
                 R->is_exec ? "Yes" : "No",
                 R->is_dma  ? "Yes" : "No");

        // Quick probes (เล็ก/กลาง/ใหญ่)
        bool ok4k  = probe_region_allocation(R->caps, 4*1024);
        bool ok32k = probe_region_allocation(R->caps, 32*1024);
        bool ok128k= probe_region_allocation(R->caps, 128*1024);
        ESP_LOGI(TAG, "  Probe alloc: 4KB=%s  32KB=%s  128KB=%s",
                 ok4k?"OK":"FAIL", ok32k?"OK":"FAIL", ok128k?"OK":"FAIL");

        // เตือนถ้าเข้าเงื่อนไข
        if (used_pct > UTIL_WARN_PCT || frag_pct > FRAG_WARN_PCT) {
            any_alert = true;
            ESP_LOGW(TAG, "  ⚠ ALERT: %s threshold exceeded (util>%.0f%% or frag>%.0f%%)",
                     R->name, UTIL_WARN_PCT, FRAG_WARN_PCT);
        }
        ESP_LOGI(TAG, "");
    }

    // สรุปภาพรวมระบบ
    ESP_LOGI(TAG, "System: Free heap=%u bytes, Min free=%u bytes, Uptime=%llu ms",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned long long)(esp_timer_get_time()/1000ULL));

    // LED เตือนรวม
    gpio_set_level(LED_ALERT, any_alert ? 1 : 0);
    ESP_LOGI(TAG, "=====================================\n");
}

// ภารกิจหลัก: สแกนซ้ำทุกช่วงเวลา + stress เล็กน้อยเพื่อสังเกต fragmentation
static void region_monitor_task(void *arg) {
    // บัฟเฟอร์ชั่วคราวสำหรับ stress (จะจอง/คืนแบบสลับขนาด เพื่อสร้าง/สังเกต fragmentation)
    const size_t pattern_sizes[] = { 1024, 2048, 4096, 8192, 16384 };
    const int pattern_len = sizeof(pattern_sizes)/sizeof(pattern_sizes[0]);

    void *stress_ptrs[16] = {0};

    while (1) {
        gpio_set_level(LED_OPTIMIZATION, 1);

        // 1) รายงานสถานะล่าสุด
        print_region_report();

        // 2) Stress เล็ก ๆ: จอง/คืนแบบสุ่มขนาด (เฉพาะ 8-bit หรือ INTERNAL เพื่อความเข้ากันได้)
        for (int i = 0; i < 16; ++i) {
            if (stress_ptrs[i] == NULL) {
                size_t sz = pattern_sizes[i % pattern_len];
                // พยายาม INTERNAL ก่อน ถ้าไม่มีให้ลอง 8BIT
                void *p = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
                if (!p) p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
                if (p) {
                    memset(p, 0xCD, sz < 64 ? sz : 64);
                    stress_ptrs[i] = p;
                }
            } else {
                heap_caps_free(stress_ptrs[i]);
                stress_ptrs[i] = NULL;
            }
        }

        gpio_set_level(LED_OPTIMIZATION, 0);
        vTaskDelay(pdMS_TO_TICKS(15000)); // เว้น 15 วิ แล้ววนใหม่
    }
}

void app_main(void) {
    // ตั้งค่า LED
    gpio_reset_pin(LED_OPTIMIZATION);
    gpio_reset_pin(LED_ALERT);
    gpio_set_direction(LED_OPTIMIZATION, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ALERT, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_OPTIMIZATION, 0);
    gpio_set_level(LED_ALERT, 0);

    ESP_LOGI(TAG, "🚀 Experiment 4: Memory Region Analysis & Fragmentation Monitor");
    ESP_LOGI(TAG, "LED19 = analyzing, LED18 = alert (high util/fragmentation)");

    // สร้าง task หลัก
    xTaskCreate(region_monitor_task, "region_mon", 4096, NULL, 5, NULL);
}
