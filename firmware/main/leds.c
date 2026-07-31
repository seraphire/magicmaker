#include "leds.h"
#include "config.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "leds";
static led_strip_handle_t s_strip = NULL;

// Physical layout, per device. Every build differs (strand cut to fit, a dead
// pixel snipped out), and one OTA image serves several units - so these are
// runtime settings seeded from the config.h defaults, not compile-time counts.
// Buffers are sized to the *_MAX ceilings so these can change without a rebuild.
static int  s_ring       = RING_LED_COUNT;
static int  s_mickey     = MICKEY_LED_COUNT;
static int  s_total      = RING_LED_COUNT + MICKEY_LED_COUNT;
static bool s_ring_first = RING_FIRST;

// Idle-glow colour (0x00RRGGBB), configurable; default the classic blue.
static uint8_t s_idle_r = 0, s_idle_g = 0, s_idle_b = 255;

void leds_set_layout(int ring, int mickey, bool ring_first)
{
    if (ring   < 1) ring   = 1;
    if (mickey < 0) mickey = 0;
    if (ring   > RING_LED_MAX)   ring   = RING_LED_MAX;
    if (mickey > MICKEY_LED_MAX) mickey = MICKEY_LED_MAX;
    s_ring = ring; s_mickey = mickey; s_total = ring + mickey;
    s_ring_first = ring_first;
    ESP_LOGI(TAG, "layout: %d ring + %d face = %d px (%s first)",
             s_ring, s_mickey, s_total, s_ring_first ? "ring" : "face");
}

void leds_set_idle_color(uint32_t rgb)
{
    s_idle_r = (rgb >> 16) & 0xFF;
    s_idle_g = (rgb >> 8)  & 0xFF;
    s_idle_b =  rgb        & 0xFF;
}

// Frame counters advanced once per *_step() call (~50 fps from the main loop).
static uint32_t s_reward_frame = 0;
static uint32_t s_idle_frame   = 0;

// Optional "cut this short" check, polled by the blocking animations at every
// frame boundary (see anim_wait). The app sets it so a new band tap can abandon
// a long celebration instead of making you wait it out.
static bool (*s_abort_check)(void) = NULL;

void leds_set_abort_check(bool (*fn)(void)) { s_abort_check = fn; }

// Frame pacing for the blocking animations. Sleeps `ms`, then reports whether
// the animation should give up its remaining frames. Every vTaskDelay in the
// animations goes through here, so abort latency is one frame.
static bool anim_wait(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
    return s_abort_check && s_abort_check();
}

// ---------------------------------------------------------------------------
// Map a logical position within a segment to a physical index on the chain.
// The ring occupies one contiguous block, Mickey the other; RING_FIRST in
// config.h says which block comes first.
// ---------------------------------------------------------------------------
static inline int ring_px(int i)
{
    return s_ring_first ? i : s_mickey + i;
}

static inline int mickey_px(int i)
{
    return s_ring_first ? s_ring + i : i;
}

// Scale a 0-255 channel by the global brightness ceiling.
static inline uint8_t dim(uint32_t c) { return (uint8_t)(c * LED_BRIGHTNESS / 255); }

// ---------------------------------------------------------------------------
// The strip has exactly one writer at a time.
//
// The main loop steps the idle glow every frame, and the CLI plays whole
// animations from the console task (cli.c). Two tasks sharing one RMT channel
// race: led_strip_refresh() enables the channel, transmits, then disables it,
// so if the other task enables first this one fails with "channel not in init
// state" and that frame is silently dropped. Frames going missing at random is
// exactly what a flicker looks like - and they also fight over the pixels,
// idle blue painting over a celebration mid-show.
//
// Recursive, because the blocking animations hold the lock for their whole run
// and still emit frames from inside it. That's deliberate: while a show is
// playing, an idle step from the main loop should wait its turn rather than
// interleave with it.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = NULL;

static inline void leds_lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static inline void leds_unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static void show(void)
{
    leds_lock();
    led_strip_refresh(s_strip);
    leds_unlock();
}

void leds_acquire(void) { leds_lock(); }
void leds_release(void) { leds_unlock(); }

void leds_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) ESP_LOGE(TAG, "no mutex for the strip; frames may be dropped");

    led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds       = TOTAL_LED_MAX,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz, same as the radio project
        // NOT with_dma. It looks like the obvious fix for frames corrupted by CPU
        // contention, but led_strip_rmt_refresh() enables the channel, transmits,
        // waits, then disables it for every frame - and with DMA that cycle
        // doesn't complete, so the channel is left enabled and every later frame
        // fails with "channel not in init state". The LEDs stop updating at all.
        .flags.with_dma = false,
        // Instead, give the refill ISR more room. A WS2812 bit is one symbol, so
        // the 64-symbol default is under three pixels - the ISR has to top the
        // buffer up every ~80 us or the strip sees a gap, latches early, and the
        // rest of the frame lands on the wrong pixels. That reads as a flicker,
        // and MP3 decoding plus its flash reads is exactly the load that causes
        // it. 192 symbols is all four of the S3's TX blocks, tripling the time
        // the ISR has to get there.
        .mem_block_symbols = 192,
    };
    // Fall back rather than abort: mem_block_symbols has to be a multiple of the
    // per-channel block size and the blocks have to be free, and a boot loop over
    // a performance tweak would be a poor trade.
    esp_err_t rc = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "RMT with %d symbols failed (%s); falling back to the default",
                 rmt_config.mem_block_symbols, esp_err_to_name(rc));
        rmt_config.mem_block_symbols = 0;
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    } else {
        ESP_LOGI(TAG, "RMT buffer: %d symbols", rmt_config.mem_block_symbols);
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "NeoPixels ready: %d total (ring %d + Mickey %d) on GPIO %d",
             s_total, s_ring, s_mickey, NEOPIXEL_GPIO);
}

void leds_off(void)
{
    led_strip_clear(s_strip);
    s_reward_frame = 0;
}

// ---------------------------------------------------------------------------
// Reward: a green comet chases around the ring with a fading tail, while the
// Mickey segment pulses green in sympathy. Green mirrors the Adafruit chase.
// ---------------------------------------------------------------------------
void leds_reward_step(void)
{
    const int   TAIL = 6;          // comet tail length in pixels
    const float FADE = 0.55f;      // brightness ratio between adjacent tail pixels

    // Comet head moves ~1 pixel every 2 frames for a smooth sweep.
    int head = (s_reward_frame / 2) % (s_ring > 0 ? s_ring : 1);

    // Ring: clear, then paint head + tail.
    for (int i = 0; i < s_ring; i++)
        led_strip_set_pixel(s_strip, ring_px(i), 0, 0, 0);

    for (int t = 0; t < TAIL; t++) {
        int idx = head - t;
        while (idx < 0) idx += s_ring;
        float level = powf(FADE, t);                 // 1.0 at head, fading back
        uint8_t g = dim((uint32_t)(255 * level));
        led_strip_set_pixel(s_strip, ring_px(idx), 0, g, 0);
    }

    // Mickey: pulse green with a sine so the "ears" glow along with the sweep.
    float pulse = 0.5f + 0.5f * sinf(s_reward_frame * 0.20f);   // 0..1
    uint8_t mg = dim((uint32_t)(255 * (0.25f + 0.75f * pulse)));
    for (int i = 0; i < s_mickey; i++)
        led_strip_set_pixel(s_strip, mickey_px(i), 0, mg, 0);

    show();
    s_reward_frame++;
}

// ---------------------------------------------------------------------------
// Celebration: the full choreographed show designed on the bench.
//   1) white comet whips around the ring, twice
//   2) blackout - one beat
//   3) ring green fades in while the mouse face sparkles white->blue->pink->green
//   4) everything lands on solid green and holds
// Runs ~4.5 s; the sound plays on its own task at the same time.
// ---------------------------------------------------------------------------
void leds_celebrate(void)
{
    const int   CHASE_TAIL  = 7;
    const float CHASE_FADE  = 0.55f;
    const int   CHASE_MS    = 18;
    const int   BEAT_MS     = 400;
    const int   GROW_FRAMES = 64;
    const int   GROW_MS     = 30;
    const int   HOLD_MS     = 1200;

    // 1) white comet, two laps around the ring
    for (int step = 0; step < 2 * s_ring; step++) {
        int head = step % s_ring;
        for (int i = 0; i < s_total; i++)
            led_strip_set_pixel(s_strip, i, 0, 0, 0);
        for (int t = 0; t < CHASE_TAIL; t++) {
            int idx = head - t;
            while (idx < 0) idx += s_ring;
            uint8_t v = dim((uint32_t)(255 * powf(CHASE_FADE, t)));
            led_strip_set_pixel(s_strip, ring_px(idx), v, v, v);
        }
        show();
        if (anim_wait(CHASE_MS)) return;
    }

    // 2) blackout beat
    led_strip_clear(s_strip);
    if (anim_wait(BEAT_MS)) return;

    // 3) ring green fade-in + face sparkle through four colors
    static const uint8_t sparkle[4][3] = {
        { 255, 255, 255 },   // white
        {  40,  90, 255 },   // blue
        { 255,  70, 140 },   // pink
        {   0, 255,  60 },   // green
    };
    for (int f = 0; f < GROW_FRAMES; f++) {
        uint8_t g = dim(255 * f / (GROW_FRAMES - 1));
        for (int i = 0; i < s_ring; i++)
            led_strip_set_pixel(s_strip, ring_px(i), 0, g, 0);

        const uint8_t *c = sparkle[(f * 4) / GROW_FRAMES];
        for (int i = 0; i < s_mickey; i++) {
            if ((esp_random() & 3) == 0) {                     // ~1 in 4 pixels lit
                uint32_t lvl = 128 + (esp_random() % 128);
                led_strip_set_pixel(s_strip, mickey_px(i),
                                    dim(c[0] * lvl / 255),
                                    dim(c[1] * lvl / 255),
                                    dim(c[2] * lvl / 255));
            } else {
                led_strip_set_pixel(s_strip, mickey_px(i), 0, 0, 0);
            }
        }
        show();
        if (anim_wait(GROW_MS)) return;
    }

    // 4) land on solid green and hold
    for (int i = 0; i < s_total; i++)
        led_strip_set_pixel(s_strip, i, 0, dim(255), 0);
    show();
    if (anim_wait(HOLD_MS)) return;

    led_strip_clear(s_strip);
    s_reward_frame = 0;
}

// ---------------------------------------------------------------------------
// Extra animations, selectable per tag via leds_play().
// ---------------------------------------------------------------------------

// Integer HSV->RGB (all 0..255). Handy for rainbow / random spark colors.
static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 43;
    uint8_t rem    = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

// Warm gold welcome: gold comet fills the ring, face fades up gold with warm
// twinkle, then the whole thing breathes gently.
static void anim_welcome(void)
{
    const uint8_t GR = 255, GG = 140, GB = 20;   // warm gold

    for (int head = 0; head < s_ring; head++) {
        if (head > 0)
            led_strip_set_pixel(s_strip, ring_px(head - 1), dim(GR), dim(GG), dim(GB));
        led_strip_set_pixel(s_strip, ring_px(head), dim(255), dim(210), dim(120)); // bright head
        show();
        if (anim_wait(22)) return;
    }
    led_strip_set_pixel(s_strip, ring_px(s_ring - 1), dim(GR), dim(GG), dim(GB));

    for (int f = 0; f < 50; f++) {
        uint8_t lvl = 255 * f / 49;
        for (int i = 0; i < s_mickey; i++) {
            if ((esp_random() & 7) == 0)
                led_strip_set_pixel(s_strip, mickey_px(i), dim(255), dim(230), dim(180));
            else
                led_strip_set_pixel(s_strip, mickey_px(i),
                                    dim(GR * lvl / 255), dim(GG * lvl / 255), dim(GB * lvl / 255));
        }
        show();
        if (anim_wait(28)) return;
    }

    for (int f = 0; f < 60; f++) {
        float br = 0.7f + 0.3f * sinf(f * 0.15f);
        for (int i = 0; i < s_total; i++)
            led_strip_set_pixel(s_strip, i, dim((uint32_t)(GR * br)),
                                dim((uint32_t)(GG * br)), dim((uint32_t)(GB * br)));
        show();
        if (anim_wait(25)) return;
    }
    led_strip_clear(s_strip);
}

// Multicolor bursts that trail and fade, building to a white finale flash.
static void anim_fireworks(void)
{
    uint8_t fb[TOTAL_LED_MAX][3];
    memset(fb, 0, sizeof(fb));

    for (int f = 0; f < 130; f++) {
        for (int i = 0; i < s_total; i++) {     // fade everything a little
            fb[i][0] = fb[i][0] * 7 / 10;
            fb[i][1] = fb[i][1] * 7 / 10;
            fb[i][2] = fb[i][2] * 7 / 10;
        }
        for (int s = 0; s < 3; s++) {                   // spawn a few new sparks
            int idx = esp_random() % s_total;
            uint8_t r, g, b;
            hsv2rgb(esp_random() & 0xFF, 255, 255, &r, &g, &b);
            fb[idx][0] = r; fb[idx][1] = g; fb[idx][2] = b;
        }
        for (int i = 0; i < s_total; i++)
            led_strip_set_pixel(s_strip, i, dim(fb[i][0]), dim(fb[i][1]), dim(fb[i][2]));
        show();
        if (anim_wait(25)) return;
    }

    for (int i = 0; i < s_total; i++)
        led_strip_set_pixel(s_strip, i, dim(255), dim(255), dim(255));
    show();
    if (anim_wait(120)) return;

    for (int f = 0; f < 20; f++) {
        uint8_t v = 255 * (19 - f) / 19;
        for (int i = 0; i < s_total; i++)
            led_strip_set_pixel(s_strip, i, dim(v), dim(v), dim(v));
        show();
        if (anim_wait(25)) return;
    }
    led_strip_clear(s_strip);
}

// Rainbow spinning around the ring; the face cycles through hue in unison.
static void anim_rainbow(void)
{
    for (int f = 0; f < 150; f++) {
        for (int i = 0; i < s_ring; i++) {
            uint8_t r, g, b;
            hsv2rgb((uint8_t)(i * 256 / s_ring + f * 3), 255, 255, &r, &g, &b);
            led_strip_set_pixel(s_strip, ring_px(i), dim(r), dim(g), dim(b));
        }
        uint8_t r, g, b;
        hsv2rgb((uint8_t)(f * 4), 255, 255, &r, &g, &b);
        for (int i = 0; i < s_mickey; i++)
            led_strip_set_pixel(s_strip, mickey_px(i), dim(r), dim(g), dim(b));
        show();
        if (anim_wait(20)) return;
    }
    led_strip_clear(s_strip);
}

// Like WELCOME but violet: a purple comet fills the ring behind a bright white
// leading pixel, the face fades up purple with white twinkle, then breathes.
static void anim_enchanted(void)
{
    const uint8_t PR = 180, PG = 0, PB = 255;   // rich saturated purple

    for (int head = 0; head < s_ring; head++) {
        if (head > 0)
            led_strip_set_pixel(s_strip, ring_px(head - 1), dim(PR), dim(PG), dim(PB));
        led_strip_set_pixel(s_strip, ring_px(head), dim(255), dim(255), dim(255)); // white head
        show();
        if (anim_wait(22)) return;
    }
    led_strip_set_pixel(s_strip, ring_px(s_ring - 1), dim(PR), dim(PG), dim(PB));

    for (int f = 0; f < 50; f++) {
        uint8_t lvl = 255 * f / 49;
        for (int i = 0; i < s_mickey; i++) {
            if ((esp_random() & 7) == 0)
                led_strip_set_pixel(s_strip, mickey_px(i), dim(255), dim(255), dim(255));
            else
                led_strip_set_pixel(s_strip, mickey_px(i),
                                    dim(PR * lvl / 255), dim(PG * lvl / 255), dim(PB * lvl / 255));
        }
        show();
        if (anim_wait(28)) return;
    }

    for (int f = 0; f < 60; f++) {
        float br = 0.7f + 0.3f * sinf(f * 0.15f);
        for (int i = 0; i < s_total; i++)
            led_strip_set_pixel(s_strip, i, dim((uint32_t)(PR * br)),
                                dim((uint32_t)(PG * br)), dim((uint32_t)(PB * br)));
        show();
        if (anim_wait(25)) return;
    }
    led_strip_clear(s_strip);
}

// Be Our Guest: white comet chases the ring, then the ring fades up blue while
// the Mickey face sparkles gold; lands on a blue ring + gold face.
static void anim_beourguest(void)
{
    const int   CHASE_TAIL  = 7;
    const float CHASE_FADE  = 0.55f;
    const int   CHASE_MS    = 18;
    const int   BEAT_MS     = 400;
    const int   GROW_FRAMES = 64;
    const int   GROW_MS     = 30;
    const int   HOLD_MS     = 1200;
    const uint8_t BLU_R = 0, BLU_G = 60, BLU_B = 255;  // ring blue (BR/BG/BB are xtensa macros!)
    const uint8_t GR = 255, GG = 140, GB = 20;         // gold face

    // 1) white comet, two laps
    for (int step = 0; step < 2 * s_ring; step++) {
        int head = step % s_ring;
        for (int i = 0; i < s_total; i++) led_strip_set_pixel(s_strip, i, 0, 0, 0);
        for (int t = 0; t < CHASE_TAIL; t++) {
            int idx = head - t;
            while (idx < 0) idx += s_ring;
            uint8_t v = dim((uint32_t)(255 * powf(CHASE_FADE, t)));
            led_strip_set_pixel(s_strip, ring_px(idx), v, v, v);
        }
        show();
        if (anim_wait(CHASE_MS)) return;
    }

    // 2) blackout beat
    led_strip_clear(s_strip);
    if (anim_wait(BEAT_MS)) return;

    // 3) ring blue fade-in + gold face sparkle
    for (int f = 0; f < GROW_FRAMES; f++) {
        uint8_t bl = 255 * f / (GROW_FRAMES - 1);
        for (int i = 0; i < s_ring; i++)
            led_strip_set_pixel(s_strip, ring_px(i),
                                dim(BLU_R * bl / 255), dim(BLU_G * bl / 255), dim(BLU_B * bl / 255));
        for (int i = 0; i < s_mickey; i++) {
            if ((esp_random() & 3) == 0) {
                uint32_t lvl = 128 + (esp_random() % 128);
                led_strip_set_pixel(s_strip, mickey_px(i),
                                    dim(GR * lvl / 255), dim(GG * lvl / 255), dim(GB * lvl / 255));
            } else {
                led_strip_set_pixel(s_strip, mickey_px(i), 0, 0, 0);
            }
        }
        show();
        if (anim_wait(GROW_MS)) return;
    }

    // 4) land: blue ring, gold face
    for (int i = 0; i < s_ring; i++)
        led_strip_set_pixel(s_strip, ring_px(i), dim(BLU_R), dim(BLU_G), dim(BLU_B));
    for (int i = 0; i < s_mickey; i++)
        led_strip_set_pixel(s_strip, mickey_px(i), dim(GR), dim(GG), dim(GB));
    show();
    if (anim_wait(HOLD_MS)) return;
    led_strip_clear(s_strip);
}

// Dispatch: pick the animation for this tag.
void leds_play(anim_id_t id)
{
    // Held for the whole show, not just per frame: an idle step from the main
    // loop should wait until the animation is done rather than interleave blue
    // breathing into a celebration. Recursive, so the frames inside still pass.
    leds_lock();
    switch (id) {
        case ANIM_WELCOME:    anim_welcome();    break;
        case ANIM_FIREWORKS:  anim_fireworks();  break;
        case ANIM_RAINBOW:    anim_rainbow();    break;
        case ANIM_ENCHANTED:  anim_enchanted();  break;
        case ANIM_BEOURGUEST: anim_beourguest(); break;
        case ANIM_CELEBRATE:
        default:              leds_celebrate();  break;
    }
    s_reward_frame = 0;
    leds_unlock();
}

// Sustain: hold/continue an animation's look while its (longer) sound finishes.
// Solid-landing shows hold their final colour; lively ones keep moving.
void leds_sustain_step(anim_id_t id)
{
    static uint32_t f = 0;
    switch (id) {
        case ANIM_BEOURGUEST: {                 // hold: blue ring + gold face
            for (int i = 0; i < s_ring; i++)
                led_strip_set_pixel(s_strip, ring_px(i), 0, dim(60), dim(255));
            for (int i = 0; i < s_mickey; i++)
                led_strip_set_pixel(s_strip, mickey_px(i), dim(255), dim(140), dim(20));
            break;
        }
        case ANIM_WELCOME: {                    // gentle gold breathe
            float br = 0.7f + 0.3f * sinf(f * 0.15f);
            for (int i = 0; i < s_total; i++)
                led_strip_set_pixel(s_strip, i, dim((uint32_t)(255 * br)),
                                    dim((uint32_t)(140 * br)), dim((uint32_t)(20 * br)));
            break;
        }
        case ANIM_ENCHANTED: {                  // gentle purple breathe
            float br = 0.7f + 0.3f * sinf(f * 0.15f);
            for (int i = 0; i < s_total; i++)
                led_strip_set_pixel(s_strip, i, dim((uint32_t)(180 * br)),
                                    0, dim((uint32_t)(255 * br)));
            break;
        }
        case ANIM_RAINBOW: {                    // keep the ring spinning
            for (int i = 0; i < s_ring; i++) {
                uint8_t r, g, b;
                hsv2rgb((uint8_t)(i * 256 / s_ring + f * 3), 255, 255, &r, &g, &b);
                led_strip_set_pixel(s_strip, ring_px(i), dim(r), dim(g), dim(b));
            }
            uint8_t r, g, b;
            hsv2rgb((uint8_t)(f * 4), 255, 255, &r, &g, &b);
            for (int i = 0; i < s_mickey; i++)
                led_strip_set_pixel(s_strip, mickey_px(i), dim(r), dim(g), dim(b));
            break;
        }
        case ANIM_FIREWORKS: {                  // keep a few sparks going
            for (int i = 0; i < s_total; i++)
                led_strip_set_pixel(s_strip, i, 0, 0, 0);
            for (int s = 0; s < 4; s++) {
                int idx = esp_random() % s_total;
                uint8_t r, g, b;
                hsv2rgb(esp_random() & 0xFF, 255, 255, &r, &g, &b);
                led_strip_set_pixel(s_strip, idx, dim(r), dim(g), dim(b));
            }
            break;
        }
        case ANIM_CELEBRATE:
        default:                                // hold solid green
            for (int i = 0; i < s_total; i++)
                led_strip_set_pixel(s_strip, i, 0, dim(255), 0);
            break;
    }
    show();
    f++;
}

// Program-mode indicator: slow amber breathe so it's obvious you're enrolling.
void leds_prog_step(void)
{
    float br = 0.35f + 0.35f * sinf(s_idle_frame * 0.12f);
    uint8_t r = dim((uint32_t)(255 * br));
    uint8_t g = dim((uint32_t)(110 * br));
    for (int i = 0; i < s_total; i++)
        led_strip_set_pixel(s_strip, i, r, g, 0);
    show();
    s_idle_frame++;
}

// Setup/recovery indicator: a slow cyan breathe over the whole strip, distinct
// from the dim-blue idle and the amber program breathe, so it's obvious the
// device is sitting in its setup access-point mode.
void leds_setup_step(void)
{
    float breathe = 0.5f + 0.5f * sinf(s_idle_frame * 0.08f);
    uint8_t g = dim((uint32_t)(30 + 110 * breathe));
    uint8_t b = dim((uint32_t)(120 + 135 * breathe));
    for (int i = 0; i < s_total; i++)
        led_strip_set_pixel(s_strip, i, 0, g, b);
    show();
    s_idle_frame++;
}

// Power-on greeting: one frame of a blue twinkle. Each spark flashes white for a
// beat, turns blue, then fades; fresh sparks keep spawning at random ring
// positions, capped so it stays sparse (never more than ~a third lit). State is
// held in a static buffer so repeated calls animate continuously - the caller
// runs it for the whole "connecting" wait, then stops when Wi-Fi comes up.
void leds_sparkle_step(void)
{
    static uint8_t life[RING_LED_MAX];            // per-pixel spark countdown
    const uint8_t  LIFE     = 48;                    // frames a spark lives
    const uint8_t  WHITE_AT = 40;                    // life > this -> white flash
    const int      MAXLIVE  = s_ring / 3;    // sparse: < half the ring

    int live = 0;
    for (int i = 0; i < s_ring; i++) if (life[i]) live++;

    for (int s = 0; s < 2 && live < MAXLIVE; s++) {  // spawn up to 2 fresh sparks
        int idx = esp_random() % s_ring;
        if (!life[idx]) { life[idx] = LIFE; live++; }
    }

    for (int i = 0; i < s_ring; i++) {
        if (life[i] > WHITE_AT) {                    // fresh: bright white flash
            uint8_t w = dim(255);
            led_strip_set_pixel(s_strip, ring_px(i), w, w, w);
        } else if (life[i]) {                         // aging: blue, fading out
            float t = (float)life[i] / WHITE_AT;      // 1..0
            led_strip_set_pixel(s_strip, ring_px(i),
                                0, dim((uint32_t)(70 * t)), dim((uint32_t)(255 * t)));
        } else {
            led_strip_set_pixel(s_strip, ring_px(i), 0, 0, 0);
        }
        if (life[i]) life[i]--;
    }
    for (int i = 0; i < s_mickey; i++)        // face stays dark
        led_strip_set_pixel(s_strip, mickey_px(i), 0, 0, 0);

    show();
}

// Button hold-cue: paint a solid color so the user sees which mode the hold is
// about to enter. Stage 1 = amber (program), 2 = blue (Wi-Fi setup), 0 = off.
void leds_hold_cue(int stage)
{
    uint8_t r = 0, g = 0, b = 0;
    if (stage == 1)      { r = dim(255); g = dim(90);  b = 0; }        // amber
    else if (stage == 2) { r = 0;        g = 0;        b = dim(255); } // blue
    for (int i = 0; i < s_total; i++)
        led_strip_set_pixel(s_strip, i, r, g, b);
    show();
}

// ---------------------------------------------------------------------------
// Idle: either fully off (faithful to Adafruit) or a slow blue "armed" breathe
// on the ring so the reader looks alive while it waits. Toggle in config.h.
// ---------------------------------------------------------------------------
// Runtime toggle for the idle glow; defaults to the IDLE_BREATHE compile flag.
static bool s_idle_on = (IDLE_BREATHE != 0);

void leds_set_idle_enabled(bool on)
{
    s_idle_on = on;
    if (!on) led_strip_clear(s_strip);      // go dark immediately
}

void leds_idle_step(void)
{
    if (!s_idle_on) {                        // glow off -> stay dark
        led_strip_clear(s_strip);
        return;
    }
    float breathe = 0.5f + 0.5f * sinf(s_idle_frame * 0.05f);   // slow 0..1
    // Scale the chosen colour by the breathe curve, keeping the same dim
    // ceiling the blue default had (40/255) so a bright colour can't glare.
    float k = breathe * (40.0f / 255.0f);
    uint8_t r = dim((uint32_t)(s_idle_r * k));
    uint8_t g = dim((uint32_t)(s_idle_g * k));
    uint8_t b = dim((uint32_t)(s_idle_b * k));
    for (int i = 0; i < s_ring; i++)
        led_strip_set_pixel(s_strip, ring_px(i), r, g, b);
    for (int i = 0; i < s_mickey; i++)
        led_strip_set_pixel(s_strip, mickey_px(i), 0, 0, 0);
    show();
    s_idle_frame++;
}
