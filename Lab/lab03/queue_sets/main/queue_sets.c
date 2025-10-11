
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"      // สำคัญสำหรับ esp_random() ใน IDF v5.5+
#include "driver/gpio.h"

static const char *TAG = "QUEUE_SETS_EXP2";

// ===== LED pins (ปรับตามบอร์ดได้) =====
#define LED_USER       GPIO_NUM_4
#define LED_NETWORK    GPIO_NUM_5
#define LED_TIMER      GPIO_NUM_18
#define LED_PROCESSOR  GPIO_NUM_19

// ===== Handles =====
static QueueHandle_t     xUserQueue      = NULL;
static QueueHandle_t     xNetworkQueue   = NULL;
static SemaphoreHandle_t xTimerSemaphore = NULL;
static QueueSetHandle_t  xQueueSet       = NULL;

// ===== Data structs =====
typedef struct {
    int      button_id;
    bool     pressed;
    uint32_t duration_ms;
} user_input_t;

typedef struct {
    char source[20];
    char message[100];
    int  priority;
} network_message_t;

typedef struct {
    uint32_t user_count;
    uint32_t network_count;
    uint32_t timer_count;
} message_stats_t;

static message_stats_t stats = {0};

static inline void blink_led(gpio_num_t pin, TickType_t ms)
{
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, 0);
}

// ======== Producers (ไม่มี Sensor) ========
static void user_input_task(void *pvParameters)
{
    user_input_t u;
    ESP_LOGI(TAG, "User input task started");
    while (1) {
        u.button_id   = 1 + (esp_random() % 3);
        u.pressed     = true;
        u.duration_ms = 100 + (esp_random() % 1000);
        if (xQueueSend(xUserQueue, &u, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🔘 User: Button %d pressed for %dms", u.button_id, u.duration_ms);
            blink_led(LED_USER, 100);
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000))); // 3–8s
    }
}

static void network_task(void *pvParameters)
{
    network_message_t m;
    const char* sources[]  = {"WiFi", "Bluetooth", "LoRa", "Ethernet"};
    const char* messages[] = {
        "Status update received","Configuration changed","Alert notification",
        "Data synchronization","Heartbeat signal"
    };

    ESP_LOGI(TAG, "Network task started");
    while (1) {
        strncpy(m.source,  sources[esp_random() % 4], sizeof(m.source) - 1);
        m.source[sizeof(m.source) - 1] = '\0';
        strncpy(m.message, messages[esp_random() % 5], sizeof(m.message) - 1);
        m.message[sizeof(m.message) - 1] = '\0';
        m.priority = 1 + (esp_random() % 5);

        if (xQueueSend(xNetworkQueue, &m, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🌐 Network [%s]: %s (P:%d)", m.source, m.message, m.priority);
            blink_led(LED_NETWORK, 50);
        }
        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 3000))); // 1–4s
    }
}

static void timer_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timer task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // ทุก 10 วินาที
        if (xSemaphoreGive(xTimerSemaphore) == pdPASS) {
            ESP_LOGI(TAG, "⏰ Timer: Periodic timer fired");
            blink_led(LED_TIMER, 100);
        }
    }
}

// ======== Processor (Queue Sets) ========
static void processor_task(void *pvParameters)
{
    QueueSetMemberHandle_t m;
    user_input_t u; network_message_t n;

    ESP_LOGI(TAG, "Processor task started - waiting for events (no sensor)...");
    while (1) {
        m = xQueueSelectFromSet(xQueueSet, portMAX_DELAY);
        if (!m) continue;

        gpio_set_level(LED_PROCESSOR, 1);

        if (m == xUserQueue) {
            if (xQueueReceive(xUserQueue, &u, 0) == pdPASS) {
                stats.user_count++;
                ESP_LOGI(TAG, "→ USER: Button %d (%dms)", u.button_id, u.duration_ms);
                switch (u.button_id) {
                    case 1: ESP_LOGI(TAG, "💡 Action: Toggle LED"); break;
                    case 2: ESP_LOGI(TAG, "📊 Action: Show status"); break;
                    case 3: ESP_LOGI(TAG, "⚙️  Action: Settings menu"); break;
                }
            }
        } else if (m == xNetworkQueue) {
            if (xQueueReceive(xNetworkQueue, &n, 0) == pdPASS) {
                stats.network_count++;
                ESP_LOGI(TAG, "→ NETWORK: [%s] %s (P:%d)", n.source, n.message, n.priority);
                if (n.priority >= 4) ESP_LOGW(TAG, "🚨 High priority network message!");
            }
        } else if (m == xTimerSemaphore) {
            if (xSemaphoreTake(xTimerSemaphore, 0) == pdPASS) {
                stats.timer_count++;
                ESP_LOGI(TAG, "→ TIMER: maintenance | 📈 User:%lu Network:%lu Timer:%lu",
                         (unsigned long)stats.user_count,
                         (unsigned long)stats.network_count,
                         (unsigned long)stats.timer_count);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));   // simulate processing time
        gpio_set_level(LED_PROCESSOR, 0);
    }
}

// ======== Monitor ========
static void monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n═══ SYSTEM MONITOR (No Sensor) ═══");
        ESP_LOGI(TAG, "  User Queue:    %u/3", (unsigned)uxQueueMessagesWaiting(xUserQueue));
        ESP_LOGI(TAG, "  Network Queue: %u/8", (unsigned)uxQueueMessagesWaiting(xNetworkQueue));
        ESP_LOGI(TAG, "Stats → User:%lu Network:%lu Timer:%lu\n",
                 (unsigned long)stats.user_count,
                 (unsigned long)stats.network_count,
                 (unsigned long)stats.timer_count);
    }
}

// ======== GPIO init ========
static void init_led_pins(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_USER) |
                        (1ULL << LED_NETWORK) |
                        (1ULL << LED_TIMER) |
                        (1ULL << LED_PROCESSOR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    gpio_set_level(LED_USER, 0);
    gpio_set_level(LED_NETWORK, 0);
    gpio_set_level(LED_TIMER, 0);
    gpio_set_level(LED_PROCESSOR, 0);
}

// ======== app_main ========
void app_main(void)
{
    ESP_LOGI(TAG, "Experiment 2: Queue Sets (NO SENSOR) starting...");
    init_led_pins();

    // Create members (ไม่มี Sensor)
    xUserQueue      = xQueueCreate(3, sizeof(user_input_t));
    xNetworkQueue   = xQueueCreate(8, sizeof(network_message_t));
    xTimerSemaphore = xSemaphoreCreateBinary();

    // Queue set length = 3 + 8 + 1
    const UBaseType_t qs_len = 3 + 8 + 1;
    xQueueSet = xQueueCreateSet(qs_len);

    if (!xUserQueue || !xNetworkQueue || !xTimerSemaphore || !xQueueSet) {
        ESP_LOGE(TAG, "Create queue/semaphore/set failed");
        return;
    }

    // Add to set (เฉพาะสมาชิกที่มีจริง)
    configASSERT(xQueueAddToSet(xUserQueue,      xQueueSet) == pdPASS);
    configASSERT(xQueueAddToSet(xNetworkQueue,   xQueueSet) == pdPASS);
    configASSERT(xQueueAddToSet(xTimerSemaphore, xQueueSet) == pdPASS);

    // Producers
    xTaskCreate(user_input_task, "UserInput", 2048, NULL, 3, NULL);
    xTaskCreate(network_task,    "Network",   2048, NULL, 3, NULL);
    xTaskCreate(timer_task,      "Timer",     2048, NULL, 2, NULL);

    // Processor & Monitor
    xTaskCreate(processor_task,  "Processor", 3072, NULL, 4, NULL);
    xTaskCreate(monitor_task,    "Monitor",   2048, NULL, 1, NULL);

    // Startup blink
    blink_led(LED_USER, 100);
    blink_led(LED_NETWORK, 100);
    blink_led(LED_TIMER, 100);
    blink_led(LED_PROCESSOR, 100);

    ESP_LOGI(TAG, "System operational (Sensor disabled).");
}
