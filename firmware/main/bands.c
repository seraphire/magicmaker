#include "bands.h"
#include <string.h>
#include <stdio.h>
#include "nvs.h"

#define NS "bandname"        // NVS namespace: UID-hex -> friendly name

// Most-recent scan, RAM only (by design - a reboot forgets it; rescan the band).
static char s_last[9] = { 0 };
static volatile bool s_have_last = false;

static void uid_hex(const uint8_t *uid, char out[9])
{
    static const char *hx = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) { out[i*2] = hx[uid[i] >> 4]; out[i*2+1] = hx[uid[i] & 0xF]; }
    out[8] = '\0';
}

void bands_sanitize_name(char *s)
{
    if (!s) return;
    // Drop ASCII control chars (< 0x20, 0x7F) and the HTML-dangerous < > ".
    // Keep everything else, incl. UTF-8 bytes (>= 0x80) so accented/emoji names
    // survive.
    char *o = s;
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) continue;
        if (c == '<' || c == '>' || c == '"') continue;
        *o++ = *p;
    }
    *o = '\0';

    // Trim leading / trailing spaces.
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';

    // Truncate to BAND_NAME_MAX bytes, but not in the middle of a UTF-8 char.
    if (n > BAND_NAME_MAX) {
        n = BAND_NAME_MAX;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // back off continuation bytes
        s[n] = '\0';
    }
}

static volatile uint8_t s_scan_seq = 0;

void bands_note_scan(const uint8_t *uid, uint8_t len)
{
    if (len < 4) return;                 // the button has no UID
    uid_hex(uid, s_last);
    s_have_last = true;
    s_scan_seq++;                        // wraps, and that's fine - see bands.h
}

uint8_t bands_scan_seq(void) { return s_scan_seq; }

bool bands_last_scan(char *out, size_t sz)
{
    if (!s_have_last) { if (sz) out[0] = '\0'; return false; }
    snprintf(out, sz, "%s", s_last);
    return true;
}

bool bands_get_name(const char *uid_hex_key, char *out, size_t sz)
{
    if (sz) out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = sz;
    esp_err_t r = nvs_get_str(h, uid_hex_key, out, &l);
    nvs_close(h);
    return r == ESP_OK && out[0] != '\0';
}

void bands_set_name(const char *uid_hex_key, const char *name)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, uid_hex_key, name ? name : "");
    nvs_commit(h);
    nvs_close(h);
}

void bands_clear_name(const char *uid_hex_key)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, uid_hex_key);       // NOT_FOUND is fine
    nvs_commit(h);
    nvs_close(h);
}
