
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "EXP4_HEALTH";

/* ===== GPIOs ===== */
#define HEALTH_LED_GPIO   GPIO_NUM_4   /* ติดเมื่อเจอ warning */
#define ERROR_LED_GPIO    GPIO_NUM_18  /* ติดเมื่อเจอเงื่อนไขวิกฤติ */

/* ===== Health policy ===== */
#define HEALTH_INTERVAL_MS        1000     /* สรุปสุขภาพทุก 1s */
#define MEMORY_LOW_THRESHOLD_B    20000    /* <20KB = หน่วยความจำต่ำ */
#define STACK_LOW_WATERMARK_MIN   256      /* <256 words (≈1KB) ถือว่าเสี่ยง */
#define DYNAMIC_MAX               40       /* สร้างไดนามิกสูงสุด */
#define BURST_SPAWN_COUNT         5        /* เพิ่มทีละกี่ตัวทุกรอบ */
#define PERIOD_BASE_MS            120      /* คาบเริ่มต้นของไดนามิก */
#define PERIOD_STEP_MS            20       /* เพิ่มคาบทีละเท่านี้ เพื่อลดชนกันของเฟส */
#define RECOVERY_RATIO_NUM        1        /* ล้างออก 1/RECOVERY_RATIO_DENOM เมื่อวิกฤติ */
#define RECOVERY_RATIO_DENOM      2

/* ===== Globals ===== */
static TimerHandle_t s_health_timer = NULL;
static TimerHandle_t s_dyn[DYNAMIC_MAX];
static uint32_t      s_dyn_cnt = 0;

static SemaphoreHandle_t s_lock; /* ป้องกันข้อมูลไดนามิกระหว่าง health_cb กับ spawner */

/* ===== Dummy callback ===== */
static void light_cb(TimerHandle_t t)
{
    (void)t;
    /* งานสั้น ๆ ไม่บล็อก เพื่อให้เห็น scheduling ภาพรวม */
    for (volatile uint32_t i=0;i<100;i++){ __asm__ __volatile__("nop"); }
}

/* ===== LED init ===== */
static void leds_init(void)
{
    gpio_reset_pin(HEALTH_LED_GPIO);
    gpio_reset_pin(ERROR_LED_GPIO);
    gpio_set_direction(HEALTH_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(ERROR_LED_GPIO,  GPIO_MODE_OUTPUT);
    gpio_set_level(HEALTH_LED_GPIO, 0);
    gpio_set_level(ERROR_LED_GPIO,  0);
}

/* ===== Dynamic timers helpers ===== */
static TimerHandle_t dyn_create(uint32_t idx, uint32_t period_ms)
{
    char name[16];
    snprintf(name, sizeof(name), "D%02lu", (unsigned long)idx);
    return xTimerCreate(name,
                        pdMS_TO_TICKS(period_ms),
                        pdTRUE,
                        (void*)(uintptr_t)(2000U + idx),
                        light_cb);
}

static void dyn_spawn_burst(uint32_t n)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    for (uint32_t i=0; i<n && s_dyn_cnt < DYNAMIC_MAX; ++i) {
        uint32_t period_ms = PERIOD_BASE_MS + (s_dyn_cnt % 10U) * PERIOD_STEP_MS;
        TimerHandle_t t = dyn_create(s_dyn_cnt, period_ms);
        if (t && xTimerStart(t, pdMS_TO_TICKS(100)) == pdPASS) {
            s_dyn[s_dyn_cnt++] = t;
            ESP_LOGI(TAG, "Spawned dynamic timer #%lu (period=%lums)",
                     (unsigned long)(s_dyn_cnt-1), (unsigned long)period_ms);
        } else if (t) {
            ESP_LOGE(TAG, "xTimerStart failed -> delete");
            xTimerDelete(t, 0);
        } else {
            ESP_LOGE(TAG, "xTimerCreate failed");
            break;
        }
    }

    xSemaphoreGive(s_lock);
}

static void dyn_cleanup_tail(uint32_t keep_count)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    if (keep_count > s_dyn_cnt) keep_count = s_dyn_cnt;
    for (uint32_t i = keep_count; i < s_dyn_cnt; ++i) {
        if (s_dyn[i]) {
            xTimerStop(s_dyn[i], pdMS_TO_TICKS(50));
            xTimerDelete(s_dyn[i], pdMS_TO_TICKS(50));
            s_dyn[i] = NULL;
        }
    }
    ESP_LOGW(TAG, "Cleanup: %lu -> %lu timers", (unsigned long)s_dyn_cnt, (unsigned long)keep_count);
    s_dyn_cnt = keep_count;

    xSemaphoreGive(s_lock);
}

/* ===== Health monitor ===== */
static void health_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    /* 1) ตัวชี้วัดหลัก */
    size_t free_heap = esp_get_free_heap_size();

    /* Timer Daemon task info */
    TaskStatus_t daemon_status;
    memset(&daemon_status, 0, sizeof(daemon_status));
    vTaskGetInfo(xTimerGetTimerDaemonTaskHandle(), &daemon_status, pdTRUE, eInvalid);

    /* หมายเหตุ: usStackHighWaterMark หน่วยเป็น "words" (4 ไบต์ต่อ word) */
    UBaseType_t stack_high_water_words = daemon_status.usStackHighWaterMark;
    uint32_t    stack_high_water_bytes = stack_high_water_words * sizeof(StackType_t);

    /* 2) สรุปสุขภาพ */
    gpio_set_level(HEALTH_LED_GPIO, 0);
    gpio_set_level(ERROR_LED_GPIO,  0);

    bool warn = false;
    bool crit = false;

    if (free_heap < MEMORY_LOW_THRESHOLD_B) {
        warn = true; crit = true;
    }
    if (stack_high_water_words > 0 && stack_high_water_words < STACK_LOW_WATERMARK_MIN) {
        warn = true;
    }

    /* 3) รายงาน */
    ESP_LOGI(TAG,
        "Health: dyn=%lu/%d | free_heap=%lu B | daemon stack HWM=%lu words (~%lu B) | task=\"%s\" prio=%lu",
        (unsigned long)s_dyn_cnt, DYNAMIC_MAX,
        (unsigned long)free_heap,
        (unsigned long)stack_high_water_words, (unsigned long)stack_high_water_bytes,
        daemon_status.pcTaskName ? daemon_status.pcTaskName : "N/A",
        (unsigned long)daemon_status.uxCurrentPriority
    );

#if ( configGENERATE_RUN_TIME_STATS == 1 )
    /* Optional: หากเปิด run-time stats จะแสดงเปอร์เซ็นต์เวลาทำงานของ daemon (ต้องตั้งค่า clock ในโปรเจกต์ด้วย) */
    extern uint32_t uxTaskGetNumberOfTasks(void);
    (void)uxTaskGetNumberOfTasks; /* กัน warning หาก toolchain เข้ม */
    /* ผู้ใช้สามารถเพิ่ม vTaskGetRunTimeStats(buffer) ตรงนี้ได้ หากเตรียมบัฟเฟอร์ */
#endif

    /* 4) แสดงไฟสถานะ */
    if (warn) gpio_set_level(HEALTH_LED_GPIO, 1);
    if (crit) gpio_set_level(ERROR_LED_GPIO, 1);

    /* 5) Recovery Policy */
    if (crit) {
        /* ล้าง dynamic timers บางส่วน (เช่น ครึ่งหนึ่ง) */
        uint32_t keep = (s_dyn_cnt * RECOVERY_RATIO_NUM) / RECOVERY_RATIO_DENOM; /* ครึ่งหนึ่ง */
        dyn_cleanup_tail(keep);
    }
}

/* ===== Scenario: ค่อย ๆ เพิ่ม dynamic timers เพื่อให้ health ทำงานจริง ===== */
static void scenario_task(void *arg)
{
    (void)arg;

    while (true) {
        /* เพิ่มไดนามิกทีละก้อน */
        dyn_spawn_burst(BURST_SPAWN_COUNT);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ===== App entry ===== */
void app_main(void)
{
    ESP_LOGI(TAG, "EXP4 Health Monitoring starting...");

    s_lock = xSemaphoreCreateMutex();
    (void)memset(s_dyn, 0, sizeof(s_dyn));

    leds_init();

    /* Health timer */
    s_health_timer = xTimerCreate("Health",
                                  pdMS_TO_TICKS(HEALTH_INTERVAL_MS),
                                  pdTRUE,
                                  NULL,
                                  health_cb);
    if (s_health_timer) {
        (void)xTimerStart(s_health_timer, 0);
    } else {
        ESP_LOGE(TAG, "Create health timer failed");
    }

    /* สร้างสคริปต์สถานการณ์ */
    if (xTaskCreate(scenario_task, "scenario", 3072, NULL, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Create scenario task failed");
        return;
    }

    ESP_LOGI(TAG, "Running. HEALTH_LED(GPIO4)=warn, ERROR_LED(GPIO18)=critical/low memory.");
}
