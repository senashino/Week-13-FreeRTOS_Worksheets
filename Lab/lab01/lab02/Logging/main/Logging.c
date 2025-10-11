#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"   // v5.x ต้อง include เอง
#include "esp_flash.h"       // API ขนาดแฟลชรุ่นใหม่
#include "esp_timer.h"

static const char *TAG = "LOGGING_DEMO";

/* ---------------- Demo ฟังก์ชันหลัก ---------------- */
void demonstrate_logging_levels(void)
{
    ESP_LOGE(TAG, "This is an ERROR message - highest priority");
    ESP_LOGW(TAG, "This is a WARNING message");
    ESP_LOGI(TAG, "This is an INFO message - default level");
    ESP_LOGD(TAG, "This is a DEBUG message - needs debug level");
    ESP_LOGV(TAG, "This is a VERBOSE message - needs verbose level");
}

void demonstrate_formatted_logging(void)
{
    int   temperature = 25;
    float voltage     = 3.3f;
    const char* status = "OK";

    ESP_LOGI(TAG, "Sensor readings:");
    ESP_LOGI(TAG, "  Temperature: %d°C", temperature);
    ESP_LOGI(TAG, "  Voltage: %.2fV", voltage);
    ESP_LOGI(TAG, "  Status: %s", status);

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ESP_LOGI(TAG, "Data dump:");
    ESP_LOG_BUFFER_HEX(TAG, data, sizeof(data));
}

void demonstrate_conditional_logging(void)
{
    int error_code = 0;

    if (error_code != 0) {
        ESP_LOGE(TAG, "Error occurred: code %d", error_code);
    } else {
        ESP_LOGI(TAG, "System is running normally");
    }

    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");
}

/* ---------------- แบบฝึกหัด ---------------- */
/* รีเนม macro กันชนกับ esp_log_color.h */
#define CLOG_COLOR_CYAN   "36"
#define CLOG_BOLD(COLOR)  "\033[1;" COLOR "m"
#define CLOG_RESET_COLOR  "\033[0m"

void custom_log(const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf(CLOG_BOLD(CLOG_COLOR_CYAN) "[CUSTOM] %s: " CLOG_RESET_COLOR, tag);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void performance_demo(void)
{
    ESP_LOGI(TAG, "=== Performance Monitoring ===");
    uint64_t start_time = esp_timer_get_time();
    for (int i = 0; i < 1000000; i++) {
        volatile int dummy = i * 2;
        (void)dummy;
    }
    uint64_t execution_time = esp_timer_get_time() - start_time;
    ESP_LOGI(TAG, "Execution time: %llu microseconds", (unsigned long long)execution_time);
    ESP_LOGI(TAG, "Execution time: %.2f milliseconds", execution_time / 1000.0);
}

void error_handling_demo(void)
{
    ESP_LOGI(TAG, "=== Error Handling Demo ===");
    esp_err_t result = ESP_OK;
    if (result == ESP_OK) ESP_LOGI(TAG, "Operation completed successfully");

    result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) ESP_LOGE(TAG, "Error: %s", esp_err_to_name(result));

    result = ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK_WITHOUT_ABORT(result);
    if (result != ESP_OK) ESP_LOGW(TAG, "Non-fatal error: %s", esp_err_to_name(result));
}
/* -------------- จบแบบฝึกหัด ---------------- */

void app_main(void)
{
    // ตั้งระดับ log ตอน runtime
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("LOGGING_DEMO", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "=== ESP32 Hello World Demo ===");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip Model: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "Free Heap: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Min Free Heap: %d bytes", esp_get_minimum_free_heap_size());

    // ข้อมูลชิป/แฟลช (API ใหม่)
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Flash size: %uMB %s",
             (unsigned)(flash_size / (1024 * 1024)),
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    // Demo
    ESP_LOGI(TAG, "\n--- Logging Levels Demo ---");
    demonstrate_logging_levels();

    ESP_LOGI(TAG, "\n--- Formatted Logging Demo ---");
    demonstrate_formatted_logging();

    ESP_LOGI(TAG, "\n--- Conditional Logging Demo ---");
    demonstrate_conditional_logging();

    // Exercises
    custom_log("SENSOR", "Temperature: %d°C", 25);
    performance_demo();
    error_handling_demo();

    int counter = 0;
    while (1) {
        ESP_LOGI(TAG, "Main loop iteration: %d", counter++);
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "Memory status - Free: %d bytes", esp_get_free_heap_size());
        }
        if (counter % 20 == 0) {
            ESP_LOGW(TAG, "Warning: Counter reached %d", counter);
        }
        if (counter > 50) {
            ESP_LOGE(TAG, "Error simulation: Counter exceeded 50!");
            counter = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
