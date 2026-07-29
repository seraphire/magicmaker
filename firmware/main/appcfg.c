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
    strncpy(cfg->manifest_url, OTA_MANIFEST_URL, sizeof(cfg->manifest_url) - 1);
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
    get_str(h, "manif", cfg->manifest_url, sizeof(cfg->manifest_url));
    cfg->config_version    = (uint32_t)get_i32(h, "ver", cfg->config_version);
    nvs_close(h);

    ESP_LOGI(TAG, "Loaded config: ssid='%s' name='%s' trip=%04d-%02d-%02d",
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
    nvs_set_str(h, "manif", cfg->manifest_url);
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
