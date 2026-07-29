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

static const char *TAG = "countdown";

#define CD_DIR      "/spiffs/cd/"
#define NS          "cd"        // NVS namespace: per-band state
#define POOL_MAX    32          // sanity cap when probing pool-N.wav

// Unit thresholds. Everything is arranged so the spoken number stays 1-31 and
// is always a single whole recording.
#define DAYS_MAX    31          // above this we speak weeks
#define WEEKS_MAX   59          // above this we speak months
#define DAYS_PER_MONTH 30.44f

// Repeat-tap jokes fire on this many consecutive taps of the SAME band.
#define CHEEKY_AFTER 3

// --- small helpers ----------------------------------------------------------
static bool have(const char *name)
{
    char p[CD_PATH_MAX];
    struct stat st;
    snprintf(p, sizeof(p), CD_DIR "%s.wav", name);
    return stat(p, &st) == 0 && st.st_size > 44;
}

static void put(char paths[][CD_PATH_MAX], int *n, int max, const char *name)
{
    if (*n >= max) return;
    snprintf(paths[*n], CD_PATH_MAX, CD_DIR "%s.wav", name);
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

// Append a random member of a pool. Returns false if the pool is empty.
static bool put_random(char paths[][CD_PATH_MAX], int *n, int max, const char *pool)
{
    int cnt = pool_size(pool);
    if (cnt <= 0) return false;
    char name[32];
    snprintf(name, sizeof(name), "%s-%d", pool, (int)(esp_random() % (uint32_t)cnt) + 1);
    put(paths, n, max, name);
    return true;
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
    if (days <= DAYS_MAX)  return "days";
    if (days <= WEEKS_MAX) return "weeks";
    return "months";
}

// --- phrase assembly --------------------------------------------------------
// Append "<number> <unit>", choosing the unit so the number stays 1-31.
// Returns false if the needed clips aren't in the bank.
static bool put_count(char paths[][CD_PATH_MAX], int *n, int max, int days)
{
    int value;
    const char *unit;
    if (days <= DAYS_MAX)       { value = days;                              unit = "days";   }
    else if (days <= WEEKS_MAX) { value = (days + 3) / 7;                    unit = "weeks";  }
    else                        { value = (int)((days / DAYS_PER_MONTH) + 0.5f); unit = "months"; }

    if (value < 1)  value = 1;
    if (value > 31) value = 31;              // bank only goes to 31
    if (value == 1) unit = (unit[0] == 'd') ? "day" : (unit[0] == 'w') ? "week" : "month";

    char num[8];
    snprintf(num, sizeof(num), "%d", value);
    if (!have(num) || !have(unit)) return false;
    put(paths, n, max, num);
    put(paths, n, max, unit);
    return true;
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
        if (since <= WEEKS_MAX && chance(70)) {
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
        if (chance(35)) put_random(paths, &n, max, "lead");
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
        else if (chance(85))                  put_random(paths, &n, max, "tail");
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
    if (days <= DAYS_MAX) return true;                          // last month: every day
    if (days == 100 || days == 50 || days == 200) return true;  // round-number landmarks
    if (days <= WEEKS_MAX) return (days % 7) == 0;              // ~2 months out: weekly
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
    if (nvs_get_blob(h, uid_hex, &rec, &len) == ESP_OK) {
        rec.day = 0;
        nvs_set_blob(h, uid_hex, &rec, sizeof(rec));
        nvs_commit(h);
    }
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
        if (put_random(paths, &n, max, "cheeky")) {
            if (anim_out) *anim_out = ANIM_ENCHANTED;
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
    if (mode == CD_OFF ||
        (mode == CD_DAILY && rec.day == (int32_t)today)) {
        nvs_close(h);
        return 0;                                   // off, or already done today
    }

    // Taper: months out, "six months to go" barely changes day to day, so speak
    // it less often and let the band's own sound play instead. Inside the last
    // month every day counts, so it goes daily. Landmarks always speak.
    if (cfg.countdown_taper && mode == CD_DAILY && !is_speaking_day(days)) {
        nvs_close(h);
        return 0;
    }

    int n = countdown_build(days, paths, max, anim_out);
    if (n <= 0) { nvs_close(h); return 0; }         // bank missing pieces

    rec.day = (int32_t)today;
    nvs_set_blob(h, key, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "band %s: %d day(s) -> %d clip(s), first %s", key, days, n, paths[0]);
    return n;
}
