#include "appcfg.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "appcfg";
#define NS "appcfg"

// Read a string key into dst (always NUL-terminated). Leaves the caller's
// default in place if the key is missing.
static void get_str(nvs_handle_t h, const char *key, char *dst, size_t sz)
{
    size_t len = sz;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) return;  // keep default
    dst[sz - 1] = '\0';
}

static int32_t get_i32(nvs_handle_t h, const char *key, int32_t def)
{
    int32_t v = def;
    nvs_get_i32(h, key, &v);
    return v;
}

static uint8_t get_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v = def;
    nvs_get_u8(h, key, &v);
    return v;
}

void appcfg_load(device_config_t *cfg)
{
    // Compiled-in defaults first, so anything unreadable is still valid.
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->device_name, CFG_DEFAULT_DEVICE_NAME, sizeof(cfg->device_name) - 1);
    cfg->trip_year        = CFG_DEFAULT_TRIP_YEAR;
    cfg->trip_month       = CFG_DEFAULT_TRIP_MONTH;
    cfg->trip_day         = CFG_DEFAULT_TRIP_DAY;
    cfg->countdown_enabled = true;
    cfg->idle_led_enabled  = true;
    cfg->countdown_taper   = true;
    cfg->boot_audio_enabled = true;
    cfg->idle_color        = 0x000000FF;      // classic blue breathe
    cfg->ring_leds         = RING_LED_COUNT;  // config.h defaults; per-device in NVS
    cfg->mickey_leds       = MICKEY_LED_COUNT;
    cfg->ring_first        = RING_FIRST;
    strncpy(cfg->manifest_url, OTA_MANIFEST_URL, sizeof(cfg->manifest_url) - 1);
    strncpy(cfg->audio_set, "trip", sizeof(cfg->audio_set) - 1);
    strncpy(cfg->trip_label, "The trip", sizeof(cfg->trip_label) - 1);
    strncpy(cfg->trip_icon, "\xF0\x9F\x8F\xB0", sizeof(cfg->trip_icon) - 1);  // castle
    cfg->config_version    = APPCFG_VERSION;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config; using defaults");
        return;
    }

    get_str(h, "ssid", cfg->wifi_ssid,     sizeof(cfg->wifi_ssid));
    get_str(h, "pass", cfg->wifi_password, sizeof(cfg->wifi_password));
    get_str(h, "name", cfg->device_name,   sizeof(cfg->device_name));
    cfg->trip_year        = get_i32(h, "trip_y", cfg->trip_year);
    cfg->trip_month       = get_i32(h, "trip_m", cfg->trip_month);
    cfg->trip_day         = get_i32(h, "trip_d", cfg->trip_day);
    cfg->countdown_enabled = get_u8(h, "cd_en", cfg->countdown_enabled);
    cfg->idle_led_enabled  = get_u8(h, "idle_en", cfg->idle_led_enabled);
    cfg->countdown_taper   = get_u8(h, "cd_taper", cfg->countdown_taper);
    cfg->boot_audio_enabled = get_u8(h, "boot_au", cfg->boot_audio_enabled);
    cfg->idle_color        = (uint32_t)get_i32(h, "idle_col", (int32_t)cfg->idle_color);
    cfg->ring_leds         = get_i32(h, "led_ring", cfg->ring_leds);
    cfg->mickey_leds       = get_i32(h, "led_face", cfg->mickey_leds);
    cfg->ring_first        = get_u8(h, "led_rf", cfg->ring_first);
    get_str(h, "manif", cfg->manifest_url, sizeof(cfg->manifest_url));
    get_str(h, "assetu", cfg->assets_url, sizeof(cfg->assets_url));
    get_str(h, "aset",   cfg->audio_set,  sizeof(cfg->audio_set));
    get_str(h, "tlabel", cfg->trip_label, sizeof(cfg->trip_label));
    get_str(h, "ticon",  cfg->trip_icon,  sizeof(cfg->trip_icon));
    cfg->config_version    = (uint32_t)get_i32(h, "ver", cfg->config_version);
    nvs_close(h);

    // Debug, not info: occ_get() synthesises the trip occasion from this config
    // on every call, so listing the occasions - or deciding which one speaks on
    // a tap - loads it eight times and buried the actual event in a wall of
    // identical lines. The read itself is cheap (NVS keeps its own page cache);
    // it was only ever the logging that made it look expensive.
    ESP_LOGD(TAG, "Loaded config: ssid='%s' name='%s' trip=%04d-%02d-%02d",
             cfg->wifi_ssid, cfg->device_name,
             cfg->trip_year, cfg->trip_month, cfg->trip_day);
}

esp_err_t appcfg_save(const device_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;

    nvs_set_str(h, "ssid", cfg->wifi_ssid);
    nvs_set_str(h, "pass", cfg->wifi_password);
    nvs_set_str(h, "name", cfg->device_name);
    nvs_set_i32(h, "trip_y", cfg->trip_year);
    nvs_set_i32(h, "trip_m", cfg->trip_month);
    nvs_set_i32(h, "trip_d", cfg->trip_day);
    nvs_set_u8(h, "cd_en",  cfg->countdown_enabled ? 1 : 0);
    nvs_set_u8(h, "idle_en", cfg->idle_led_enabled ? 1 : 0);
    nvs_set_u8(h, "cd_taper", cfg->countdown_taper ? 1 : 0);
    nvs_set_u8(h, "boot_au", cfg->boot_audio_enabled ? 1 : 0);
    nvs_set_i32(h, "idle_col", (int32_t)cfg->idle_color);
    nvs_set_i32(h, "led_ring", cfg->ring_leds);
    nvs_set_i32(h, "led_face", cfg->mickey_leds);
    nvs_set_u8(h, "led_rf", cfg->ring_first ? 1 : 0);
    nvs_set_str(h, "manif", cfg->manifest_url);
    nvs_set_str(h, "assetu", cfg->assets_url);
    nvs_set_str(h, "aset",   cfg->audio_set);
    nvs_set_str(h, "tlabel", cfg->trip_label);
    nvs_set_str(h, "ticon",  cfg->trip_icon);
    nvs_set_i32(h, "ver", (int32_t)APPCFG_VERSION);

    r = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) ESP_LOGI(TAG, "Saved config (ssid='%s')", cfg->wifi_ssid);
    return r;
}

bool appcfg_has_wifi(const device_config_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0';
}

void appcfg_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");     // NOT_FOUND is fine
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "Wi-Fi credentials cleared (recovery)");
}
