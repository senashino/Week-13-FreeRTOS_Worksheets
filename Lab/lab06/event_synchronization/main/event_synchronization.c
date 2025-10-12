#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"   // esp_get_free_heap_size(), esp_get_minimum_free_heap_size()
#include "sdkconfig.h"

static const char *TAG = "PERF_MON";

/* ================= GPIO ================= */
#define LED_OK       GPIO_NUM_2
#define LED_WARNING  GPIO_NUM_4

/* ================ Event Group (Health) ================ */
static EventGroupHandle_t health_events;

/* บิตสถานะ/เตือน */
#define HEALTH_OK_BIT        (1 << 0)
#define WARN_MEMORY_BIT      (1 << 1)
#define WARN_STACK_BIT       (1 << 2)
#define WARN_CPU_BIT         (1 << 3)

/* ================== งานจำลองโหลด ================== */
static TaskHandle_t light_task_handle   = NULL;
static TaskHandle_t medium_task_handle  = NULL;
static TaskHandle_t heavy_task_handle   = NULL;

/* ================== เกณฑ์เตือน (ปรับได้) ================== */
#define STACK_WARN_THRESHOLD_BYTES   512   // เหลือน้อยกว่าเท่านี้ = เตือน
#define CPU_WARN_UTIL_PERCENT        75    // CPU ใช้งานเฉลี่ย > 75% = เตือน
#define MEM_WARN_THRESHOLD_BYTES     (40*1024) // free heap < 40KB = เตือน

/* ================== Idle hook สำหรับ CPU util ================== */
static volatile uint32_t idle_counter = 0;
static uint32_t          idle_ref_max = 0;  // ค่ามากสุดที่สังเกต (อ้างเป็น 0% ใช้งาน)
static uint32_t          last_idle_counter = 0;

static void idle_hook(void)
{
    // จะถูกเรียกใน Idle Task (@CPU ที่ register)
    idle_counter++;
}

static void register_idle_hook(void)
{
#if CONFIG_FREERTOS_USE_IDLE_HOOK
    // ใน ESP-IDF มี API ลงทะเบียน Idle Hook ต่อ CPU ปัจจุบัน
    extern bool esp_register_freertos_idle_hook(void (*hook)(void));
    if (esp_register_freertos_idle_hook(idle_hook)) {
        ESP_LOGI(TAG, "Idle hook registered (per-CPU).");
    } else {
        ESP_LOGW(TAG, "Register idle hook failed.");
    }
#else
    ESP_LOGW(TAG, "CONFIG_FREERTOS_USE_IDLE_HOOK disabled; CPU util will be skipped.");
#endif
}

/* ================ ยูทิล LED ================ */
static inline void led_on(gpio_num_t pin){ gpio_set_level(pin, 1); }
static inline void led_off(gpio_num_t pin){ gpio_set_level(pin, 0); }

/* ================ งานโหลดจำลอง ================ */
static void light_task(void *arg)
{
    ESP_LOGI(TAG, "Light task started");
    while (1) {
        // ทำงานสั้น ๆ แล้วพักเยอะ (โหลดน้อย)
        for (int i = 0; i < 2000; ++i) {
            __asm__ __volatile__("nop");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void medium_task(void *arg)
{
    ESP_LOGI(TAG, "Medium task started");
    while (1) {
        // ทำงานปานกลาง
        for (int r = 0; r < 3; ++r) {
            uint64_t t0 = esp_timer_get_time();
            while ((esp_timer_get_time() - t0) < 1500) { // busy ~1.5ms x 3 ~ 4.5ms
                __asm__ __volatile__("nop");
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void heavy_task(void *arg)
{
    ESP_LOGI(TAG, "Heavy task started");
    while (1) {
        // ทำงานหนัก: busy ประมาณ 30–60ms ต่อรอบ แล้วพักสั้น
        uint32_t busy_ms = 30 + (esp_random() % 31);
        uint64_t t0 = esp_timer_get_time();
        while ((esp_timer_get_time() - t0) < (busy_ms * 1000ULL)) {
            // ทำอะไรเล็ก ๆ ให้ compiler ไม่ optimize ทิ้ง
            __asm__ __volatile__("nop");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ======== เครื่องมือช่วยวัด/แสดงผล ======== */
static const char* state_str(eTaskState s) {
    switch (s) {
        case eRunning:   return "Running";
        case eReady:     return "Ready";
        case eBlocked:   return "Blocked";
        case eSuspended: return "Suspended";
        case eDeleted:   return "Deleted";
        default:         return "?";
    }
}

static void report_task_stack(const char* name, TaskHandle_t h)
{
    if (!h) return;
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(h); // หน่วย: words
    uint32_t bytes = watermark * sizeof(StackType_t);
    ESP_LOGI(TAG, "Stack %s: high water mark = %u bytes", name, (unsigned)bytes);
}

/* ======== มอนิเตอร์หลัก ======== */
static void perf_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Performance monitor started");

    // ให้เวลาระบบ stabilize เล็กน้อย
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // รายงานทุก 5 วินาที

        // 1) หน่วยความจำ
        size_t heap_now = esp_get_free_heap_size();
        size_t heap_min = esp_get_minimum_free_heap_size();

        // 2) สแต็กของงานต่าง ๆ
        report_task_stack("light",  light_task_handle);
        report_task_stack("medium", medium_task_handle);
        report_task_stack("heavy",  heavy_task_handle);
        report_task_stack("monitor", xTaskGetCurrentTaskHandle());

        // 3) สถานะงาน (อย่างย่อ)
        eTaskState sL = eTaskGetState(light_task_handle);
        eTaskState sM = eTaskGetState(medium_task_handle);
        eTaskState sH = eTaskGetState(heavy_task_handle);

        // 4) ประมาณ CPU utilization ผ่าน idle counter (ถ้ามี idle hook)
        uint32_t util_pct = 0;
#if CONFIG_FREERTOS_USE_IDLE_HOOK
        uint32_t now = idle_counter;
        uint32_t delta = now - last_idle_counter;
        last_idle_counter = now;
        if (delta > idle_ref_max) idle_ref_max = delta;     // อัปเดต baseline เมื่อเห็นค่ามากสุด (ว่างสุด)
        if (idle_ref_max > 0 && delta <= idle_ref_max) {
            // ยิ่ง delta (idle) น้อย → ใช้งาน CPU มาก
            util_pct = (uint32_t)((100ULL * (idle_ref_max - delta)) / idle_ref_max);
        }
#else
        util_pct = 0; // ไม่รองรับ idle hook
#endif

        // 5) อัปเดตบิตเหตุการณ์ตามเกณฑ์
        EventBits_t warn = 0;
        if (heap_now < MEM_WARN_THRESHOLD_BYTES) warn |= WARN_MEMORY_BIT;

        // เช็กสแต็กต่ำกว่ากำหนดของงานหลัก ๆ
        if (light_task_handle && (uxTaskGetStackHighWaterMark(light_task_handle) * sizeof(StackType_t) < STACK_WARN_THRESHOLD_BYTES))  warn |= WARN_STACK_BIT;
        if (medium_task_handle && (uxTaskGetStackHighWaterMark(medium_task_handle) * sizeof(StackType_t) < STACK_WARN_THRESHOLD_BYTES)) warn |= WARN_STACK_BIT;
        if (heavy_task_handle && (uxTaskGetStackHighWaterMark(heavy_task_handle) * sizeof(StackType_t) < STACK_WARN_THRESHOLD_BYTES))  warn |= WARN_STACK_BIT;

        if (util_pct > CPU_WARN_UTIL_PERCENT) warn |= WARN_CPU_BIT;

        // เซ็ต/เคลียร์อีเวนต์
        if (warn) {
            xEventGroupClearBits(health_events, HEALTH_OK_BIT);
            xEventGroupSetBits(health_events, warn);
            led_off(LED_OK);
            led_on(LED_WARNING);
        } else {
            xEventGroupClearBits(health_events, WARN_MEMORY_BIT | WARN_STACK_BIT | WARN_CPU_BIT);
            xEventGroupSetBits(health_events, HEALTH_OK_BIT);
            led_on(LED_OK);
            led_off(LED_WARNING);
        }

        // 6) รายงาน
        ESP_LOGI(TAG,
            "\n📊 === SYSTEM HEALTH REPORT ===\n"
            "Heap free:        %u bytes\n"
            "Heap min free:    %u bytes\n"
            "Tasks state:      light=%s, medium=%s, heavy=%s\n"
#if CONFIG_FREERTOS_USE_IDLE_HOOK
            "CPU util (est.):  %u %%   (idle_delta=%u, baseline=%u)\n"
#else
            "CPU util:         (idle hook disabled in sdkconfig)\n"
#endif
            "Event bits:       0x%08x\n"
            "Uptime:           %llu ms\n"
            "===============================\n",
            (unsigned)heap_now,
            (unsigned)heap_min,
            state_str(sL), state_str(sM), state_str(sH),
#if CONFIG_FREERTOS_USE_IDLE_HOOK
            (unsigned)util_pct, (unsigned)(delta), (unsigned)idle_ref_max,
#endif
            (unsigned)xEventGroupGetBits(health_events),
            (unsigned long long)(esp_timer_get_time()/1000ULL)
        );
    }
}

/* ================== app_main ================== */
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 System Performance – Experiment 4");

    // ตั้งค่า GPIO
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<LED_OK) | (1ULL<<LED_WARNING),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    led_off(LED_OK);
    led_off(LED_WARNING);

    // Event group
    health_events = xEventGroupCreate();
    if (!health_events) {
        ESP_LOGE(TAG, "Create health_events failed");
        return;
    }
    xEventGroupSetBits(health_events, HEALTH_OK_BIT);

    // ลงทะเบียน idle hook (ถ้าเปิดไว้ใน sdkconfig)
    register_idle_hook();

    // สร้างงานโหลด
    xTaskCreate(light_task,  "LoadLight",  2048, NULL, 2, &light_task_handle);
    xTaskCreate(medium_task, "LoadMedium", 2560, NULL, 3, &medium_task_handle);
    xTaskCreate(heavy_task,  "LoadHeavy",  3072, NULL, 4, &heavy_task_handle);

    // มอนิเตอร์
    xTaskCreate(perf_monitor_task, "PerfMon", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "✅ Tasks started. Watch LED2(OK) / LED4(WARN) and serial logs.");
}
