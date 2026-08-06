// ---------------------------------------------------------------------------
// needhelp.h - "I can't finish this on my own."
//
// Some updates the device genuinely cannot apply by itself: an audio pack that
// declares a firmware floor above what's running, and later, a release that
// needs a person to export their settings first. Today those fail into the log,
// which is precisely where nobody is looking - the reader carries on sounding
// completely healthy while quietly never updating again.
//
// So it says so out loud, after a scan, in its own voice.
//
// DELIBERATELY RAM-ONLY. The state is re-derived from the next sync rather than
// remembered, which means it heals itself: fix the cause and the complaint
// stops without anything having to go back and clear a flag. A persisted
// version could outlive its reason and leave a working device asking for help
// forever - the same argument as deriving the active occasion from the calendar
// instead of storing it.
//
// The spoken clips name only "Magic Maker dot local", never a version, a
// diagnosis or a URL. All of that changes and a recording can't; it belongs on
// the page, where a release can rewrite it instead of re-recording it.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HELP_NONE = 0,
    HELP_PACK_FW,          // an audio pack needs newer firmware than we run
} help_why_t;

// Raise or clear the condition. Calling set() with the same reason twice is
// free - it only logs on a change, so a sync running every few hours doesn't
// fill the log with the same line.
void        needhelp_set(help_why_t why, const char *needs_fw);
void        needhelp_clear(void);

bool        needhelp_active(void);
help_why_t  needhelp_why(void);
const char *needhelp_needs_fw(void);     // "" when not known

// One short human sentence for the web page. Never spoken - the page can carry
// detail that a clip cannot.
const char *needhelp_text(void);

// A clip to speak, avoiding whichever was used last so three lines read as a
// character rather than a stuck recording. False if the pool isn't installed,
// which is the normal state on a device that has never synced the prompts.
bool        needhelp_clip(char *out, size_t sz);
