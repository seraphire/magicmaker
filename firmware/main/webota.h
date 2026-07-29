// ---------------------------------------------------------------------------
// webota.h - browser-based firmware upload (Stage 5, roadmap section 12).
//
// Serves a small "upload a .bin" page and a POST endpoint that streams the
// image straight into the inactive OTA slot (via ota.c). The upload is a raw
// octet-stream (a tiny fetch() posts the file bytes) - deliberately NOT
// multipart, so there's no boundary-parsing of untrusted input.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Register the upload routes (GET /update page, POST /ota handler) on an
// existing server. The routes are always registered but refuse to do anything
// until firmware upload is enabled (see webota_set_upload_enabled). Called by
// the portal in both AP (setup) and STA (normal) modes.
void webota_register(httpd_handle_t srv);

// Gate the firmware-upload endpoints. Off by default: /update and /ota return
// 403 until this is turned on. Enabled automatically in setup/AP mode; can also
// be flipped on over the authenticated serial console (CLI 'update-mode') so a
// developer can push from a desk without the button. A reboot resets it to off.
void webota_set_upload_enabled(bool on);
