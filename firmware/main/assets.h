// ---------------------------------------------------------------------------
// assets.h - ship MEDIA (and web assets) over the air, alongside firmware.
//
// esp_https_ota only swaps the app slot; sounds/animations live on the LittleFS
// data partition. This module syncs those files to match the manifest's
// "assets" section, downloading only the files whose sha256 differs from what's
// already installed (tracked in /spiffs/installed.json). Each file is streamed
// to "<name>.new", hash-verified, then atomically renamed over the original, so
// a power cut never leaves a half-written asset.
//
// Manifest "assets" section:
//   "assets": {
//     "base_url": "https://host/magicmaker/assets/",
//     "files": [
//       { "path": "Program/welcome-magic.wav", "sha256": "a1b2...", "bytes": 47554 },
//       ...
//     ],
//     "remove": [ "old-prompt.wav" ]   // optional: retire files no longer needed
//   }
// A file's on-flash path is /spiffs/<path>; <path> is sanitized (no "..",
// no leading '/', restricted charset) so a manifest can't escape the FS. The
// "remove" list is explicit (not "delete anything not in files") so a bad
// manifest can't wipe the device.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Sync the data FS to the "assets" section of a manifest JSON string. Downloads
// only changed/new files (per-file sha256 vs /spiffs/installed.json). A manifest
// with no "assets" section is a no-op success. Needs Wi-Fi up + clock set (TLS).
//
// `n_updated` (optional) receives the number of files actually downloaded.
// Returns ESP_OK if the FS matches the manifest (incl. "nothing to do"), or
// ESP_FAIL if the manifest was malformed or any listed file failed to
// fetch/verify. Partial progress is persisted, so a re-run resumes.
esp_err_t assets_sync_json(const char *manifest_json, int *n_updated);
