// main/producer_consumer.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "PROD_CONS";

// ====== Pin Mapping ======
#define LED_PRODUCER_1 GPIO_NUM_2
#define LED_PRODUCER_2 GPIO_NUM_4
#define LED_PRODUCER_3 GPIO_NUM_5
#define LED_CONSUMER_1 GPIO_NUM_18
#define LED_CONSUMER_2 GPIO_NUM_19

// ====== Queue & Sync ======
static QueueHandle_t xProductQueue = NULL;
static SemaphoreHandle_t xPrintMutex = NULL;

// ====== Stats ======
typedef struct {
    uint32_t produced;
    uint32_t consumed;
    uint32_t dropped;
} stats_t;

static stats_t global_stats = {0, 0, 0};

// ====== Product ======
typedef struct {
    int      producer_id;
    int      product_id;
    char     product_name[30];
    uint32_t production_tick;
    int      processing_time_ms;
} product_t;

// ====== Safe printf ======
static void safe_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (xPrintMutex && xSemaphoreTake(xPrintMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        vprintf(fmt, args);
        xSemaphoreGive(xPrintMutex);
    }
    va_end(args);
}

// ====== Producer Task ======
static void producer_task(void *pvParameters) {
    int producer_id = *((int*)pvParameters);
    product_t product;
    int product_counter = 0;
    gpio_num_t led_pin;

    switch (producer_id) {
        case 1: led_pin = LED_PRODUCER_1; break;
        case 2: led_pin = LED_PRODUCER_2; break;
        case 3: led_pin = LED_PRODUCER_3; break;
        default: led_pin = LED_PRODUCER_1; break;
    }

    safe_printf("Producer %d started\n", producer_id);

    while (1) {
        product.producer_id = producer_id;
        product.product_id = product_counter++;
        snprintf(product.product_name, sizeof(product.product_name),
                 "Product-P%d-#%d", producer_id, product.product_id);
        product.production_tick = xTaskGetTickCount();
        product.processing_time_ms = 500 + (esp_random() % 2000); // 0.5–2.5s

        BaseType_t ok = xQueueSend(xProductQueue, &product, pdMS_TO_TICKS(100));
        if (ok == pdPASS) {
            global_stats.produced++;
            safe_printf("✓ Producer %d: Created %s (processing: %dms)\n",
                        producer_id, product.product_name, product.processing_time_ms);
            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(led_pin, 0);
        } else {
            global_stats.dropped++;
            safe_printf("✗ Producer %d: Queue full! Dropped %s\n",
                        producer_id, product.product_name);
        }

        int delay_ms = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ====== Consumer Task ======
static void consumer_task(void *pvParameters) {
    int consumer_id = *((int*)pvParameters);
    product_t product;
    gpio_num_t led_pin;

    switch (consumer_id) {
        case 1: led_pin = LED_CONSUMER_1; break;
        case 2: led_pin = LED_CONSUMER_2; break;
        default: led_pin = LED_CONSUMER_1; break;
    }

    safe_printf("Consumer %d started\n", consumer_id);

    while (1) {
        BaseType_t ok = xQueueReceive(xProductQueue, &product, pdMS_TO_TICKS(5000));
        if (ok == pdPASS) {
            global_stats.consumed++;
            TickType_t now = xTaskGetTickCount();
            uint32_t queue_time_ms = (uint32_t)((now - product.production_tick) * portTICK_PERIOD_MS);

            safe_printf("→ Consumer %d: Processing %s (queue time: %lums)\n",
                        consumer_id, product.product_name, (unsigned long)queue_time_ms);

            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(product.processing_time_ms));
            gpio_set_level(led_pin, 0);

            safe_printf("✓ Consumer %d: Finished %s\n", consumer_id, product.product_name);
        } else {
            safe_printf("⏰ Consumer %d: No products to process (timeout)\n", consumer_id);
        }
    }
}

// ====== Statistics Task ======
static void statistics_task(void *pvParameters) {
    safe_printf("Statistics task started\n");
    while (1) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);

        safe_printf("\n═══ SYSTEM STATISTICS ═══\n");
        safe_printf("Products Produced: %lu\n", (unsigned long)global_stats.produced);
        safe_printf("Products Consumed: %lu\n", (unsigned long)global_stats.consumed);
        safe_printf("Products Dropped:  %lu\n", (unsigned long)global_stats.dropped);
        safe_printf("Queue Backlog:     %u\n", (unsigned)q_items);

        float eff = 0.0f;
        if (global_stats.produced > 0) {
            eff = ((float)global_stats.consumed / (float)global_stats.produced) * 100.0f;
        }
        safe_printf("System Efficiency: %.1f%%\n", eff);

        printf("Queue: [");
        for (int i = 0; i < 10; i++) printf(i < (int)q_items ? "■" : "□");
        printf("]\n");
        safe_printf("═══════════════════════════\n\n");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ====== Load Balancer ======
static void load_balancer_task(void *pvParameters) {
    const int HIGH_LOAD_THRESHOLD = 8;
    safe_printf("Load balancer started\n");

    while (1) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);
        if (q_items > HIGH_LOAD_THRESHOLD) {
            safe_printf("⚠️  HIGH LOAD DETECTED! Queue size: %u\n", (unsigned)q_items);
            safe_printf("💡 Suggestion: Add more consumers or optimize processing\n");

            gpio_set_level(LED_PRODUCER_1, 1);
            gpio_set_level(LED_PRODUCER_2, 1);
            gpio_set_level(LED_PRODUCER_3, 1);
            gpio_set_level(LED_CONSUMER_1, 1);
            gpio_set_level(LED_CONSUMER_2, 1);

            vTaskDelay(pdMS_TO_TICKS(200));  // ✅ แก้บรรทัดแตกบรรทัดผิด

            gpio_set_level(LED_PRODUCER_1, 0);
            gpio_set_level(LED_PRODUCER_2, 0);
            gpio_set_level(LED_PRODUCER_3, 0);
            gpio_set_level(LED_CONSUMER_1, 0);
            gpio_set_level(LED_CONSUMER_2, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ====== GPIO Init ======
static void init_led_pins(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_PRODUCER_1) |
                        (1ULL << LED_PRODUCER_2) |
                        (1ULL << LED_PRODUCER_3) |
                        (1ULL << LED_CONSUMER_1) |
                        (1ULL << LED_CONSUMER_2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    gpio_set_level(LED_PRODUCER_1, 0);
    gpio_set_level(LED_PRODUCER_2, 0);
    gpio_set_level(LED_PRODUCER_3, 0);
    gpio_set_level(LED_CONSUMER_1, 0);
    gpio_set_level(LED_CONSUMER_2, 0);
}

// ====== app_main ======
void app_main(void) {
    ESP_LOGI(TAG, "Producer-Consumer System Lab Starting...");

    init_led_pins();

    xProductQueue = xQueueCreate(10, sizeof(product_t));
    xPrintMutex   = xSemaphoreCreateMutex();

    if (!xProductQueue || !xPrintMutex) {
        ESP_LOGE(TAG, "Failed to create queue or mutex!");
        return;
    }
    ESP_LOGI(TAG, "Queue and mutex created successfully");

    // ==== IDs ต้องเป็น static ====
    static int producer1_id = 1, producer2_id = 2, producer3_id = 3;
    static int producer4_id = 4;   // ✅ เพิ่ม Producer #4
    static int consumer1_id = 1, consumer2_id = 2;

    // ใช้ consumer2_id แบบ no-op เพื่อปิด warning เมื่อยังไม่สร้าง Consumer2
    (void)consumer2_id;            // ✅ ปิด warning unused-variable

    // ==== Producers (prio 3) ====
    xTaskCreate(producer_task, "Producer1", 3072, &producer1_id, 3, NULL);
    xTaskCreate(producer_task, "Producer2", 3072, &producer2_id, 3, NULL);
    xTaskCreate(producer_task, "Producer3", 3072, &producer3_id, 3, NULL);
    xTaskCreate(producer_task, "Producer4", 3072, &producer4_id, 3, NULL); // ✅ เพิ่ม Producer #4

    // ==== Consumers (prio 2) ====
    xTaskCreate(consumer_task, "Consumer1", 3072, &consumer1_id, 2, NULL);
    // xTaskCreate(consumer_task, "Consumer2", 3072, &consumer2_id, 2, NULL); // ✅ ปิด Consumer #2 ตามใบงาน

    // ==== Monitoring (prio 1) ====
    xTaskCreate(statistics_task, "Statistics", 3072, NULL, 1, NULL);
    xTaskCreate(load_balancer_task, "LoadBalancer", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "All tasks created. System operational. (P=4, C=1)");
}
