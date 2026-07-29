#include "store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "store";
#define NS "tags"      // NVS namespace

// One enrolled tag: which sound to play and which animation to run.
typedef struct {
    char    sound[48];
    uint8_t anim;
} entry_t;

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

bool store_lookup(const uint8_t uid[4], char *sound, size_t sound_sz, uint8_t *anim)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    char key[9];
    uid_key(uid, key);

    entry_t e;
    size_t sz = sizeof(e);
    esp_err_t r = nvs_get_blob(h, key, &e, &sz);
    nvs_close(h);
    if (r != ESP_OK) return false;

    strncpy(sound, e.sound, sound_sz - 1);
    sound[sound_sz - 1] = '\0';
    if (anim) *anim = e.anim;
    return true;
}

esp_err_t store_save(const uint8_t uid[4], const char *sound, uint8_t anim)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;

    char key[9];
    uid_key(uid, key);

    entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.sound, sound, sizeof(e.sound) - 1);
    e.anim = anim;

    r = nvs_set_blob(h, key, &e, sizeof(e));
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) ESP_LOGI(TAG, "Saved %s -> %s (anim %u)", key, e.sound, anim);
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

void store_list(void (*cb)(const char *uid_hex, const char *sound, uint8_t anim, void *ctx),
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
            size_t sz = sizeof(e);
            if (nvs_get_blob(h, info.key, &e, &sz) == ESP_OK) {
                e.sound[sizeof(e.sound) - 1] = '\0';
                cb(info.key, e.sound, e.anim, ctx);
            }
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
