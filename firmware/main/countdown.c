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
#include "leds.h"
#include "audio.h"

static const char *TAG = "countdown";

#define CD_DIR      "/spiffs/cd/"
#define NS          "cd"        // NVS namespace: per-band state
#define POOL_MAX    32          // sanity cap when probing pool-N.wav

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

// --- small helpers ----------------------------------------------------------
// ".wav" here is the logical name; audio_resolve() also accepts the .mp3 the
// build script may have produced instead, so a re-encode never empties a pool.
static bool have(const char *name)
{
    char p[CD_PATH_MAX];
    snprintf(p, sizeof(p), CD_DIR "%s.wav", name);
    return audio_resolve(p, NULL, 0);
}

static void put(char paths[][CD_PATH_MAX], int *n, int max, const char *name)
{
    if (*n >= max) return;
    snprintf(paths[*n], CD_PATH_MAX, CD_DIR "%s.wav", name);   // resolved at play time
    (*n)++;
}

// How many clips exist in a numbered pool ("lead" -> lead-1.wav, lead-2.wav...).
static int pool_size(const char *pool)
{
    char name[32];
    int n = 0;
    while (n < POOL_MAX) {
        snprintf(name, sizeof(name), "%s-%d", pool, n + 1);
        if (!have(name)) break;
        n++;
    }
    return n;
}

// Last clip used from the pools worth not repeating, 1-based; 0 = none yet.
// Lead and tail are carried in the band's record (see cd_rec_t.idx) because the
// record is already written whenever the countdown speaks, so remembering costs
// nothing. Cheeky is RAM-only on purpose: it fires on consecutive taps of one
// band, which never span a reboot, so there is nothing to persist.
#define SLOT_LEAD   0
#define SLOT_TAIL   1
#define SLOT_CHEEKY 2
#define SLOT_NONE  (-1)
static uint8_t s_last[3];

// Append a random member of a pool. Returns false if the pool is empty.
//
// With a slot, the clip used last time is excluded. Drawing from the reduced
// set rather than re-rolling matters twice over: re-rolling can spin on a pool
// of two, and simply stepping past a collision would make the clip after the
// last one twice as likely. This stays uniform over what is allowed.
static bool put_random_slot(char paths[][CD_PATH_MAX], int *n, int max,
                            const char *pool, int slot)
{
    int cnt = pool_size(pool);
    if (cnt <= 0) return false;

    int pick;
    uint8_t avoid = (slot >= 0) ? s_last[slot] : 0;
    if (cnt > 1 && avoid >= 1 && avoid <= cnt) {
        pick = (int)(esp_random() % (uint32_t)(cnt - 1)) + 1;   // 1..cnt-1
        if (pick >= avoid) pick++;                              // skip the one just used
    } else {
        pick = (int)(esp_random() % (uint32_t)cnt) + 1;
    }
    if (slot >= 0) s_last[slot] = (uint8_t)pick;

    char name[32];
    snprintf(name, sizeof(name), "%s-%d", pool, pick);
    put(paths, n, max, name);
    return true;
}

// Pools where an immediate repeat would not be noticed - they play rarely, or
// only ever one at a time.
static bool put_random(char paths[][CD_PATH_MAX], int *n, int max, const char *pool)
{
    return put_random_slot(paths, n, max, pool, SLOT_NONE);
}

// Pack/unpack the two persisted slots into the record's one int16_t: lead in the
// low byte, tail in the high byte. Pools cap at POOL_MAX (32), so a byte each is
// ample, and the -1 that means "nothing yet" unpacks to two out-of-range values
// which the guard above already ignores.
static void seen_load(int16_t packed)
{
    s_last[SLOT_LEAD] = (packed >= 0) ? (uint8_t)( packed       & 0xFF) : 0;
    s_last[SLOT_TAIL] = (packed >= 0) ? (uint8_t)((packed >> 8) & 0xFF) : 0;
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
// Days since 1970-01-01 for a civil date (Hinnant). No mktime/DST pitfalls -
// just a serial day number, so subtracting two dates counts calendar days.
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static bool date_math(const device_config_t *cfg, long *today, int *days)
{
    struct tm now;
    if (!ntp_localtime(&now)) return false;
    *today = days_from_civil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    *days  = (int)(days_from_civil(cfg->trip_year, cfg->trip_month, cfg->trip_day) - *today);
    return true;
}

int countdown_days_remaining(void)
{
    device_config_t cfg;
    appcfg_load(&cfg);
    long today; int days;
    if (!date_math(&cfg, &today, &days)) return INT_MIN;
    return days;
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
typedef struct { int32_t day; int16_t idx; uint8_t mode; } cd_rec_t;

#define CD_DAILY  0
#define CD_ALWAYS 1
#define CD_OFF    2

static void band_key(const uint8_t *uid, uint8_t len, char key[12])
{
    if (len >= 4) snprintf(key, 12, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    else          strcpy(key, "btn");
}

int countdown_get_mode(const char *uid_hex)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return CD_DAILY;
    cd_rec_t rec = { 0 };
    size_t len = sizeof(rec);
    int mode = CD_DAILY;
    if (nvs_get_blob(h, uid_hex, &rec, &len) == ESP_OK && rec.mode <= CD_OFF) mode = rec.mode;
    nvs_close(h);
    return mode;
}

void countdown_set_mode(const char *uid_hex, int mode)
{
    if (mode < CD_DAILY || mode > CD_OFF) mode = CD_DAILY;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    cd_rec_t rec = { .day = 0, .idx = -1, .mode = CD_DAILY };
    size_t len = sizeof(rec);
    nvs_get_blob(h, uid_hex, &rec, &len);
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
            cd_rec_t rec; size_t len = sizeof(rec);
            if (nvs_get_blob(h, info.key, &rec, &len) == ESP_OK && rec.day != 0) {
                rec.day = 0;                    // keep idx + mode
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
    cd_rec_t rec = { .day = 0, .idx = -1, .mode = CD_DAILY };
    size_t len = sizeof(rec);
    // Read to keep the band's mode and last-clip index, but write either way.
    // Conditioning the write on a successful read means a band with no record
    // yet - or one whose stored blob doesn't match the current struct size, so
    // nvs_get_blob returns INVALID_LENGTH - silently keeps its greeted-today
    // mark, and the button on the page appears to do nothing at all.
    nvs_get_blob(h, uid_hex, &rec, &len);        // best effort
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

    device_config_t cfg;
    appcfg_load(&cfg);
    if (!cfg.countdown_enabled) return 0;          // "No trip planned"

    long today; int days;
    if (!date_math(&cfg, &today, &days)) return 0; // clock not set -> skip

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return 0;
    cd_rec_t rec = { .day = 0, .idx = -1, .mode = CD_DAILY };
    size_t len = sizeof(rec);
    nvs_get_blob(h, key, &rec, &len);

    int mode = (rec.mode <= CD_OFF) ? rec.mode : CD_DAILY;

    // "Again today" on the web page sets this. It means play it on the next tap,
    // full stop - so it skips the once-a-day mark, the taper, and even an Off
    // mode. Clearing only the day mark wasn't enough: months from the trip the
    // taper speaks just occasionally, so the button let the tap past one gate
    // and the next one stopped it, and from the page it looked like the button
    // did nothing.
    bool forced = (rec.day == CD_FORCE);

    if (!forced) {
        if (mode == CD_OFF || (mode == CD_DAILY && rec.day == (int32_t)today)) {
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

    rec.day = (int32_t)today;
    rec.idx = seen_store();      // rides along on a write that was happening anyway
    nvs_set_blob(h, key, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "band %s: %d day(s) -> %d clip(s), first %s", key, days, n, paths[0]);
    return n;
}
