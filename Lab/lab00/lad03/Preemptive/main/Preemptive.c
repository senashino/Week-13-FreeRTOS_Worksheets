#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN    GPIO_NUM_2   // Task1 indicator
#define LED2_PIN    GPIO_NUM_4   // Task2 indicator
#define LED3_PIN    GPIO_NUM_5   // Emergency indicator
#define BUTTON_PIN  GPIO_NUM_0   // Emergency button (active-low, pull-up)

static const char *PREEMPT_TAG = "PREEMPTIVE";

static volatile bool preempt_emergency = false;
static uint64_t preempt_start_time = 0;
static uint32_t preempt_max_response = 0;

static void preemptive_task1(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task1: %lu", (unsigned long)count++);

        gpio_set_level(LED1_PIN, 1);

        // Simulate work WITHOUT yielding (scheduler may preempt)
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 50000; j++) {
                volatile int dummy = j * 2;
                (void)dummy;
            }
        }

        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void preemptive_task2(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(PREEMPT_TAG, "Preempt Task2: %lu", (unsigned long)count++);

        gpio_set_level(LED2_PIN, 1);

        // Longer compute, still preemptable by higher-priority task
        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 30000; j++) {
                volatile int dummy = j + i;
                (void)dummy;
            }
        }

        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void preemptive_emergency_task(void *pvParameters)
{
    while (1) {
        // Poll button every 5 ms (active-low)
        if (gpio_get_level(BUTTON_PIN) == 0 && !preempt_emergency) {
            preempt_emergency = true;
            preempt_start_time = esp_timer_get_time();

            // This high-priority task is already running; measure response
            uint64_t response_time = esp_timer_get_time() - preempt_start_time;
            uint32_t response_ms = (uint32_t)(response_time / 1000);

            if (response_ms > preempt_max_response) {
                preempt_max_response = response_ms;
            }

            ESP_LOGW(PREEMPT_TAG, "IMMEDIATE EMERGENCY! Response: %u ms (Max: %u ms)",
                     response_ms, preempt_max_response);

            gpio_set_level(LED3_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED3_PIN, 0);

            preempt_emergency = false;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Button configuration (input + pull-up)
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    ESP_LOGI(PREEMPT_TAG, "=== Preemptive Multitasking Demo ===");
    ESP_LOGI(PREEMPT_TAG, "RTOS will preempt tasks automatically. Press BUTTON to test emergency response.");

    // Create tasks with different priorities
    // Priority guideline: higher number = higher priority
    xTaskCreate(preemptive_task1,           "PreTask1",  4096, NULL, 2, NULL); // Normal
    xTaskCreate(preemptive_task2,           "PreTask2",  4096, NULL, 1, NULL); // Low
    xTaskCreate(preemptive_emergency_task,  "Emergency", 4096, NULL, 5, NULL); // High

    // app_main can return; scheduler keeps tasks running
}
