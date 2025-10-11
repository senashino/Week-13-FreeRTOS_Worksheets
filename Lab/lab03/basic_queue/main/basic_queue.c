#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "QUEUE_LAB";

/* ====== Pin definition (ปรับได้ตามบอร์ด) ====== */
#define LED_SENDER   GPIO_NUM_2
#define LED_RECEIVER GPIO_NUM_4

/* ====== Queue handle ====== */
static QueueHandle_t xQueue = NULL;

/* ====== Message structure ====== */
typedef struct {
    int id;
    char message[50];
    uint32_t timestamp;  // tick count
} queue_message_t;

/* ====== Sender Task ====== */
static void sender_task(void *pvParameters)
{
    queue_message_t msg;
    int counter = 0;

    ESP_LOGI(TAG, "Sender task started");

    for (;;) {
        // เตรียมข้อมูล
        msg.id = counter++;
        snprintf(msg.message, sizeof(msg.message), "Hello from sender #%d", msg.id);
        msg.timestamp = (uint32_t)xTaskGetTickCount();

        // ส่งเข้า queue (รอสูงสุด 1000ms)
        BaseType_t ok = xQueueSend(xQueue, &msg, pdMS_TO_TICKS(1000));
        if (ok == pdPASS) {
            ESP_LOGI(TAG, "Sent: ID=%d, MSG=%s, Time=%lu",
                     msg.id, msg.message, (unsigned long)msg.timestamp);

            // กระพริบ LED sender
            gpio_set_level(LED_SENDER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_SENDER, 0);
        } else {
            ESP_LOGW(TAG, "Failed to send message (queue full?)");
        }

        // ส่งทุก 2 วินาที (ปรับตามโจทย์ทดลอง)
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ====== Receiver Task ====== */
static void receiver_task(void *pvParameters)
{
    queue_message_t rx;

    ESP_LOGI(TAG, "Receiver task started");

    for (;;) {
        // รอรับ (timeout 5s)
        BaseType_t ok = xQueueReceive(xQueue, &rx, pdMS_TO_TICKS(5000));
        if (ok == pdPASS) {
            ESP_LOGI(TAG, "Received: ID=%d, MSG=%s, Time=%lu",
                     rx.id, rx.message, (unsigned long)rx.timestamp);

            // กระพริบ LED receiver
            gpio_set_level(LED_RECEIVER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_RECEIVER, 0);

            // จำลองการประมวลผล 1.5s
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            ESP_LOGW(TAG, "No message received within timeout");
        }
    }
}

/* ====== Monitor Task ====== */
static void queue_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Queue monitor task started");

    for (;;) {
        UBaseType_t waiting = uxQueueMessagesWaiting(xQueue);
        UBaseType_t spaces  = uxQueueSpacesAvailable(xQueue);

        ESP_LOGI(TAG, "Queue Status - Messages: %u, Free spaces: %u",
                 (unsigned)waiting, (unsigned)spaces);

        // แสดง bar 5 ช่องให้ดูความแน่นแบบคร่าว ๆ
        printf("Queue: [");
        for (int i = 0; i < 5; i++) {
            if (i < (int)waiting) printf("■");
            else                  printf("□");
        }
        printf("]\n");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ====== GPIO helper ====== */
static void leds_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_SENDER) | (1ULL << LED_RECEIVER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    gpio_set_level(LED_SENDER, 0);
    gpio_set_level(LED_RECEIVER, 0);
}

/* ====== app_main ====== */
void app_main(void)
{
    ESP_LOGI(TAG, "Basic Queue Operations Lab Starting...");

    leds_init();

    // สร้าง queue ขนาดรับได้ 5 message
    xQueue = xQueueCreate(5, sizeof(queue_message_t));
    if (xQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue!");
        vTaskDelay(portMAX_DELAY);
        return;
    }
    ESP_LOGI(TAG, "Queue created successfully (size: 5 messages)");

    // สร้าง tasks
    BaseType_t ok1 = xTaskCreate(sender_task,  "Sender",  3072, NULL, 2, NULL);
    BaseType_t ok2 = xTaskCreate(receiver_task,"Receiver",3072, NULL, 1, NULL);
    BaseType_t ok3 = xTaskCreate(queue_monitor_task, "Monitor", 3072, NULL, 1, NULL);

    if (ok1 != pdPASS || ok2 != pdPASS || ok3 != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
    } else {
        ESP_LOGI(TAG, "All tasks created. Scheduler running...");
    }
}
