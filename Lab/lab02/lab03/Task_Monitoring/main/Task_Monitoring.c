#include <stdio.h>
#include <string.h>
#include <stdlib.h>             // malloc/free

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"      // esp_get_free_heap_size()
#include "esp_rom_sys.h"        // esp_rom_printf(), esp_rom_delay_us
#include "freertos/portmacro.h" // portDISABLE_INTERRUPTS()

#define LED_OK GPIO_NUM_2       // Stack OK indicator
#define LED_WARNING GPIO_NUM_4  // Stack warning indicator

static const char *TAG = "STACK_MONITOR";

// Stack monitoring configuration
#define STACK_WARNING_THRESHOLD 512  // bytes
#define STACK_CRITICAL_THRESHOLD 256 // bytes

// Task handles for monitoring
TaskHandle_t light_task_handle = NULL;
TaskHandle_t medium_task_handle = NULL;
TaskHandle_t heavy_task_handle = NULL;

// ---------------- Stack monitor task ----------------
void stack_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack Monitor Task started");

    while (1) {
        ESP_LOGI(TAG, "\n=== STACK USAGE REPORT ===");

        TaskHandle_t tasks[] = {
            light_task_handle,
            medium_task_handle,
            heavy_task_handle,
            xTaskGetCurrentTaskHandle() // Monitor itself
        };

        const char* task_names[] = {
            "LightTask",
            "MediumTask",
            "HeavyTask",
            "StackMonitor"
        };

        bool stack_warning = false;
        bool stack_critical = false;

        for (int i = 0; i < 4; i++) {
            if (tasks[i] != NULL) {
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(tasks[i]);
                uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);

                ESP_LOGI(TAG, "%s: %lu bytes remaining", task_names[i], (unsigned long)stack_bytes);

                if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
                    ESP_LOGE(TAG, "CRITICAL: %s stack very low!", task_names[i]);
                    stack_critical = true;
                } else if (stack_bytes < STACK_WARNING_THRESHOLD) {
                    ESP_LOGW(TAG, "WARNING: %s stack low", task_names[i]);
                    stack_warning = true;
                }
            }
        }

        // Update LED indicators
        if (stack_critical) {
            for (int i = 0; i < 10; i++) {
                gpio_set_level(LED_WARNING, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_WARNING, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            gpio_set_level(LED_OK, 0);
        } else if (stack_warning) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }

        ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min  heap: %u bytes", (unsigned)esp_get_minimum_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(3000)); // Monitor every 3 seconds
    }
}

// ---------------- Light task (low stack) ----------------
void light_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    int counter = 0;

    while (1) {
        counter++;
        ESP_LOGI(TAG, "Light task cycle: %d", counter);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Light task stack: %lu bytes", (unsigned long)(stack_remaining * sizeof(StackType_t)));

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------------- Medium task (moderate stack) ----------------
void medium_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");

    while (1) {
        char buffer[256];
        int numbers[50];

        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';

        for (int i = 0; i < 50; i++) {
            numbers[i] = i * i;
        }

        ESP_LOGI(TAG, "Medium task: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Medium task stack: %lu bytes", (unsigned long)(stack_remaining * sizeof(StackType_t)));

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------------- Heavy task (high stack use) ----------------
void heavy_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Heavy Stack Task started (high usage - watch for overflow!)");
    int cycle = 0;

    while (1) {
        cycle++;

        // ใหญ่พอจะกดดัน stack
        char large_buffer[1024];
        int  large_numbers[200];
        char another_buffer[512];

        ESP_LOGW(TAG, "Heavy task cycle %d: Using large stack arrays", cycle);

        memset(large_buffer, 'X', sizeof(large_buffer) - 1);
        large_buffer[sizeof(large_buffer) - 1] = '\0';

        for (int i = 0; i < 200; i++) {
            large_numbers[i] = i * cycle;
        }

        snprintf(another_buffer, sizeof(another_buffer), "Cycle %d with large data processing", cycle);

        ESP_LOGI(TAG, "Heavy task: %s", another_buffer);
        ESP_LOGI(TAG, "Large buffer length: %d", (int)strlen(large_buffer));
        ESP_LOGI(TAG, "Last number: %d", large_numbers[199]);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);

        if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
            ESP_LOGE(TAG, "DANGER: Heavy task stack critically low: %lu bytes!", (unsigned long)stack_bytes);
        } else {
            ESP_LOGW(TAG, "Heavy task stack: %lu bytes remaining", (unsigned long)stack_bytes);
        }

        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

// ---------------- Recursion demo ----------------
static void recursive_function(int depth)
{
    char local_array[100];
    snprintf(local_array, sizeof(local_array), "Recursion depth: %d", depth);
    ESP_LOGI(TAG, "%s", local_array);

    UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
    uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
    ESP_LOGI(TAG, "Depth %d: Stack remaining: %lu bytes", depth, (unsigned long)stack_bytes);

    if (stack_bytes < 200) {
        ESP_LOGE(TAG, "Stopping recursion at depth %d - stack too low!", depth);
        return;
    }
    if (depth < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        recursive_function(depth + 1);
    }
}

void recursion_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Recursion Demo Task started");
    while (1) {
        ESP_LOGW(TAG, "=== STARTING RECURSION DEMO ===");
        recursive_function(1);
        ESP_LOGW(TAG, "=== RECURSION DEMO COMPLETED ===");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ---------------- Optimized heavy task (use heap) ----------------
void optimized_heavy_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Optimized Heavy Task started");

    char *large_buffer   = (char *)malloc(1024);
    int  *large_numbers  = (int  *)malloc(200 * sizeof(int));
    char *another_buffer = (char *)malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        // ใช้ ROM printf แทน LOG เพื่อความปลอดภัย
        esp_rom_printf("Failed to allocate heap memory\n");
        free(large_buffer);
        free(large_numbers);
        free(another_buffer);
        vTaskDelete(NULL);
        return;
    }

    int cycle = 0;
    while (1) {
        cycle++;
        ESP_LOGI(TAG, "Optimized task cycle %d: Using heap instead of stack", cycle);

        memset(large_buffer, 'Y', 1023);
        large_buffer[1023] = '\0';
        for (int i = 0; i < 200; i++) {
            large_numbers[i] = i * cycle;
        }
        snprintf(another_buffer, 512, "Optimized cycle %d", cycle);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Optimized task stack: %lu bytes remaining",
                 (unsigned long)(stack_remaining * sizeof(StackType_t)));

        vTaskDelay(pdMS_TO_TICKS(4000));
    }

    // (จะไม่ถึงจุดนี้)
    free(large_buffer);
    free(large_numbers);
    free(another_buffer);
}

// ---------------- Stack overflow hook (no RTOS API inside) ----------------
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    // ใช้ฟังก์ชัน ROM เพื่อลดการพึ่งพา RTOS/heap
    esp_rom_printf("STACK_OVERFLOW: %s\n", pcTaskName);

    portDISABLE_INTERRUPTS();

    // กระพริบ LED WARNING แบบ busy-wait
    for (int i = 0; i < 20; i++) {
        gpio_set_level(LED_WARNING, 1);
        esp_rom_delay_us(50000);  // 50 ms
        gpio_set_level(LED_WARNING, 0);
        esp_rom_delay_us(50000);  // 50 ms
    }
    esp_restart();
}

// ---------------- app_main ----------------
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Stack Monitoring Demo ===");

    // GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "LED Indicators: GPIO2=OK, GPIO4=WARNING");

    BaseType_t result;

    // Light task - 1KB
    result = xTaskCreate(light_stack_task, "LightTask", 1024, NULL, 2, &light_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create LightTask");

    // Medium task - 2KB
    result = xTaskCreate(medium_stack_task, "MediumTask", 2048, NULL, 2, &medium_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create MediumTask");

    // Heavy task - 4KB (เพิ่มเพื่อเลี่ยง crash ทันที แต่ยังเห็น WARNING/CRITICAL)
    result = xTaskCreate(heavy_stack_task, "HeavyTask", 4096, NULL, 2, &heavy_task_handle);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create HeavyTask");

    // ถ้าต้องการสาธิตแบบ “ปลอดภัยสุด” ใช้เวอร์ชัน heap แทน (สลับคอมเมนต์สองบรรทัดนี้)
    // xTaskCreate(optimized_heavy_task, "HeavyTaskOpt", 2048, NULL, 2, NULL);

    // Recursion demo - 3KB
    result = xTaskCreate(recursion_demo_task, "RecursionDemo", 3072, NULL, 1, NULL);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create RecursionDemo");

    // Stack monitor - 4KB, priority สูงกว่า
    result = xTaskCreate(stack_monitor_task, "StackMonitor", 4096, NULL, 3, NULL);
    if (result != pdPASS) ESP_LOGE(TAG, "Failed to create StackMonitor");

    ESP_LOGI(TAG, "All tasks created. Monitor reports every 3 seconds.");
}
