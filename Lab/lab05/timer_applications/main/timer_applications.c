
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

static const char *TAG = "EXP4_HEALTH";

// ===== Pins =====
#define STATUS_LED   GPIO_NUM_2   // กระพริบตอนรายงาน
#define WARN_LED     GPIO_NUM_4   // ติดเมื่อพบปัญหา (heap/heartbeat)

// ===== Timings =====
#define STATUS_REPORT_MS     3000   // รายงานทุก 3s
#define JITTER_TIMER_MS       100   // ตัวจับเวลาวัดจิตเตอร์ (คาบเป้า 100ms)

// Heartbeat period/timeout ของแต่ละงาน (มิลลิวินาที)
#define LIGHT_BEAT_MS         200
#define MEDIUM_BEAT_MS        100
#define HEAVY_WORK_MS          50
#define HEAVY_IDLE_MS          50
#define HEARTBEAT_TIMEOUT_MS  800   // ถ้าเงียบเกินนี้ ถือว่าพลาด 1 ครั้ง

// Heap thresholds
#define HEAP_WARN_BYTES     12000   // เตือนถ้าต่ำกว่า
#define HEAP_CRIT_BYTES      8000   // อันตรายมาก

// ===== Types / Globals =====
typedef struct {
    const char *name;
    TaskHandle_t handle;
    TickType_t   last_beat_ticks;   // เวลา heartbeat ล่าสุด (ticks)
    uint32_t     beats_total;       // รวมจำนวน heartbeat
    uint32_t     missed_total;      // รวม missed
    bool         overdue_now;       // flag สำหรับรอบล่าสุด
} task_health_t;

static task_health_t g_light = { .name = "Light" };
static task_health_t g_medium = { .name = "Medium" };
static task_health_t g_heavy = { .name = "Heavy"  };

static TimerHandle_t status_timer = NULL;
static TimerHandle_t jitter_timer = NULL;

// Jitter metrics
static volatile uint32_t jitter_count = 0;
static volatile int64_t  jitter_sum_us = 0;    // ผลรวม error (us) แบบ absolute
static volatile int32_t  jitter_max_us = 0;    // ค่าสูงสุดแบบ absolute
static volatile int64_t  last_tick_us = 0;

static volatile bool system_healthy = true;

// ===== Prototypes =====
static void init_hardware(void);
static void light_task(void *arg);
static void medium_task(void *arg);
static void heavy_task(void *arg);
static inline void heartbeat(task_health_t *th);

static void status_timer_cb(TimerHandle_t xTimer);
static void jitter_timer_cb(TimerHandle_t xTimer);

// task ย่อยสำหรับกระพริบ STATUS_LED สั้นๆ (แทน lambda)
static void blink_once_task(void *pv);

// ===== Helper: heartbeat update =====
static inline void heartbeat(task_health_t *th)
{
    th->last_beat_ticks = xTaskGetTickCount();
    th->beats_total++;
}

// ===== Tasks (จำลองโหลดต่างระดับ) =====
static void light_task(void *arg)
{
    task_health_t *th = (task_health_t *)arg;
    ESP_LOGI(TAG, "%s task started", th->name);
    while (1) {
        heartbeat(th);
        vTaskDelay(pdMS_TO_TICKS(LIGHT_BEAT_MS));
    }
}

static void medium_task(void *arg)
{
    task_health_t *th = (task_health_t *)arg;
    ESP_LOGI(TAG, "%s task started", th->name);
    while (1) {
        heartbeat(th);
        vTaskDelay(pdMS_TO_TICKS(MEDIUM_BEAT_MS));
    }
}

static void heavy_task(void *arg)
{
    task_health_t *th = (task_health_t *)arg;
    ESP_LOGI(TAG, "%s task started", th->name);
    while (1) {
        // จำลองงานหนัก ~HEAVY_WORK_MS ด้วย busy loop เล็กน้อย
        int64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < (HEAVY_WORK_MS * 1000)) {
            __asm__ __volatile__("nop"); // กินเวลาเล็กน้อย
        }
        heartbeat(th);
        vTaskDelay(pdMS_TO_TICKS(HEAVY_IDLE_MS));
    }
}

// ===== Timer: วัดจิตเตอร์ของ software timer =====
static void jitter_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    int64_t now = esp_timer_get_time(); // us
    if (last_tick_us != 0) {
        int64_t delta_us = now - last_tick_us;                  // ช่วงเวลาจริง
        int32_t err_us   = (int32_t)(delta_us - JITTER_TIMER_MS * 1000); // คลาดจากคาบเป้า
        if (err_us < 0) err_us = -err_us;                       // absolute
        jitter_sum_us += err_us;
        if (err_us > jitter_max_us) jitter_max_us = err_us;
        jitter_count++;
    }
    last_tick_us = now;
}

// ===== Timer: รายงานสถานะและตรวจสุขภาพระบบ =====
static void status_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    // --- Uptime ---
    TickType_t ticks = xTaskGetTickCount();
    uint32_t uptime_ms = (uint32_t) (ticks * portTICK_PERIOD_MS);

    // --- Heap ---
    size_t free_heap     = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();

    // --- ตรวจ heartbeat ของแต่ละ task ---
    task_health_t *arr[3] = { &g_light, &g_medium, &g_heavy };
    uint32_t overdue_count = 0;
    for (int i = 0; i < 3; ++i) {
        task_health_t *th = arr[i];
        TickType_t last = th->last_beat_ticks;
        uint32_t ms_since = (uint32_t)((ticks - last) * portTICK_PERIOD_MS);
        bool overdue = (ms_since > HEARTBEAT_TIMEOUT_MS);
        th->overdue_now = overdue;
        if (overdue) {
            th->missed_total++;
            overdue_count++;
        }
    }

    // --- จิตเตอร์ ---
    float jitter_avg_us = 0.0f;
    int32_t jitter_max_local = jitter_max_us;
    uint32_t jitter_cnt_local = jitter_count;
    if (jitter_cnt_local > 0) {
        jitter_avg_us = (float)jitter_sum_us / (float)jitter_cnt_local;
    }

    // --- สรุปสถานะสุขภาพระบบ ---
    bool heap_bad = (free_heap < HEAP_WARN_BYTES) || (min_free_heap < HEAP_WARN_BYTES);
    bool critical = (free_heap < HEAP_CRIT_BYTES) || (min_free_heap < HEAP_CRIT_BYTES);
    system_healthy = (!heap_bad && overdue_count == 0);

    // LED แจ้งเตือน
    gpio_set_level(WARN_LED, (!system_healthy || critical) ? 1 : 0);

    // กระพริบ STATUS_LED สั้น ๆ เพื่อบอกว่ารายงานแล้ว (ใช้ task เล็ก ๆ)
    gpio_set_level(STATUS_LED, 1);
    (void)xTaskCreate(blink_once_task, "BlinkOnce", 1024, NULL, 1, NULL);

    // --- พิมพ์รายงาน ---
    ESP_LOGI(TAG,
        "\n===== SYSTEM STATUS =====\n"
        "Uptime: %lu.%03lu s\n"
        "Health: %s%s\n"
        "Heap: free=%u B, min=%u B%s\n"
        "Timer Jitter: avg=%.2f us, max=%d us (period=%d ms, n=%lu)\n"
        "Tasks:\n"
        "  Light : beats=%lu, missed=%lu, overdue=%s\n"
        "  Medium: beats=%lu, missed=%lu, overdue=%s\n"
        "  Heavy : beats=%lu, missed=%lu, overdue=%s\n"
        "=========================",
        (unsigned long)(uptime_ms/1000), (unsigned long)(uptime_ms%1000),
        system_healthy ? "✅ OK" : "❌ ISSUE",
        critical ? " (CRITICAL HEAP!)" : "",
        (unsigned)free_heap, (unsigned)min_free_heap,
        heap_bad ? "  ⚠️" : "",
        jitter_avg_us, (int)jitter_max_local, JITTER_TIMER_MS, (unsigned long)jitter_cnt_local,
        (unsigned long)g_light.beats_total,  (unsigned long)g_light.missed_total,  g_light.overdue_now  ? "YES" : "NO",
        (unsigned long)g_medium.beats_total, (unsigned long)g_medium.missed_total, g_medium.overdue_now ? "YES" : "NO",
        (unsigned long)g_heavy.beats_total,  (unsigned long)g_heavy.missed_total,  g_heavy.overdue_now  ? "YES" : "NO"
    );
}

// ===== Task กระพริบไฟสั้น ๆ (แทน lambda) =====
static void blink_once_task(void *pv)
{
    (void)pv;
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(STATUS_LED, 0);
    vTaskDelete(NULL);
}

// ===== Hardware =====
static void init_hardware(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<STATUS_LED) | (1ULL<<WARN_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(WARN_LED, 0);
    ESP_LOGI(TAG, "GPIO init OK (STATUS=%d, WARN=%d)", STATUS_LED, WARN_LED);
}

// ===== app_main =====
void app_main(void)
{
    ESP_LOGI(TAG, "Experiment 4: System Health Monitoring");

    init_hardware();

    // สร้าง tasks
    xTaskCreate(light_task,  "LightTask",  2048, &g_light,  3, &g_light.handle);
    xTaskCreate(medium_task, "MedTask",    2048, &g_medium, 4, &g_medium.handle);
    xTaskCreate(heavy_task,  "HeavyTask",  3072, &g_heavy,  5, &g_heavy.handle);

    // กำหนดค่าเริ่ม heartbeat = ตอนนี้ (กัน false positive)
    TickType_t now = xTaskGetTickCount();
    g_light.last_beat_ticks  = now;
    g_medium.last_beat_ticks = now;
    g_heavy.last_beat_ticks  = now;

    // สร้าง timers
    status_timer = xTimerCreate("StatusTimer",
                                pdMS_TO_TICKS(STATUS_REPORT_MS),
                                pdTRUE,   // auto-reload
                                (void*)0,
                                status_timer_cb);

    jitter_timer = xTimerCreate("JitterTimer",
                                pdMS_TO_TICKS(JITTER_TIMER_MS),
                                pdTRUE,   // auto-reload
                                (void*)0,
                                jitter_timer_cb);

    if (!status_timer || !jitter_timer) {
        ESP_LOGE(TAG, "Timer create failed");
        return;
    }

    // เริ่มต้น
    last_tick_us = 0;
    jitter_sum_us = 0;
    jitter_max_us = 0;
    jitter_count  = 0;

    xTimerStart(status_timer, 0);
    xTimerStart(jitter_timer, 0);

    ESP_LOGI(TAG, "เริ่มรายงานทุก %d ms, กำลังวัด jitter timer %d ms",
             STATUS_REPORT_MS, JITTER_TIMER_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
