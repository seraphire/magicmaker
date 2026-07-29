#include "assets.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

static const char *TAG = "assets";

#define MOUNT          "/spiffs"
#define INSTALLED_PATH MOUNT "/installed.json"
#define INSTALLED_TMP  MOUNT "/installed.json.new"
#define MAX_FILES      64                 // sanity cap on a manifest's file list
#define FREE_SLACK     (64 * 1024)        // keep this much FS headroom spare

// --- small helpers ----------------------------------------------------------
static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = hx[in[i] >> 4]; out[i*2+1] = hx[in[i] & 0xF]; }
    out[n*2] = '\0';
}

// A manifest-supplied path must stay inside the FS and use a safe charset.
static bool path_ok(const char *p)
{
    if (!p || !*p || p[0] == '/') return false;
    if (strstr(p, "..")) return false;
    if (strcmp(p, "installed.json") == 0) return false;    // don't let it clobber our state
    for (const char *c = p; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '/' || *c == '.' || *c == '-' || *c == '_'))
            return false;
    }
    return true;
}

// mkdir -p for the directory part of a full "/spiffs/a/b/file" path.
static void ensure_parent_dirs(const char *fullpath)
{
    char tmp[192];
    strncpy(tmp, fullpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *s = tmp + 1; *s; s++) {                     // skip the leading '/'
        if (*s == '/') { *s = '\0'; mkdir(tmp, 0777); *s = '/'; }
    }
}

static size_t fs_free_bytes(void)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) return 0;
    return (total > used) ? (total - used) : 0;
}

// --- installed.json (path -> sha256 we currently have) ----------------------
static cJSON *installed_load(void)
{
    FILE *f = fopen(INSTALLED_PATH, "r");
    if (!f) return cJSON_CreateObject();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    cJSON *obj = NULL;
    if (sz > 0 && sz < 16384) {
        char *buf = malloc(sz + 1);
        if (buf) {
            size_t n = fread(buf, 1, sz, f);
            buf[n] = '\0';
            obj = cJSON_Parse(buf);
            free(buf);
        }
    }
    fclose(f);
    return obj ? obj : cJSON_CreateObject();
}

static void installed_save(const cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return;
    FILE *f = fopen(INSTALLED_TMP, "w");
    if (f) {
        fwrite(s, 1, strlen(s), f);
        fclose(f);
        rename(INSTALLED_TMP, INSTALLED_PATH);             // atomic swap
    }
    free(s);
}

static void installed_set(cJSON *obj, const char *path, const char *sha)
{
    cJSON_DeleteItemFromObject(obj, path);                 // replace if present
    cJSON_AddStringToObject(obj, path, sha);
}

// --- streaming download with on-the-fly sha256 ------------------------------
typedef struct { FILE *f; mbedtls_sha256_context sha; size_t written; bool err; } dl_ctx_t;

static esp_err_t dl_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        dl_ctx_t *c = (dl_ctx_t *)e->user_data;
        if (c && !c->err && c->f && e->data_len > 0) {
            if (fwrite(e->data, 1, e->data_len, c->f) != (size_t)e->data_len) { c->err = true; return ESP_FAIL; }
            mbedtls_sha256_update(&c->sha, (const unsigned char *)e->data, e->data_len);
            c->written += e->data_len;
        }
    }
    return ESP_OK;
}

// Download `url` to `tmp`, hashing as it writes; verify the hash matches
// `expect_hex` (case-insensitive) before returning OK. Deletes `tmp` on any
// failure so no partial/corrupt file survives.
static esp_err_t download_verify(const char *url, const char *tmp, const char *expect_hex)
{
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGE(TAG, "cannot open %s for write", tmp); return ESP_FAIL; }

    dl_ctx_t c = { .f = f, .written = 0, .err = false };
    mbedtls_sha256_init(&c.sha);
    mbedtls_sha256_starts(&c.sha, 0);                      // 0 = SHA-256 (not 224)

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = dl_event,
        .user_data         = &c,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    esp_err_t r = cl ? esp_http_client_perform(cl) : ESP_FAIL;
    int code = cl ? esp_http_client_get_status_code(cl) : -1;
    if (cl) esp_http_client_cleanup(cl);

    unsigned char digest[32];
    mbedtls_sha256_finish(&c.sha, digest);
    mbedtls_sha256_free(&c.sha);
    fclose(f);

    if (r != ESP_OK || code != 200 || c.err) {
        ESP_LOGE(TAG, "download %s failed (err=%s http=%d io_err=%d)",
                 url, esp_err_to_name(r), code, c.err);
        unlink(tmp);
        return ESP_FAIL;
    }

    char got[65];
    to_hex(digest, 32, got);
    if (expect_hex && *expect_hex && strcasecmp(got, expect_hex) != 0) {
        ESP_LOGE(TAG, "sha256 mismatch for %s: got %.16s.. want %.16s..", tmp, got, expect_hex);
        unlink(tmp);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "fetched %s (%u bytes, sha ok)", url, (unsigned)c.written);
    return ESP_OK;
}

// --- one file --------------------------------------------------------------
// Returns: 1 = downloaded, 0 = already up to date (skipped), -1 = failed.
static int sync_one(const char *base_url, cJSON *entry, cJSON *installed)
{
    cJSON *jp   = cJSON_GetObjectItem(entry, "path");
    cJSON *jsha = cJSON_GetObjectItem(entry, "sha256");
    cJSON *jby  = cJSON_GetObjectItem(entry, "bytes");
    if (!cJSON_IsString(jp) || !cJSON_IsString(jsha)) {
        ESP_LOGW(TAG, "asset entry missing path/sha256 - skipping");
        return -1;
    }
    const char *path = jp->valuestring;
    const char *sha  = jsha->valuestring;
    if (!path_ok(path)) { ESP_LOGW(TAG, "unsafe asset path '%s' - skipping", path); return -1; }

    // Already have this exact content?
    cJSON *cur = cJSON_GetObjectItem(installed, path);
    if (cJSON_IsString(cur) && strcasecmp(cur->valuestring, sha) == 0) {
        ESP_LOGD(TAG, "up to date: %s", path);
        return 0;
    }

    // Free-space guard (best effort; needs the "bytes" hint).
    if (cJSON_IsNumber(jby)) {
        size_t need = (size_t)jby->valuedouble + FREE_SLACK;
        size_t have = fs_free_bytes();
        if (need > have) {
            ESP_LOGE(TAG, "no room for %s (need ~%u, free %u) - skipping",
                     path, (unsigned)need, (unsigned)have);
            return -1;
        }
    }

    char url[320], dst[192], tmp[200];
    snprintf(url, sizeof(url), "%s%s", base_url, path);
    snprintf(dst, sizeof(dst), MOUNT "/%s", path);
    snprintf(tmp, sizeof(tmp), "%s.new", dst);

    ensure_parent_dirs(dst);
    if (download_verify(url, tmp, sha) != ESP_OK) return -1;

    if (rename(tmp, dst) != 0) {                           // atomic replace on LittleFS
        ESP_LOGE(TAG, "rename %s -> %s failed", tmp, dst);
        unlink(tmp);
        return -1;
    }
    installed_set(installed, path, sha);
    installed_save(installed);                             // persist per-file, so a crash resumes
    ESP_LOGI(TAG, "installed asset: %s", path);
    return 1;
}

// --- public: sync the whole "assets" section --------------------------------
esp_err_t assets_sync_json(const char *manifest_json, int *n_updated)
{
    if (n_updated) *n_updated = 0;

    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) { ESP_LOGW(TAG, "manifest is not valid JSON"); return ESP_FAIL; }

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsObject(assets)) {                         // firmware-only manifest: nothing to do
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *base  = cJSON_GetObjectItem(assets, "base_url");
    cJSON *files = cJSON_GetObjectItem(assets, "files");

    // files is optional (a "remove"-only manifest is valid); base_url is only
    // required when there are files to fetch.
    int count = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
    if (count > 0 && !cJSON_IsString(base)) {
        ESP_LOGW(TAG, "assets has files but no base_url");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    if (count > MAX_FILES) {
        ESP_LOGW(TAG, "manifest lists %d assets; capping at %d", count, MAX_FILES);
        count = MAX_FILES;
    }

    cJSON *installed = installed_load();
    int updated = 0, failed = 0, skipped = 0, removed = 0;

    for (int i = 0; i < count; i++) {
        int r = sync_one(base->valuestring, cJSON_GetArrayItem(files, i), installed);
        if (r == 1) updated++;
        else if (r == 0) skipped++;
        else failed++;
    }

    // Optional "remove": retire files no longer needed (incl. baked-in ones).
    // Explicit list, NOT "delete anything not in files" - a partial/wrong
    // manifest must never be able to wipe the device's audio.
    cJSON *rm = cJSON_GetObjectItem(assets, "remove");
    if (cJSON_IsArray(rm)) {
        cJSON *it;
        cJSON_ArrayForEach(it, rm) {
            if (!cJSON_IsString(it)) continue;
            const char *path = it->valuestring;
            if (!path_ok(path)) { ESP_LOGW(TAG, "unsafe remove path '%s' - skipping", path); continue; }

            char dst[192];
            snprintf(dst, sizeof(dst), MOUNT "/%s", path);
            int u = unlink(dst);
            cJSON_DeleteItemFromObject(installed, path);   // drop tracking either way
            if (u == 0) { removed++; ESP_LOGI(TAG, "removed asset: %s", path); }
            else if (errno == ENOENT) ESP_LOGD(TAG, "remove: %s already gone", path);
            else ESP_LOGW(TAG, "remove %s failed (errno %d)", dst, errno);
        }
    }

    installed_save(installed);
    cJSON_Delete(installed);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "asset sync: %d updated, %d up-to-date, %d removed, %d failed",
             updated, skipped, removed, failed);
    if (n_updated) *n_updated = updated;
    return failed ? ESP_FAIL : ESP_OK;
}
