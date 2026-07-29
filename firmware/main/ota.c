#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "cJSON.h"
#include "verscmp.h"
#include "store.h"
#include "assets.h"
#include "appcfg.h"

static const char *TAG = "ota";

// Manifest can now carry an asset list, so it's bigger than the old flat form.
#define OTA_MANIFEST_MAX 8192

bool ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "OTA image confirmed healthy on '%s' (rollback cancelled)", run->label);
        else
            ESP_LOGW(TAG, "could not mark image valid");
        return true;            // this boot is a freshly-installed image
    }
    return false;
}

// --- OTA install session state (single-flight) ------------------------------
static esp_ota_handle_t       s_handle  = 0;
static const esp_partition_t *s_part    = NULL;
static bool                   s_active  = false;
static size_t                 s_written = 0;

esp_err_t ota_begin(size_t size_hint)
{
    if (s_active) return ESP_ERR_INVALID_STATE;        // one session at a time

    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p) { ESP_LOGE(TAG, "no OTA partition"); return ESP_FAIL; }

    if (size_hint > p->size) {                         // reject oversize up front
        ESP_LOGE(TAG, "image %u > slot %u - rejected", (unsigned)size_hint, (unsigned)p->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t r = esp_ota_begin(p, (size_hint ? size_hint : OTA_SIZE_UNKNOWN), &s_handle);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(r)); return r; }

    s_part = p; s_active = true; s_written = 0;
    ESP_LOGI(TAG, "OTA begin -> '%s' (slot %u bytes)", p->label, (unsigned)p->size);
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;
    if (s_written + len > s_part->size) {              // hard cap = slot size
        ESP_LOGE(TAG, "image exceeds slot - aborting");
        ota_abort_session();
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t r = esp_ota_write(s_handle, data, len);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(r)); ota_abort_session(); return r; }
    s_written += len;
    return ESP_OK;
}

esp_err_t ota_finish(void)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;

    esp_err_t r = esp_ota_end(s_handle);               // validates the image
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "image invalid: %s - firmware unchanged", esp_err_to_name(r));
        s_active = false; s_part = NULL;
        return r;
    }
    r = esp_ota_set_boot_partition(s_part);            // switch boot only if valid
    s_active = false;
    if (r != ESP_OK) { ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(r)); s_part = NULL; return r; }

    ESP_LOGI(TAG, "OTA complete: %u bytes -> next boot '%s'", (unsigned)s_written, s_part->label);
    s_part = NULL;
    return ESP_OK;
}

void ota_abort_session(void)
{
    if (s_active) {
        esp_ota_abort(s_handle);
        s_active = false; s_part = NULL; s_written = 0;
        ESP_LOGW(TAG, "OTA session aborted - firmware unchanged");
    }
}

size_t ota_bytes_written(void) { return s_written; }

// Accumulates response body into the caller's buffer as chunks arrive.
typedef struct { char *buf; size_t cap; size_t len; } fetch_ctx_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        fetch_ctx_t *c = (fetch_ctx_t *)e->user_data;
        if (c && c->buf && c->cap) {
            size_t space = (c->cap - 1) - c->len;          // leave room for NUL
            size_t n = ((size_t)e->data_len < space) ? (size_t)e->data_len : space;
            if (n) {
                memcpy(c->buf + c->len, e->data, n);
                c->len += n;
                c->buf[c->len] = '\0';
            }
        }
    }
    return ESP_OK;
}

int ota_http_get(const char *url, char *out, size_t out_sz)
{
    if (out && out_sz) out[0] = '\0';
    fetch_ctx_t ctx = { .buf = out, .cap = out_sz, .len = 0 };

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = on_event,
        .user_data         = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,   // bundled root CAs
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return -1;

    esp_err_t r    = esp_http_client_perform(cl);
    int       code = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    if (r != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(r));
        return -1;
    }
    ESP_LOGI(TAG, "GET %s -> HTTP %d, %d bytes", url, code, (int)ctx.len);
    return (int)ctx.len;
}

int ota_parse_manifest(const char *json, ota_manifest_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "manifest is not valid JSON"); return -1; }

    // Preferred (nested) shape: {"firmware":{"version":..,"url":..}, "assets":{..}}.
    // Falls back to the legacy flat shape {"version":..,"firmware_url":..} so old
    // manifests (and the on-device selftest) still parse.
    cJSON *fwobj = cJSON_GetObjectItem(root, "firmware");
    cJSON *ver, *fw;
    if (cJSON_IsObject(fwobj)) {
        ver = cJSON_GetObjectItem(fwobj, "version");
        fw  = cJSON_GetObjectItem(fwobj, "url");
    } else {
        ver = cJSON_GetObjectItem(root, "version");
        fw  = cJSON_GetObjectItem(root, "firmware_url");
    }
    // Optional self-relocation pointer - parsed regardless of the firmware
    // section (a manifest may relocate without shipping firmware).
    out->manifest_url[0] = '\0';
    cJSON *mu = cJSON_GetObjectItem(root, "manifest_url");
    if (cJSON_IsString(mu)) {
        strncpy(out->manifest_url, mu->valuestring, sizeof(out->manifest_url) - 1);
        out->manifest_url[sizeof(out->manifest_url) - 1] = '\0';
    }

    int rc = -1;
    if (cJSON_IsString(ver) && cJSON_IsString(fw)) {
        strncpy(out->version, ver->valuestring, sizeof(out->version) - 1);
        out->version[sizeof(out->version) - 1] = '\0';
        strncpy(out->firmware_url, fw->valuestring, sizeof(out->firmware_url) - 1);
        out->firmware_url[sizeof(out->firmware_url) - 1] = '\0';
        rc = 0;
    } else {
        ESP_LOGW(TAG, "manifest missing 'version' or 'firmware_url'");
    }
    cJSON_Delete(root);
    return rc;
}

int ota_fetch_manifest(const char *url, ota_manifest_t *out)
{
    char *buf = malloc(OTA_MANIFEST_MAX); // off-stack: TLS already uses a lot
    if (!buf) return -1;
    int n = ota_http_get(url, buf, OTA_MANIFEST_MAX);
    int rc = (n > 0) ? ota_parse_manifest(buf, out) : -1;
    free(buf);
    return rc;
}

static bool url_scheme_ok(const char *url)
{
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

esp_err_t ota_install_from_url(const char *url)
{
    if (!url || !url_scheme_ok(url)) {
        ESP_LOGE(TAG, "refusing non-http(s) url");
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // TLS via bundled root CAs
        .timeout_ms        = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    ESP_LOGI(TAG, "OTA download: %s", url);
    esp_err_t r = esp_https_ota(&cfg);       // stream -> slot, validate, set boot
    if (r == ESP_OK) ESP_LOGI(TAG, "image installed - reboot to run it");
    else             ESP_LOGE(TAG, "OTA download/install failed: %s", esp_err_to_name(r));
    return r;
}

esp_err_t ota_update_from_manifest(const char *manifest_url,
                                   const char *cur_version, bool *installed)
{
    if (installed) *installed = false;

    // Fetch the manifest once; both the asset sync and the firmware check read it.
    char *json = malloc(OTA_MANIFEST_MAX);
    if (!json) return ESP_FAIL;
    int n = ota_http_get(manifest_url, json, OTA_MANIFEST_MAX);
    if (n <= 0) { free(json); return ESP_FAIL; }

    // Assets FIRST: new firmware may reference new sounds, so land the media
    // before switching the app slot. Media with old firmware is harmless; new
    // firmware missing its media is not - so if assets fail, we defer firmware.
    int assets_updated = 0;
    bool assets_ok = (assets_sync_json(json, &assets_updated) == ESP_OK);
    if (assets_updated > 0)
        ESP_LOGI(TAG, "synced %d asset file(s) from manifest", assets_updated);

    ota_manifest_t m;
    int prc = ota_parse_manifest(json, &m);

    // Self-relocation: if the manifest names a new manifest_url, adopt it for
    // future checks (host migration). Persisted only - we do NOT re-fetch it
    // this run, which sidesteps any A->B->A redirect loop. No setup-mode gate:
    // this manifest is already trusted to deliver firmware, so trusting it to
    // move the pointer is the same trust boundary.
    if (m.manifest_url[0] && url_scheme_ok(m.manifest_url)) {
        device_config_t cfg;
        appcfg_load(&cfg);
        if (strcmp(cfg.manifest_url, m.manifest_url) != 0) {
            if (strlen(m.manifest_url) < sizeof(cfg.manifest_url)) {
                ESP_LOGW(TAG, "manifest relocated: '%s' -> '%s'", cfg.manifest_url, m.manifest_url);
                strncpy(cfg.manifest_url, m.manifest_url, sizeof(cfg.manifest_url) - 1);
                cfg.manifest_url[sizeof(cfg.manifest_url) - 1] = '\0';
                appcfg_save(&cfg);
            } else {
                ESP_LOGW(TAG, "new manifest URL too long (%u) - keeping current",
                         (unsigned)strlen(m.manifest_url));
            }
        }
    }

    // Firmware: install only if the manifest version is newer.
    esp_err_t rc = ESP_OK;
    if (prc == 0 && version_cmp(m.version, cur_version) > 0) {
        if (!assets_ok) {
            ESP_LOGW(TAG, "assets incomplete - deferring firmware %s->%s until they sync",
                     cur_version, m.version);
        } else {
            ESP_LOGI(TAG, "updating %s -> %s from %s", cur_version, m.version, m.firmware_url);
            rc = ota_install_from_url(m.firmware_url);
            if (rc == ESP_OK) {
                if (installed) *installed = true;
                // A manifest-driven update is *self-initiated*, not "unexpectedly
                // online", so the reboot into the new image should be silent.
                // One-shot: the boot path clears it after skipping the greeting.
                // (Only this path sets it - web-upload pushes and `ota-url` still
                // announce, as intended.)
                store_set_flag("quiet_boot", 1);
            }
        }
    } else {
        ESP_LOGI(TAG, "firmware up to date (running %s)", cur_version);
    }

    free(json);
    // Surface an asset failure even when no firmware update was due, so the
    // caller/user knows a re-run is needed.
    if (rc == ESP_OK && !assets_ok) rc = ESP_FAIL;
    return rc;
}
