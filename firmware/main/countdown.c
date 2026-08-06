#include "countdown.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "ntp.h"
#include "appcfg.h"
#include "occasions.h"
#include "leds.h"
#include "audio.h"

static const char *TAG = "countdown";

#define CD_DIR      "/spiffs/cd/"
#define NS          "cd"        // NVS namespace: per-band state

// Unit stepping - which unit we reach for first, so the spoken number stays 1-31
// and is always a single whole recording. These choose the STARTING tier only:
// put_count() steps to a coarser unit when the bank has no clip for it, so the
// recordings, not these numbers, have the final say on resolution.
#define CD_TIER_DAYS   0
#define CD_TIER_WEEKS  1
#define CD_TIER_MONTHS 2
#define CD_DAYS_MAX    31       // beyond this, start at weeks
#define CD_WEEKS_MAX   59       // beyond this, start at months
#define DAYS_PER_MONTH 30.44f

// How often it speaks at all - a different question from which unit it speaks
// in, though both used to share one constant. That coupling meant swapping the
// audio bank would silently move the "every day" window, so they're separate now
// even though the values happen to match today.
#define CD_DAILY_WINDOW  31     // inside this many days, speak every day
#define CD_WEEKLY_WINDOW 59     // out to here weekly, beyond it fortnightly

// Repeat-tap jokes fire on this many consecutive taps of the SAME band.
#define CHEEKY_AFTER 3

// Sentinel in cd_rec_t.day meaning "play on the next tap regardless" - set by
// the web page's "Again today". Real day numbers are days-since-1970, so any
// negative value is unambiguous.
#define CD_FORCE (-1)

// --- where a clip lives -----------------------------------------------------
// The bank is split by what actually varies:
//
//   cd/num/          d4, w2, m6, and      "four days" is four days whether it's
//                                         a trip, a birthday or Christmas
//   cd/sets/<id>/    preamble, lead-*,    THIS is what makes it a Disney trip
//                    tail-*, cheeky-* …
//
// so a new occasion is ~54 clips instead of a second copy of all 90.
//
// Resolution is a search path - active set first, shared underneath - with one
// rule that matters more than it looks:
//
//   WHICHEVER DIRECTORY ANSWERS FOR A NAME OWNS THE WHOLE FAMILY.
//
// Not a per-file union. If the birthday set ships any tail-*, its tails are the
// entire pool. Union them and a birthday countdown would occasionally close
// with a Disney line, at random - rare enough to read as a glitch, wrong enough
// to break the illusion. Masking is the difference between "a set" and "some
// extra files".
static char s_set[16] = "trip";

void countdown_set_audio_set(const char *id)
{
    snprintf(s_set, sizeof(s_set), "%s", (id && id[0]) ? id : "trip");
}

// Logical base path for `name`, or false if nothing anywhere answers to it.
// ".wav" is a logical extension throughout - audio_resolve accepts the .mp3 the
// build script may have produced instead, so a re-encode never empties a pool.
// `sz` is deliberately the caller's buffer minus room for the ".wav" that put()
// appends - the compiler can't infer that on its own, and a silent truncation
// here would produce a path that resolves to nothing.
static bool cd_logical(const char *name, char *out, size_t sz)
{
    uint8_t probe[1];
    if (sz > CD_PATH_MAX - 4) sz = CD_PATH_MAX - 4;

    snprintf(out, sz, CD_DIR "sets/%s/%s", s_set, name);
    if (audio_variants(out, probe, 1) > 0) return true;

    snprintf(out, sz, CD_DIR "num/%s", name);
    if (audio_variants(out, probe, 1) > 0) return true;

    out[0] = '\0';
    return false;
}

static bool have(const char *name)
{
    char p[CD_PATH_MAX];
    return cd_logical(name, p, sizeof(p));
}

static void put(char paths[][CD_PATH_MAX], int *n, int max, const char *name)
{
    if (*n >= max) return;
    char logical[CD_PATH_MAX - 4];
    if (!cd_logical(name, logical, sizeof(logical))) return;
    snprintf(paths[*n], CD_PATH_MAX, "%s.wav", logical);   // resolved at play time
    (*n)++;
}

// Last clip used from the pools worth not repeating, as a variant NUMBER;
// CD_NO_LAST = none yet. Not 0 for "none": 0 is a real variant (the bare name),
// and although no countdown pool has one today, "lead.mp3 exists" should not
// quietly become "never avoid a repeat of lead.mp3".
// Lead and tail are carried in the band's record (see cd_rec_t.idx) because the
// record is already written whenever the countdown speaks, so remembering costs
// nothing. Cheeky is RAM-only on purpose: it fires on consecutive taps of one
// band, which never span a reboot, so there is nothing to persist.
#define SLOT_LEAD   0
#define SLOT_TAIL   1
#define SLOT_CHEEKY 2
#define SLOT_NONE  (-1)
#define CD_NO_LAST 0xFF
static uint8_t s_last[3] = { CD_NO_LAST, CD_NO_LAST, CD_NO_LAST };

// Append a random member of a pool. Returns false if the pool is empty.
//
// With a slot, the clip used last time is excluded. Drawing from the reduced
// set rather than re-rolling matters twice over: re-rolling can spin on a pool
// of two, and simply stepping past a collision would make the clip after the
// last one twice as likely. This stays uniform over what is allowed.
static bool put_random_slot(char paths[][CD_PATH_MAX], int *n, int max,
                            const char *pool, int slot)
{
    if (*n >= max) return false;

    // audio_pick_variant does the drawing, the no-repeat step, and the
    // enumeration - by reading the directory, so a retired clip in the middle
    // of a pool no longer hides everything after it.
    char logical[CD_PATH_MAX];
    if (!cd_logical(pool, logical, sizeof(logical))) return false;

    uint8_t avoid = (slot >= 0) ? s_last[slot] : CD_NO_LAST;
    char path[CD_PATH_MAX];
    int chosen = audio_pick_variant(logical, avoid, path, sizeof(path));
    if (chosen < 0) return false;

    // Store the variant NUMBER, not a position in the list - it has to keep
    // meaning the same clip after a pack adds or retires others.
    if (slot >= 0) s_last[slot] = (uint8_t)chosen;

    snprintf(paths[(*n)++], CD_PATH_MAX, "%s", path);
    return true;
}

// Pools where an immediate repeat would not be noticed - they play rarely, or
// only ever one at a time.
static bool put_random(char paths[][CD_PATH_MAX], int *n, int max, const char *pool)
{
    return put_random_slot(paths, n, max, pool, SLOT_NONE);
}

// Pack/unpack the two persisted slots into the record's one int16_t: lead in
// the low byte, tail in the high byte. Variant numbers cap at
// AUDIO_VARIANT_MAX, so a byte each is ample.
//
// The two sentinels line up exactly: a fresh record's idx is -1, which is
// 0xFFFF, which unpacks to CD_NO_LAST in both slots - and packing two
// CD_NO_LASTs gives -1 back. "Nothing remembered" means the same thing in the
// record and in RAM, with no special case at either end.
static void seen_load(int16_t packed)
{
    s_last[SLOT_LEAD] = (uint8_t)( (uint16_t)packed       & 0xFF);
    s_last[SLOT_TAIL] = (uint8_t)(((uint16_t)packed >> 8) & 0xFF);
}

static int16_t seen_store(void)
{
    return (int16_t)(((uint16_t)s_last[SLOT_TAIL] << 8) | s_last[SLOT_LEAD]);
}

static bool chance(int percent)
{
    return (int)(esp_random() % 100) < percent;
}

// --- date math --------------------------------------------------------------
// The calendar arithmetic lives in occasions.c now, because the window check
// and the spoken phrase have to agree about what a day is.
static bool date_math(int occ, long *today, int *days)
{
    struct tm now;
    if (!ntp_localtime(&now)) return false;
    int d = occ_days_remaining(occ);
    if (d == INT_MIN) return false;
    *today = occ_days_from_civil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    *days  = d;
    return true;
}

int countdown_days_remaining(void)
{
    // Fall back to the trip when nothing is in its window, so this keeps
    // answering the question it always answered - the CLI and the web page ask
    // it to display a number, not to decide whether to speak.
    int occ = occ_active();
    if (occ < 0) occ = OCC_TRIP;
    return occ_days_remaining(occ);
}

const char *countdown_tier_name(int days)
{
    if (days <  0) return "after";
    if (days == 0) return "today";
    if (days == 1) return "tomorrow";
    if (days <= CD_DAYS_MAX)  return "days";
    if (days <= CD_WEEKS_MAX) return "weeks";
    return "months";
}

// --- phrase assembly --------------------------------------------------------
// How many of `unit` this many days works out to, or 0 if that unit is too
// coarse to describe the distance at all (three days is not "one week").
static int count_in(int tier, int days)
{
    int v = (tier == CD_TIER_DAYS)  ? days
          : (tier == CD_TIER_WEEKS) ? (days + 3) / 7
          :                           (int)((days / DAYS_PER_MONTH) + 0.5f);
    if (v < 1)  return 0;
    if (v > 31) v = 31;                      // no bank speaks past thirty-one
    return v;
}

// Append the count to the phrase.
//
// Two bank shapes are supported. A *baked* bank has one clip per count with the
// unit already in it - "d4" is "four days", "w2" is "2 weeks" - which is what a
// generated voice wants, because a number and a unit recorded separately never
// quite join. A *split* bank has "4" and "days" as separate clips, which is what
// the original human recordings are. Baked wins where both exist.
//
// The bank also decides the resolution. We start at the tier that suits the
// distance, then step to a coarser unit if that clip is missing - so a bank
// holding d1..d13 says "two weeks" at seventeen days out, while one holding
// d1..d31 says "seventeen days", with no constant to keep in sync. Stepping only
// ever goes coarser, and count_in() returns 0 rather than let three days round
// down into "one week".
static bool put_count(char paths[][CD_PATH_MAX], int *n, int max, int days)
{
    int start = (days <= CD_DAYS_MAX)  ? CD_TIER_DAYS
              : (days <= CD_WEEKS_MAX) ? CD_TIER_WEEKS
              :                          CD_TIER_MONTHS;

    for (int tier = start; tier <= CD_TIER_MONTHS; tier++) {
        int value = count_in(tier, days);
        if (value == 0) continue;            // too coarse for this distance
        char baked[8];
        snprintf(baked, sizeof(baked), "%c%d", "dwm"[tier], value);
        if (have(baked)) { put(paths, n, max, baked); return true; }
    }

    // Split bank: "<number>" + "<unit>", the unit word switching to singular at
    // one. Only the starting tier is tried - a split bank is the complete
    // original one, so a miss here means the clip genuinely isn't there.
    int value = count_in(start, days);
    if (value == 0) return false;
    const char *unit = (start == CD_TIER_DAYS)  ? (value == 1 ? "day"   : "days")
                     : (start == CD_TIER_WEEKS) ? (value == 1 ? "week"  : "weeks")
                     :                            (value == 1 ? "month" : "months");
    char num[8];
    snprintf(num, sizeof(num), "%d", value);
    if (!have(num) || !have(unit)) return false;
    put(paths, n, max, num);
    put(paths, n, max, unit);
    return true;
}

int countdown_cheeky(char paths[][CD_PATH_MAX], int max)
{
    int n = 0;
    return put_random_slot(paths, &n, max, "cheeky", SLOT_CHEEKY) ? n : 0;
}

int countdown_build(int days, char paths[][CD_PATH_MAX], int max, uint8_t *anim_out)
{
    int n = 0;
    uint8_t anim = ANIM_CELEBRATE;

    // A short warm-up before the count, so the daily moment announces itself
    // instead of starting cold. Only here - the cheeky lines stay instant,
    // which is the joke. The last two days get a more excited cut if it exists.
    const char *pre = (days >= 0 && days <= 1 && have("preamble-dayof"))
                        ? "preamble-dayof" : "preamble";
    if (have(pre)) put(paths, &n, max, pre);
    else           put_random(paths, &n, max, "preamble");   // preamble-N pool

    if (days < 0) {
        // --- after the trip -------------------------------------------------
        int since = -days;
        anim = ANIM_ENCHANTED;
        // For a couple of months, count up ("twelve days since our trip").
        // After that a growing number just gets sad, so taper to the warm lines.
        bool counted = false;
        if (since <= CD_WEEKS_MAX && chance(70)) {
            int before = n;
            if (put_count(paths, &n, max, since) &&
                put_random(paths, &n, max, "tail-since")) {
                counted = true;
            } else {
                n = before;                   // bank incomplete - fall through
            }
        }
        if (!counted) {
            // Occasionally remind them the countdown can be restarted.
            if (have("after-howto") && chance(25)) put(paths, &n, max, "after-howto");
            else if (!put_random(paths, &n, max, "after")) return 0;
        }
        if (anim_out) *anim_out = anim;
        return n;
    }

    // --- before / on the trip ----------------------------------------------
    // "today"/"tomorrow" are bare words - a six-month countdown landing on a
    // lone "Today" is a let-down. Prefer a today-N / tomorrow-N pool of complete
    // announcements ("Today's the day! We're going to Disney!"); those are whole
    // sentences, so they take no closer. Fall back to the bare word.
    bool full_line = false;
    if (days == 0) {
        anim = ANIM_FIREWORKS;
        if (put_random(paths, &n, max, "today")) full_line = true;
        else if (have("today")) put(paths, &n, max, "today");
        else return 0;
    } else if (days == 1) {
        anim = ANIM_FIREWORKS;
        if (put_random(paths, &n, max, "tomorrow")) full_line = true;
        else if (have("tomorrow")) put(paths, &n, max, "tomorrow");
        else return 0;
    } else {
        anim = (days <= 7)  ? ANIM_FIREWORKS
             : (days <= 14) ? ANIM_RAINBOW
             : (days <= 30) ? ANIM_BEOURGUEST
                            : ANIM_WELCOME;
        // A lead-in only sometimes, so it doesn't get predictable.
        if (chance(35)) put_random_slot(paths, &n, max, "lead", SLOT_LEAD);
        int before = n;
        if (!put_count(paths, &n, max, days)) { n = before; return 0; }
    }

    // Closer. The normal trailers grammatically follow a COUNT ("twelve days to
    // go", "six weeks until the castle"), so they can't be used after
    // today/tomorrow - that produced "Today to go!". Those two days take a
    // standalone closer only.
    if (days <= 1) {
        // A full-sentence announcement needs nothing after it.
        if (!full_line && days == 1 && have("mile-final"))
            put(paths, &n, max, "mile-final");
        // else: no closer - the bare word after the fanfare stands on its own.
    } else {
        const char *mile = NULL;
        if      (days == 7)              mile = "mile-week";
        else if (days == 30)             mile = "mile-month";
        else if (days <= 3)              mile = "mile-final";

        if (mile && have(mile) && chance(80)) put(paths, &n, max, mile);
        else if (chance(85))                  put_random_slot(paths, &n, max, "tail", SLOT_TAIL);
    }

    if (anim_out) *anim_out = anim;
    return n;
}

// Should the countdown speak at all on a day this far out? (Taper mode.)
// Inside the last month every day counts; further out it thins to weekly and
// then fortnightly, but landmark numbers always get their moment.
static bool is_speaking_day(int days)
{
    if (days < 0)  return (-days) <= 7 || ((-days) % 7) == 0;  // after: first week, then weekly
    if (days <= CD_DAILY_WINDOW) return true;                   // last month: every day
    if (days == 100 || days == 50 || days == 200) return true;  // round-number landmarks
    if (days <= CD_WEEKLY_WINDOW) return (days % 7) == 0;       // ~2 months out: weekly
    return (days % 14) == 0;                                    // further: fortnightly
}

// --- per-band state ---------------------------------------------------------
// {day we last greeted this band, which variant, mode}. Same 8-byte footprint
// as the original {day,idx} thanks to padding, so older records read cleanly.
// How often a band speaks the countdown. Declared here rather than further
// down because rec_load() below validates against them.
#define CD_DAILY  0
#define CD_ALWAYS 1
#define CD_OFF    2

// APPEND ONLY, for the reasons spelled out in store.c. This one is less
// dangerous than an enrolment - the worst case is a band forgetting it was
// greeted today, or reverting to the default speaking mode - but "less
// dangerous" is not a reason to lose a setting somebody chose.
// `day` stayed exactly where it was and still means occasion 0, the trip. The
// per-occasion marks were APPENDED after it rather than folded into an array
// starting at zero, because every record already in the field is the original
// eight bytes and rec_load copies only as far as the overlap - so an existing
// band keeps its trip mark, and gains seven zeroed ones meaning "not yet
// spoken", which is true.
//
// Separate marks per occasion, not one shared mark, is the whole point: a tap
// that spent today's greeting on Christmas must not also silence the trip. It's
// what lets repeat taps DRAIN the queue instead of the occasions competing for
// one slot. (#36 builds on this.)
typedef struct {
    int32_t day;                    // occasion 0 (the trip)
    int16_t idx;
    uint8_t mode;
    uint8_t _pad;                   // was implicit padding; named so the offsets
                                    // below are the compiler's business, not luck
    int32_t day_n[OCC_MAX - 1];     // occasions 1..OCC_MAX-1
} cd_rec_t;

static int32_t rec_day(const cd_rec_t *r, int occ)
{
    if (occ <= 0 || occ >= OCC_MAX) return r->day;
    return r->day_n[occ - 1];
}

static void rec_set_day(cd_rec_t *r, int occ, int32_t v)
{
    if (occ <= 0 || occ >= OCC_MAX) r->day = v;
    else                            r->day_n[occ - 1] = v;
}

// Length-tolerant read, matching store.c's entry_load(). Returns false if there
// is no record at all; a record of an unexpected SIZE is read as far as the
// overlap goes rather than discarded, so growing this struct later - or an OTA
// rollback after a build that already grew it - doesn't reset what the owner
// picked.
// Generously larger than sizeof(cd_rec_t), which now grows with OCC_MAX. Sized
// to today's struct, this becomes a trap that springs the next time a field is
// appended - the read silently truncates and the new field always reads back
// zero, a symptom that looks nothing like its cause.
#define CD_READ_MAX 256

static bool rec_load(nvs_handle_t h, const char *uid_hex, cd_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    out->idx  = -1;                    // defaults, for a record that predates a field
    out->mode = CD_DAILY;

    uint8_t buf[CD_READ_MAX];
    size_t  sz = sizeof(buf);
    if (nvs_get_blob(h, uid_hex, buf, &sz) != ESP_OK || sz == 0) return false;

    memcpy(out, buf, (sz < sizeof(*out)) ? sz : sizeof(*out));
    if (out->mode > CD_OFF) out->mode = CD_DAILY;   // stored value is input, not a promise
    return true;
}

static void band_key(const uint8_t *uid, uint8_t len, char key[12])
{
    if (len >= 4) snprintf(key, 12, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    else          strcpy(key, "btn");
}

int countdown_get_mode(const char *uid_hex)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return CD_DAILY;
    cd_rec_t rec;
    int mode = rec_load(h, uid_hex, &rec) ? rec.mode : CD_DAILY;
    nvs_close(h);
    return mode;
}

void countdown_set_mode(const char *uid_hex, int mode)
{
    if (mode < CD_DAILY || mode > CD_OFF) mode = CD_DAILY;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    cd_rec_t rec;
    rec_load(h, uid_hex, &rec);        // best effort: defaults if absent
    rec.mode = (uint8_t)mode;
    nvs_set_blob(h, uid_hex, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);
}

// Clear "already greeted today" for EVERY band, keeping their modes. Called when
// the trip date changes: otherwise the daily gate hides the new count until
// tomorrow, which looks exactly like the date not having saved.
void countdown_reset_all(void)
{
    nvs_iterator_t it = NULL;
    esp_err_t r = nvs_entry_find("nvs", NS, NVS_TYPE_BLOB, &it);
    int cleared = 0;
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            cd_rec_t rec;
            if (rec_load(h, info.key, &rec)) {
                // Every occasion's mark, not just the trip's. Clearing one and
                // leaving the rest would mean changing the date re-armed the
                // countdown for some bands and silently not for others.
                rec.day = 0;                    // keep idx + mode
                memset(rec.day_n, 0, sizeof(rec.day_n));
                nvs_set_blob(h, info.key, &rec, sizeof(rec));
                nvs_commit(h);
                cleared++;
            }
            nvs_close(h);
        }
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (cleared) ESP_LOGI(TAG, "trip date changed - re-armed %d band(s)", cleared);
}

void countdown_reset_today(const char *uid_hex)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    cd_rec_t rec;
    // Read to keep the band's mode and last-clip index, but write either way.
    // Conditioning the write on a successful read means a band with no record
    // yet silently keeps its greeted-today mark, and the button on the page
    // appears to do nothing at all. (rec_load also no longer throws away a
    // record whose size doesn't match this build - see its comment.)
    rec_load(h, uid_hex, &rec);                  // best effort
    rec.day = CD_FORCE;                          // not just "unmarked" - forced
    esp_err_t rc = nvs_set_blob(h, uid_hex, &rec, sizeof(rec));
    if (rc == ESP_OK) nvs_commit(h);
    else ESP_LOGW(TAG, "reset-today %s failed: %s", uid_hex, esp_err_to_name(rc));
    nvs_close(h);
}

// --- the per-tap decision ---------------------------------------------------
int countdown_due(const uint8_t *uid, uint8_t uid_len,
                  char paths[][CD_PATH_MAX], int max, uint8_t *anim_out)
{
    char key[12];
    band_key(uid, uid_len, key);

    // Consecutive taps of the same band (RAM only - a reboot forgetting this is
    // fine, and arguably kinder). Tapping a different band resets the streak.
    static char s_last_key[12] = { 0 };
    static int  s_streak = 0;
    if (strcmp(key, s_last_key) == 0) s_streak++;
    else { strncpy(s_last_key, key, sizeof(s_last_key) - 1); s_streak = 1; }

    if (s_streak >= CHEEKY_AFTER) {
        int n = 0;
        if (put_random_slot(paths, &n, max, "cheeky", SLOT_CHEEKY)) {
            // A pulse that follows the voice, not a choreographed show - these
            // are one-liners for tapping the same band twice, and a full
            // celebration makes an aside feel like an event.
            if (anim_out) *anim_out = ANIM_PULSE;
            ESP_LOGI(TAG, "band %s tapped %dx - cheeky: %s", key, s_streak, paths[0]);
            s_streak = 0;      // start the count over, so the NEXT taps go back to
                               // the band's own sound - otherwise every tap from
                               // here on is cheeky and the card's sound is lost
            return n;
        }
    }

    // Which occasion speaks is DERIVED from the calendar, every tap. Nothing
    // stores "it's Christmas now", so nothing can be wrong about it in March.
    int occ = occ_active();
    if (occ < 0) return 0;                         // nothing planned, or nothing
                                                   // inside its window today

    // The occasion names its own audio set, which is what makes it sound like
    // itself. For the trip that set IS cfg.audio_set, so the CLI override keeps
    // working unchanged; for a seasonal one the occasion wins, because a
    // Christmas countdown speaking in the trip's voice is the bug this replaces.
    occasion_t o;
    if (occ_get(occ, &o)) countdown_set_audio_set(o.id);

    device_config_t cfg;
    appcfg_load(&cfg);

    long today; int days;
    if (!date_math(occ, &today, &days)) return 0;  // clock not set -> skip

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 0;
    cd_rec_t rec;
    rec_load(h, key, &rec);                      // defaults if absent or short

    int mode = rec.mode;                         // rec_load already validated it

    // "Again today" on the web page sets this. It means play it on the next tap,
    // full stop - so it skips the once-a-day mark, the taper, and even an Off
    // mode. Clearing only the day mark wasn't enough: months from the trip the
    // taper speaks just occasionally, so the button let the tap past one gate
    // and the next one stopped it, and from the page it looked like the button
    // did nothing.
    // "Again today" is deliberately checked on the TRIP's mark whichever
    // occasion is speaking: the button means "say something on the next tap",
    // and making the owner guess which occasion the device considers current
    // would defeat the point of the occasion being derived in the first place.
    bool forced = (rec.day == CD_FORCE);

    if (!forced) {
        if (mode == CD_OFF || (mode == CD_DAILY && rec_day(&rec, occ) == (int32_t)today)) {
            ESP_LOGD(TAG, "band %s: %s", key,
                     mode == CD_OFF ? "countdown off" : "already greeted today");
            nvs_close(h);
            return 0;
        }
        // Taper: months out, "six months to go" barely changes day to day, so
        // speak it less often and let the band's own sound play instead. Inside
        // the last month every day counts, so it goes daily. Landmarks always
        // speak.
        if (cfg.countdown_taper && mode == CD_DAILY && !is_speaking_day(days)) {
            ESP_LOGD(TAG, "band %s: taper - %d days out isn't a speaking day", key, days);
            nvs_close(h);
            return 0;
        }
    }

    // Seed the "don't repeat" slots from this band's own record, so the phrase
    // built below avoids whatever it used last time. Per band, not global: two
    // bands should not steer each other's variety.
    seen_load(rec.idx);

    int n = countdown_build(days, paths, max, anim_out);
    if (n <= 0) { nvs_close(h); return 0; }         // bank missing pieces

    if (forced) rec.day = 0;     // consume the force, or every tap from here on
                                 // is forced and the daily gate never returns
    rec_set_day(&rec, occ, (int32_t)today);
    rec.idx = seen_store();      // rides along on a write that was happening anyway
    nvs_set_blob(h, key, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "band %s: %d day(s) -> %d clip(s), first %s", key, days, n, paths[0]);
    return n;
}
