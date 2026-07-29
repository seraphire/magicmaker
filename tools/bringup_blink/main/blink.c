// ---------------------------------------------------------------------------
// Stage-1 bring-up test: "is the ESP alive?"
//
// Blinks the onboard RGB LED (a single WS2812 on GPIO 48) red->green->blue,
// and prints a heartbeat to the monitor. Needs NO external wiring - just the
// dev board in its sockets, powered over its own USB-C.
//
// If the LED cycles colors and the monitor shows the heartbeat, the board
// boots and runs code. Then bring the next part on.
//
// If GPIO 48 doesn't light on your particular board, try 38 (some variants).
// ---------------------------------------------------------------------------
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"

#define ONBOARD_RGB_GPIO 48

static const char *TAG = "bringup";

void app_main(void)
{
    ESP_LOGI(TAG, "=== Stage 1: ESP alive test ===");

    led_strip_config_t strip_cfg = { .strip_gpio_num = ONBOARD_RGB_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000, .flags.with_dma = false };
    led_strip_handle_t rgb;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &rgb));

    const uint8_t colors[3][3] = { {40,0,0}, {0,40,0}, {0,0,40} }; // dim R, G, B
    const char *names[3] = { "RED", "GREEN", "BLUE" };
    int i = 0;
    while (1) {
        led_strip_set_pixel(rgb, 0, colors[i][0], colors[i][1], colors[i][2]);
        led_strip_refresh(rgb);
        ESP_LOGI(TAG, "alive - %s", names[i]);
        i = (i + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
