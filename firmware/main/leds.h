// ---------------------------------------------------------------------------
// leds.h - NeoPixel effects for the two-segment strip (ring + Mickey).
//
// The step functions are meant to be called repeatedly (~50 fps) from the main
// loop, so LED animation and audio run at the same time without blocking each
// other - the same structure as the Adafruit `while audio.playing: animate()`.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

void leds_init(void);

// All pixels off.
void leds_off(void);

// One frame of the "waiting for a tap" look. Call while idle.
void leds_idle_step(void);

// Turn the idle glow on/off at runtime (default follows IDLE_BREATHE).
void leds_set_idle_enabled(bool on);

// One frame of the "you're in program mode" look (slow amber breathe).
void leds_prog_step(void);

// One frame of the "setup / recovery mode is active" look (slow cyan breathe),
// shown while the device is running its SoftAP setup portal.
void leds_setup_step(void);

// Power-on greeting: one frame of a blue twinkle across the ring - random pixels
// flash white, shift to blue, and fade out, never lighting more than ~a third at
// once. Non-blocking (call repeatedly, ~50 fps); it keeps twinkling for as long
// as you keep calling it, so it can run for the whole "connecting to Wi-Fi" wait.
void leds_sparkle_step(void);

// Button hold-cue: solid color while a long press is building, so you can see
// which mode you're about to enter. 0 = off, 1 = program (amber), 2 = Wi-Fi
// setup (blue). Non-blocking single frame.
void leds_hold_cue(int stage);

// One frame of the "band accepted!" celebration. Call while audio plays.
void leds_reward_step(void);

// The full choreographed celebration (white chase -> beat -> green fade +
// face sparkle -> solid green). Blocks ~4.5 s; play the sound just before it.
void leds_celebrate(void);

// Named animations, one per "kind of moment". Map tags to these in tags.h.
// ANIM_CELEBRATE is the default for unknown tags.
typedef enum {
    ANIM_CELEBRATE = 0,   // green classic (white chase -> green fade + sparkle)
    ANIM_WELCOME,         // warm gold fill + twinkle, gentle breathe
    ANIM_FIREWORKS,       // multicolor bursts across the whole strip, white finale
    ANIM_RAINBOW,         // rainbow spinning round the ring, face cycling hue
    ANIM_ENCHANTED,       // purple fill + bright white leading pixel (gold's twin)
    ANIM_BEOURGUEST,      // white chase -> blue ring fade, gold-sparkle Mickey face
    ANIM_COUNT
} anim_id_t;

// Play a full animation by id (blocks for its ~few-second duration).
// Start the sound just before calling this.
void leds_play(anim_id_t id);

// Register a check the blocking animations poll at every frame boundary; when it
// returns true they abandon the show immediately. Lets a new band tap cut a long
// celebration short instead of waiting it out. NULL disables (the default).
void leds_set_abort_check(bool (*fn)(void));

// One frame of an animation's "sustain" look, for holding the moment until the
// sound finishes (some clips are longer than the animation). Solid-landing
// animations hold their final colour; the lively ones (rainbow/fireworks/gold/
// purple) keep moving. Call repeatedly (~50 fps) while audio is still playing.
void leds_sustain_step(anim_id_t id);
