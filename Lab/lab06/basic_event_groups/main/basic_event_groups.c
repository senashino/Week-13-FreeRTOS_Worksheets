#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "EVENT_LAB_EXP4";

/* ===== GPIO Map (ESP32) =====
 * GPIO2  = Network Ready
 * GPIO4  = Sensor Ready
 * GPIO5  = Config Ready
 * GPIO18 = Storage Ready
 * GPIO19 = System Ready
 */
#define LED_NETWORK_READY   GPIO_NUM_2
#define LED_SENSOR_READY    GPIO_NUM_4
#define LED_CONFIG_READY    GPIO_NUM_5
#define LED_STORAGE_READY   GPIO_NUM_18
#define LED_SYSTEM_READY    GPIO_NUM_19

/* Event bits */
#define NETWORK_READY_BIT   (1 << 0)
#define SENSOR_READY_BIT    (1 << 1)
#define CONFIG_READY_BIT    (1 << 2)
#define STORAGE_READY_BIT   (1 << 3)
#define SYSTEM_READY_BIT    (1 << 4)

/* Grouping */
#define BASIC_SYSTEM_BITS   (NETWORK_READY_BIT | CONFIG_READY_BIT)
#define ALL_SUBSYSTEM_BITS  (NETWORK_READY_BIT | SENSOR_READY_BIT | CONFIG_READY_BIT | STORAGE_READY_BIT)

/* ===== ปรับค่า Delay/Timeout ได้จาก -D ตอน build ===== */
#ifndef NET_INIT_MS
#define NET_INIT_MS       2000
#endif
#ifndef CFG_INIT_MS
#define CFG_INIT_MS       3000
#endif
#ifndef SENS_INIT_MS
#define SENS_INIT_MS      6000
#endif
#ifndef STOR_INIT_MS
#define STOR_INIT_MS      1000
#endif

#ifndef PHASE1_TIMEOUT_MS   /* รอ Network+Config */
#define PHASE1_TIMEOUT_MS  4000
#endif
#ifndef PHASE2_TIMEOUT_MS   /* รอ ALL subsystems */
#define PHASE2_TIMEOUT_MS  8000
#endif

static EventGroupHandle_t system_events = NULL;

/* ===== Utilities ===== */
static void setup_gpio(void) {
    gpio_set_direction(LED_NETWORK_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SENSOR_READY,  GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONFIG_READY,  GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STORAGE_READY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM_READY,  GPIO_MODE_OUTPUT);

    gpio_set_level(LED_NETWORK_READY, 0);
    gpio_set_level(LED_SENSOR_READY,  0);
    gpio_set_level(LED_CONFIG_READY,  0);
    gpio_set_level(LED_STORAGE_READY, 0);
    gpio_set_level(LED_SYSTEM_READY,  0);
}

static void print_bits(EventBits_t b) {
    ESP_LOGI(TAG, "Bits=0x%02X [NET:%d SEN:%d CFG:%d STO:%d SYS:%d]",
             (unsigned)b,
             (b & NETWORK_READY_BIT)  ? 1 : 0,
             (b & SENSOR_READY_BIT)   ? 1 : 0,
             (b & CONFIG_READY_BIT)   ? 1 : 0,
             (b & STORAGE_READY_BIT)  ? 1 : 0,
             (b & SYSTEM_READY_BIT)   ? 1 : 0);
}

/* ===== Subsystem tasks (ดีเลย์ตาม MS ที่กำหนด) ===== */
static void net_task(void *pv)  { vTaskDelay(pdMS_TO_TICKS(NET_INIT_MS));  xEventGroupSetBits(system_events, NETWORK_READY_BIT); gpio_set_level(LED_NETWORK_READY,1); ESP_LOGI(TAG, "Network ready in %d ms", NET_INIT_MS);  vTaskDelete(NULL); }
static void cfg_task(void *pv)  { vTaskDelay(pdMS_TO_TICKS(CFG_INIT_MS));  xEventGroupSetBits(system_events, CONFIG_READY_BIT);  gpio_set_level(LED_CONFIG_READY,1); ESP_LOGI(TAG, "Config  ready in %d ms", CFG_INIT_MS);  vTaskDelete(NULL); }
static void sens_task(void *pv) { vTaskDelay(pdMS_TO_TICKS(SENS_INIT_MS)); xEventGroupSetBits(system_events, SENSOR_READY_BIT);  gpio_set_level(LED_SENSOR_READY,1); ESP_LOGI(TAG, "Sensor  ready in %d ms", SENS_INIT_MS); vTaskDelete(NULL); }
static void stor_task(void *pv) { vTaskDelay(pdMS_TO_TICKS(STOR_INIT_MS)); xEventGroupSetBits(system_events, STORAGE_READY_BIT); gpio_set_level(LED_STORAGE_READY,1);ESP_LOGI(TAG, "Storage ready in %d ms", STOR_INIT_MS); vTaskDelete(NULL); }

/* ===== Timing / Coordinator =====
 * - Phase 1: รอ BASIC (Network+Config) ด้วย timeout
 * - Phase 2: รอ ALL subsystems ด้วย timeout
 * - สรุปเวลาและสถานะ แล้วเฝ้าดูสถานะต่อเนื่อง
 */
static void timing_task(void *pv) {
    TickType_t t0 = xTaskGetTickCount();
    ESP_LOGI(TAG, "Phase 1: wait BASIC (NET+CFG), timeout=%d ms", PHASE1_TIMEOUT_MS);
    EventBits_t b1 = xEventGroupWaitBits(system_events, BASIC_SYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(PHASE1_TIMEOUT_MS));
    TickType_t t1 = xTaskGetTickCount();
    ESP_LOGI(TAG, "Phase1 result=0x%02X, elapsed=%lu ms", (unsigned)b1, (unsigned long)((t1 - t0) * portTICK_PERIOD_MS));
    print_bits(b1);

    ESP_LOGI(TAG, "Phase 2: wait ALL subsystems, timeout=%d ms", PHASE2_TIMEOUT_MS);
    EventBits_t b2 = xEventGroupWaitBits(system_events, ALL_SUBSYSTEM_BITS, pdFALSE, pdTRUE, pdMS_TO_TICKS(PHASE2_TIMEOUT_MS));
    TickType_t t2 = xTaskGetTickCount();
    ESP_LOGI(TAG, "Phase2 result=0x%02X, elapsed=%lu ms", (unsigned)b2, (unsigned long)((t2 - t1) * portTICK_PERIOD_MS));
    print_bits(b2);

    if ((b2 & ALL_SUBSYSTEM_BITS) == ALL_SUBSYSTEM_BITS) {
        xEventGroupSetBits(system_events, SYSTEM_READY_BIT);
        gpio_set_level(LED_SYSTEM_READY, 1);
        ESP_LOGI(TAG, "SYSTEM READY");
    } else {
        ESP_LOGW(TAG, "Timeout before ALL ready -> SYSTEM NOT READY");
    }

    /* เฝ้ารายงานสถานะแบบต่อเนื่อง */
    while (1) {
        EventBits_t b = xEventGroupGetBits(system_events);
        bool all_ok = ((b & ALL_SUBSYSTEM_BITS) == ALL_SUBSYSTEM_BITS);
        if (all_ok) gpio_set_level(LED_SYSTEM_READY, 1);
        else        gpio_set_level(LED_SYSTEM_READY, 0);

        ESP_LOGI(TAG, "State snapshot: ALL=%s", all_ok ? "YES" : "NO");
        print_bits(b);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Exp4: Timing Analysis (override delays/timeouts via -D)");
    setup_gpio();

    system_events = xEventGroupCreate();
    if (!system_events) {
        ESP_LOGE(TAG, "Failed to create Event Group");
        return;
    }

    xTaskCreate(net_task,   "net",   2048, NULL, 5, NULL);
    xTaskCreate(cfg_task,   "cfg",   2048, NULL, 5, NULL);
    xTaskCreate(sens_task,  "sens",  2048, NULL, 4, NULL);
    xTaskCreate(stor_task,  "stor",  2048, NULL, 4, NULL);
    xTaskCreate(timing_task,"timing",3072, NULL, 6, NULL);
}
