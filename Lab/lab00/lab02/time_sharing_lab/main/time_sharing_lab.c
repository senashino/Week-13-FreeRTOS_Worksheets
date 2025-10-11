// main.c — Lab 2: Time-Sharing (Single file)
// เลือกส่วนที่จะรัน: 1 = Part 1, 2 = Part 2 (Variable time slices), 3 = Part 3 (Problem demo)
#define RUN_PART 2

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN GPIO_NUM_2
#define LED2_PIN GPIO_NUM_4
#define LED3_PIN GPIO_NUM_5
#define LED4_PIN GPIO_NUM_18

static const char *TAG = "TIME_SHARING";

/* ==============================
 * Common: Tasks & Globals
 * ============================== */

typedef enum {
    TASK_SENSOR,
    TASK_PROCESS,
    TASK_ACTUATOR,
    TASK_DISPLAY,
    TASK_COUNT
} task_id_t;

// ใช้เฉพาะ Part 1
#define TIME_SLICE_MS 50

static uint32_t task_counter = 0;
static uint64_t context_switch_time = 0;   // รวมเวลาที่ใช้ทำงานใน manual_scheduler()
static uint32_t context_switches = 0;

/* ---------------- Simulated workloads ---------------- */
static void simulate_sensor_task(void)
{
    static uint32_t sensor_count = 0;
    ESP_LOGI(TAG, "Sensor Task %u", (unsigned)sensor_count++);

    gpio_set_level(LED1_PIN, 1);
    for (int i = 0; i < 10000; i++) { volatile int dummy = i; (void)dummy; }
    gpio_set_level(LED1_PIN, 0);
}

static void simulate_processing_task(void)
{
    static uint32_t process_count = 0;
    ESP_LOGI(TAG, "Processing Task %u", (unsigned)process_count++);

    gpio_set_level(LED2_PIN, 1);
    for (int i = 0; i < 100000; i++) { volatile int dummy = i * i; (void)dummy; }
    gpio_set_level(LED2_PIN, 0);
}

static void simulate_actuator_task(void)
{
    static uint32_t actuator_count = 0;
    ESP_LOGI(TAG, "Actuator Task %u", (unsigned)actuator_count++);

    gpio_set_level(LED3_PIN, 1);
    for (int i = 0; i < 50000; i++) { volatile int dummy = i + 100; (void)dummy; }
    gpio_set_level(LED3_PIN, 0);
}

static void simulate_display_task(void)
{
    static uint32_t display_count = 0;
    ESP_LOGI(TAG, "Display Task %u", (unsigned)display_count++);

    gpio_set_level(LED4_PIN, 1);
    for (int i = 0; i < 20000; i++) { volatile int dummy = i / 2; (void)dummy; }
    gpio_set_level(LED4_PIN, 0);
}

/* ---------------- Manual scheduler (ใช้ร่วมกันทุก Part) ---------------- */
static void manual_scheduler(void)
{
    uint64_t start_time = esp_timer_get_time();

    // บันทึกการสลับ (จำลอง)
    context_switches++;

    // Overhead ตอนสลับ context (จำลอง)
    for (int i = 0; i < 1000; i++) { volatile int dummy = i; (void)dummy; }

    // เลือกงานตามรอบ
    switch (task_counter % TASK_COUNT) {
        case TASK_SENSOR:   simulate_sensor_task();     break;
        case TASK_PROCESS:  simulate_processing_task(); break;
        case TASK_ACTUATOR: simulate_actuator_task();   break;
        case TASK_DISPLAY:  simulate_display_task();    break;
    }

    // Overhead หลังสลับ (จำลอง)
    for (int i = 0; i < 1000; i++) { volatile int dummy = i; (void)dummy; }

    uint64_t end_time = esp_timer_get_time();
    context_switch_time += (end_time - start_time);

    task_counter++;
}

/* ==============================
 * Part 1: Simple Time-Sharing
 * ============================== */
static void run_part1_simple_timesharing(void)
{
    ESP_LOGI(TAG, "Part 1: Simple Time-Sharing (TIME_SLICE_MS=%d)", TIME_SLICE_MS);

    uint64_t start_time = esp_timer_get_time();
    uint32_t round_count = 0;

    while (1) {
        manual_scheduler();
        vTaskDelay(pdMS_TO_TICKS(TIME_SLICE_MS));

        // รายงานทุก 20 context switches
        if (context_switches % 20 == 0) {
            round_count++;
            uint64_t current_time = esp_timer_get_time();
            uint64_t total_time   = current_time - start_time;

            float cpu_utilization     = (total_time > 0) ? ((float)context_switch_time / (float)total_time) * 100.0f : 0.0f;
            float overhead_percentage = 100.0f - cpu_utilization;

            ESP_LOGI(TAG, "=== Round %u Statistics ===", (unsigned)round_count);
            ESP_LOGI(TAG, "Context switches: %u", (unsigned)context_switches);
            ESP_LOGI(TAG, "Total time: %llu us", (unsigned long long)total_time);
            ESP_LOGI(TAG, "Task execution time: %llu us", (unsigned long long)context_switch_time);
            ESP_LOGI(TAG, "CPU utilization: %.1f%%", cpu_utilization);
            ESP_LOGI(TAG, "Overhead: %.1f%%", overhead_percentage);
            ESP_LOGI(TAG, "Avg time per task: %llu us",
                     (unsigned long long)(context_switches ? (context_switch_time / context_switches) : 0ULL));
        }
    }
}

/* ===========================================
 * Part 2: Time-Sharing with Variable Workloads
 * =========================================== */
static void variable_time_slice_experiment(void)
{
    ESP_LOGI(TAG, "=== Variable Time Slice Experiment ===");

    const uint32_t time_slices[] = {10, 25, 50, 100, 200};
    const int num_slices = sizeof(time_slices) / sizeof(time_slices[0]);

    for (int i = 0; i < num_slices; i++) {
        uint32_t slice_ms = time_slices[i];

        // รีเซ็ตตัวนับสำหรับรอบนี้
        context_switches    = 0;
        context_switch_time = 0;
        task_counter        = 0;

        ESP_LOGI(TAG, "Testing time slice: %u ms", slice_ms);

        uint64_t t_start = esp_timer_get_time();

        // รัน 50 ครั้ง (โดยเว้นระยะตาม slice)
        for (int j = 0; j < 50; j++) {
            manual_scheduler();
            vTaskDelay(pdMS_TO_TICKS(slice_ms));
        }

        uint64_t t_end       = esp_timer_get_time();
        uint64_t test_dur_us = t_end - t_start;

        float efficiency = (test_dur_us > 0)
                           ? ((float)context_switch_time / (float)test_dur_us) * 100.0f
                           : 0.0f;

        ESP_LOGI(TAG, "Time slice %u ms: Efficiency %.1f%%", slice_ms, efficiency);
        ESP_LOGI(TAG, "Context switches: %u", (unsigned)context_switches);

        // เว้นช่วงระหว่างชุดทดสอบให้เห็นชัด
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "=== End Variable Time Slice Experiment ===");
}

/* ==============================
 * Part 3: Problem Demonstrations
 * ============================== */
static void demonstrate_problems(void)
{
    ESP_LOGI(TAG, "\n=== Demonstrating Time-Sharing Problems ===");

    // Problem 1: Priority Inversion
    ESP_LOGI(TAG, "Problem 1: No priority support");
    ESP_LOGI(TAG, "Critical task must wait for less important tasks");

    // Problem 2: Fixed time slice issues
    ESP_LOGI(TAG, "Problem 2: Fixed time slice problems");
    ESP_LOGI(TAG, "Short tasks waste time, long tasks get interrupted");

    // Problem 3: Context switching overhead
    ESP_LOGI(TAG, "Problem 3: Context switching overhead");
    ESP_LOGI(TAG, "Time wasted in switching between tasks");

    // Problem 4: No inter-task communication
    ESP_LOGI(TAG, "Problem 4: No proper inter-task communication");
    ESP_LOGI(TAG, "Tasks cannot communicate safely");

    ESP_LOGI(TAG, "=== End Problem Demonstrations ===\n");
}

/* ==============================
 * app_main: เลือก part ที่จะรัน
 * ============================== */
void app_main(void)
{
    // ตั้งค่า GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) |
                        (1ULL << LED3_PIN) | (1ULL << LED4_PIN),
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&io_conf);

    // ให้แน่ใจว่า Log ระดับ INFO แสดงผล
    esp_log_level_set("*", ESP_LOG_INFO);

#if RUN_PART == 1
    run_part1_simple_timesharing();

#elif RUN_PART == 2
    variable_time_slice_experiment();
    // กัน watchdog ด้วยการหลับยาวหลังจบ
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }

#elif RUN_PART == 3
    demonstrate_problems();
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }

#else
#   error "Please set RUN_PART to 1, 2, or 3"
#endif
}
