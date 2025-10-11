// main.c — FreeRTOS Task States Demonstration for ESP32
// Covers: Step 1 (Basic), Step 2 (Advanced transitions), Step 3 (Monitoring)
// + Exercise 1 (State change counter) + Exercise 2 (Custom LED state display)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

// ===================== GPIO Mapping =====================
#define LED_RUNNING    GPIO_NUM_2     // แสดง Running state
#define LED_READY      GPIO_NUM_4     // แสดง Ready state
#define LED_BLOCKED    GPIO_NUM_5     // แสดง Blocked state
#define LED_SUSPENDED  GPIO_NUM_18    // แสดง Suspended state

#define BUTTON1_PIN    GPIO_NUM_0     // Suspend/Resume
// ถ้าไม่มี pull-up ภายนอก ให้เปลี่ยนเป็น GPIO_NUM_15 จะสะดวกกว่า
#define BUTTON2_PIN    GPIO_NUM_35    // Give semaphore (Input-only, ต้องมี pull-up ภายนอก)

// ===================== Logging Tag ======================
static const char *TAG = "TASK_STATES";

// ===================== Task Handles =====================
TaskHandle_t state_demo_task_handle   = NULL;
TaskHandle_t ready_demo_task_handle   = NULL;
TaskHandle_t control_task_handle      = NULL;
TaskHandle_t monitor_task_handle      = NULL;
TaskHandle_t external_delete_handle   = NULL;

// ===================== Sync Objects =====================
SemaphoreHandle_t demo_semaphore = NULL;

// ===================== State Names ======================
const char* state_names[] = {
    "Running",   // 0 eRunning
    "Ready",     // 1 eReady
    "Blocked",   // 2 eBlocked
    "Suspended", // 3 eSuspended
    "Deleted",   // 4 eDeleted
    "Invalid"    // 5
};

static inline const char* get_state_name(eTaskState s) {
    if (s <= eDeleted) return state_names[s];
    return state_names[5];
}

// ===================== Exercise 1: State Change Counter =====================
// นับจำนวนครั้งที่ task เปลี่ยน state (จำลองนับในจุดที่เราคุม transition)
volatile uint32_t state_changes[5] = {0}; // index ตาม eTaskState
static eTaskState g_last_state = eInvalid;

static void count_state_change(eTaskState old_state, eTaskState new_state) {
    if (old_state != new_state && new_state <= eDeleted) {
        state_changes[new_state]++;
        ESP_LOGI(TAG, "State change: %s -> %s (Count[%s]=%lu)",
                 get_state_name(old_state),
                 get_state_name(new_state),
                 get_state_name(new_state),
                 (unsigned long)state_changes[new_state]);
        g_last_state = new_state;
    }
}

// ===================== Exercise 2: Custom LED Indicator =====================
static void update_state_display(eTaskState current_state) {
    // ปิดทั้งหมดก่อน
    gpio_set_level(LED_RUNNING,   0);
    gpio_set_level(LED_READY,     0);
    gpio_set_level(LED_BLOCKED,   0);
    gpio_set_level(LED_SUSPENDED, 0);

    switch (current_state) {
        case eRunning:   gpio_set_level(LED_RUNNING,   1); break;
        case eReady:     gpio_set_level(LED_READY,     1); break;
        case eBlocked:   gpio_set_level(LED_BLOCKED,   1); break;
        case eSuspended: gpio_set_level(LED_SUSPENDED, 1); break;
        default:
            // กะพริบทั้งหมด 3 ครั้งสำหรับ state ที่ไม่รู้จัก
            for (int i = 0; i < 3; i++) {
                gpio_set_level(LED_RUNNING,   1);
                gpio_set_level(LED_READY,     1);
                gpio_set_level(LED_BLOCKED,   1);
                gpio_set_level(LED_SUSPENDED, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_RUNNING,   0);
                gpio_set_level(LED_READY,     0);
                gpio_set_level(LED_BLOCKED,   0);
                gpio_set_level(LED_SUSPENDED, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            break;
    }
}

// ===================== Step 1: Basic Task States Demo =====================
static void state_demo_task(void *pvParameters) {
    ESP_LOGI(TAG, "State Demo Task started");
    int cycle = 0;

    // เริ่มที่ Ready -> Running (เมื่อได้ CPU)
    eTaskState cur = eRunning, prev = eReady;
    count_state_change(prev, cur);
    update_state_display(cur);

    while (1) {
        cycle++;
        ESP_LOGI(TAG, "=== Cycle %d ===", cycle);

        // ---- RUNNING ----
        ESP_LOGI(TAG, "Task is RUNNING");
        prev = cur; cur = eRunning;
        count_state_change(prev, cur);
        update_state_display(cur);

        // งานค่อนข้างหนักเพื่อให้เห็นการกิน CPU
        for (int i = 0; i < 250000; i++) {
            volatile int dummy = i * 2;
            (void)dummy;
        }

        // ---- READY (yield) ----
        ESP_LOGI(TAG, "Task will be READY (yielding to equal priority task)");
        prev = cur; cur = eReady;
        count_state_change(prev, cur);
        update_state_display(cur);

        taskYIELD(); // ให้ task อื่น priority เท่ากันได้รัน
        vTaskDelay(pdMS_TO_TICKS(100)); // กลับมาอยู่ Ready ชั่วครู่

        // ---- BLOCKED (semaphore) ----
        ESP_LOGI(TAG, "Task will be BLOCKED (waiting for semaphore)");
        prev = cur; cur = eBlocked;
        count_state_change(prev, cur);
        update_state_display(cur);

        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(2000)) == pdTRUE) {
            ESP_LOGI(TAG, "Got semaphore! Task RUNNING briefly");
            prev = cur; cur = eRunning;
            count_state_change(prev, cur);
            update_state_display(cur);

            vTaskDelay(pdMS_TO_TICKS(200)); // ทำงานสั้นๆ
        } else {
            ESP_LOGI(TAG, "Semaphore timeout! Continue workflow...");
        }

        // ---- BLOCKED (delay) ----
        ESP_LOGI(TAG, "Task is BLOCKED (vTaskDelay)");
        prev = cur; cur = eBlocked;
        count_state_change(prev, cur);
        update_state_display(cur);

        vTaskDelay(pdMS_TO_TICKS(800));   // Blocked ใน delay
        // กลับไป ready โดยอัตโนมัติ จากนั้น scheduler จะให้ running เมื่อถึงคิว
    }
}

// Task ที่ priority เท่ากันเพื่อสาธิต Ready (เมื่อ state_demo_task yield)
static void ready_state_demo_task(void *pvParameters) {
    while (1) {
        ESP_LOGD(TAG, "Ready demo task running (equal priority)");
        // ทำงานเล็กน้อย
        for (int i = 0; i < 50000; i++) {
            volatile int d = i;
            (void)d;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// ===================== Step 2: Advanced State Transitions =====================
// Self-deleting task
static void self_deleting_task(void *pvParameters) {
    int *lifetime_s = (int *)pvParameters;
    int t = lifetime_s ? *lifetime_s : 10;

    ESP_LOGI(TAG, "Self-delete task will live for %d seconds", t);

    for (int i = t; i > 0; i--) {
        ESP_LOGI(TAG, "Self-delete countdown: %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "Self-deleting task going to DELETED state");
    // นับว่าเข้าสู่ deleted
    count_state_change(g_last_state, eDeleted);
    vTaskDelete(NULL);
}

// Task ที่จะถูก delete จากภายนอก
static void external_delete_task(void *pvParameters) {
    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "External delete task running: %d", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // จะไม่มาถึงจุดนี้ เพราะถูกลบจากภายนอก
}

// ===================== Step 3: Control & Monitoring =====================

static void monitor_task_states(void) {
    ESP_LOGI(TAG, "=== DETAILED TASK STATE MONITOR ===");

    TaskHandle_t tasks[] = {
        state_demo_task_handle,
        control_task_handle,
        ready_demo_task_handle,
        monitor_task_handle,
        external_delete_handle
    };

    const char* names[] = {
        "StateDemo",
        "Control",
        "ReadyDemo",
        "Monitor",
        "ExtDelete"
    };

    const int n = sizeof(tasks)/sizeof(tasks[0]);
    for (int i = 0; i < n; i++) {
        if (tasks[i]) {
            eTaskState s = eTaskGetState(tasks[i]);
            UBaseType_t prio = uxTaskPriorityGet(tasks[i]);
            UBaseType_t stack_rem_words = uxTaskGetStackHighWaterMark(tasks[i]);
            ESP_LOGI(TAG, "%-10s: State=%-9s | Prio=%u | StackRem=%u bytes",
                     names[i],
                     get_state_name(s),
                     (unsigned)prio,
                     (unsigned)(stack_rem_words * sizeof(StackType_t)));
        }
    }
}

static void control_task(void *pvParameters) {
    ESP_LOGI(TAG, "Control task started");
    bool suspended = false;
    int  control_ticks = 0;
    bool external_deleted = false;

    while (1) {
        control_ticks++;

        // ปุ่ม 1: Suspend/Resume
        if (gpio_get_level(BUTTON1_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (gpio_get_level(BUTTON1_PIN) == 0) {
                if (!suspended) {
                    ESP_LOGW(TAG, "=== SUSPEND StateDemo ===");
                    vTaskSuspend(state_demo_task_handle);
                    suspended = true;
                    update_state_display(eSuspended);
                    count_state_change(g_last_state, eSuspended);
                } else {
                    ESP_LOGW(TAG, "=== RESUME StateDemo ===");
                    vTaskResume(state_demo_task_handle);
                    suspended = false;
                    // เมื่อ resume กลับไป Ready ก่อน และจะได้ Running ตาม scheduling
                    count_state_change(g_last_state, eReady);
                    update_state_display(eReady);
                }
                // รอปล่อยปุ่ม
                while (gpio_get_level(BUTTON1_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // ปุ่ม 2: Give semaphore
        if (gpio_get_level(BUTTON2_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if (gpio_get_level(BUTTON2_PIN) == 0) {
                ESP_LOGW(TAG, "=== GIVE SEMAPHORE ===");
                xSemaphoreGive(demo_semaphore);
                while (gpio_get_level(BUTTON2_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // ลบ external task หลัง ~15 วินาที
        if (!external_deleted && control_ticks == 150) { // 150 * 100ms = 15s
            if (external_delete_handle) {
                ESP_LOGW(TAG, "Deleting external task");
                vTaskDelete(external_delete_handle);
                external_deleted = true;
                // บันทึกสถานะเป็น deleted
                count_state_change(g_last_state, eDeleted);
            }
        }

        // รายงาน task list และ runtime stats ทุก ~3 วินาที
        if (control_ticks % 30 == 0) {
            ESP_LOGI(TAG, "=== TASK STATUS REPORT ===");
            char *task_list = (char *)malloc(1024);
            char *stats     = (char *)malloc(1024);
            if (task_list && stats) {
                vTaskList(task_list);
                ESP_LOGI(TAG, "Name          State Prio Stack Num\n%s", task_list);

                vTaskGetRunTimeStats(stats);
                ESP_LOGI(TAG, "Task          Abs Time   %%Time\n%s", stats);
            }
            free(task_list);
            free(stats);

            monitor_task_states();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// แสดงภาพรวมระบบเป็นช่วงๆ
static void system_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started (5s interval)");
    while (1) {
        ESP_LOGI(TAG, "=== SYSTEM MONITOR PULSE ===");
        monitor_task_states();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ===================== app_main: Init & Task Creation =====================

// ---- Runtime Stats Integration ----
// ต้องเปิดใน menuconfig:
// Component config -> FreeRTOS ->
//   [*] Enable FreeRTOS trace facility
//   [*] Enable FreeRTOS stats formatting functions
//   [*] Generate runtime stats
//   [*] Use esp_timer for runtime stats (ถ้ามีตัวเลือกนี้ใน ESP-IDF เวอร์ชันคุณ)
//
// ถ้าไม่มีตัวเลือก “Use esp_timer for runtime stats” ให้ใช้ define ด้านล่าง:
#ifndef portCONFIGURE_TIMER_FOR_RUN_TIME_STATS
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()  // esp-timer พร้อมใช้ใน ESP-IDF
#endif
#ifndef portGET_RUN_TIME_COUNTER_VALUE
#define portGET_RUN_TIME_COUNTER_VALUE() (esp_timer_get_time() / 1000) // us -> ms
#endif

void app_main(void) {
    ESP_LOGI(TAG, "=== FreeRTOS Task States Demo (ESP32) ===");

    // ---- Configure LEDs ----
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask =
            (1ULL << LED_RUNNING) |
            (1ULL << LED_READY) |
            (1ULL << LED_BLOCKED) |
            (1ULL << LED_SUSPENDED),
        .pull_down_en = 0,
        .pull_up_en   = 0
    };
    gpio_config(&io_conf);

    // Turn all off initially
    gpio_set_level(LED_RUNNING,   0);
    gpio_set_level(LED_READY,     0);
    gpio_set_level(LED_BLOCKED,   0);
    gpio_set_level(LED_SUSPENDED, 0);

    // ---- Configure Buttons ----
    // หมายเหตุ: GPIO35 เป็น input-only และไม่มี internal pull-up
    // ถ้าใช้ GPIO35 ต้องมี pull-up ภายนอก (10k ไป 3.3V)
    // ถ้าไม่มี ให้เปลี่ยน BUTTON2_PIN เป็น GPIO_NUM_15 และตั้ง pull_up_en = 1
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON1_PIN) | (1ULL << BUTTON2_PIN),
        .pull_down_en = 0,
        .pull_up_en   = 1 // ใช้ pull-up สำหรับปุ่มที่รองรับ (GPIO0 มี internal pull-up)
    };
    // ถ้าเป็น GPIO35 ให้ปิด pull-up ใน config เพื่อความถูกต้องของ API
#if BUTTON2_PIN == GPIO_NUM_35
    btn_conf.pull_up_en = 1; // จะมีผลกับ GPIO0; GPIO35 ไม่รองรับภายใน แต่ค่า mask ครอบคลุมรวมกันได้
#endif
    gpio_config(&btn_conf);

    // ---- Create Binary Semaphore ----
    demo_semaphore = xSemaphoreCreateBinary();
    if (!demo_semaphore) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    ESP_LOGI(TAG, "LED Indicators: GPIO2=Running, GPIO4=Ready, GPIO5=Blocked, GPIO18=Suspended");
    ESP_LOGI(TAG, "Buttons: GPIO0= Suspend/Resume, GPIO35(or 15)= Give Semaphore");

    // ---- Create Tasks ----
    // สอง task แรกใช้ priority เดียวกันเพื่อโชว์ taskYIELD/Ready
    BaseType_t ok;
    ok = xTaskCreate(state_demo_task, "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    if (ok != pdPASS) ESP_LOGE(TAG, "Create StateDemo failed");

    ok = xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, &ready_demo_task_handle);
    if (ok != pdPASS) ESP_LOGE(TAG, "Create ReadyDemo failed");

    ok = xTaskCreate(control_task, "Control", 3072, NULL, 4, &control_task_handle);
    if (ok != pdPASS) ESP_LOGE(TAG, "Create Control failed");

    ok = xTaskCreate(system_monitor_task, "Monitor", 4096, NULL, 1, &monitor_task_handle);
    if (ok != pdPASS) ESP_LOGE(TAG, "Create Monitor failed");

    // ---- Advanced transitions ----
    static int self_delete_time = 10; // วินาที
    xTaskCreate(self_deleting_task, "SelfDelete", 2048, &self_delete_time, 2, NULL);
    xTaskCreate(external_delete_task, "ExtDelete", 2048, NULL, 2, &external_delete_handle);

    ESP_LOGI(TAG, "All tasks created. Monitoring task states...");
}
