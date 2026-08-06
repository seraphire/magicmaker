// ---------------------------------------------------------------------------
// ota.h - firmware update over HTTPS (Stages 3-6 of the roadmap).
//
// Stage 3 (here): a plain HTTPS GET into a buffer, to prove the chain works -
// DNS, TLS via the bundled root CAs, a valid clock (from NTP), and HTTP
// handling. Manifest parsing and the actual image install build on this.
// ---------------------------------------------------------------------------
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// HTTPS GET `url` into `out` (always NUL-terminated; truncated to out_sz-1).
// Returns the number of body bytes received, or -1 on error. Needs Wi-Fi up
// and the clock set (TLS cert validation checks the date).
int ota_http_get(const char *url, char *out, size_t out_sz);

// Parsed OTA manifest (Stage 4).
typedef struct {
    char version[24];        // e.g. "1.0.1"
    char firmware_url[256];  // where the .bin lives
    char manifest_url[160];  // optional top-level "manifest_url": relocate future
                             // checks here (host migration). Empty if absent.
    char sha256[65];         // expected hash of the image
    char sig[160];           // base64 ECDSA-P256 signature over that hash. An
                             // OTA install is REFUSED without it - a check you
                             // can skip by omitting a field is not a check.
} ota_manifest_t;

// Parse a manifest JSON string into *out ("version" + "firmware_url").
// Returns 0 on success, -1 if not JSON or missing fields. (Split out so it's
// testable without a network fetch.)
int ota_parse_manifest(const char *json, ota_manifest_t *out);

// Fetch the JSON manifest at `url` and parse it. Returns 0, or -1 on error.
int ota_fetch_manifest(const char *url, ota_manifest_t *out);

// ---- Rollback (Stage 6) ---------------------------------------------------
// Confirm the running image is healthy. If we booted a freshly-OTA'd image
// (state "pending verify"), this cancels the pending rollback so it sticks.
// A no-op for normal boots. Call once, early, after core hardware is up - if a
// bad image never reaches this (crash/boot-loop), the bootloader auto-reverts
// to the previous slot. Returns true if this boot IS a freshly-installed image
// (was pending-verify) - i.e. "we just updated" - so the caller can react (e.g.
// an "all updated!" chime); false on ordinary boots.
bool ota_mark_valid(void);

// ---- OTA install core (Stage 5) -------------------------------------------
// Write a new firmware image to the *inactive* OTA slot, defensively:
// single session at a time, every write hard-capped to the slot size, image
// validated before the boot partition is switched. The running firmware is
// never touched until ota_finish() succeeds.

// Begin an update. `size_hint` is the expected image size (0 = unknown); if
// known and larger than the slot, it's rejected up front. Returns ESP_OK, or
// an error (incl. ESP_ERR_INVALID_STATE if one is already in progress).
esp_err_t ota_begin(size_t size_hint);

// Append `len` bytes. Aborts the session and errors if the total would exceed
// the slot. Safe to call only between ota_begin() and ota_finish().
esp_err_t ota_write(const void *data, size_t len);

// Validate the written image and, only if valid, set it as the next boot
// partition. Caller reboots on ESP_OK. On error the running firmware stays.
esp_err_t ota_finish(void);

// Abort an in-progress session (e.g. dropped upload). Running firmware intact.
void ota_abort_session(void);

// Bytes written in the current/last session (for logging/UI).
size_t ota_bytes_written(void);

// ---- Pull-based update (Stage 5b) -----------------------------------------
// Download a firmware image from `url` and install it to the inactive slot
// (streamed + validated via esp_https_ota; follows redirects; http:// allowed
// for LAN testing). On ESP_OK the caller reboots to run it.
// `expect_sha256` (optional, may be NULL or "") is checked against the image
// actually written before the boot partition is switched. The manifest has
// carried this hash all along and NOTHING looked at it - a truncated download or
// a swapped binary would install and reboot into whatever arrived. Verifying it
// is not authenticity (anyone who can change the file can change the manifest)
// but it does close the accident case, which is the likelier one.
esp_err_t ota_install_from_url(const char *url, const char *expect_sha256,
                               const char *expect_sig);

// Full auto-update: fetch the manifest, and only if its version is newer than
// `cur_version`, download + install its firmware_url. Sets *installed on apply.
// Returns ESP_OK on success (whether or not an update was applied).
// `verify_assets` forces the audio pack to be checked against what is actually
// on flash, instead of trusting the pack version. Pass true where it's cheap
// and worth it (boot, or a person asking), false on the routine cycle.
esp_err_t ota_update_from_manifest(const char *manifest_url,
                                   const char *cur_version, bool *installed,
                                   bool verify_assets);
