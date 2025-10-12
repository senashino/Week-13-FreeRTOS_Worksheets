#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "COUNTING_SEM_EXP3";

/* ================== Config: Experiment 3 ================== */
#define MAX_RESOURCES   3      // ★ Resources = 3
#define NUM_PRODUCERS   8      // ★ Producers = 8  (มากกว่าทรัพยากร)
#define PRODUCER_STACK  3072
#define MONITOR_STACK   3072
#define STAT_STACK      3072
#define LOADGEN_STACK   2048

// แสดงสถานะ Resource ด้วย 3 LED
static const gpio_num_t LED_RESOURCE_PINS[MAX_RESOURCES] = {
    GPIO_NUM_2,   // R1
    GPIO_NUM_4,   // R2
    GPIO_NUM_5    // R3
};

#define LED_PRODUCER GPIO_NUM_18
#define LED_SYSTEM   GPIO_NUM_19
/* ========================================================= */

SemaphoreHandle_t xCountingSemaphore;

typedef struct {
    int  resource_id;
    bool in_use;
    char current_user[20];
    uint32_t usage_count;
    uint32_t total_usage_time_ms;
} resource_t;

static resource_t resources[MAX_RESOURCES];

typedef struct {
    uint32_t total_requests;
    uint32_t successful_acquisitions;
    uint32_t failed_acquisitions;
    uint32_t resources_in_use;
} system_stats_t;

static system_stats_t stats = {0};

static inline void led_on(int idx){ if (idx>=0 && idx<MAX_RESOURCES) gpio_set_level(LED_RESOURCE_PINS[idx], 1); }
static inline void led_off(int idx){ if (idx>=0 && idx<MAX_RESOURCES) gpio_set_level(LED_RESOURCE_PINS[idx], 0); }

static int acquire_resource(const char* user_name)
{
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!resources[i].in_use) {
            resources[i].in_use = true;
            strncpy(resources[i].current_user, user_name, sizeof(resources[i].current_user)-1);
            resources[i].current_user[sizeof(resources[i].current_user)-1] = '\0';
            resources[i].usage_count++;
            led_on(i);
            stats.resources_in_use++;
            return i;
        }
    }
    return -1;
}

static void release_resource(int idx, uint32_t use_ms)
{
    if (idx>=0 && idx<MAX_RESOURCES) {
        resources[idx].in_use = false;
        resources[idx].total_usage_time_ms += use_ms;
        resources[idx].current_user[0] = '\0';
        led_off(idx);
        if (stats.resources_in_use) stats.resources_in_use--;
    }
}

static void producer_task(void *pvParameters)
{
    int id = *((int*)pvParameters);
    char name[20]; snprintf(name, sizeof(name), "Producer%d", id);
    ESP_LOGI(TAG, "%s started", name);

    while (1) {
        stats.total_requests++;

        // แสดงว่ามีคำขอเข้า queue
        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(LED_PRODUCER, 0);

        TickType_t t0 = xTaskGetTickCount();

        // รอสูงสุด 8 วินาที ให้เกิดโอกาส timeout ในบางช่วงโหลดสูง
        if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(8000)) == pdTRUE) {
            uint32_t wait_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            stats.successful_acquisitions++;

            int idx = acquire_resource(name);
            if (idx >= 0) {
                // ใช้งานทรัพยากร 1–4 วินาที
                uint32_t use_ms = 1000 + (esp_random() % 3000);
                ESP_LOGI(TAG, "✓ %s: Acquired R%d (wait:%ums), using %ums",
                         name, idx+1, wait_ms, use_ms);
                vTaskDelay(pdMS_TO_TICKS(use_ms));
                release_resource(idx, use_ms);
                xSemaphoreGive(xCountingSemaphore);
                ESP_LOGI(TAG, "✓ %s: Released R%d", name, idx+1);
            } else {
                // ป้องกันกรณีผิดพลาด
                ESP_LOGE(TAG, "✗ %s: Took semaphore but no resource free!", name);
                xSemaphoreGive(xCountingSemaphore);
            }
        } else {
            stats.failed_acquisitions++;
            ESP_LOGW(TAG, "⏰ %s: Timeout waiting for resource", name);
            // backoff เล็กน้อยก่อนลองใหม่
            vTaskDelay(pdMS_TO_TICKS(500 + (esp_random()%500)));
        }

        // เว้นช่วง 2–5 วินาที
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    }
}

static void resource_monitor_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int available = uxSemaphoreGetCount(xCountingSemaphore);
        int used = MAX_RESOURCES - available;

        ESP_LOGI(TAG, "\n📊 RESOURCE POOL STATUS");
        ESP_LOGI(TAG, "Available: %d/%d  |  In use: %d", available, MAX_RESOURCES, used);

        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (resources[i].in_use) {
                ESP_LOGI(TAG, "  R%d: BUSY (User:%s, Usage:%lu times)",
                         i+1, resources[i].current_user, resources[i].usage_count);
            } else {
                ESP_LOGI(TAG, "  R%d: FREE (Total usage:%lu times)", i+1, resources[i].usage_count);
            }
        }
        printf("Pool: [");
        for (int i = 0; i < MAX_RESOURCES; i++) printf(resources[i].in_use ? "■" : "□");
        printf("] Available: %d\n", available);
        ESP_LOGI(TAG, "────────────────────────────\n");
    }
}

static void statistics_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG, "\n📈 SYSTEM STATISTICS");
        ESP_LOGI(TAG, "Total requests: %lu", stats.total_requests);
        ESP_LOGI(TAG, "Successful acquisitions: %lu", stats.successful_acquisitions);
        ESP_LOGI(TAG, "Failed acquisitions: %lu", stats.failed_acquisitions);
        ESP_LOGI(TAG, "Current in use: %lu", stats.resources_in_use);
        if (stats.total_requests) {
            float ok = (float)stats.successful_acquisitions * 100.0f / (float)stats.total_requests;
            ESP_LOGI(TAG, "Success rate: %.1f%%", ok);
        }
        for (int i = 0; i < MAX_RESOURCES; i++) {
            ESP_LOGI(TAG, "  R%d: %lu uses, %lums total",
                     i+1, resources[i].usage_count, resources[i].total_usage_time_ms);
        }
        ESP_LOGI(TAG, "────────────────────────────\n");
    }
}

static void load_generator_task(void *pvParameters)
{
    // ใช้กระตุ้นโหลดให้เกิด contention ชัดเจน
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGW(TAG, "🚀 LOAD BURST");
        gpio_set_level(LED_SYSTEM, 1);

        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < MAX_RESOURCES + 3; i++) {
                if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                    int idx = acquire_resource("LoadGen");
                    if (idx >= 0) {
                        vTaskDelay(pdMS_TO_TICKS(400));
                        release_resource(idx, 400);
                    }
                    xSemaphoreGive(xCountingSemaphore);
                }
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            vTaskDelay(pdMS_TO_TICKS(700));
        }

        gpio_set_level(LED_SYSTEM, 0);
        ESP_LOGI(TAG, "END BURST\n");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Experiment 3: 3 Resources, 8 Producers");

    // init resource table
    for (int i = 0; i < MAX_RESOURCES; i++) {
        resources[i].resource_id = i + 1;
        resources[i].in_use = false;
        resources[i].current_user[0] = '\0';
        resources[i].usage_count = 0;
        resources[i].total_usage_time_ms = 0;
    }

    // LEDs
    for (int i = 0; i < MAX_RESOURCES; i++) {
        gpio_set_direction(LED_RESOURCE_PINS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(LED_RESOURCE_PINS[i], 0);
    }
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PRODUCER, 0);
    gpio_set_level(LED_SYSTEM, 0);

    // Counting semaphore
    xCountingSemaphore = xSemaphoreCreateCounting(MAX_RESOURCES, MAX_RESOURCES);
    if (!xCountingSemaphore) { ESP_LOGE(TAG, "Create semaphore failed"); return; }

    // Producers
    static int ids[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) ids[i] = i + 1;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        char n[20]; snprintf(n, sizeof(n), "Producer%d", i+1);
        xTaskCreate(producer_task, n, PRODUCER_STACK, &ids[i], 3, NULL);
    }

    // Monitors
    xTaskCreate(resource_monitor_task, "ResMonitor", MONITOR_STACK, NULL, 2, NULL);
    xTaskCreate(statistics_task,      "Statistics",  STAT_STACK,  NULL, 1, NULL);
    xTaskCreate(load_generator_task,  "LoadGen",     LOADGEN_STACK, NULL, 4, NULL);

    // startup sweep
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < MAX_RESOURCES; i++) { gpio_set_level(LED_RESOURCE_PINS[i], 1); vTaskDelay(pdMS_TO_TICKS(120)); }
        gpio_set_level(LED_PRODUCER, 1); gpio_set_level(LED_SYSTEM, 1); vTaskDelay(pdMS_TO_TICKS(250));
        for (int i = 0; i < MAX_RESOURCES; i++) gpio_set_level(LED_RESOURCE_PINS[i], 0);
        gpio_set_level(LED_PRODUCER, 0); gpio_set_level(LED_SYSTEM, 0); vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "System up: Resources=%d, Producers=%d", MAX_RESOURCES, NUM_PRODUCERS);
}
