#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h" // เผื่อใช้ randomness เพิ่มเติมในอนาคต

static const char *TAG = "SCENARIO_LAB4";

/* ---------------- GPIO ---------------- */
#define LED_LIVING_ROOM    GPIO_NUM_2
#define LED_KITCHEN        GPIO_NUM_4
#define LED_BEDROOM        GPIO_NUM_5
#define LED_SECURITY       GPIO_NUM_18
#define LED_EMERGENCY      GPIO_NUM_19

/* ---------------- Event Groups ---------------- */
static EventGroupHandle_t sensor_events;
static EventGroupHandle_t pattern_events;

/* Sensor Events (input layer) */
#define MOTION_DETECTED_BIT     (1 << 0)
#define DOOR_OPENED_BIT         (1 << 1)
#define DOOR_CLOSED_BIT         (1 << 2)
#define LIGHT_ON_BIT            (1 << 3)
#define LIGHT_OFF_BIT           (1 << 4)
#define TEMPERATURE_HIGH_BIT    (1 << 5)
#define TEMPERATURE_LOW_BIT     (1 << 6)
#define SOUND_DETECTED_BIT      (1 << 7)
#define PRESENCE_CONFIRMED_BIT  (1 << 8)

/* Pattern Events (output layer) */
#define PATTERN_NORMAL_ENTRY_BIT    (1 << 0)
#define PATTERN_BREAK_IN_BIT        (1 << 1)
#define PATTERN_GOODNIGHT_BIT       (1 << 2)
#define PATTERN_WAKE_UP_BIT         (1 << 3)
#define PATTERN_LEAVING_BIT         (1 << 4)
#define PATTERN_RETURNING_BIT       (1 << 5)

/* ---------------- Device / System State ---------------- */
typedef enum {
    HOME_STATE_IDLE = 0,
    HOME_STATE_OCCUPIED,
    HOME_STATE_AWAY,
    HOME_STATE_SECURITY_ARMED,
    HOME_STATE_SLEEP,
    HOME_STATE_EMERGENCY
} home_state_t;

static const char* state_name(home_state_t s){
    switch(s){
        case HOME_STATE_IDLE: return "Idle";
        case HOME_STATE_OCCUPIED: return "Occupied";
        case HOME_STATE_AWAY: return "Away";
        case HOME_STATE_SECURITY_ARMED: return "Security Armed";
        case HOME_STATE_SLEEP: return "Sleep";
        case HOME_STATE_EMERGENCY: return "Emergency";
        default: return "Unknown";
    }
}

typedef struct {
    bool living_room;
    bool kitchen;
    bool bedroom;
    bool security_armed;
    bool emergency;
    home_state_t state;
} smart_home_t;

static smart_home_t SH = {0};

/* ---------------- Event History ---------------- */
#define EVENT_HISTORY_SIZE 48
typedef struct {
    EventBits_t bits;
    uint64_t    t_us;
} event_record_t;

static event_record_t hist[EVENT_HISTORY_SIZE];
static volatile int   hist_head = 0;

static inline void add_event(EventBits_t b){
    hist[hist_head].bits = b;
    hist[hist_head].t_us = esp_timer_get_time();
    hist_head = (hist_head + 1) % EVENT_HISTORY_SIZE;
}

/* ---------------- Patterns ---------------- */
typedef struct {
    const char* name;
    EventBits_t seq[4];   // end with 0
    uint32_t    window_ms;
    EventBits_t result_bit;
    // เงื่อนไขเสริมแบบง่าย (เช่น ต้อง armed / ต้องอยู่ใน SLEEP)
    bool        require_armed;
    bool        require_sleep;
} event_pattern_t;

static const event_pattern_t patterns[] = {
    { "Normal Entry",  { DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0 }, 10000, PATTERN_NORMAL_ENTRY_BIT,  false, false },
    { "Break-in",      { DOOR_OPENED_BIT, MOTION_DETECTED_BIT, 0, 0 },               5000,  PATTERN_BREAK_IN_BIT,      true,  false },
    { "Goodnight",     { LIGHT_OFF_BIT, MOTION_DETECTED_BIT, LIGHT_OFF_BIT, 0 },     30000, PATTERN_GOODNIGHT_BIT,     false, false },
    { "Wake-up",       { MOTION_DETECTED_BIT, LIGHT_ON_BIT, 0, 0 },                  5000,  PATTERN_WAKE_UP_BIT,       false, true  },
    { "Leaving",       { LIGHT_OFF_BIT, DOOR_OPENED_BIT, DOOR_CLOSED_BIT, 0 },       15000, PATTERN_LEAVING_BIT,       false, false },
    { "Returning",     { DOOR_OPENED_BIT, MOTION_DETECTED_BIT, DOOR_CLOSED_BIT, 0 }, 8000,  PATTERN_RETURNING_BIT,      true,  false },
};
#define NUM_PATTERNS (sizeof(patterns)/sizeof(patterns[0]))

/* ---------------- Actions ---------------- */
static void apply_outputs(void){
    gpio_set_level(LED_LIVING_ROOM, SH.living_room);
    gpio_set_level(LED_KITCHEN,     SH.kitchen);
    gpio_set_level(LED_BEDROOM,     SH.bedroom);
    gpio_set_level(LED_SECURITY,    SH.security_armed);
    gpio_set_level(LED_EMERGENCY,   SH.emergency);
}

static void to_state(home_state_t s){
    ESP_LOGI(TAG, "🏠 State: %s → %s", state_name(SH.state), state_name(s));
    SH.state = s;
}

static void act_normal_entry(void){
    ESP_LOGI(TAG, "🏠 Normal Entry → Welcome home");
    SH.living_room = true;
    SH.security_armed = false;
    SH.emergency = false;
    to_state(HOME_STATE_OCCUPIED);
    apply_outputs();
}

static void act_break_in(void){
    ESP_LOGW(TAG, "🚨 Break-in detected!");
    SH.emergency = true;
    to_state(HOME_STATE_EMERGENCY);
    apply_outputs();
}

static void act_goodnight(void){
    ESP_LOGI(TAG, "🌙 Goodnight routine");
    SH.living_room = false;
    SH.kitchen = false;
    SH.bedroom = true;
    to_state(HOME_STATE_SLEEP);
    apply_outputs();
}

static void act_wakeup(void){
    ESP_LOGI(TAG, "☀️ Wake-up routine");
    SH.bedroom = true;
    SH.kitchen = true;
    to_state(HOME_STATE_OCCUPIED);
    apply_outputs();
}

static void act_leaving(void){
    ESP_LOGI(TAG, "🚪 Leaving home");
    SH.living_room = SH.kitchen = SH.bedroom = false;
    SH.security_armed = true; // arm หลังออก
    to_state(HOME_STATE_SECURITY_ARMED);
    apply_outputs();
}

static void act_returning(void){
    ESP_LOGI(TAG, "🔓 Returning home (disarm)");
    SH.security_armed = false;
    to_state(HOME_STATE_OCCUPIED);
    apply_outputs();
}

/* ---------------- Matcher ---------------- */
static bool match_pattern(const event_pattern_t* p){
    if (!p) return false;

    // เงื่อนไขเสริม
    if (p->require_armed && !SH.security_armed) return false;
    if (p->require_sleep && SH.state != HOME_STATE_SLEEP) return false;

    EventBits_t need[4] = { p->seq[0], p->seq[1], p->seq[2], p->seq[3] };
    if (need[0] == 0) return false;

    uint64_t now = esp_timer_get_time();
    uint64_t win = (uint64_t)p->window_ms * 1000ULL;

    // เก็บเหตุการณ์ใน window เรียงเก่า→ใหม่
    event_record_t buf[EVENT_HISTORY_SIZE];
    int cnt = 0;
    for (int i=0;i<EVENT_HISTORY_SIZE;i++){
        int idx = (hist_head - EVENT_HISTORY_SIZE + i + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
        if (hist[idx].t_us == 0) continue;
        if (now - hist[idx].t_us <= win) buf[cnt++] = hist[idx];
    }
    if (cnt == 0) return false;

    int want = 0;
    for (int i=0;i<cnt && need[want]!=0;i++){
        if (buf[i].bits & need[want]) want++;
    }
    return (need[want]==0);
}

/* ---------------- Pattern Engine ---------------- */
static void pattern_engine_task(void *arg){
    ESP_LOGI(TAG, "🧠 Pattern engine started");
    while (1){
        EventBits_t s = xEventGroupWaitBits(
            sensor_events, 0x1FF, pdFALSE, pdFALSE, portMAX_DELAY);

        // เก็บทุกบิตที่เกิดขึ้น
        for (int b=0;b<=8;b++){
            EventBits_t m = (1<<b);
            if (s & m) add_event(m);
        }

        // ตรวจลิสต์แพทเทิร์น
        for (int i=0;i<NUM_PATTERNS;i++){
            if (match_pattern(&patterns[i])){
                ESP_LOGI(TAG, "🎯 Pattern matched: %s", patterns[i].name);
                xEventGroupSetBits(pattern_events, patterns[i].result_bit);

                switch (patterns[i].result_bit){
                    case PATTERN_NORMAL_ENTRY_BIT:  act_normal_entry();  break;
                    case PATTERN_BREAK_IN_BIT:      act_break_in();      break;
                    case PATTERN_GOODNIGHT_BIT:     act_goodnight();     break;
                    case PATTERN_WAKE_UP_BIT:       act_wakeup();        break;
                    case PATTERN_LEAVING_BIT:       act_leaving();       break;
                    case PATTERN_RETURNING_BIT:     act_returning();     break;
                    default: break;
                }

                // กันยิงซ้ำในหน้าต่างเดิม
                xEventGroupClearBits(sensor_events,
                    DOOR_OPENED_BIT|DOOR_CLOSED_BIT|MOTION_DETECTED_BIT|LIGHT_ON_BIT|LIGHT_OFF_BIT);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- Helper: push sensor event + wait ---------------- */
static void push(EventBits_t e, uint32_t ms, const char* msg){
    if (msg) ESP_LOGI(TAG, "↳ %s", msg);
    xEventGroupSetBits(sensor_events, e);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------------- Scenario Runner ----------------
   จำลองเหตุการณ์เรียงลำดับ:
   1) Leaving home
   2) Break-in (while armed)
   3) Goodnight routine
   4) Returning home
--------------------------------------------------- */
static void scenario_runner_task(void *arg){
    ESP_LOGI(TAG, "🎬 Scenario runner started");
    SH.state = HOME_STATE_IDLE;
    apply_outputs();

    while (1){
        /* SCENARIO 1: Leaving home */
        ESP_LOGI(TAG, "\n===== Scenario 1: Leaving Home =====");
        // สมมุติเริ่มจาก OCCUPIED: เปิดไฟห้องนั่งเล่น → ดับไฟ → เปิด/ปิดประตู
        SH.living_room = true; apply_outputs();
        vTaskDelay(pdMS_TO_TICKS(800));
        push(LIGHT_OFF_BIT,  500, "User turns OFF lights");
        push(DOOR_OPENED_BIT,600, "Door opened");
        push(DOOR_CLOSED_BIT,600, "Door closed");
        // Pattern engine จะเรียก act_leaving() → Security Armed
        vTaskDelay(pdMS_TO_TICKS(1500));

        /* SCENARIO 2: Break-in while armed */
        ESP_LOGI(TAG, "\n===== Scenario 2: Break-in Detection =====");
        // ให้แน่ใจว่า armed แล้ว
        SH.security_armed = true; to_state(HOME_STATE_SECURITY_ARMED); apply_outputs();
        vTaskDelay(pdMS_TO_TICKS(600));
        push(DOOR_OPENED_BIT, 500, "Intruder opens door");
        push(MOTION_DETECTED_BIT, 400, "Motion detected inside");
        // Pattern engine จะเรียก act_break_in() → Emergency
        vTaskDelay(pdMS_TO_TICKS(3000));
        // เคลียร์ emergency (เดโม่)
        SH.emergency = false; apply_outputs();
        to_state(HOME_STATE_AWAY);

        /* SCENARIO 3: Goodnight routine */
        ESP_LOGI(TAG, "\n===== Scenario 3: Goodnight Routine =====");
        // ถือว่าผู้ใช้กลับมาอยู่บ้านก่อน (normal entry)
        SH.security_armed = false; to_state(HOME_STATE_OCCUPIED); apply_outputs();
        vTaskDelay(pdMS_TO_TICKS(800));
        push(LIGHT_OFF_BIT,       600, "Turn off lights");
        push(MOTION_DETECTED_BIT, 600, "Last-minute motion");
        push(LIGHT_OFF_BIT,       600, "Ensure all lights off");
        // Pattern engine → act_goodnight() → Sleep
        vTaskDelay(pdMS_TO_TICKS(2000));
        // จำลอง wake-up
        push(MOTION_DETECTED_BIT, 500, "Morning motion");
        push(LIGHT_ON_BIT,        500, "Turn on kitchen/bedroom lights");
        // Pattern engine → act_wakeup() → Occupied
        vTaskDelay(pdMS_TO_TICKS(1500));

        /* SCENARIO 4: Returning home (from armed) */
        ESP_LOGI(TAG, "\n===== Scenario 4: Returning Home =====");
        SH.security_armed = true; to_state(HOME_STATE_SECURITY_ARMED); apply_outputs();
        vTaskDelay(pdMS_TO_TICKS(800));
        push(DOOR_OPENED_BIT,     500, "Owner opens door");
        push(MOTION_DETECTED_BIT, 500, "Owner moves inside");
        push(DOOR_CLOSED_BIT,     500, "Door closed");
        // Pattern engine → act_returning() → disarm + Occupied
        vTaskDelay(pdMS_TO_TICKS(2500));

        ESP_LOGI(TAG, "✅ Completed all scenarios. Restarting in 5s...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------------- Monitor ---------------- */
static void monitor_task(void *arg){
    while (1){
        EventBits_t s = xEventGroupGetBits(sensor_events);
        EventBits_t p = xEventGroupGetBits(pattern_events);
        ESP_LOGI(TAG, "📊 State=%s  L:%d K:%d B:%d  Arm:%d  Emg:%d  S=0x%03X P=0x%03X Free=%d",
                 state_name(SH.state), SH.living_room, SH.kitchen, SH.bedroom,
                 SH.security_armed, SH.emergency, (unsigned)s, (unsigned)p, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ---------------- App Main ---------------- */
void app_main(void){
    ESP_LOGI(TAG, "🚀 Lab 3 - Experiment 4: Real-world Scenarios (ready)");

    // GPIO init
    gpio_set_direction(LED_LIVING_ROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_KITCHEN,     GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BEDROOM,     GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SECURITY,    GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_EMERGENCY,   GPIO_MODE_OUTPUT);
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 0);
    gpio_set_level(LED_SECURITY, 0);
    gpio_set_level(LED_EMERGENCY, 0);

    // Event groups
    sensor_events  = xEventGroupCreate();
    pattern_events = xEventGroupCreate();

    // Tasks
    xTaskCreate(pattern_engine_task,  "PatternEngine", 4096, NULL, 8, NULL);
    xTaskCreate(scenario_runner_task, "ScenarioRun",   4096, NULL, 7, NULL);
    xTaskCreate(monitor_task,         "Monitor",       2048, NULL, 3, NULL);
}
