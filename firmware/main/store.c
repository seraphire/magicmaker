#include "store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "store";
#define NS "tags"      // NVS namespace

// One enrolled tag: WHICH SOUND, by id, and which animation to run.
//
// The id ("chime", or "chime-2" to pin one variant) rather than a path, because
// a path nails the band to a file. Themes need to move files - a Halloween pack
// wants to own `chime` and make it a creaking door - and with a stored path
// every band would silently un-assign the moment one did.
//
// Resolution is the same search path the countdown uses: the active theme
// first, the core bank underneath. So a card assigned `chime` follows the
// season, and one assigned `startours` always plays Star Tours because nothing
// else answers to that name. Themed cards and floating cards fall out of the
// naming, with no flag to store.
//
// ---------------------------------------------------------------------------
// APPEND ONLY from here on. Never reorder a field, never resize one.
// ---------------------------------------------------------------------------
// The one exception was spending this change: with no device yet out of reach,
// dropping the old 48-byte path cost nothing. Once one ships, a change that
// makes a record unreadable doesn't throw an error - it un-enrols every band in
// the house, quietly, and the owner finds their cards playing the wrong sounds
// one morning. Growing the struct stays safe because entry_load() tolerates a
// length mismatch both ways; reordering never will, since the bytes would land
// in the wrong fields and be accepted.
typedef struct {
    char    sound_id[32];   // "" = the RANDOM action
    uint8_t anim;
} entry_t;

// "/spiffs/chime-2.wav" -> "chime-2". Directory and extension only; the "-N" is
// KEPT, because that is how a band pins one variant.
//
// Local rather than sounds.c's base_id(), which strips the suffix - right for
// matching a variant back to its group's animation, wrong for storage, where
// dropping it would silently widen a pinned card to the whole pool.
static void path_to_id(const char *path, char *out, size_t sz)
{
    if (!path || !path[0]) { out[0] = '\0'; return; }
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    size_t n = strlen(s);
    const char *dot = strrchr(s, '.');
    if (dot && dot > s) n = (size_t)(dot - s);
    if (n >= sz) n = sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

// Read a record without insisting it is exactly the size this firmware expects.
//
// The plain nvs_get_blob(&e, sizeof(e)) it replaces returns INVALID_LENGTH on
// any mismatch, and the caller then reports "not enrolled". Both directions
// happen for real:
//   - shorter, because an older firmware wrote a smaller struct;
//   - longer, because a NEWER one wrote a bigger struct and then OTA rollback
//     put this build back in charge.
// The second is the nasty one: it means a failed update can cost the owner
// their enrolments even though nothing was wrong with the data.
//
// Reading with a generous buffer succeeds either way (NVS only complains when
// the buffer is too small) and reports what was actually there. Copy the
// overlap, leave anything this build doesn't have on disk at its zero default.
#define ENTRY_READ_MAX 128           // headroom for records future builds write

// The pre-1.3 record: { char sound[48]; uint8_t anim; } = 49 bytes, holding a
// literal path. Distinguishable from the current 33-byte record by SIZE alone,
// which is what makes the conversion certain rather than a guess.
#define LEGACY_PATH_LEN  48
#define LEGACY_SIZE      (LEGACY_PATH_LEN + 1)

static bool entry_load(nvs_handle_t h, const char *key, entry_t *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t buf[ENTRY_READ_MAX];
    size_t  sz = sizeof(buf);
    if (nvs_get_blob(h, key, buf, &sz) != ESP_OK || sz == 0) return false;

    if (sz >= LEGACY_SIZE) {
        // Old shape: a literal path where the id now lives. Convert on read, so
        // nobody has to re-tap twelve cards. Transitional - delete once the last
        // device that could be holding one has been reflashed.
        char path[LEGACY_PATH_LEN + 1];
        memcpy(path, buf, LEGACY_PATH_LEN);
        path[LEGACY_PATH_LEN] = '\0';
        path_to_id(path, out->sound_id, sizeof(out->sound_id));
        out->anim = buf[LEGACY_PATH_LEN];
        ESP_LOGI(TAG, "migrated %s: '%s' -> id '%s'", key, path, out->sound_id);
        return true;
    }

    memcpy(out, buf, (sz < sizeof(*out)) ? sz : sizeof(*out));
    out->sound_id[sizeof(out->sound_id) - 1] = '\0';   // a stored string is input, not a promise
    return true;
}

// NVS keys must be <= 15 chars; an 8-char hex UID fits comfortably.
static void uid_key(const uint8_t uid[4], char key[9])
{
    static const char *hx = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) {
        key[i * 2]     = hx[uid[i] >> 4];
        key[i * 2 + 1] = hx[uid[i] & 0x0F];
    }
    key[8] = '\0';
}

void store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase; reinitialising");
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool store_lookup(const uint8_t uid[4], char *sound_id, size_t id_sz, uint8_t *anim)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    char key[9];
    uid_key(uid, key);

    entry_t e;
    bool ok = entry_load(h, key, &e);
    nvs_close(h);
    if (!ok) return false;

    strncpy(sound_id, e.sound_id, id_sz - 1);
    sound_id[id_sz - 1] = '\0';
    if (anim) *anim = e.anim;
    return true;
}

esp_err_t store_save(const uint8_t uid[4], const char *sound_id, uint8_t anim)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;

    char key[9];
    uid_key(uid, key);

    entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.sound_id, sound_id ? sound_id : "", sizeof(e.sound_id) - 1);
    e.anim = anim;

    r = nvs_set_blob(h, key, &e, sizeof(e));
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) ESP_LOGI(TAG, "Saved %s -> id '%s' (anim %u)", key, e.sound_id, anim);
    return r;
}

esp_err_t store_erase(const uint8_t uid[4])
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;

    char key[9];
    uid_key(uid, key);

    r = nvs_erase_key(h, key);            // NOT_FOUND is fine - already random
    if (r == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) ESP_LOGI(TAG, "Erased %s (back to random)", key);
    return r;
}

void store_list(void (*cb)(const char *uid_hex, const char *sound_id, uint8_t anim, void *ctx),
                void *ctx)
{
    nvs_iterator_t it = NULL;
    esp_err_t r = nvs_entry_find("nvs", NS, NVS_TYPE_BLOB, &it);
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);              // info.key = the UID hex string

        nvs_handle_t h;
        if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
            entry_t e;
            if (entry_load(h, info.key, &e))
                cb(info.key, e.sound_id, e.anim, ctx);
            nvs_close(h);
        }
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);                    // safe on NULL
}

// --- simple persisted settings (separate "cfg" namespace) ------------------
uint8_t store_get_flag(const char *key, uint8_t def)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return def;
    uint8_t v = def;
    nvs_get_u8(h, key, &v);        // leaves v = def if the key isn't set
    nvs_close(h);
    return v;
}

void store_set_flag(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

void store_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing entire NVS partition");
    nvs_flash_erase();          // wipes tags + appcfg + cfg; reboot follows
}
