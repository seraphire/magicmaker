// ---------------------------------------------------------------------------
// trigger.h - "something asked the reader to react."
//
// Right now that's a debounced button press. When the PN532 NFC reader arrives,
// this is the ONLY file that needs to change: keep the same two functions, and
// have trigger_poll() return true when a MagicBand / NFC tag is detected.
// main.c never needs to know which one it is.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Configure the trigger source (GPIO for the button; later, I2C + PN532).
void trigger_init(void);

// Non-blocking. Returns true exactly once per fresh activation
// (one button press = one true; one band tap = one true).
// Call it repeatedly from the main loop.
// On a fresh activation, fills `uid` (a 4-byte buffer) with the tag UID and
// sets *uid_len (0 for the button, which has no UID). Returns true once per tap.
bool trigger_poll(uint8_t *uid, uint8_t *uid_len);

// Discard anything pending and restart the re-trigger cooldown from now. Call
// after a moment finishes: the cooldown is measured from the last read, so a
// multi-second show outlives it and a card still sitting on the reader would
// fire again the instant the show ends (taps appearing to "queue up").
void trigger_flush(void);
