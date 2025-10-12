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

static const char *TAG = "COUNTING_SEM";

/* ================== Config (ทดลองที่ 2) ================== */
// เพิ่ม Resource เป็น 5 ตามเงื่อนไขของการทดลองที่ 2
#define MAX_RESOURCES   5      // ★★ ปรับเป็น 5 ★★
#define NUM_PRODUCERS   5      // ผู้ผลิต 5 ตัว (เท่าเดิม)
#define PRODUCER_STACK  3072
#define MONITOR_STACK   3072
#define STAT_STACK      3072
#define LOADGEN_STACK   2048

// แผง LED สำหรับแสดง Resource (ใช้เฉพาะ index 0..MAX_RESOURCES-1)
static const gpio_num_t LED_RESOURCE_PINS[5] = {
    GPIO_NUM_2,   // Resource 1
    GPIO_NUM_4,   // Resource 2
    GPIO_NUM_5,   // Resource 3
    GPIO_NUM_21,  // Resource 4 (เพิ่ม)
    GPIO_NUM_22   // Resource 5 (เพิ่ม)
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

static inline void led_on(int idx)  { if (idx >= 0 && idx < MAX_RESOURCES) gpio_set_level(LED_RESOURCE_PINS[idx], 1); }
static inline void led_off(int idx) { if (idx >= 0 && idx < MAX_RESOURCES) gpio_set_level(LED_RESOURCE_PINS[idx], 0); }

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

static void release_resource(int resource_index, uint32_t usage_time_ms)
{
    if (resource_index >= 0 && resource_index < MAX_RESOURCES) {
        resources[resource_index].in_use = false;
        resources[resource_index].total_usage_time_ms += usage_time_ms;
        resources[resource_index].current_user[0] = '\0';
        led_off(resource_index);
        if (stats.resources_in_use) stats.resources_in_use--;
    }
}

static void producer_task(void *pvParameters)
{
    int producer_id = *((int*)pvParameters);
    char task_name[20];
    snprintf(task_name, sizeof(task_name), "Producer%d", producer_id);
    ESP_LOGI(TAG, "%s started", task_name);

    while (1) {
        stats.total_requests++;

        // blink producer request
        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(40));
        gpio_set_level(LED_PRODUCER, 0);

        TickType_t t0 = xTaskGetTickCount();
        if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(8000)) == pdTRUE) {
            uint32_t wait_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
            stats.successful_acquisitions++;

            int res_idx = acquire_resource(task_name);
            if (res_idx >= 0) {
                uint32_t use_ms = 1000 + (esp_random() % 3000); // 1–4s
                ESP_LOGI(TAG, "✓ %s: Acquired resource %d (wait: %ums), using for %ums",
                         task_name, res_idx + 1, wait_ms, use_ms);
                vTaskDelay(pdMS_TO_TICKS(use_ms));
                release_resource(res_idx, use_ms);
                xSemaphoreGive(xCountingSemaphore);
                ESP_LOGI(TAG, "✓ %s: Released resource %d", task_name, res_idx + 1);
            } else {
                ESP_LOGE(TAG, "✗ %s: Took semaphore but no resource free!", task_name);
                xSemaphoreGive(xCountingSemaphore);
            }
        } else {
            stats.failed_acquisitions++;
            ESP_LOGW(TAG, "⏰ %s: Timeout waiting for resource", task_name);
        }

        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000))); // 2–5s
    }
}

static void resource_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Resource monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int available = uxSemaphoreGetCount(xCountingSemaphore);
        int used = MAX_RESOURCES - available;
        ESP_LOGI(TAG, "\n📊 RESOURCE POOL STATUS");
        ESP_LOGI(TAG, "Available resources: %d/%d", available, MAX_RESOURCES);
        ESP_LOGI(TAG, "Resources in use: %d", used);

        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (resources[i].in_use) {
                ESP_LOGI(TAG, "  Resource %d: BUSY (User: %s, Usage: %lu times)",
                         i+1, resources[i].current_user, resources[i].usage_count);
            } else {
                ESP_LOGI(TAG, "  Resource %d: FREE (Total usage: %lu times)",
                         i+1, resources[i].usage_count);
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
    ESP_LOGI(TAG, "Statistics task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG, "\n📈 SYSTEM STATISTICS");
        ESP_LOGI(TAG, "Total requests: %lu", stats.total_requests);
        ESP_LOGI(TAG, "Successful acquisitions: %lu", stats.successful_acquisitions);
        ESP_LOGI(TAG, "Failed acquisitions: %lu", stats.failed_acquisitions);
        ESP_LOGI(TAG, "Current resources in use: %lu", stats.resources_in_use);
        if (stats.total_requests) {
            float success_rate = (float)stats.successful_acquisitions * 100.0f / (float)stats.total_requests;
            ESP_LOGI(TAG, "Success rate: %.1f%%", success_rate);
        }

        uint32_t total_uses = 0, total_time = 0;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            total_uses += resources[i].usage_count;
            total_time += resources[i].total_usage_time_ms;
            ESP_LOGI(TAG, "  Resource %d: %lu uses, %lums total",
                     i+1, resources[i].usage_count, resources[i].total_usage_time_ms);
        }
        ESP_LOGI(TAG, "Total usage events: %lu, Total time: %lums", total_uses, total_time);
        ESP_LOGI(TAG, "────────────────────────────\n");
    }
}

static void load_generator_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Load generator started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGW(TAG, "🚀 LOAD GENERATOR: Burst start");
        gpio_set_level(LED_SYSTEM, 1);

        for (int round = 0; round < 3; round++) {
            ESP_LOGI(TAG, "  Burst %d/3", round + 1);
            for (int i = 0; i < MAX_RESOURCES + 2; i++) {
                if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                    int idx = acquire_resource("LoadGen");
                    if (idx >= 0) {
                        vTaskDelay(pdMS_TO_TICKS(400));
                        release_resource(idx, 400);
                    }
                    xSemaphoreGive(xCountingSemaphore);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            vTaskDelay(pdMS_TO_TICKS(800));
        }

        gpio_set_level(LED_SYSTEM, 0);
        ESP_LOGI(TAG, "LOAD GENERATOR: Burst done\n");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Counting Semaphores Lab – Experiment 2 (5 Resources)");

    // Init resource table
    for (int i = 0; i < MAX_RESOURCES; i++) {
        resources[i].resource_id = i + 1;
        resources[i].in_use = false;
        resources[i].current_user[0] = '\0';
        resources[i].usage_count = 0;
        resources[i].total_usage_time_ms = 0;
    }

    // Configure LEDs
    for (int i = 0; i < MAX_RESOURCES; i++) {
        gpio_set_direction(LED_RESOURCE_PINS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(LED_RESOURCE_PINS[i], 0);
    }
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PRODUCER, 0);
    gpio_set_level(LED_SYSTEM, 0);

    // Counting semaphore with initial count = MAX_RESOURCES
    xCountingSemaphore = xSemaphoreCreateCounting(MAX_RESOURCES, MAX_RESOURCES);
    if (xCountingSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create counting semaphore!");
        return;
    }

    ESP_LOGI(TAG, "Semaphore created (max count: %d)", MAX_RESOURCES);

    // Producer IDs must be static
    static int producer_ids[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) producer_ids[i] = i + 1;

    // Create producers
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        char name[20]; snprintf(name, sizeof(name), "Producer%d", i+1);
        xTaskCreate(producer_task, name, PRODUCER_STACK, &producer_ids[i], 3, NULL);
    }

    // Create monitors
    xTaskCreate(resource_monitor_task, "ResMonitor", MONITOR_STACK, NULL, 2, NULL);
    xTaskCreate(statistics_task,      "Statistics",  STAT_STACK,  NULL, 1, NULL);
    xTaskCreate(load_generator_task,  "LoadGen",     LOADGEN_STACK, NULL, 4, NULL);

    // Startup LED sweep
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < MAX_RESOURCES; i++) {
            gpio_set_level(LED_RESOURCE_PINS[i], 1);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        gpio_set_level(LED_PRODUCER, 1);
        gpio_set_level(LED_SYSTEM, 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        for (int i = 0; i < MAX_RESOURCES; i++) gpio_set_level(LED_RESOURCE_PINS[i], 0);
        gpio_set_level(LED_PRODUCER, 0);
        gpio_set_level(LED_SYSTEM, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "System operational: Resources=%d, Producers=%d", MAX_RESOURCES, NUM_PRODUCERS);
}
