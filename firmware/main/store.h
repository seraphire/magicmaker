// ---------------------------------------------------------------------------
// store.h - persistent tag enrollments in NVS (survive power-off).
//
// Runtime enrollments made in program mode live here. They take priority over
// the compiled-in defaults in tags.h, so you can reassign a card on the device
// without reflashing.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Init NVS. Call once at boot.
void store_init(void);

// Look up a UID. On hit, copies the sound ID into `sound_id` and the animation
// into *anim, and returns true. Returns false if the UID isn't enrolled.
//
// An ID, not a path: "chime", or "chime-2" to pin one variant, or "" for the
// RANDOM action. Resolved to a file at TAP time against the active theme, so a
// band follows the season rather than being nailed to whatever file existed the
// day it was enrolled.
bool store_lookup(const uint8_t uid[4], char *sound_id, size_t id_sz, uint8_t *anim);

// Save (or overwrite) an enrollment for this UID. Persists immediately.
esp_err_t store_save(const uint8_t uid[4], const char *sound_id, uint8_t anim);

// Remove one UID's enrollment (back to random). ESP_OK, or ESP_ERR_NVS_NOT_FOUND
// if it wasn't enrolled - either way the card ends up random.
esp_err_t store_erase(const uint8_t uid[4]);

// Enumerate every enrolled UID, calling `cb` for each with the UID as an 8-hex
// string, its stored sound ("" = the random action), and its animation id. For
// the web band-manager to list registered bands.
void store_list(void (*cb)(const char *uid_hex, const char *sound_id, uint8_t anim, void *ctx),
                void *ctx);

// Small persisted settings (e.g. idle-light on/off). Survive power-off.
uint8_t store_get_flag(const char *key, uint8_t def);
void    store_set_flag(const char *key, uint8_t val);

// Full factory reset: erase the entire NVS partition (Wi-Fi config, enrolled
// cards, and all flags). Reboot right after - open NVS handles are stale.
void store_factory_reset(void);
