#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "NO_MUTEX_LAB";

// LED pins for different tasks
#define LED_TASK1 GPIO_NUM_2
#define LED_TASK2 GPIO_NUM_4
#define LED_TASK3 GPIO_NUM_5
#define LED_CRITICAL GPIO_NUM_18

// Shared resources (ไม่มี Mutex ป้องกัน)
typedef struct {
    uint32_t counter;
    char shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

shared_resource_t shared_data = {0, "", 0, 0};

typedef struct {
    uint32_t access_total;
    uint32_t corruption_detected;
} access_stats_t;

access_stats_t stats = {0, 0};

// Function to calculate simple checksum
uint32_t calculate_checksum(const char* data, uint32_t counter) {
    uint32_t sum = counter;
    for (int i = 0; data[i] != '\0'; i++) {
        sum += (uint32_t)data[i] * (i + 1);
    }
    return sum;
}

// Critical section (ไม่มี Mutex)
void access_shared_resource(const char* task_name, gpio_num_t led_pin) {
    char temp_buffer[100];
    uint32_t temp_counter;
    uint32_t expected_checksum;

    gpio_set_level(led_pin, 1);
    gpio_set_level(LED_CRITICAL, 1);

    // === CRITICAL SECTION (ไม่ปลอดภัย) ===
    temp_counter = shared_data.counter;
    strcpy(temp_buffer, shared_data.shared_buffer);
    expected_checksum = shared_data.checksum;

    uint32_t calc = calculate_checksum(temp_buffer, temp_counter);
    if (calc != expected_checksum && shared_data.access_count > 0) {
        ESP_LOGE(TAG, "[%s] ⚠️ DATA CORRUPTION DETECTED!", task_name);
        stats.corruption_detected++;
    }

    ESP_LOGI(TAG, "[%s] Current counter=%lu buffer='%s'",
             task_name, (unsigned long)temp_counter, temp_buffer);

    // หน่วงเวลาเพื่อเปิดโอกาสให้ Task อื่นแทรก
    vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 400)));

    // เขียนข้อมูลใหม่โดยไม่ล็อก
    shared_data.counter = temp_counter + 1;
    snprintf(shared_data.shared_buffer, sizeof(shared_data.shared_buffer),
             "Modified by %s #%lu", task_name, (unsigned long)shared_data.counter);
    shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count++;
    stats.access_total++;

    ESP_LOGI(TAG, "[%s] Modified counter=%lu buffer='%s'",
             task_name, (unsigned long)shared_data.counter, shared_data.shared_buffer);

    gpio_set_level(led_pin, 0);
    gpio_set_level(LED_CRITICAL, 0);
}

// Tasks ---------------------------------------------------------
void high_priority_task(void *pv) {
    ESP_LOGI(TAG, "High Priority Task started");
    while (1) {
        access_shared_resource("HIGH_PRI", LED_TASK1);
        vTaskDelay(pdMS_TO_TICKS(1500 + (esp_random() % 1000)));
    }
}

void medium_priority_task(void *pv) {
    ESP_LOGI(TAG, "Medium Priority Task started");
    while (1) {
        access_shared_resource("MED_PRI", LED_TASK2);
        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 800)));
    }
}

void low_priority_task(void *pv) {
    ESP_LOGI(TAG, "Low Priority Task started");
    while (1) {
        access_shared_resource("LOW_PRI", LED_TASK3);
        vTaskDelay(pdMS_TO_TICKS(800 + (esp_random() % 500)));
    }
}

void monitor_task(void *pv) {
    ESP_LOGI(TAG, "System monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t chk = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        if (chk != shared_data.checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "⚠️ CURRENT DATA CORRUPTION DETECTED!");
            stats.corruption_detected++;
        }
        ESP_LOGI(TAG, "\n═══ NO MUTEX MONITOR ═══");
        ESP_LOGI(TAG, "Counter=%lu  Access=%lu  Corrupted=%lu",
                 (unsigned long)shared_data.counter,
                 (unsigned long)stats.access_total,
                 (unsigned long)stats.corruption_detected);
        ESP_LOGI(TAG, "Buffer='%s'", shared_data.shared_buffer);
        ESP_LOGI(TAG, "═════════════════════════\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Experiment 2: NO MUTEX (Race Condition Demo)");

    // GPIO setup
    gpio_set_direction(LED_TASK1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CRITICAL, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_TASK1, 0);
    gpio_set_level(LED_TASK2, 0);
    gpio_set_level(LED_TASK3, 0);
    gpio_set_level(LED_CRITICAL, 0);

    // init shared resource
    shared_data.counter = 0;
    strcpy(shared_data.shared_buffer, "Initial state");
    shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count = 0;

    // สร้าง Tasks
    xTaskCreate(high_priority_task, "HighPri", 3072, NULL, 5, NULL);
    xTaskCreate(medium_priority_task, "MedPri", 3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task, "LowPri", 3072, NULL, 2, NULL);
    xTaskCreate(monitor_task, "Monitor", 3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "System running — Watch for ⚠️ DATA CORRUPTION messages!");
}
