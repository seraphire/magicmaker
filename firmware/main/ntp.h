// ---------------------------------------------------------------------------
// ntp.h - SNTP time sync (Stage 2). No RTC on the board, so the clock resets
// every power cycle and is re-synced here once Wi-Fi is up.
//
// A failure never blocks startup: if the clock can't be set, the scanner still
// runs; time-dependent features (countdown, HTTPS OTA cert checks) just wait
// for a good sync on a later boot.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <time.h>

// Set the timezone, start SNTP, and block up to timeout_ms for a valid time.
// Returns true if the clock was set. Call once after joining Wi-Fi.
bool ntp_sync(int timeout_ms);

// True if the system clock has been set from NTP this boot.
bool ntp_is_synced(void);

// Fill *out with the current LOCAL time. False if the clock isn't set yet.
bool ntp_localtime(struct tm *out);
