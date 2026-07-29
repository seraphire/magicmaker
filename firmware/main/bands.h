// ---------------------------------------------------------------------------
// bands.h - web band-manager support: the last band scanned (RAM only) and
// friendly per-band names (NVS). The enrollment itself (sound + animation) stays
// in store.c; this adds the "who is this band" layer the web page edits.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BAND_NAME_MAX 32   // friendly name cap (bytes, UTF-8 aware)

// Clean an inbound free-text name in place: drop ASCII control chars and the
// HTML-dangerous < > ", trim surrounding spaces, and truncate to BAND_NAME_MAX
// bytes without splitting a UTF-8 character. Used for band names and the device
// name (NOT Wi-Fi creds, which may legitimately contain any character).
void bands_sanitize_name(char *s);

// Record the most recently scanned band. RAM only - a reboot forgets it (just
// rescan). Called from the tap loop; ignores the button (len < 4, no UID).
void bands_note_scan(const uint8_t *uid, uint8_t len);

// Most recently scanned band's UID as an 8-hex string into out (>=9 bytes).
// False if nothing has been scanned since boot.
bool bands_last_scan(char *out, size_t sz);

// Friendly name for a band, keyed by its 8-hex UID, persisted in NVS.
// bands_get_name returns false (and out="") when the band has no name.
bool bands_get_name(const char *uid_hex, char *out, size_t sz);
void bands_set_name(const char *uid_hex, const char *name);
void bands_clear_name(const char *uid_hex);
