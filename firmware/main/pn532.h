// ---------------------------------------------------------------------------
// pn532.h - minimal PN532 NFC reader driver over I2C (ESP-IDF i2c_master).
//
// Just enough to detect a MagicBand: init + SAMConfig, an optional firmware
// check, and a passive-target read that returns the UID. Used by trigger.c
// when TRIGGER_USE_NFC == 1.
//
// NOTE: written against the standard PN532 I2C protocol but not yet verified
// on hardware (the reader is still in transit). If the first read fails, the
// usual culprits are the module's mode switches (must be I2C) and SDA/SCL pins.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Bring up I2C and put the PN532 in normal read mode (SAMConfiguration).
esp_err_t pn532_init(void);

// Optional sanity check. Returns true and fills *version (IC<<24 | Ver<<16 |
// Rev<<8 | Support) if the chip answers GetFirmwareVersion.
bool pn532_get_firmware_version(uint32_t *version);

// Look for one ISO14443A (MIFARE) target for up to timeout_ms. On success,
// copies the UID into uid (buffer must hold >= 7 bytes) and sets *uid_len.
// Returns false if no card is present within the timeout. (Blocking/polled.)
bool pn532_read_passive_target(uint8_t *uid, uint8_t *uid_len, uint32_t timeout_ms);

// --- Interrupt-driven variant -------------------------------------------
// Arm detection: sends InListPassiveTarget and verifies the ACK, then returns
// immediately. The PN532 will pull its IRQ line LOW once a band is found.
bool pn532_start_passive_detection(void);

// Read the result after IRQ asserts (or after pn532_wait_ready). Fills uid /
// uid_len and returns true if a band was found; false otherwise.
bool pn532_read_detected_target(uint8_t *uid, uint8_t *uid_len);
