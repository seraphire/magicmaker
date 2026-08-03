// ---------------------------------------------------------------------------
// appcfg.h - device configuration in NVS (Wi-Fi creds, trip date, flags).
//
// Separate from store.c (which holds per-card enrollments). Stored as
// individual NVS keys in the "appcfg" namespace so future firmware can migrate
// one field at a time. Every field has a compiled-in default, so a fresh or
// wiped device always starts with a valid config.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Bump when the on-disk shape changes so older saves can be migrated.
#define APPCFG_VERSION 3

typedef struct {
    char wifi_ssid[33];      // home network SSID (empty = not provisioned)
    char wifi_password[65];  // WPA2 passphrase

    char device_name[33];    // friendly name (used in AP SSID suffix later)

    int  trip_year;          // Disney trip target date
    int  trip_month;
    int  trip_day;

    bool countdown_enabled;
    bool idle_led_enabled;

    // true  = far from the trip the countdown only speaks occasionally (weekly,
    //         then fortnightly), going daily inside the last month;
    // false = every day, however far out.
    // Months out, "six months to go" barely changes day to day, so tapering
    // keeps it feeling like an event.
    bool countdown_taper;

    // Play the power-on greeting and the "online" chime? Off is for the times
    // it lives on a desk and you'd rather it woke up quietly.
    bool boot_audio_enabled;

    // Idle-glow colour, 0x00RRGGBB. Defaults to the classic slow blue breathe.
    uint32_t idle_color;

    // Physical LED layout, per device - every build is wired slightly
    // differently and one OTA image serves several units.
    int  ring_leds;
    int  mickey_leds;
    bool ring_first;

    char manifest_url[129];  // OTA update manifest (editable only in setup mode)

    // Audio pack manifest, PER DEVICE. Empty = no asset sync at all, which is
    // the default: one reader can carry a birthday set that no other reader and
    // no public repository ever sees.
    //
    // Deliberately separate from manifest_url. That one is public - anything
    // listed in it is published, and there is no unlisted state - so a voice
    // bank licensed for personal use only, or a clip recorded for one
    // particular child, cannot live there.
    char assets_url[129];

    uint32_t config_version;
} device_config_t;

// Load config from NVS, filling defaults for any missing keys. Never fails:
// an unreadable store just yields the compiled-in defaults.
void appcfg_load(device_config_t *cfg);

// Persist the whole config. Returns ESP_OK on success.
esp_err_t appcfg_save(const device_config_t *cfg);

// True once a non-empty SSID is stored (i.e. the device is provisioned).
bool appcfg_has_wifi(const device_config_t *cfg);

// Wipe only the Wi-Fi credentials (recovery). Leaves trip date, flags, and all
// card enrollments untouched.
void appcfg_clear_wifi(void);
