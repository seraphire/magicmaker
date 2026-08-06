#include "needhelp.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "audio.h"

static const char *TAG = "needhelp";

#define HELP_POOL "/spiffs/Program/help"

static help_why_t s_why = HELP_NONE;
static char       s_needs_fw[24] = "";
static char       s_text[160]    = "";

// Which clip spoke last, as a variant NUMBER (not a position in the list, which
// would stop meaning the same clip the moment a pack adds or retires one).
// 0xFF = none yet, matching the countdown pools; 0 is a real variant.
#define NO_LAST 0xFF
static uint8_t s_last = NO_LAST;

// A version string arrives from a manifest on the network and ends up inside a
// JSON string on the poll endpoint. Keep only what a version can legitimately
// be made of, so a stray quote in somebody's manifest can't close the string
// early and turn a status field into a parser's problem. Whitelist, not
// blacklist: the set of valid characters here is tiny and known.
static void copy_version(char *dst, size_t sz, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o < sz - 1; p++) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_')
            dst[o++] = c;
    }
    dst[o] = '\0';
}

void needhelp_set(help_why_t why, const char *needs_fw)
{
    char fw[sizeof(s_needs_fw)];
    copy_version(fw, sizeof(fw), (needs_fw && needs_fw[0]) ? needs_fw : "");
    bool changed = (why != s_why) || (strcmp(fw, s_needs_fw) != 0);

    s_why = why;
    snprintf(s_needs_fw, sizeof(s_needs_fw), "%s", fw);

    switch (why) {
    case HELP_PACK_FW:
        // Written for somebody holding a phone, not for a log reader: it says
        // what to do first, because the reason this exists at all is that the
        // update might not survive the settings.
        snprintf(s_text, sizeof(s_text),
                 "New sounds are waiting, but they need firmware %s. "
                 "Export your settings below first, then update.",
                 s_needs_fw[0] ? s_needs_fw : "newer than this");
        break;
    default:
        s_text[0] = '\0';
        break;
    }

    // Only on a change: the pack sync runs every few hours and would otherwise
    // repeat this line forever, which trains you to ignore it.
    if (changed && why != HELP_NONE)
        ESP_LOGW(TAG, "needs a person: %s", s_text);
}

void needhelp_clear(void)
{
    if (s_why != HELP_NONE) ESP_LOGI(TAG, "resolved - no longer asking for help");
    s_why = HELP_NONE;
    s_needs_fw[0] = '\0';
    s_text[0] = '\0';
}

bool        needhelp_active(void)   { return s_why != HELP_NONE; }
help_why_t  needhelp_why(void)      { return s_why; }
const char *needhelp_needs_fw(void) { return s_needs_fw; }
const char *needhelp_text(void)     { return s_text; }

bool needhelp_clip(char *out, size_t sz)
{
    if (s_why == HELP_NONE) return false;

    // audio_pick_variant enumerates by reading the directory, so a retired clip
    // in the middle of the pool doesn't hide everything after it, and it does
    // the no-repeat step for us.
    int chosen = audio_pick_variant(HELP_POOL, s_last, out, sz);
    if (chosen < 0) {
        // No prompts installed. Say nothing rather than something broken - a
        // device that has never synced its Program/ clips is not a device whose
        // owner is helped by silence-shaped noise.
        return false;
    }
    s_last = (uint8_t)chosen;
    return true;
}
