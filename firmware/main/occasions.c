#include "occasions.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"
#include "ntp.h"
#include "appcfg.h"

static const char *TAG = "occasions";

#define NS "occ"        // NVS namespace: one blob per slot, key "o1".."o7"

// One blob per slot rather than a single array. Two reasons, both learned the
// hard way: a partial write can only damage the record it was writing, and a
// record that grows a field later is length-tolerant on its own (see
// occ_read_slot) instead of invalidating the whole set the way a fixed-size
// array blob would. nvs_get_blob returns INVALID_LENGTH on ANY size mismatch,
// and a caller that reads that as "nothing stored" silently forgets everything.
// Comfortably larger than sizeof(occasion_t) so appending a field doesn't
// silently start truncating reads - a buffer sized to today's struct is a trap
// that springs the next time the struct grows, and the symptom (a field that
// reads back empty) looks nothing like the cause.
#define OCC_READ_MAX 256

// Copy a UTF-8 string without splitting a character. An emoji is a multi-byte
// sequence, often several joined by zero-width joiners; a plain strncpy that
// lands mid-sequence leaves a partial codepoint that renders as a replacement
// box, or - worse, with a severed ZWJ - as two unrelated glyphs. Back up to the
// last byte that starts a character (continuation bytes are 10xxxxxx) and cut
// there, so an over-long icon loses its tail instead of becoming rubbish.
void occ_icon_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

long occ_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void slot_key(int i, char key[8])
{
    snprintf(key, 8, "o%d", i);
}

// The trip, presented as an occasion. Synthesised, never stored - see the
// header. lead_days 0 makes it unbounded, which is precisely what the trip
// countdown has always done: it speaks from however far out, and the taper
// (not a window) decides how often.
static bool trip_as_occasion(occasion_t *out)
{
    device_config_t cfg;
    appcfg_load(&cfg);

    memset(out, 0, sizeof(*out));
    snprintf(out->id,    sizeof(out->id),    "%s",
             cfg.audio_set[0] ? cfg.audio_set : "trip");
    snprintf(out->label, sizeof(out->label), "%s",
             cfg.trip_label[0] ? cfg.trip_label : "The trip");
    occ_icon_copy(out->icon, sizeof(out->icon), cfg.trip_icon);
    out->year      = (uint16_t)cfg.trip_year;
    out->month     = (uint8_t) cfg.trip_month;
    out->day       = (uint8_t) cfg.trip_day;
    out->lead_days = 0;
    out->flags     = cfg.countdown_enabled ? OCC_F_ENABLED : 0;
    return true;
}

static bool valid(const occasion_t *o)
{
    return o->id[0] &&
           o->month >= 1 && o->month <= 12 &&
           o->day   >= 1 && o->day   <= 31;
}

// Encode one Unicode codepoint as UTF-8. Returns the byte count (0 if it
// doesn't fit or isn't a legal scalar value).
static int utf8_encode(uint32_t cp, char *out, size_t room)
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;  // surrogates
                                                                    // are not
                                                                    // characters
    if (cp < 0x80)     { if (room < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800)    { if (room < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));   out[1] = (char)(0x80 | (cp & 0x3F));
        return 2; }
    if (cp < 0x10000)  { if (room < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    if (room < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));      out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Accept an icon as either literal UTF-8 or as codepoints: "U+1F383", and
// joined sequences as "U+1F468+200D+1F373".
//
// The codepoint form exists because the serial console cannot carry an emoji.
// It ECHOES bytes above 0x7F - so typing one looks like it worked - but strips
// them before the line is split, and the argument vanishes entirely: argc came
// back 7 where the identical command with an ASCII character gave 8. A form
// that survives a plain terminal is the difference between this being settable
// on a cable and not. The web UI will send real UTF-8, which is why both are
// accepted rather than replacing one with the other.
void occ_icon_parse(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    if ((src[0] != 'U' && src[0] != 'u') || src[1] != '+') {
        occ_icon_copy(dst, dst_sz, src);
        return;
    }

    size_t out = 0;
    const char *p = src + 2;
    while (*p) {
        char *end = NULL;
        uint32_t cp = (uint32_t)strtoul(p, &end, 16);
        if (end == p) break;                       // not a hex digit; stop
        int n = utf8_encode(cp, dst + out, dst_sz - 1 - out);
        if (n == 0) break;                         // no room, or not a character
        out += (size_t)n;
        p = end;
        if (*p == '+') p++;                        // joined sequence
        else if (*p == 'U' || *p == 'u') { p++; if (*p == '+') p++; }
        else break;
    }
    dst[out] = '\0';
}

bool occ_get(int i, occasion_t *out)
{
    if (!out || i < 0 || i >= OCC_MAX) return false;
    if (i == OCC_TRIP) return trip_as_occasion(out);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t buf[OCC_READ_MAX];
    size_t  sz = sizeof(buf);
    char key[8];
    slot_key(i, key);
    esp_err_t rc = nvs_get_blob(h, key, buf, &sz);
    nvs_close(h);
    if (rc != ESP_OK || sz == 0) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out, buf, (sz < sizeof(*out)) ? sz : sizeof(*out));

    // A stored record is input, not a promise. A corrupt month would otherwise
    // reach occ_days_from_civil and come back as a date centuries away, which
    // reads as "the countdown is broken" rather than "that record is bad".
    out->id[OCC_ID_LEN - 1]       = '\0';
    out->label[OCC_LABEL_LEN - 1] = '\0';
    out->icon[OCC_ICON_LEN - 1]   = '\0';
    if (!valid(out)) { ESP_LOGW(TAG, "slot %d holds an invalid record", i); return false; }
    return true;
}

bool occ_set(int i, const occasion_t *o)
{
    if (!o || i <= OCC_TRIP || i >= OCC_MAX) return false;   // slot 0 is appcfg's
    if (!valid(o)) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[8];
    slot_key(i, key);
    esp_err_t rc = nvs_set_blob(h, key, o, sizeof(*o));
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    if (rc != ESP_OK) ESP_LOGW(TAG, "slot %d save failed: %s", i, esp_err_to_name(rc));
    return rc == ESP_OK;
}

bool occ_clear(int i)
{
    if (i <= OCC_TRIP || i >= OCC_MAX) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[8];
    slot_key(i, key);
    esp_err_t rc = nvs_erase_key(h, key);
    if (rc == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return rc == ESP_OK || rc == ESP_ERR_NVS_NOT_FOUND;
}

int occ_days_remaining(int i)
{
    occasion_t o;
    if (!occ_get(i, &o)) return INT_MIN;

    struct tm now;
    if (!ntp_localtime(&now)) return INT_MIN;

    int  y     = now.tm_year + 1900;
    long today = occ_days_from_civil(y, now.tm_mon + 1, now.tm_mday);

    if (!(o.flags & OCC_F_ANNUAL))
        return (int)(occ_days_from_civil(o.year, o.month, o.day) - today);

    // Annual: this year's instance, or next year's once it's gone by. Never
    // negative - "minus three hundred days to Christmas" is not something
    // anybody says, and the after-the-event lines exist for a trip that has
    // happened, not for a date that comes round again.
    long d = occ_days_from_civil(y, o.month, o.day) - today;
    if (d < 0) d = occ_days_from_civil(y + 1, o.month, o.day) - today;
    return (int)d;
}

bool occ_in_window(int i)
{
    occasion_t o;
    if (!occ_get(i, &o)) return false;
    int days = occ_days_remaining(i);
    if (days == INT_MIN) return false;
    if (o.lead_days == 0) return true;      // unbounded: the trip, and anything
                                            // else that wants to speak all year
    return days >= 0 && days <= (int)o.lead_days;
}

int occ_active(void)
{
    int best = -1, best_days = INT_MAX;

    for (int i = 0; i < OCC_MAX; i++) {
        occasion_t o;
        if (!occ_get(i, &o)) continue;
        if (!(o.flags & OCC_F_ENABLED)) continue;
        if (!occ_in_window(i)) continue;

        int days = occ_days_remaining(i);
        if (days == INT_MIN) continue;

        // Nearest wins, and a date already past loses to any date still ahead -
        // so the trip's "days since" lines keep playing right up until the first
        // seasonal window opens, and then politely stand aside.
        int rank = (days < 0) ? (INT_MAX / 2 - days) : days;
        if (rank < best_days) { best_days = rank; best = i; }
    }
    return best;
}

int occ_count(void)
{
    int n = 0;
    for (int i = 0; i < OCC_MAX; i++) {
        occasion_t o;
        if (occ_get(i, &o)) n++;
    }
    return n;
}
