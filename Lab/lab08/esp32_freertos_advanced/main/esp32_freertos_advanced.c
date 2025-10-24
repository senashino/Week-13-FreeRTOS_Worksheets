#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_ipc.h"

static const char *TAG = "APP";
#define PRO_CPU_NUM 0
#define APP_CPU_NUM 1

/* ===========================
 * 1) SMP demo: แจก task หลายตัวให้เห็นว่ารันข้ามคอร์
 * =========================== */
static void core_info_task(void *arg) {
    int id = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "[SMP] Task %d start on core %d", id, xPortGetCoreID());
    while (1) {
        ESP_LOGI(TAG, "[SMP] Task %d heartbeat core %d", id, xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void run_smp_demo(void) {
    ESP_LOGI(TAG, "Starting SMP demo");
    // ไม่กำหนด affinity -> ปล่อยให้ scheduler กระจายเอง
    xTaskCreate(core_info_task, "SMP1", 2048, (void*)1, 5, NULL);
    xTaskCreate(core_info_task, "SMP2", 2048, (void*)2, 5, NULL);
    // กำหนด affinity บ้างให้เห็นความต่าง
    xTaskCreatePinnedToCore(core_info_task, "SMP3", 2048, (void*)3, 6, NULL, PRO_CPU_NUM);
    xTaskCreatePinnedToCore(core_info_task, "SMP4", 2048, (void*)4, 6, NULL, APP_CPU_NUM);
}

/* ===========================
 * 2) IPC demo: เรียกฟังก์ชันไปทำงานบนอีกคอร์แบบบล็อกจนเสร็จ
 * =========================== */
static void remote_core_function(void *arg) {
    int *v = (int*)arg;
    *v = (*v) * 2 + xPortGetCoreID();
    ESP_LOGI(TAG, "[IPC] run on core %d, result=%d", xPortGetCoreID(), *v);
}
static void run_ipc_demo(void) {
    int cur = xPortGetCoreID();
    int tgt = (cur == 0) ? 1 : 0;
    int val = 21;
    ESP_LOGI(TAG, "[IPC] call core %d from core %d", tgt, cur);
    // esp_ipc_call_blocking: เรียกแล้วรอจน remote_core_function ทำเสร็จ
    esp_err_t err = esp_ipc_call_blocking(tgt, remote_core_function, &val);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[IPC] done, val=%d", val);
    } else {
        ESP_LOGE(TAG, "[IPC] error %d", err);
    }
}

/* ===========================
 * 3) GPTimer + ISR: ISR ปล่อย semaphore -> task กระพริบ LED (GPIO2)
 * =========================== */
static SemaphoreHandle_t s_timer_sem;

static bool IRAM_ATTR timer_cb(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t *edata,
                               void *user_data)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_data, &hp);
    return hp == pdTRUE; // ให้ระบบทำ context switch หลังออก ISR ถ้าจำเป็น
}

static void gpt_handler_task(void *p) {
    bool led = false;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_NUM_2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    while (1) {
        if (xSemaphoreTake(s_timer_sem, portMAX_DELAY) == pdTRUE) {
            led = !led;
            gpio_set_level(GPIO_NUM_2, led);
            ESP_LOGI(TAG, "[TIMER] tick on core %d, LED=%d", xPortGetCoreID(), led);
        }
    }
}

static void run_gptimer_demo(void) {
    s_timer_sem = xSemaphoreCreateBinary();

    gptimer_handle_t tim;
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1 tick = 1 us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &tim));

    gptimer_event_callbacks_t cbs = {.on_alarm = timer_cb};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tim, &cbs, s_timer_sem));

    gptimer_alarm_config_t acfg = {
        .reload_count = 0,
        .alarm_count = 500000, // 0.5s
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tim, &acfg));
    ESP_ERROR_CHECK(gptimer_enable(tim));
    ESP_ERROR_CHECK(gptimer_start(tim));

    // ให้ task จัดการเหตุการณ์จาก ISR (pin ไว้ core 0 เพื่อลด jitter)
    xTaskCreatePinnedToCore(gpt_handler_task, "TMRH", 2048, NULL, 15, NULL, PRO_CPU_NUM);
}

/* ===========================
 * 4) มอนิเตอร์เบาๆ (ไม่ต้องเปิด trace)
 * =========================== */
static void light_monitor_task(void *p) {
    while (1) {
        ESP_LOGI(TAG, "Core %d | Free heap: %u bytes", xPortGetCoreID(), esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ===========================
 * app_main: เรียกเดโมทั้งหมด
 * =========================== */
void app_main(void) {
    ESP_LOGI(TAG, "ESP-IDF FreeRTOS Advanced Demo | start on core %d", xPortGetCoreID());

#ifdef CONFIG_DEMO_SMP
    run_smp_demo();
#endif
#ifdef CONFIG_DEMO_IPC
    run_ipc_demo();
#endif
#ifdef CONFIG_DEMO_GPTIMER
    run_gptimer_demo();
#endif
    xTaskCreate(light_monitor_task, "Mon", 2048, NULL, 3, NULL);
}
