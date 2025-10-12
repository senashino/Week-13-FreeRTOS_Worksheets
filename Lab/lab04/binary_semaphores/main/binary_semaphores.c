#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_random.h"
#include "esp_err.h"

static const char *TAG = "BINARY_SEM";

// GPIO Mapping
#define LED_PRODUCER GPIO_NUM_2
#define LED_CONSUMER GPIO_NUM_4
#define LED_TIMER    GPIO_NUM_5
#define BUTTON_PIN   GPIO_NUM_0  // BOOT button

// Semaphore handles
    static SemaphoreHandle_t xBinarySemaphore   = NULL;
static SemaphoreHandle_t xTimerSemaphore    = NULL;
static SemaphoreHandle_t xButtonSemaphore   = NULL;

// Timer handle
static gptimer_handle_t gptimer = NULL;

// Statistics struct
typedef struct {
    uint32_t signals_sent;
    uint32_t signals_received;
    uint32_t timer_events;
    uint32_t button_presses;
} semaphore_stats_t;

static semaphore_stats_t stats = {0, 0, 0, 0};

// ================= ISR Callbacks ===================
static bool IRAM_ATTR timer_callback(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xTimerSemaphore, &xHigherPriorityTaskWoken);
    return (xHigherPriorityTaskWoken == pdTRUE);
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xButtonSemaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR();
}

// ================= Producer Task ===================
static void producer_task(void *pvParameters)
{
    int event_counter = 0;
    ESP_LOGI(TAG, "Producer task started (Multiple Give Test)");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(3000)); // ทุก 3 วินาที
        event_counter++;
        ESP_LOGI(TAG, "🔥 Producer: Generating event batch #%d", event_counter);

        // ให้ semaphore 3 ครั้งติดกัน
        for (int i = 0; i < 3; i++) {
            if (xSemaphoreGive(xBinarySemaphore) == pdTRUE) {
                stats.signals_sent++;
                ESP_LOGI(TAG, "✅ Producer: Give #%d success", i + 1);
            } else {
                ESP_LOGW(TAG, "⚠️ Producer: Give #%d failed (already given)", i + 1);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_PRODUCER, 0);
    }
}

// ================= Consumer Task ===================
static void consumer_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Consumer task started - waiting for events...");
    while (1)
    {
        ESP_LOGI(TAG, "🔍 Consumer: Waiting for event...");
        if (xSemaphoreTake(xBinarySemaphore, pdMS_TO_TICKS(10000)) == pdTRUE) {
            stats.signals_received++;
            ESP_LOGI(TAG, "⚡ Consumer: Event received! Processing...");
            gpio_set_level(LED_CONSUMER, 1);
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
            gpio_set_level(LED_CONSUMER, 0);
            ESP_LOGI(TAG, "✓ Consumer: Event processed");
        } else {
            ESP_LOGW(TAG, "⏰ Consumer: Timeout waiting for event");
        }
    }
}

// ================= Timer Event Task ===================
static void timer_event_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timer event task started");
    while (1)
    {
        if (xSemaphoreTake(xTimerSemaphore, portMAX_DELAY) == pdTRUE) {
            stats.timer_events++;
            ESP_LOGI(TAG, "⏱️  Timer: Periodic event #%lu",
                     (unsigned long)stats.timer_events);
            gpio_set_level(LED_TIMER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_TIMER, 0);

            if (stats.timer_events % 5 == 0) {
                ESP_LOGI(TAG, "📊 Stats - Sent:%lu, Received:%lu, Timer:%lu, Button:%lu",
                         (unsigned long)stats.signals_sent,
                         (unsigned long)stats.signals_received,
                         (unsigned long)stats.timer_events,
                         (unsigned long)stats.button_presses);
            }
        }
    }
}

// ================= Button Event Task ===================
static void button_event_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Button event task started");
    while (1)
    {
        if (xSemaphoreTake(xButtonSemaphore, portMAX_DELAY) == pdTRUE) {
            stats.button_presses++;
            ESP_LOGI(TAG, "🔘 Button: Press detected #%lu",
                     (unsigned long)stats.button_presses);
            vTaskDelay(pdMS_TO_TICKS(300)); // debounce
            ESP_LOGI(TAG, "🚀 Button: Triggering immediate producer event");
            xSemaphoreGive(xBinarySemaphore);
            stats.signals_sent++;
        }
    }
}

// ================= Monitor Task ===================
static void monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitor started");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n═══ SEMAPHORE SYSTEM MONITOR ═══");
        ESP_LOGI(TAG, "Binary Semaphore Available: %s",
                 uxSemaphoreGetCount(xBinarySemaphore) ? "YES" : "NO");
        ESP_LOGI(TAG, "Timer Semaphore Count: %u",
                 (unsigned)uxSemaphoreGetCount(xTimerSemaphore));
        ESP_LOGI(TAG, "Button Semaphore Count: %u",
                 (unsigned)uxSemaphoreGetCount(xButtonSemaphore));

        ESP_LOGI(TAG, "Event Statistics:");
        ESP_LOGI(TAG, "  Producer Events: %lu", (unsigned long)stats.signals_sent);
        ESP_LOGI(TAG, "  Consumer Events: %lu", (unsigned long)stats.signals_received);
        ESP_LOGI(TAG, "  Timer Events:    %lu", (unsigned long)stats.timer_events);
        ESP_LOGI(TAG, "  Button Presses:  %lu", (unsigned long)stats.button_presses);

        float efficiency = stats.signals_sent > 0 ?
                           (float)stats.signals_received / (float)stats.signals_sent * 100.0f : 0.0f;
        ESP_LOGI(TAG, "  System Efficiency: %.1f%%", efficiency);
        ESP_LOGI(TAG, "══════════════════════════════\n");
    }
}

// ================= App Main ===================
void app_main(void)
{
    ESP_LOGI(TAG, "Binary Semaphores Lab - Experiment #2 Starting...");

    // --- Configure LEDs ---
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_PRODUCER) | (1ULL << LED_CONSUMER) | (1ULL << LED_TIMER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // --- Configure Button ---
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    // --- Create Semaphores ---
    xBinarySemaphore = xSemaphoreCreateBinary();
    xTimerSemaphore  = xSemaphoreCreateBinary();
    xButtonSemaphore = xSemaphoreCreateBinary();
    if (!xBinarySemaphore || !xTimerSemaphore || !xButtonSemaphore) {
        ESP_LOGE(TAG, "Failed to create semaphores!");
        return;
    }

    // --- GPIO ISR service ---
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service error: %s", esp_err_to_name(isr_err));
        return;
    }
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);

    // --- Timer setup ---
    gptimer_config_t tcfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&tcfg, &gptimer));
    gptimer_event_callbacks_t cbs = {.on_alarm = timer_callback};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    gptimer_alarm_config_t acfg = {
        .alarm_count = 8000000,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &acfg));
    ESP_ERROR_CHECK(gptimer_start(gptimer));

    ESP_LOGI(TAG, "Timer configured for 8-second intervals");

    // --- Create Tasks ---
    xTaskCreate(producer_task,     "Producer",    3072, NULL, 3, NULL);
    xTaskCreate(consumer_task,     "Consumer",    3072, NULL, 2, NULL);
    xTaskCreate(timer_event_task,  "TimerEvent",  3072, NULL, 2, NULL);
    xTaskCreate(button_event_task, "ButtonEvent", 3072, NULL, 4, NULL);
    xTaskCreate(monitor_task,      "Monitor",     3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "System operational. 💡 Press BOOT to trigger event!");
}
