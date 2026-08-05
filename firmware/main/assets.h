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
#include <stddef.h>
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

// Fetch and apply an audio PACK - a manifest of its own, at its own URL, holding
// only assets. This is what `assets_url` in the device config points at, and the
// reason it exists is that the firmware manifest is public: anything listed
// there is published, with no unlisted state. A pack is per device.
//
// Two gates the raw applier doesn't have:
//
//   "requires_fw": "1.2.0"  - a bank built for newer firmware may reference
//       clips that older code never asks for, which fails silently rather than
//       loudly. Below the floor, this logs and returns ESP_OK with 0 updated:
//       DEFERRED, not failed, so it can never block the very firmware update
//       that would qualify the device.
//
//   "version": "…"          - short-circuits the common case. Without it every
//       housekeeping cycle re-fetches the list to conclude nothing happened.
//       The applied version is stored WITH the URL it came from: once each
//       owner supplies their own audio, version 3 here and version 3 there are
//       unrelated documents, so a URL change invalidates automatically.
//
// A "firmware" object in a pack is ignored - firmware comes from manifest_url
// only, so a mistyped or hostile assets_url cannot talk a device into
// installing an image.
//
//   "include": [ "sets/xmas.json", "sets/hallow.json" ]
//
// Sub-packs, each an ordinary assets document, fetched and applied one at a
// time. Three themed occasions take the bank past 270 files against a 128-entry
// cap and a 16 KB buffer; splitting the document keeps peak memory flat where
// growing the buffer would not. Relative paths resolve against the parent
// manifest's directory. Depth 1 - an include inside an include is ignored.
//
// A failing sub-pack costs its own set, not the bank: the others still apply,
// but the run fails so the pack version isn't recorded and the next check
// retries. Per-set publishing falls out of this - tweaking one Christmas line
// republishes ~6 KB instead of re-listing 270 unchanged clips.
// `deferred` (optional) is set true when the firmware floor turned the sync
// away. Without it, "0 files, no errors" reads identically to "already up to
// date", which is the one thing a deferral must not look like.
//
// `force` skips the version short-circuit and checks every file against what is
// actually on flash. Pass it whenever a person asked for this sync: the
// short-circuit assumes nothing changed locally, and a file deleted on the
// device leaves the version unchanged, so a cached "already applied" is exactly
// the wrong answer to someone typing `sync-media` because a sound went missing.
// The scheduled path passes false - that's where the saved round trips matter.
esp_err_t assets_sync_pack(const char *url, const char *cur_fw,
                           int *n_updated, bool *deferred, bool force);

// The pack version currently applied, or "" if none. Written into `out`.
void assets_pack_version(char *out, size_t sz);
