#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ====== CONFIG SWITCHES ======
#define ENABLE_ROUND_ROBIN     1   // Step 2
#define ENABLE_INVERSION_DEMO  1   // Step 3
#define ENABLE_RUNTIME_STATS   1   // พิมพ์ตาราง Run-time stats หลังจบทดสอบ (ต้องเปิดเมนูคอนฟิกด้านล่าง)

// ====== PINS ======
#define LED_HIGH_PIN  GPIO_NUM_2   // High priority
#define LED_MED_PIN   GPIO_NUM_4   // Medium priority
#define LED_LOW_PIN   GPIO_NUM_5   // Low priority
#define BUTTON_PIN    GPIO_NUM_0   // Pull-up บนบอร์ดส่วนใหญ่

static const char *TAG = "PRIORITY_DEMO";

// ====== GLOBAL STATE ======
static volatile bool priority_test_running = false;
static volatile uint32_t high_task_count = 0;
static volatile uint32_t med_task_count  = 0;
static volatile uint32_t low_task_count  = 0;

// สำหรับ Step 2
static TaskHandle_t eq1_h = NULL, eq2_h = NULL, eq3_h = NULL;

// สำหรับ Step 3 (Priority Inversion)
#if ENABLE_INVERSION_DEMO
static SemaphoreHandle_t g_mutex = NULL; // ใช้ "Mutex" เพื่อให้ Priority Inheritance ทำงาน
#endif

// ====== UTILS ======
static inline void led_on(gpio_num_t pin){ gpio_set_level(pin, 1); }
static inline void led_off(gpio_num_t pin){ gpio_set_level(pin, 0); }

// ====== TASKS: Step 1 ======
static void high_priority_task(void *pv) {
    ESP_LOGI(TAG, "High Priority Task started (prio=%d)", uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            high_task_count++;
            led_on(LED_HIGH_PIN);

            // งานสั้นๆ แล้วพัก เพื่อให้เห็น preempt ชัด
            for (volatile int i = 0; i < 60000; i++) { /* burn */ }

            led_off(LED_HIGH_PIN);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void medium_priority_task(void *pv) {
    ESP_LOGI(TAG, "Medium Priority Task started (prio=%d)", uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            med_task_count++;
            led_on(LED_MED_PIN);

            for (volatile int i = 0; i < 120000; i++) { /* burn */ }

            led_off(LED_MED_PIN);
            vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void low_priority_task(void *pv) {
    ESP_LOGI(TAG, "Low Priority Task started (prio=%d)", uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            low_task_count++;
            led_on(LED_LOW_PIN);

            // งานยาว แทรก delay สั้นๆ กัน WDT และให้เกิดการสลับคอนเท็กซ์
            for (int i = 0; i < 600000; i++) {
                volatile int d = i ^ 0x55AA;
                (void)d;
                if ((i % 80000) == 0) {
                    vTaskDelay(1);
                }
            }

            led_off(LED_LOW_PIN);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ====== TASKS: Step 2 (Round-robin on equal priority) ======
#if ENABLE_ROUND_ROBIN
static void equal_priority_task(void *pv) {
    int id = (int)(intptr_t)pv;
    char name[16]; snprintf(name, sizeof(name), "Equal%d", id);
    ESP_LOGI(TAG, "%s started (prio=%d)", name, uxTaskPriorityGet(NULL));
    while (1) {
        if (priority_test_running) {
            // ไม่ yield เอง ปล่อยให้ RTOS time-slice (configTICK_RATE_HZ) จัดการ
            ESP_LOGI(TAG, "[RR] %s running", name);
            for (volatile int i = 0; i < 300000; i++) { /* burn */ }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

// ====== TASKS: Step 3 (Priority Inversion Demo) ======
#if ENABLE_INVERSION_DEMO
// สถานการณ์จำลอง: Low จับ resource นานๆ, High ต้องใช้เหมือนกัน, Medium คอยขัดขวาง CPU
static void inv_high(void *pv) {
    while (1) {
        if (priority_test_running) {
            ESP_LOGW(TAG, "[INV] HIGH needs resource -> take mutex");
            // ถ้า Low ถือ mutex อยู่ High จะบล็อก แต่ "Mutex" จะยก priority ของ Low ให้อัตโนมัติ
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            ESP_LOGI(TAG, "[INV] HIGH got resource");
            vTaskDelay(pdMS_TO_TICKS(80)); // ใช้สั้นๆ
            xSemaphoreGive(g_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}

static void inv_low(void *pv) {
    while (1) {
        if (priority_test_running) {
            ESP_LOGI(TAG, "[INV] LOW taking resource long");
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            // ใช้ทรัพยากรนานพอที่จะเห็น medium แทรก (ถ้าไม่มี inheritance)
            vTaskDelay(pdMS_TO_TICKS(1800));
            xSemaphoreGive(g_mutex);
            ESP_LOGI(TAG, "[INV] LOW released resource");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void inv_medium_noise(void *pv) {
    while (1) {
        if (priority_test_running) {
            // งานรบกวน CPU ให้เห็นภาพว่าถ้าไม่มี inheritance High จะค้างรอ Low นานเพราะ Medium แทรก
            for (int k = 0; k < 4; k++) {
                for (volatile int i = 0; i < 220000; i++) { /* burn */ }
                vTaskDelay(1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
#endif

// ====== CONTROL TASK ======
static void control_task(void *pv) {
    ESP_LOGI(TAG, "Control Task started (hold BUTTON to start test)");

    // ดีบาวซ์ปุ่มและทำ Edge detect
    bool last_level = true, prev_report = true;
    TickType_t press_ts = 0;

    while (1) {
        bool level = gpio_get_level(BUTTON_PIN); // pull-up active low
        if (level == 0 && last_level == 1) {
            press_ts = xTaskGetTickCount();
        }
        // กดค้าง >= 80ms ถือว่า valid press
        if (level == 0 && (xTaskGetTickCount() - press_ts) > pdMS_TO_TICKS(80) && prev_report) {
            prev_report = false;
            // เริ่มการทดสอบ 10 วินาที
            ESP_LOGW(TAG, "=== START PRIORITY TEST (10s) ===");
            high_task_count = med_task_count = low_task_count = 0;
            priority_test_running = true;
            vTaskDelay(pdMS_TO_TICKS(10000));
            priority_test_running = false;

            ESP_LOGW(TAG, "=== RESULTS ===");
            ESP_LOGI(TAG, "High runs:   %" PRIu32, high_task_count);
            ESP_LOGI(TAG, "Medium runs: %" PRIu32, med_task_count);
            ESP_LOGI(TAG, "Low runs:    %" PRIu32, low_task_count);

            uint32_t total = high_task_count + med_task_count + low_task_count;
            if (total) {
                ESP_LOGI(TAG, "High   : %.1f %%", (float)high_task_count * 100.0f / (float)total);
                ESP_LOGI(TAG, "Medium : %.1f %%", (float)med_task_count  * 100.0f / (float)total);
                ESP_LOGI(TAG, "Low    : %.1f %%", (float)low_task_count  * 100.0f / (float)total);
            }

#if ENABLE_RUNTIME_STATS
            // ต้องเปิดคอนฟิกตามหัวข้อด้านล่าง
            char *stats = (char *)heap_caps_malloc(2048, MALLOC_CAP_8BIT);
            if (stats) {
                vTaskGetRunTimeStats(stats);
                ESP_LOGW(TAG, "--- Run-time Stats (%% CPU) ---\n%s", stats);
                heap_caps_free(stats);
            }
#endif
            ESP_LOGW(TAG, "=== END TEST ===");
        }
        if (level == 1) { prev_report = true; }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ====== APP MAIN ======
void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Priority Scheduling Demo (ESP-IDF v5.x) ===");

    // LEDs
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<LED_HIGH_PIN) | (1ULL<<LED_MED_PIN) | (1ULL<<LED_LOW_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    led_off(LED_HIGH_PIN); led_off(LED_MED_PIN); led_off(LED_LOW_PIN);

    // Button (pull-up)
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL<<BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn);

#if ENABLE_INVERSION_DEMO
    g_mutex = xSemaphoreCreateMutex(); // ต้องเป็น "Mutex" (ไม่ใช่ Binary semaphore) เพื่อให้ PI ทำงาน
#endif

    // Step 1 tasks (กำหนดลำดับความสำคัญ)
    xTaskCreate(high_priority_task,  "HighPrio", 3072, NULL, 5, NULL);
    xTaskCreate(medium_priority_task,"MedPrio",  3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task,   "LowPrio",  3072, NULL, 1, NULL);

    // Control
    xTaskCreate(control_task, "Control", 4096, NULL, 4, NULL);

#if ENABLE_ROUND_ROBIN
    // Step 2: equal priority (prio=2)
    xTaskCreate(equal_priority_task, "Equal1", 2048, (void*)1, 2, &eq1_h);
    xTaskCreate(equal_priority_task, "Equal2", 2048, (void*)2, 2, &eq2_h);
    xTaskCreate(equal_priority_task, "Equal3", 2048, (void*)3, 2, &eq3_h);
#endif

#if ENABLE_INVERSION_DEMO
    // Step 3: ตั้งระดับเพื่อให้เกิดสถานการณ์ Inversion แบบคลาสสิก
    xTaskCreate(inv_high,          "INV_H",  3072, NULL, 6, NULL); // สูงกว่า HighPrio
    xTaskCreate(inv_medium_noise,  "INV_M",  3072, NULL, 4, NULL); // คอยแทรก
    xTaskCreate(inv_low,           "INV_L",  3072, NULL, 2, NULL); // ถือ mutex นานๆ
#endif

    ESP_LOGI(TAG, "Press and HOLD the button (GPIO0) to run 10s test.");
    ESP_LOGI(TAG, "LEDs -> GPIO2:High, GPIO4:Med, GPIO5:Low");
}
