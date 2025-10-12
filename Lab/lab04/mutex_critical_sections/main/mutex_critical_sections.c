#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "MUTEX_EXP3";

/* ====== PRIORITY SETUP (Experiment 3) ======
 * เปลี่ยนให้ LOW สูงสุด และ HIGH ต่ำสุด
 * ดูผลว่าใครเข้าถึงทรัพยากรบ่อยขึ้น และ mutex ยังกัน corruption ได้
 */
#define PRIORITY_HIGH      2
#define PRIORITY_MED       3
#define PRIORITY_LOW       5
#define PRIORITY_CPU_BURST 4   // งานกลางกิน CPU (ไม่จับ mutex) เพื่อกดดัน scheduler
#define PRIORITY_MONITOR   1

// GPIO (ปรับตามบอร์ดได้)
#define LED_TASK1    GPIO_NUM_2   // HIGH_PRI
#define LED_TASK2    GPIO_NUM_4   // MED_PRI
#define LED_TASK3    GPIO_NUM_5   // LOW_PRI
#define LED_CRITICAL GPIO_NUM_18  // กำลังอยู่ใน critical section

// Mutex
static SemaphoreHandle_t xMutex;

/* -------- Shared Resource -------- */
typedef struct {
    uint32_t counter;
    char     shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

static shared_resource_t shared_data = {0,"",0,0};

typedef struct {
    uint32_t successful_access;
    uint32_t failed_access;
    uint32_t corruption_detected;
} access_stats_t;

static access_stats_t stats = {0,0,0};

/* -------- Utils -------- */
static uint32_t checksum(const char* s, uint32_t c){
    uint32_t sum = c;
    for (int i=0; s[i]!='\0'; ++i) sum += (uint32_t)s[i]*(i+1);
    return sum;
}
static void led_setup(gpio_num_t pin){ gpio_reset_pin(pin); gpio_set_direction(pin, GPIO_MODE_OUTPUT); gpio_set_level(pin,0); }

/* -------- Critical Section (with mutex) -------- */
static void access_shared(const char* name, gpio_num_t led_pin, int hold_ms){
    ESP_LOGI(TAG, "[%s] Requesting…", name);
    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5000)) == pdTRUE){
        stats.successful_access++;
        gpio_set_level(led_pin,1); gpio_set_level(LED_CRITICAL,1);

        // read
        uint32_t c  = shared_data.counter;
        char buf[100]; strcpy(buf, shared_data.shared_buffer);
        uint32_t exp = shared_data.checksum;

        // verify before modify
        uint32_t calc = checksum(buf, c);
        if (calc != exp && shared_data.access_count > 0){
            stats.corruption_detected++;
            ESP_LOGE(TAG,"[%s] ⚠️ DATA CORRUPTION DETECTED! exp=%lu calc=%lu",
                     name,(unsigned long)exp,(unsigned long)calc);
        }

        // simulate work inside CS (ยืดเวลาตาม hold_ms)
        vTaskDelay(pdMS_TO_TICKS(hold_ms));

        // write
        shared_data.counter = c + 1;
        snprintf(shared_data.shared_buffer,sizeof(shared_data.shared_buffer),
                 "Modified by %s #%lu", name, (unsigned long)shared_data.counter);
        shared_data.checksum = checksum(shared_data.shared_buffer, shared_data.counter);
        shared_data.access_count++;

        ESP_LOGI(TAG,"[%s] ✓ Modified: counter=%lu, buffer='%s'",
                 name,(unsigned long)shared_data.counter, shared_data.shared_buffer);

        gpio_set_level(led_pin,0); gpio_set_level(LED_CRITICAL,0);
        xSemaphoreGive(xMutex);
    } else {
        stats.failed_access++;
        ESP_LOGW(TAG,"[%s] ✗ Mutex timeout", name);
    }
}

/* -------- Tasks -------- */
// ให้ HIGH พยายามเข้าถี่ แต่ prio ต่ำสุด -> จะได้เข้าช้ากว่า
static void high_task(void*){
    ESP_LOGI(TAG,"HIGH_PRI started (prio=%d)", uxTaskPriorityGet(NULL));
    while(1){
        access_shared("HIGH_PRI", LED_TASK1, 120);     // hold สั้น
        vTaskDelay(pdMS_TO_TICKS(800 + (esp_random()%600)));  // ถี่
    }
}
// MED ปกติ
static void med_task(void*){
    ESP_LOGI(TAG,"MED_PRI started (prio=%d)", uxTaskPriorityGet(NULL));
    while(1){
        access_shared("MED_PRI", LED_TASK2, 160);
        vTaskDelay(pdMS_TO_TICKS(1200 + (esp_random()%800)));
    }
}
// LOW มี prio สูงสุด และถือ mutex นานขึ้นเพื่อเห็นอิทธิพล
static void low_task(void*){
    ESP_LOGI(TAG,"LOW_PRI started (prio=%d)", uxTaskPriorityGet(NULL));
    while(1){
        access_shared("LOW_PRI", LED_TASK3, 350);      // จงใจถือ mutex นานขึ้น
        vTaskDelay(pdMS_TO_TICKS(1400 + (esp_random()%900)));
    }
}

// งานกลางกิน CPU ไม่แตะ mutex -> ใช้กดดัน scheduler (ถ้าไม่มี PI จะไปรบกวน low ตอนถือ mutex)
static void cpu_burst_task(void*){
    ESP_LOGI(TAG,"CPU_BURST started (prio=%d)", uxTaskPriorityGet(NULL));
    while(1){
        vTaskDelay(pdMS_TO_TICKS(2500));
        ESP_LOGI(TAG,"CPU burst…");
        for (volatile int i=0;i<1200000;i++) { /* busy */ }
        ESP_LOGI(TAG,"CPU burst done");
    }
}

// Monitor
static void monitor_task(void*){
    while(1){
        vTaskDelay(pdMS_TO_TICKS(12000));
        uint32_t chk = checksum(shared_data.shared_buffer, shared_data.counter);
        if (chk != shared_data.checksum && shared_data.access_count>0){
            stats.corruption_detected++;
            ESP_LOGE(TAG,"⚠️ CURRENT DATA CORRUPTION DETECTED!");
        }
        uint32_t total = stats.successful_access + stats.failed_access;
        float rate = total? (float)stats.successful_access/total*100.0f : 0.0f;
        ESP_LOGI(TAG,"\n═══ EXP3 MONITOR (Mutex ON, Changed Priority) ═══");
        ESP_LOGI(TAG,"Counter=%lu  AccessCount=%lu  Corrupted=%lu  SuccessRate=%.1f%%",
                 (unsigned long)shared_data.counter,
                 (unsigned long)shared_data.access_count,
                 (unsigned long)stats.corruption_detected, rate);
        ESP_LOGI(TAG,"Buffer='%s'\n", shared_data.shared_buffer);
    }
}

void app_main(void){
    ESP_LOGI(TAG,"Experiment 3 starting… (LOW prio highest, HIGH prio lowest)");

    // LEDs
    led_setup(LED_TASK1); led_setup(LED_TASK2); led_setup(LED_TASK3); led_setup(LED_CRITICAL);

    // Mutex
    xMutex = xSemaphoreCreateMutex();
    if (!xMutex){ ESP_LOGE(TAG,"Create mutex failed!"); return; }

    // Init shared
    shared_data.counter = 0;
    strcpy(shared_data.shared_buffer, "Initial state");
    shared_data.checksum = checksum(shared_data.shared_buffer, shared_data.counter);
    shared_data.access_count = 0;

    // Create tasks with CHANGED priorities
    xTaskCreate(high_task,      "HighPri",   3072, NULL, PRIORITY_HIGH,      NULL);
    xTaskCreate(med_task,       "MedPri",    3072, NULL, PRIORITY_MED,       NULL);
    xTaskCreate(low_task,       "LowPri",    3072, NULL, PRIORITY_LOW,       NULL);
    xTaskCreate(cpu_burst_task, "CpuBurst",  2048, NULL, PRIORITY_CPU_BURST, NULL);
    xTaskCreate(monitor_task,   "Monitor",   3072, NULL, PRIORITY_MONITOR,   NULL);

    ESP_LOGI(TAG,"Created tasks — High=%d, Med=%d, Low=%d, CPU=%d, Monitor=%d",
             PRIORITY_HIGH, PRIORITY_MED, PRIORITY_LOW, PRIORITY_CPU_BURST, PRIORITY_MONITOR);

    // quick LED sweep
    for (int i=0;i<2;i++){
        gpio_set_level(LED_TASK1,1); vTaskDelay(pdMS_TO_TICKS(120)); gpio_set_level(LED_TASK1,0);
        gpio_set_level(LED_TASK2,1); vTaskDelay(pdMS_TO_TICKS(120)); gpio_set_level(LED_TASK2,0);
        gpio_set_level(LED_TASK3,1); vTaskDelay(pdMS_TO_TICKS(120)); gpio_set_level(LED_TASK3,0);
        gpio_set_level(LED_CRITICAL,1); vTaskDelay(pdMS_TO_TICKS(120)); gpio_set_level(LED_CRITICAL,0);
    }
    ESP_LOGI(TAG,"Running — สังเกตว่า LOW_PRI จะเข้าถึงบ่อยและถือ mutex นานสุด");
}
