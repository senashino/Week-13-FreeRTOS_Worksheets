# Lab 3: Stack Monitoring และ Debugging

## วัตถุประสงค์
เรียนรู้การจัดการและตรวจสอบ Stack Memory ใน FreeRTOS เพื่อป้องกัน Stack Overflow

## เวลาที่ใช้
30 นาที

## อุปกรณ์ที่ต้องใช้
- ESP32 Development Board
- LED 2 ดวง (สำหรับสถานะ)

## ทฤษฎี Stack Management

### Stack ใน FreeRTOS
- แต่ละ Task มี Stack เป็นของตัวเอง
- Stack เก็บ local variables, function parameters, return addresses
- Stack Overflow เป็นปัญหาที่พบบ่อยใน embedded systems
- FreeRTOS มี built-in stack monitoring features

## ขั้นตอนการทดลอง

### Step 1: Basic Stack Monitoring (15 นาที)

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#define LED_OK GPIO_NUM_2       // Stack OK indicator
#define LED_WARNING GPIO_NUM_4  // Stack warning indicator

static const char *TAG = "STACK_MONITOR";

// Stack monitoring configuration
#define STACK_WARNING_THRESHOLD 512  // bytes
#define STACK_CRITICAL_THRESHOLD 256 // bytes

// Task handles for monitoring
TaskHandle_t light_task_handle = NULL;
TaskHandle_t medium_task_handle = NULL;
TaskHandle_t heavy_task_handle = NULL;

// Stack monitoring task
void stack_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack Monitor Task started");
    
    while (1) {
        ESP_LOGI(TAG, "\n=== STACK USAGE REPORT ===");
        
        // Monitor all tasks
        TaskHandle_t tasks[] = {
            light_task_handle,
            medium_task_handle,
            heavy_task_handle,
            xTaskGetCurrentTaskHandle() // Monitor itself
        };
        
        const char* task_names[] = {
            "LightTask",
            "MediumTask", 
            "HeavyTask",
            "StackMonitor"
        };
        
        bool stack_warning = false;
        bool stack_critical = false;
        
        for (int i = 0; i < 4; i++) {
            if (tasks[i] != NULL) {
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(tasks[i]);
                uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
                
                ESP_LOGI(TAG, "%s: %d bytes remaining", task_names[i], stack_bytes);
                
                if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
                    ESP_LOGE(TAG, "CRITICAL: %s stack very low!", task_names[i]);
                    stack_critical = true;
                } else if (stack_bytes < STACK_WARNING_THRESHOLD) {
                    ESP_LOGW(TAG, "WARNING: %s stack low", task_names[i]);
                    stack_warning = true;
                }
            }
        }
        
        // Update LED indicators
        if (stack_critical) {
            // Blink warning LED rapidly
            for (int i = 0; i < 10; i++) {
                gpio_set_level(LED_WARNING, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_set_level(LED_WARNING, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            gpio_set_level(LED_OK, 0);
        } else if (stack_warning) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }
        
        // System-wide memory info
        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %d bytes", esp_get_minimum_free_heap_size());
        
        vTaskDelay(pdMS_TO_TICKS(3000)); // Monitor every 3 seconds
    }
}

// Light task - minimal stack usage
void light_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    
    int counter = 0;
    
    while (1) {
        // Minimal local variables
        counter++;
        
        ESP_LOGI(TAG, "Light task cycle: %d", counter);
        
        // Check own stack
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Light task stack: %d bytes", stack_remaining * sizeof(StackType_t));
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// Medium task - moderate stack usage  
void medium_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");
    
    while (1) {
        // Medium-sized local arrays
        char buffer[256];
        int numbers[50];
        
        // Fill arrays
        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        
        for (int i = 0; i < 50; i++) {
            numbers[i] = i * i;
        }
        
        ESP_LOGI(TAG, "Medium task: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);
        
        // Check stack usage
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGD(TAG, "Medium task stack: %d bytes", stack_remaining * sizeof(StackType_t));
        
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// Heavy task - high stack usage (potential overflow)
void heavy_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Heavy Stack Task started (high usage - watch for overflow!)");
    
    int cycle = 0;
    
    while (1) {
        cycle++;
        
        // Large local arrays - this might cause stack overflow!
        char large_buffer[1024];
        int large_numbers[200];
        char another_buffer[512];
        
        ESP_LOGW(TAG, "Heavy task cycle %d: Using large stack arrays", cycle);
        
        // Fill arrays to actually use the memory
        memset(large_buffer, 'X', sizeof(large_buffer) - 1);
        large_buffer[sizeof(large_buffer) - 1] = '\0';
        
        for (int i = 0; i < 200; i++) {
            large_numbers[i] = i * cycle;
        }
        
        snprintf(another_buffer, sizeof(another_buffer), 
                 "Cycle %d with large data processing", cycle);
        
        ESP_LOGI(TAG, "Heavy task: %s", another_buffer);
        ESP_LOGI(TAG, "Large buffer length: %d", strlen(large_buffer));
        ESP_LOGI(TAG, "Last number: %d", large_numbers[199]);
        
        // Check stack - this should show very low remaining stack
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
        
        if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
            ESP_LOGE(TAG, "DANGER: Heavy task stack critically low: %d bytes!", stack_bytes);
        } else {
            ESP_LOGW(TAG, "Heavy task stack: %d bytes remaining", stack_bytes);
        }
        
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

// Recursive function to demonstrate stack overflow
void recursive_function(int depth, char *buffer)
{
    // Each recursion uses stack space
    char local_array[100];
    
    // Fill local array
    snprintf(local_array, sizeof(local_array), "Recursion depth: %d", depth);
    
    ESP_LOGI(TAG, "%s", local_array);
    
    // Check stack before going deeper
    UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
    uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
    
    ESP_LOGI(TAG, "Depth %d: Stack remaining: %d bytes", depth, stack_bytes);
    
    if (stack_bytes < 200) {
        ESP_LOGE(TAG, "Stopping recursion at depth %d - stack too low!", depth);
        return;
    }
    
    if (depth < 20) {  // Limit recursion to prevent complete stack overflow
        vTaskDelay(pdMS_TO_TICKS(500));
        recursive_function(depth + 1, buffer);
    }
}

// Task to demonstrate recursion and stack usage
void recursion_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Recursion Demo Task started");
    
    while (1) {
        ESP_LOGW(TAG, "=== STARTING RECURSION DEMO ===");
        
        char shared_buffer[200];
        recursive_function(1, shared_buffer);
        
        ESP_LOGW(TAG, "=== RECURSION DEMO COMPLETED ===");
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // Wait 10 seconds before next demo
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Stack Monitoring Demo ===");
    
    // GPIO Configuration
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "LED Indicators:");
    ESP_LOGI(TAG, "GPIO2 = Stack OK, GPIO4 = Stack Warning/Critical");
    
    ESP_LOGI(TAG, "Creating tasks with different stack sizes...");
    
    // Create tasks with different stack allocations
    BaseType_t result;
    
    // Light task - small stack (1KB)
    result = xTaskCreate(light_stack_task, "LightTask", 1024, NULL, 2, &light_task_handle);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create light task");
    }
    
    // Medium task - medium stack (2KB) 
    result = xTaskCreate(medium_stack_task, "MediumTask", 2048, NULL, 2, &medium_task_handle);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create medium task");
    }
    
    // Heavy task - same stack as medium but uses more (will show warnings)
    result = xTaskCreate(heavy_stack_task, "HeavyTask", 2048, NULL, 2, &heavy_task_handle);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heavy task");
    }
    
    // Recursion demo - moderate stack
    result = xTaskCreate(recursion_demo_task, "RecursionDemo", 3072, NULL, 1, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recursion demo task");
    }
    
    // Stack monitor - needs larger stack for its own operations
    result = xTaskCreate(stack_monitor_task, "StackMonitor", 4096, NULL, 3, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stack monitor task");
    }
    
    ESP_LOGI(TAG, "All tasks created. Monitor will report every 3 seconds.");
    ESP_LOGW(TAG, "Watch for stack warnings from Heavy Task!");
}
```

### Step 2: Stack Overflow Detection (10 นาที)

เพิ่มการตรวจจับ Stack Overflow:

```c
// เพิ่มใน main file

// Stack overflow hook function
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_LOGE("STACK_OVERFLOW", "Task %s has overflowed its stack!", pcTaskName);
    ESP_LOGE("STACK_OVERFLOW", "System will restart...");
    
    // Blink warning LED rapidly before restart
    for (int i = 0; i < 20; i++) {
        gpio_set_level(LED_WARNING, 1);
        vTaskDelay(pdMS_TO_TICKS(25));
        gpio_set_level(LED_WARNING, 0);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    
    // Force restart
    esp_restart();
}

// เพิ่มใน sdkconfig หรือ menuconfig:
// CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2 (ตรวจสอบแบบเต็ม)
```

### Step 3: Stack Optimization (5 นาที)

```c
// Optimized version ของ heavy task
void optimized_heavy_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Optimized Heavy Task started");
    
    // ใช้ heap แทน stack สำหรับ large data
    char *large_buffer = malloc(1024);
    int *large_numbers = malloc(200 * sizeof(int));
    char *another_buffer = malloc(512);
    
    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "Failed to allocate heap memory");
        free(large_buffer);
        free(large_numbers);
        free(another_buffer);
        vTaskDelete(NULL);
        return;
    }
    
    int cycle = 0;
    
    while (1) {
        cycle++;
        
        ESP_LOGI(TAG, "Optimized task cycle %d: Using heap instead of stack", cycle);
        
        // ใช้ heap memory
        memset(large_buffer, 'Y', 1023);
        large_buffer[1023] = '\0';
        
        for (int i = 0; i < 200; i++) {
            large_numbers[i] = i * cycle;
        }
        
        snprintf(another_buffer, 512, "Optimized cycle %d", cycle);
        
        // Stack usage should be much lower now
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Optimized task stack: %d bytes remaining", 
                 stack_remaining * sizeof(StackType_t));
        
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
    
    // Clean up (จริงๆ แล้วจุดนี้จะไม่ถูกเรียก)
    free(large_buffer);
    free(large_numbers);
    free(another_buffer);
}
```

## การทดสอบและวิเคราะห์

### การสังเกต
1. **LED GPIO2** - สว่างเมื่อ stack ทุก task ปลอดภัย
2. **LED GPIO4** - สว่าง/กระพริบเมื่อมี stack warning/critical
3. **Serial Monitor** - แสดงรายงาน stack usage ทุก 3 วินาที

### การวิเคราะห์ผลลัพธ์
- **Light Task** - ใช้ stack น้อย มี remaining มาก
- **Medium Task** - ใช้ stack ปานกลาง
- **Heavy Task** - ใช้ stack มาก อาจมี warning
- **Recursion Demo** - แสดง stack usage ที่เพิ่มขึ้นตาม depth

## แบบฝึกหัด

### Exercise 1: Stack Size Optimization

```c
// ทดสอบ stack size ต่างๆ
void test_stack_sizes(void)
{
    // ทดสอบสร้าง task ด้วย stack sizes ต่างกัน
    uint32_t test_sizes[] = {512, 1024, 2048, 4096};
    
    for (int i = 0; i < 4; i++) {
        char task_name[20];
        snprintf(task_name, sizeof(task_name), "Test%d", test_sizes[i]);
        
        BaseType_t result = xTaskCreate(heavy_stack_task, task_name, test_sizes[i], NULL, 1, NULL);
        
        ESP_LOGI(TAG, "Task with %d bytes stack: %s", 
                 test_sizes[i], result == pdPASS ? "Created" : "Failed");
    }
}
```

### Exercise 2: Dynamic Stack Monitoring

```c
void dynamic_stack_monitor(TaskHandle_t task_handle, const char* task_name)
{
    static UBaseType_t previous_remaining = 0;
    
    UBaseType_t current_remaining = uxTaskGetStackHighWaterMark(task_handle);
    
    if (previous_remaining != 0) {
        if (current_remaining < previous_remaining) {
            ESP_LOGW(TAG, "%s stack usage increased by %d bytes", 
                     task_name, 
                     (previous_remaining - current_remaining) * sizeof(StackType_t));
        }
    }
    
    previous_remaining = current_remaining;
}
```

## คำถามสำหรับวิเคราะห์

1. Task ไหนใช้ stack มากที่สุด? เพราะอะไร?
    // HeavyTask ใช้ stack มากที่สุด เพราะมีการประกาศตัวแปรขนาดใหญ่ภายในฟังก์ชัน
2. การใช้ heap แทน stack มีข้อดีอย่างไร?
    // การใช้ heap (malloc/free) จะช่วยลดการใช้หน่วยความจำของ stack เพราะหน่วยความจำขนาดใหญ่ถูกจัดสรรใน heap ซึ่งมีพื้นที่รวมมากกว่าและไม่จำกัดตามขนาด stack ของแต่ละ task
3. Stack overflow เกิดขึ้นเมื่อไหร่และทำอย่างไรป้องกัน?
    // เกิดขึ้นเมื่อ task ใช้หน่วยความจำ stack เกินขนาดที่กำหนดไว้ เช่น การประกาศตัวแปรใหญ่เกินไป หรือ recursion ลึกเกินไป
    // วิธีป้องกัน: ตั้งค่า stack size ให้เพียงพอ, ใช้ heap สำหรับข้อมูลขนาดใหญ่, เปิดการตรวจจับ stack overflow (CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2), และตรวจสอบค่าคงเหลือของ stack เป็นระยะ
4. การตั้งค่า stack size ควรพิจารณาจากอะไร?
    // ควรพิจารณาจากลักษณะการทำงานของ task เช่น จำนวนตัวแปรภายใน, การเรียกฟังก์ชันซ้อนกัน, ความถี่ในการใช้หน่วยความจำ, และเผื่อพื้นที่สำหรับ worst-case เพื่อป้องกัน overflow
5. Recursion ส่งผลต่อ stack usage อย่างไร?
    // Recursion ใช้ stack เพิ่มขึ้นตามระดับความลึกของการเรียกฟังก์ชัน เพราะแต่ละชั้นของการเรียกจะสร้างตัวแปร local และข้อมูลการ return ใหม่บน stack ทำให้หน่วยความจำลดลงอย่างรวดเร็วและเสี่ยงต่อการเกิด stack overflow

## ผลการทดลองที่คาดหวัง

| Task | Stack Size | Usage Pattern | Warning Level |
|------|------------|---------------|---------------|
| Light | 1024 bytes | 200-400 bytes used | Safe |
| Medium | 2048 bytes | 600-800 bytes used | Safe |
| Heavy | 2048 bytes | 1400-1800 bytes used | Warning |
| Recursion | 3072 bytes | Varies by depth | Monitor |
![alt text](image.png)

## การแก้ไขปัญหา Stack Overflow

### วิธีการป้องกัน:
1. **วางแผน Stack Size** - คำนวณ worst-case usage
2. **ใช้ Heap สำหรับ Large Data** - malloc/free แทน local arrays
3. **หลีกเลี่ยง Deep Recursion** - ใช้ iterative algorithms
4. **Monitor Continuously** - ตรวจสอบ stack usage เป็นประจำ
5. **Enable Stack Checking** - ใช้ FreeRTOS stack overflow detection

### Configuration ที่แนะนำ:
```c
// ใน sdkconfig
CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2
CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
```

## บทสรุป

การทดลองนี้แสดงให้เห็นถึง:
1. **Stack Management** - การจัดการ stack อย่างมีประสิทธิภาพ
2. **Monitoring Techniques** - เทคนิคการตรวจสอบ stack
3. **Overflow Prevention** - การป้องกัน stack overflow
4. **Optimization Strategies** - กลยุทธ์การ optimize stack usage
5. **Debugging Tools** - เครื่องมือสำหรับ debug stack problems

**Best Practices:**
- ออกแบบ stack size อย่างรอบคอบ
- Monitor stack usage เป็นประจำ
- ใช้ heap สำหรับ large temporary data
- Enable stack overflow checking
- Test worst-case scenarios