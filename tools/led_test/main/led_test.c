// ---------------------------------------------------------------------------
// WS2812 test - CELEBRATION ANIMATION PROTOTYPE
//
// Sequence (loops forever):
//   1) White comet chases around the RING, twice, fast.
//   2) Blackout - one beat.
//   3) Ring: green fades in.
//      Mouse face: sparkles WHITE -> BLUE -> PINK -> GREEN in step.
//   4) Everything lands on solid green, holds, then repeats.
//
// Layout (measured): ring = pixels 0-45, face = pixels 46-81.
// ---------------------------------------------------------------------------
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_random.h"
#include <math.h>

// --- layout / tuning ---------------------------------------------------------
#define LED_GPIO     4      // matches firmware (was 21)
#define TOTAL_LEDS   81     // was 82; first pixel died and was cut off
#define RING_START   0
#define RING_COUNT   45     // was 46
#define FACE_START   45     // was 46
#define FACE_COUNT   36

#define BRIGHT       175     // master brightness ceiling, 0-255

#define CHASE_TAIL   7       // comet tail length (pixels)
#define CHASE_MS     18      // per-step delay: 2 laps of 46 px ~= 0.9 s
#define BEAT_MS      400     // the blackout beat
#define GROW_FRAMES  64      // frames for fade+sparkle phase (30 ms each ~= 1.9 s)
#define HOLD_MS      1200    // solid green hold at the end
// ---------------------------------------------------------------------------

static const char *TAG = "led_test";
static led_strip_handle_t strip;

// scale a 0-255 color channel by master brightness
static inline uint8_t sc(uint32_t c) { return (uint8_t)(c * BRIGHT / 255); }

// --- phase 1: white comet around the ring, 2 laps ---------------------------
static void ring_chase(int laps)
{
    for (int step = 0; step < laps * RING_COUNT; step++) {
        int head = step % RING_COUNT;
        led_strip_clear(strip);
        for (int t = 0; t < CHASE_TAIL; t++) {
            int idx = head - t;
            while (idx < 0) idx += RING_COUNT;
            float fade = powf(0.55f, t);              // 1.0 at head, dimming back
            uint8_t v = sc((uint32_t)(255 * fade));
            led_strip_set_pixel(strip, RING_START + idx, v, v, v);
        }
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(CHASE_MS));
    }
}

// --- phase 3: green fade-in on ring + color sparkles on the face ------------
static void grow_and_sparkle(void)
{
    // sparkle colors per quarter of the phase: white -> blue -> pink -> green
    static const uint8_t colors[4][3] = {
        { 255, 255, 255 },   // white
        {  40,  90, 255 },   // blue
        { 255,  70, 140 },   // pink
        {   0, 255,  60 },   // green
    };

    for (int f = 0; f < GROW_FRAMES; f++) {
        // ring: green brightness ramps 0 -> full across the whole phase
        uint8_t g = sc(255 * f / (GROW_FRAMES - 1));
        for (int i = 0; i < RING_COUNT; i++)
            led_strip_set_pixel(strip, RING_START + i, 0, g, 0);

        // face: sparkle in the current phase color
        const uint8_t *c = colors[(f * 4) / GROW_FRAMES];
        for (int i = 0; i < FACE_COUNT; i++) {
            if ((esp_random() & 3) == 0) {            // ~1 in 4 pixels lit
                uint32_t lvl = 128 + (esp_random() % 128);   // twinkly levels
                led_strip_set_pixel(strip, FACE_START + i,
                                    sc(c[0] * lvl / 255),
                                    sc(c[1] * lvl / 255),
                                    sc(c[2] * lvl / 255));
            } else {
                led_strip_set_pixel(strip, FACE_START + i, 0, 0, 0);
            }
        }
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // land it: ring + face solid green
    for (int i = 0; i < TOTAL_LEDS; i++)
        led_strip_set_pixel(strip, i, 0, sc(255), 0);
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
}

void app_main(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = TOTAL_LEDS,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    led_strip_clear(strip);
    ESP_LOGI(TAG, "Celebration animation: ring %d px, face %d px", RING_COUNT, FACE_COUNT);

    while (1) {
        ring_chase(2);                      // 1) white chase, two laps

        led_strip_clear(strip);             // 2) blackout beat
        vTaskDelay(pdMS_TO_TICKS(BEAT_MS));

        grow_and_sparkle();                 // 3+4) green fade + sparkles -> solid

        led_strip_clear(strip);             // brief rest, then encore
        vTaskDelay(pdMS_TO_TICKS(800));
    }
}
