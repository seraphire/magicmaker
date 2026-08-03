#include "sounds.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_random.h"
#include "audio.h"

static const char *TAG = "sounds";

#define SOUND_DIR   "/spiffs/"
#define GROUP_MAX   24        // assignable groups; the dropdown gets unusable
                              // long before this and it is only ~2.7 KB static
#define VARIANT_MAX 16        // probe ceiling for name-1, name-2, ...

// One assignable action. A GROUP, not a file: "chime" covers chime.wav plus any
// chime-1, chime-2 ... beside it, and a tap picks between them.
typedef struct {
    char      id[24];         // stable storage key, from the filename stem
    char      label[32];      // friendly name, derived unless overridden below
    char      path[48];       // logical base path - need not exist as a file
    anim_id_t anim;           // default at enrollment; the band stores its own
    uint8_t   variants;       // how many clips are actually behind it
} group_t;

static group_t s_g[GROUP_MAX];
static int     s_n = 0;

// Names worth keeping when derivation would do worse. Everything else gets a
// label from its filename, which is why "be-our-guest" needs no entry here.
static const struct { const char *id; const char *label; anim_id_t anim; } KNOWN[] = {
    { "startours",    "Star Tours",              ANIM_RAINBOW    },  // not "Startours"
    { "walt-welcome", "Walt's Welcome",          ANIM_WELCOME    },  // apostrophe
    { "operational",  "All Systems Operational", ANIM_CELEBRATE  },
    { "foolish",      "Foolish Mortals",         ANIM_ENCHANTED  },
    { "excellent",    "Excellent!",              ANIM_FIREWORKS  },
    { "beourguest",   "Be Our Guest",            ANIM_BEOURGUEST },  // legacy id
    { "be-our-guest", "Be Our Guest",            ANIM_BEOURGUEST },
    { "hello",        "Hello World",             ANIM_WELCOME    },
    { "chime",        "Chime",                   ANIM_CELEBRATE  },
};
#define KNOWN_N ((int)(sizeof(KNOWN) / sizeof(KNOWN[0])))

// --- small helpers ----------------------------------------------------------
// "/spiffs/chime-2.wav" -> "chime". Drops the directory, the extension, and a
// trailing "-<digits>" so a variant answers as its group. Without that last
// step a pinned variant would match nothing and silently lose its animation.
static void base_id(const char *path, char *out, size_t sz)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;

    size_t n = strlen(s);
    const char *dot = strrchr(s, '.');
    if (dot && dot > s) n = (size_t)(dot - s);

    // Peel a trailing "-<digits>", but only if something is left in front of it
    // ("mile-1" is a variant of "mile"; "-3" alone is not a name).
    size_t k = n;
    while (k > 0 && isdigit((unsigned char)s[k - 1])) k--;
    if (k > 1 && k < n && s[k - 1] == '-') n = k - 1;

    if (n >= sz) n = sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

// "be-our-guest" -> "Be Our Guest". Good enough that only oddities need KNOWN.
static void derive_label(const char *id, char *out, size_t sz)
{
    size_t o = 0;
    bool start = true;
    for (const char *p = id; *p && o < sz - 1; p++) {
        char c = *p;
        if (c == '-' || c == '_') { out[o++] = ' '; start = true; continue; }
        out[o++] = start ? (char)toupper((unsigned char)c) : c;
        start = false;
    }
    out[o] = '\0';
}

static int find_group(const char *id)
{
    for (int i = 0; i < s_n; i++) if (strcmp(s_g[i].id, id) == 0) return i;
    return -1;
}

// --- discovery --------------------------------------------------------------
void sounds_init(void)
{
    s_n = 0;
    DIR *d = opendir(SOUND_DIR);
    if (!d) { ESP_LOGE(TAG, "cannot open " SOUND_DIR); return; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        // Only the root: Program/, cd/ and www/ are system content, not
        // assignable actions.
        // The image is built with --name-max=64, so a name cannot exceed that;
        // the precision is stated anyway because dirent declares d_name far
        // larger and the compiler is right not to take our word for it.
        char full[96];
        snprintf(full, sizeof(full), SOUND_DIR "%.64s", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0) continue;

        char id[24];
        base_id(e->d_name, id, sizeof(id));
        if (id[0] == '\0') continue;

        int i = find_group(id);
        if (i < 0) {
            if (s_n >= GROUP_MAX) { ESP_LOGW(TAG, "more than %d groups; ignoring %s", GROUP_MAX, id); continue; }
            i = s_n++;
            memset(&s_g[i], 0, sizeof(s_g[i]));
            snprintf(s_g[i].id, sizeof(s_g[i].id), "%s", id);
            // The stored path is the LOGICAL base - ".wav" is a logical
            // extension too (audio_resolve accepts the .mp3 the build made), and
            // the base file need not exist: a pack may ship only mansion-1..3.
            snprintf(s_g[i].path, sizeof(s_g[i].path), SOUND_DIR "%s.wav", id);
            derive_label(id, s_g[i].label, sizeof(s_g[i].label));
            s_g[i].anim = ANIM_CELEBRATE;
            for (int k = 0; k < KNOWN_N; k++) {
                if (strcmp(KNOWN[k].id, id) == 0) {
                    snprintf(s_g[i].label, sizeof(s_g[i].label), "%s", KNOWN[k].label);
                    s_g[i].anim = KNOWN[k].anim;
                    break;
                }
            }
        }
        if (s_g[i].variants < 255) s_g[i].variants++;
    }
    closedir(d);

    ESP_LOGI(TAG, "%d assignable sound(s) found:", s_n);
    for (int i = 0; i < s_n; i++)
        ESP_LOGI(TAG, "  %-14s %-26s x%u", s_g[i].id, s_g[i].label, s_g[i].variants);
}

// --- the list ---------------------------------------------------------------
int         sound_count(void)  { return s_n; }
const char *sound_path(int i)  { return (i >= 0 && i < s_n) ? s_g[i].path  : NULL; }
const char *sound_id(int i)    { return (i >= 0 && i < s_n) ? s_g[i].id    : NULL; }
const char *sound_label(int i) { return (i >= 0 && i < s_n) ? s_g[i].label : NULL; }
int         sound_variants(int i) { return (i >= 0 && i < s_n) ? s_g[i].variants : 0; }

// Matching goes through base_id, so a band pinned to "/spiffs/chime-2.wav"
// still answers as "chime" and keeps its animation. Matching on the exact path
// meant a pinned variant fell through to ANIM_CELEBRATE, which looks like a
// lighting bug rather than a naming one.
anim_id_t sound_anim(const char *path)
{
    if (!path || !path[0]) return ANIM_CELEBRATE;
    char id[24];
    base_id(path, id, sizeof(id));
    int i = find_group(id);
    if (i >= 0) return s_g[i].anim;
    for (int k = 0; k < KNOWN_N; k++)          // still answer for a file that
        if (strcmp(KNOWN[k].id, id) == 0) return KNOWN[k].anim;   // has since gone
    return ANIM_CELEBRATE;
}

// Ids the compiled table used before discovery derived them from filenames.
// The files are walt-welcome.wav and be-our-guest.wav, so discovery now calls
// them that; a page loaded before the update would still POST the old id.
// Enrollments are unaffected either way - those store the path, not the id.
static const struct { const char *was; const char *now; } LEGACY_ID[] = {
    { "walt",       "walt-welcome" },
    { "beourguest", "be-our-guest" },
};

bool sound_path_for_id(const char *id, char *out, size_t sz)
{
    if (!id) return false;
    if (strcmp(id, SOUND_RANDOM_ID) == 0) { if (sz) out[0] = '\0'; return true; }
    int i = find_group(id);
    for (int k = 0; i < 0 && k < (int)(sizeof(LEGACY_ID)/sizeof(LEGACY_ID[0])); k++)
        if (strcmp(LEGACY_ID[k].was, id) == 0) i = find_group(LEGACY_ID[k].now);
    if (i < 0) return false;
    snprintf(out, sz, "%s", s_g[i].path);
    return true;
}

bool sound_id_for_path(const char *path, char *out, size_t sz)
{
    if (!path || path[0] == '\0') { snprintf(out, sz, "%s", SOUND_RANDOM_ID); return true; }
    char id[24];
    base_id(path, id, sizeof(id));
    if (find_group(id) < 0) return false;
    snprintf(out, sz, "%s", id);
    return true;
}

// --- what actually plays ----------------------------------------------------
// A stored path is a logical base: "/spiffs/chime.wav" means chime OR any of
// its numbered variants. Expand HERE rather than at enrollment, or the band is
// locked to whichever clip existed the day it was set up.
//
// A pinned variant costs nothing extra: "/spiffs/chime-2.wav" probes
// "chime-2-1", finds none, and comes back a pool of one - so grouped-random and
// play-exactly-this are the same code path with no flag to store.
void sound_pick(const char *logical, char *out, size_t sz)
{
    if (!logical || !logical[0]) { if (sz) out[0] = '\0'; return; }
    snprintf(out, sz, "%s", logical);          // fall back to what we were given

    char stem[48];
    snprintf(stem, sizeof(stem), "%s", logical);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    const char *cand[VARIANT_MAX];
    static char buf[VARIANT_MAX][64];
    int n = 0;

    snprintf(buf[n], sizeof(buf[0]), "%s.wav", stem);
    if (audio_resolve(buf[n], NULL, 0)) cand[n] = buf[n], n++;

    for (int v = 1; n < VARIANT_MAX; v++) {
        snprintf(buf[n], sizeof(buf[0]), "%s-%d.wav", stem, v);
        if (!audio_resolve(buf[n], NULL, 0)) break;   // stop at the first gap
        cand[n] = buf[n]; n++;
    }

    if (n > 0) snprintf(out, sz, "%s", cand[esp_random() % (uint32_t)n]);
}
