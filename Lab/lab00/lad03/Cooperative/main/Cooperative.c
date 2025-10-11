#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN    GPIO_NUM_2   // Task 1 indicator
#define LED2_PIN    GPIO_NUM_4   // Task 2 indicator
#define LED3_PIN    GPIO_NUM_5   // Emergency indicator
#define BUTTON_PIN  GPIO_NUM_0   // Emergency button (active-low, uses pull-up)

static const char *TAG = "COOPERATIVE";

// Global variables for cooperative scheduling
static volatile bool emergency_flag = false;
static uint64_t task_start_time = 0;
static uint32_t max_response_time = 0;

// Task structure
typedef struct {
    void (*task_function)(void);
    const char* name;
    bool ready;
} coop_task_t;

// Cooperative task implementations
static void cooperative_task1(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG, "Coop Task1 running: %lu", (unsigned long)count++);

    gpio_set_level(LED1_PIN, 1);

    // Simulate work with voluntary yielding
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 50000; j++) {
            volatile int dummy = j * 2;
            (void)dummy;
        }

        if (emergency_flag) {
            ESP_LOGW(TAG, "Task1 yielding for emergency");
            gpio_set_level(LED1_PIN, 0);
            return; // Yield immediately
        }

        vTaskDelay(1); // minimal yield
    }

    gpio_set_level(LED1_PIN, 0);
}

static void cooperative_task2(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG, "Coop Task2 running: %lu", (unsigned long)count++);

    gpio_set_level(LED2_PIN, 1);

    // Simulate longer work with yielding
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 30000; j++) {
            volatile int dummy = j + i;
            (void)dummy;
        }

        if (emergency_flag) {
            ESP_LOGW(TAG, "Task2 yielding for emergency");
            gpio_set_level(LED2_PIN, 0);
            return;
        }

        vTaskDelay(1);
    }

    gpio_set_level(LED2_PIN, 0);
}

static void cooperative_task3_emergency(void)
{
    if (emergency_flag) {
        uint64_t response_time = esp_timer_get_time() - task_start_time;
        uint32_t response_ms = (uint32_t)(response_time / 1000);

        if (response_ms > max_response_time) {
            max_response_time = response_ms;
        }

        ESP_LOGW(TAG, "EMERGENCY RESPONSE! Response: %u ms (Max: %u ms)",
                 response_ms, max_response_time);

        gpio_set_level(LED3_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED3_PIN, 0);

        emergency_flag = false;
    }
}

// Simple cooperative scheduler (round-robin)
static void cooperative_scheduler(void)
{
    coop_task_t tasks[] = {
        {cooperative_task1, "Task1", true},
        {cooperative_task2, "Task2", true},
        {cooperative_task3_emergency, "Emergency", true}
    };

    const int num_tasks = sizeof(tasks) / sizeof(tasks[0]);
    int current_task = 0;

    while (1) {
        // Check for emergency button (active-low)
        if (gpio_get_level(BUTTON_PIN) == 0 && !emergency_flag) {
            emergency_flag = true;
            task_start_time = esp_timer_get_time();
            ESP_LOGW(TAG, "Emergency button pressed!");
        }

        // Run current task if ready
        if (tasks[current_task].ready) {
            tasks[current_task].task_function();
        }

        // Next task
        current_task = (current_task + 1) % num_tasks;

        // Small delay to be friendly with WDT
        vTaskDelay(pdMS_TO_TICKS(10));
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

    ESP_LOGI(TAG, "=== Cooperative Multitasking Demo ===");
    ESP_LOGI(TAG, "Tasks will yield voluntarily. Press BUTTON to test emergency response.");

    cooperative_scheduler(); // never returns
}
