// main.c — Software Timers Lab: Experiment 3 (เพิ่ม Timer Load)

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "SW_TIMERS_EXP3";

// LED pins
#define LED_BLINK     GPIO_NUM_2      // Fast blink timer
#define LED_HEARTBEAT GPIO_NUM_4      // Heartbeat timer
#define LED_STATUS    GPIO_NUM_5      // Status timer
#define LED_ONESHOT   GPIO_NUM_18     // One-shot timer

// Periods (ms)
#define BLINK_PERIOD      500
#define HEARTBEAT_PERIOD  2000
#define STATUS_PERIOD     5000
#define ONESHOT_DELAY     3000

// ======= Forward declarations =======
static void blink_timer_callback(TimerHandle_t xTimer);
static void heartbeat_timer_callback(TimerHandle_t xTimer);
static void status_timer_callback(TimerHandle_t xTimer);
static void oneshot_timer_callback(TimerHandle_t xTimer);
static void dynamic_timer_callback(TimerHandle_t xTimer);
// เพิ่มสำหรับทดลองที่ 3
static void extra_callback(TimerHandle_t xTimer);

// ======= Handles =======
TimerHandle_t xBlinkTimer;
TimerHandle_t xHeartbeatTimer;
TimerHandle_t xStatusTimer;
TimerHandle_t xOneShotTimer;
TimerHandle_t xDynamicTimer;

// ======= Stats =======
typedef struct {
    uint32_t blink_count;
    uint32_t heartbeat_count;
    uint32_t status_count;
    uint32_t oneshot_count;
    uint32_t dynamic_count;
    uint32_t extra_count;     // รวมกิจกรรมจาก extra timers (ทุกตัว)
} timer_stats_t;

static timer_stats_t stats = {0};

// LED states
static bool led_blink_state = false;

// ======= Callbacks =======
// Blink timer callback (auto-reload)
static void blink_timer_callback(TimerHandle_t xTimer) {
    stats.blink_count++;
    led_blink_state = !led_blink_state;
    gpio_set_level(LED_BLINK, led_blink_state);

    ESP_LOGI(TAG, "💫 Blink: #%lu (LED=%s)",
             stats.blink_count, led_blink_state ? "ON" : "OFF");

    // Every 20 blinks, trigger one-shot timer
    if (stats.blink_count % 20 == 0) {
        if (xTimerStart(xOneShotTimer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Start one-shot failed");
        } else {
            ESP_LOGI(TAG, "🚀 One-shot armed (3s)");
        }
    }
}

// Heartbeat timer callback (auto-reload)
// หมายเหตุ: มี vTaskDelay เพื่อสาธิตผลกับ Timer Service Task
static void heartbeat_timer_callback(TimerHandle_t xTimer) {
    stats.heartbeat_count++;
    ESP_LOGI(TAG, "💓 Heartbeat: #%lu", stats.heartbeat_count);

    // Double blink
    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);

    // Randomly adjust blink timer period (25%)
    if ((esp_random() & 0x3) == 0) {
        uint32_t new_period = 300 + (esp_random() % 400); // 300–700ms
        if (xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100) == pdPASS) {
            ESP_LOGI(TAG, "🔧 Blink period -> %lums", new_period);
        }
    }
}

// Status timer callback (auto-reload)
static void status_timer_callback(TimerHandle_t xTimer) {
    stats.status_count++;

    // Flash LED_STATUS (มี delay เพื่อสาธิต)
    gpio_set_level(LED_STATUS, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_STATUS, 0);

    // Print stats
    ESP_LOGI(TAG, "📊 STATUS #%lu", stats.status_count);
    ESP_LOGI(TAG, "  Blink:     %lu", stats.blink_count);
    ESP_LOGI(TAG, "  Heartbeat: %lu", stats.heartbeat_count);
    ESP_LOGI(TAG, "  Status:    %lu", stats.status_count);
    ESP_LOGI(TAG, "  One-shot:  %lu", stats.oneshot_count);
    ESP_LOGI(TAG, "  Dynamic:   %lu", stats.dynamic_count);
    ESP_LOGI(TAG, "  ExtraSum:  %lu", stats.extra_count);

    ESP_LOGI(TAG, "Timers:");
    ESP_LOGI(TAG, "  Blink     -> %s, %lums",
             xTimerIsTimerActive(xBlinkTimer) ? "ACTIVE" : "INACTIVE",
             xTimerGetPeriod(xBlinkTimer) * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "  Heartbeat -> %s, %lums",
             xTimerIsTimerActive(xHeartbeatTimer) ? "ACTIVE" : "INACTIVE",
             xTimerGetPeriod(xHeartbeatTimer) * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "  Status    -> %s, %lums",
             xTimerIsTimerActive(xStatusTimer) ? "ACTIVE" : "INACTIVE",
             xTimerGetPeriod(xStatusTimer) * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "  One-shot  -> %s",
             xTimerIsTimerActive(xOneShotTimer) ? "ACTIVE" : "INACTIVE");
}

// One-shot timer callback
static void oneshot_timer_callback(TimerHandle_t xTimer) {
    stats.oneshot_count++;
    ESP_LOGI(TAG, "⚡ One-shot: #%lu", stats.oneshot_count);

    // Quick 5 flashes
    for (int i = 0; i < 5; i++) {
        gpio_set_level(LED_ONESHOT, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_ONESHOT, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Create a dynamic one-shot timer (period random 1–4s)
    uint32_t random_period = 1000 + (esp_random() % 3000);
    TimerHandle_t t = xTimerCreate("Dyn", pdMS_TO_TICKS(random_period),
                                   pdFALSE, NULL, dynamic_timer_callback);
    if (t && xTimerStart(t, 0) == pdPASS) {
        ESP_LOGI(TAG, "🎲 Dynamic created (period %lums)", random_period);
    } else {
        ESP_LOGW(TAG, "Dynamic create/start failed");
        if (t) xTimerDelete(t, 0);
    }
}

// Dynamic timer callback (created at runtime)
static void dynamic_timer_callback(TimerHandle_t xTimer) {
    stats.dynamic_count++;

    // Pulse all LEDs shortly, then restore blink LED state
    gpio_set_level(LED_BLINK, 1);
    gpio_set_level(LED_HEARTBEAT, 1);
    gpio_set_level(LED_STATUS, 1);
    gpio_set_level(LED_ONESHOT, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(LED_BLINK, led_blink_state);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    // Delete itself
    if (xTimerDelete(xTimer, 100) == pdPASS) {
        ESP_LOGI(TAG, "Dynamic deleted");
    } else {
        ESP_LOGW(TAG, "Dynamic delete failed");
    }
}

// ======= Extra timers (Experiment 3) =======
// ทำงานเบาๆ และไม่ block — เพียงนับ/กระพริบสถานะสั้นๆ
static void extra_callback(TimerHandle_t xTimer) {
    // รวมสถิติทุก extra timer ไว้ที่ตัวเดียว (extra_count)
    stats.extra_count++;

    // ระบุ ID timer จาก pvTimerID
    intptr_t id = (intptr_t) pvTimerGetTimerID(xTimer);

    // “ไม่” ใช้ vTaskDelay ใน callback เพื่อไม่บล็อก Timer Service Task
    // แค่ติ๊ด LED_STATUS สั้นๆ แบบ non-block (set แล้ว clear ทันที)
    gpio_set_level(LED_STATUS, 1);
    gpio_set_level(LED_STATUS, 0);

    // log บางรอบเพื่อลดสแปม
    if ((stats.extra_count % 50) == 0) {
        TickType_t period_ticks = xTimerGetPeriod(xTimer);
        uint32_t period_ms = period_ticks * portTICK_PERIOD_MS;
        ESP_LOGI(TAG, "✨ Extra[%ld] tick, ExtraSum=%lu (P=%lums)",
                 (long)id, stats.extra_count, period_ms);
    }
}

// ======= Optional: Control task (สุ่มเปลี่ยน/หยุด/รีเซ็ต) =======
static void timer_control_task(void *pv) {
    ESP_LOGI(TAG, "Timer control task start");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        int action = esp_random() % 3;
        switch (action) {
            case 0:
                ESP_LOGI(TAG, "⏸️ stop Heartbeat 5s");
                xTimerStop(xHeartbeatTimer, 100);
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "▶️ start Heartbeat");
                xTimerStart(xHeartbeatTimer, 100);
                break;
            case 1:
                ESP_LOGI(TAG, "🔄 reset Status");
                xTimerReset(xStatusTimer, 100);
                break;
            default: {
                uint32_t np = 200 + (esp_random() % 600);
                ESP_LOGI(TAG, "⚙️ change Blink -> %lums", np);
                xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(np), 100);
            } break;
        }
    }
}

// ======= app_main =======
void app_main(void) {
    ESP_LOGI(TAG, "Software Timers — Experiment 3 (Extra Load) Starting...");

    // GPIO init
    gpio_set_direction(LED_BLINK,     GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_HEARTBEAT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STATUS,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ONESHOT,   GPIO_MODE_OUTPUT);
    gpio_set_level(LED_BLINK, 0);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    // Base timers
    xBlinkTimer = xTimerCreate("Blink",
                               pdMS_TO_TICKS(BLINK_PERIOD),
                               pdTRUE, (void*)1, blink_timer_callback);
    xHeartbeatTimer = xTimerCreate("Heartbeat",
                                   pdMS_TO_TICKS(HEARTBEAT_PERIOD),
                                   pdTRUE, (void*)2, heartbeat_timer_callback);
    xStatusTimer = xTimerCreate("Status",
                                pdMS_TO_TICKS(STATUS_PERIOD),
                                pdTRUE, (void*)3, status_timer_callback);
    xOneShotTimer = xTimerCreate("OneShot",
                                 pdMS_TO_TICKS(ONESHOT_DELAY),
                                 pdFALSE, (void*)4, oneshot_timer_callback);

    if (!xBlinkTimer || !xHeartbeatTimer || !xStatusTimer || !xOneShotTimer) {
        ESP_LOGE(TAG, "Timer create failed — check CONFIG_FREERTOS_USE_TIMERS=y");
        return;
    }

    // Start base timers
    xTimerStart(xBlinkTimer, 0);
    xTimerStart(xHeartbeatTimer, 0);
    xTimerStart(xStatusTimer, 0);

    // ===== EXPERIMENT 3: เพิ่ม Timer Load =====
    // สร้าง extra timers 10 ตัว (auto-reload) คาบ 100,150,...,550 ms
    for (int i = 0; i < 10; i++) {
        uint32_t period = 100 + i * 50;
        TimerHandle_t extra = xTimerCreate("Extra",
            pdMS_TO_TICKS(period),
            pdTRUE,                         // auto-reload
            (void*)(intptr_t)i,             // ID
            extra_callback);
        if (extra) {
            if (xTimerStart(extra, 0) != pdPASS) {
                ESP_LOGW(TAG, "Extra[%d] start failed", i);
            } else {
                ESP_LOGI(TAG, "Extra[%d] started (period=%ums)", i, period);
            }
        } else {
            ESP_LOGW(TAG, "Extra[%d] create failed", i);
        }
    }

    // Control task (เดิม)
    xTaskCreate(timer_control_task, "TimerControl", 2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "Ready. LEDs:");
    ESP_LOGI(TAG, "  GPIO2  Blink (fast, period changes)");
    ESP_LOGI(TAG, "  GPIO4  Heartbeat (double blink)");
    ESP_LOGI(TAG, "  GPIO5  Status (5s stats + extra blips)");
    ESP_LOGI(TAG, "  GPIO18 One-shot (5 quick flashes)");
}
