#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("Hello, ESP32 World!\n");
    int counter = 0;
    while (1) {
        printf("ESP32 is running... %d\n", counter++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
