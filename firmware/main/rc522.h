// ---------------------------------------------------------------------------
// rc522.h - minimal MFRC522 RFID reader driver over SPI.
//
// Just enough to detect a 13.56 MHz (ISO14443A) card/MagicBand and read its
// UID - which is all we need to use a tap as a trigger. No card programming
// required; the UID is baked in at the factory.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Bring up SPI + the reader (hard reset, configure, antenna on).
esp_err_t rc522_init(void);

// Poll for a card. Returns true and fills uid (>=4 bytes) + *uid_len when a
// card is present in the field; false otherwise.
bool rc522_read_uid(uint8_t *uid, uint8_t *uid_len);

// Self-test version byte: 0x91/0x92 = genuine, 0x88/0xB2 = common clones,
// 0x00 or 0xFF = the reader isn't answering (wiring/power problem).
uint8_t rc522_version(void);
