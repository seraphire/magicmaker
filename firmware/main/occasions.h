// ---------------------------------------------------------------------------
// occasions.h - several countdowns, each with its own window.
//
// The device used to know one date. That was the right shape for a gift built
// around one trip, and the wrong shape for a reader that stays on a shelf: once
// 20 January 2027 passes, the whole point of the object expires.
//
// An occasion is a date, a lead time, and the name of the audio set that gives
// it its voice. Christmas with `lead_days: 24` wakes up on 1 December EVERY
// year with nobody touching it, speaks for its 24 days, and goes quiet again.
//
// WHICH OCCASION IS ACTIVE IS DERIVED, NEVER STORED. It's whichever one is
// nearest and inside its window, recomputed from the clock every time it's
// asked. There is no "current occasion" field, so there is nothing to fall out
// of sync with the calendar - the failure where the device insists it's still
// Christmas in March cannot be represented.
//
// Slot 0 is special and deliberately so: it IS the trip date in appcfg, not a
// copy of it. The existing web page, CLI and "No trip planned" checkbox go on
// editing exactly what they always edited, and nothing had to be migrated for
// a device already in the field. Slots 1..7 are stored records.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Sixteen, not eight. Christmas, Halloween, the trip and two birthdays is five
// before anyone gets creative, and the ceiling is cheap: a slot is one NVS blob,
// and the per-band record grows four bytes per slot (~7 KB across a hundred
// bands, in a 64 KB partition). Raising it later would be safe but pointless
// churn - stored-data changes are free right now and won't be after the gift
// leaves, so buy the headroom while it costs nothing.
//
// Sixteen is not the real answer to "how many occasions exist", though: the
// device should eventually hold only what's near-term and take the rest from a
// pushed schedule (#38, and OCC_F_MANAGED below is already the hook for it).
// A list that has to fit entirely on the device is the thing to grow out of,
// not a number to keep raising.
#define OCC_MAX        16
#define OCC_ID_LEN     16      // also the audio-set directory name
#define OCC_LABEL_LEN  32

// One emoji, as UTF-8. Sixteen bytes because an emoji is not one character:
// a plain one is 4 bytes, a variation selector adds 3, and a ZWJ sequence
// (family, profession, flag) can run past 20. This holds anything reasonable
// and truncates the rest at a codepoint boundary rather than mid-sequence -
// half a ZWJ sequence renders as two unrelated glyphs, which looks like a bug.
//
// Deliberately an emoji rather than an image: it costs a handful of bytes, and
// it's drawn by the VIEWER'S device, so it's always the native glyph on their
// phone with nothing to upload, resize, cache or serve. Artwork is a separate
// feature with a separate cost (#45), not a better version of this one.
#define OCC_ICON_LEN   16

// The trip. Backed by appcfg's trip_y/m/d, not by a stored occasion record.
#define OCC_TRIP       0

// Bit flags. Absent bits are the safe reading in every case, so a record from a
// future build that gains a flag still behaves sanely here.
#define OCC_F_ENABLED  0x01
#define OCC_F_ANNUAL   0x02    // ignore `year`; recurs on the same month/day
// Pushed by the remote schedule rather than added by the owner. A sync may
// replace a managed record; it must never touch one without this bit, or the
// first schedule push quietly deletes something somebody added by hand.
#define OCC_F_MANAGED  0x04

typedef struct {
    char     id[OCC_ID_LEN];        // audio set directory, e.g. "xmas"
    char     label[OCC_LABEL_LEN];  // for the web page, e.g. "Christmas"
    uint16_t year;                  // ignored when OCC_F_ANNUAL
    uint8_t  month;                 // 1-12
    uint8_t  day;                   // 1-31
    uint16_t lead_days;             // how long before the date it speaks.
                                    // 0 = unbounded, which is the trip's
                                    // behaviour and why the trip needs no window
    uint8_t  flags;
    uint8_t  _pad;                  // keep the struct's size explicit
    char     icon[OCC_ICON_LEN];    // APPENDED, so records written before this
                                    // field existed read back with an empty
                                    // icon rather than being discarded
} occasion_t;

// Read slot `i` (0..OCC_MAX-1). False if the slot is empty or out of range.
// Slot 0 is synthesised from appcfg on every call.
bool occ_get(int i, occasion_t *out);

// Write slot `i`. Slot 0 is refused - edit the trip through appcfg, so there is
// exactly one writer for it.
bool occ_set(int i, const occasion_t *o);

// Empty slot `i` (0 is refused, as above).
bool occ_clear(int i);

// Days from local-today to this occasion's NEXT occurrence. INT_MIN if the
// clock isn't set or the slot is empty. Negative only for a one-off date that
// has passed - an annual occasion rolls to next year rather than going
// negative, because "minus 300 days to Christmas" is not a thing anyone says.
int  occ_days_remaining(int i);

// Is this occasion close enough to speak? An unbounded one (lead_days 0) always
// is, provided it hasn't passed.
bool occ_in_window(int i);

// The occasion that should speak right now: enabled, in its window, and nearest.
// -1 if nothing qualifies. Derived every call - see the header comment.
int  occ_active(void);

// How many slots hold something (including the trip when it's enabled).
int  occ_count(void);

// Days since 1970-01-01 for a civil date (Hinnant's algorithm). No mktime, so
// no DST pitfalls - just a serial day number, and subtracting two of them counts
// calendar days. Exported because the window check and the spoken phrase need
// the same arithmetic, and two copies of a calendar routine is one too many.
long occ_days_from_civil(int y, unsigned m, unsigned d);

// Copy a UTF-8 string into a fixed buffer without splitting a character. Use
// this for any icon field: a plain strncpy or snprintf truncates on BYTES, and
// an emoji cut mid-sequence renders as a replacement box - or, if the cut lands
// on a zero-width joiner, as two unrelated glyphs, which looks like a bug in
// whatever drew it rather than in whatever stored it.
void occ_icon_copy(char *dst, size_t dst_sz, const char *src);

// Accept an icon as literal UTF-8 OR as codepoints - "U+1F383", and joined
// sequences as "U+1F468+200D+1F373". Use this for anything typed at the serial
// console: the console echoes bytes above 0x7F but strips them before parsing,
// so a pasted emoji looks accepted and arrives as nothing at all. The web UI
// sends real UTF-8, so both forms are accepted rather than one replacing the
// other. Falls back to occ_icon_copy when the input isn't in "U+" form.
void occ_icon_parse(char *dst, size_t dst_sz, const char *src);
