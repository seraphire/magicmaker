#include "ota.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "ota_pubkey.h"
#include <strings.h>
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
    cJSON *ver, *fw, *sha, *sig;
    if (cJSON_IsObject(fwobj)) {
        ver = cJSON_GetObjectItem(fwobj, "version");
        fw  = cJSON_GetObjectItem(fwobj, "url");
        sha = cJSON_GetObjectItem(fwobj, "sha256");
        sig = cJSON_GetObjectItem(fwobj, "sig");
    } else {
        ver = cJSON_GetObjectItem(root, "version");
        fw  = cJSON_GetObjectItem(root, "firmware_url");
        sha = cJSON_GetObjectItem(root, "sha256");
        sig = cJSON_GetObjectItem(root, "sig");
    }
    out->sha256[0] = '\0';
    if (cJSON_IsString(sha) && strlen(sha->valuestring) == 64) {
        strncpy(out->sha256, sha->valuestring, sizeof(out->sha256) - 1);
        out->sha256[sizeof(out->sha256) - 1] = '\0';
    }
    out->sig[0] = '\0';
    if (cJSON_IsString(sig) && strlen(sig->valuestring) < sizeof(out->sig))
        strcpy(out->sig, sig->valuestring);
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

// Hash `len` bytes of the slot the image was just written to.
//
// Read back from flash rather than hashed on the way in, because
// esp_https_ota owns the stream - it handles the redirect chain to GitHub's CDN
// and the TLS session, and reimplementing that to get at the bytes would trade
// a real risk for a cosmetic one. What landed on flash is also the thing we
// actually care about: a download that hashed correctly but wrote badly is
// still a broken image.
static esp_err_t hash_written_image(size_t len, char out_hex[65], uint8_t out_raw[32])
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p || len == 0 || len > p->size) return ESP_ERR_INVALID_SIZE;

    uint8_t *buf = malloc(4096);
    if (!buf) return ESP_ERR_NO_MEM;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    esp_err_t r = ESP_OK;
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;
        if (n > 4096) n = 4096;
        r = esp_partition_read(p, off, buf, n);
        if (r != ESP_OK) break;
        mbedtls_sha256_update(&sha, buf, n);
        off += n;
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    free(buf);

    if (r != ESP_OK) return r;
    if (out_raw) memcpy(out_raw, digest, 32);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out_hex[i*2] = hx[digest[i] >> 4]; out_hex[i*2+1] = hx[digest[i] & 0xF]; }
    out_hex[64] = '\0';
    return ESP_OK;
}

// Base64 -> bytes. Small and local; mbedtls_base64_decode would pull the whole
// module in for one call site.
static int b64_decode(const char *in, uint8_t *out, size_t out_sz)
{
    static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0; int bits = 0; size_t n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ') continue;
        const char *q = strchr(A, *p);
        if (!q) return -1;                       // not base64 - reject, don't guess
        acc = (acc << 6) | (uint32_t)(q - A);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_sz) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

// Is `sig_b64` a valid signature, by our key, over `digest`?
//
// The signature covers the image's SHA-256 rather than the manifest as a whole,
// so the manifest stays editable - retitle a release, add an asset, relocate the
// host - without re-signing. What's signed is the only thing that must not
// change: which bytes are the firmware.
static bool sig_ok(const uint8_t digest[32], const char *sig_b64)
{
    uint8_t sig[96];
    int siglen = b64_decode(sig_b64, sig, sizeof(sig));
    if (siglen <= 0) { ESP_LOGE(TAG, "signature is not valid base64"); return false; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_parse_public_key(&pk, OTA_PUBKEY_DER, sizeof(OTA_PUBKEY_DER));
    if (r != 0) { ESP_LOGE(TAG, "built-in public key won't parse (%d)", r); mbedtls_pk_free(&pk); return false; }

    r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, (size_t)siglen);
    mbedtls_pk_free(&pk);
    if (r != 0) { ESP_LOGE(TAG, "signature does NOT verify (mbedtls %d)", r); return false; }
    return true;
}

esp_err_t ota_install_from_url(const char *url, const char *expect_sha256,
                               const char *expect_sig)
{
    if (!url || !url_scheme_ok(url)) {
        ESP_LOGE(TAG, "refusing non-http(s) url");
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // TLS via bundled root CAs
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        // A GitHub release URL 302s to a signed CDN link that runs to several
        // hundred characters of query string. The default 512-byte TX buffer
        // can't hold that request line, and the redirect fails as an opaque
        // "Failed to open HTTP connection". Give it room.
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
    };
    // Follow the redirect chain to the CDN host.
    esp_https_ota_config_t cfg = {
        .http_config           = &http,
        .partial_http_download = false,
    };

    ESP_LOGI(TAG, "OTA download: %s", url);

    // The step-by-step API rather than the one-shot esp_https_ota(), for one
    // reason: the one-shot call sets the boot partition itself, so there is no
    // moment between "the image is on flash" and "the device will boot it" in
    // which to check anything. Here finish() is ours to withhold.
    esp_https_ota_handle_t h = NULL;
    esp_err_t r = esp_https_ota_begin(&cfg, &h);
    if (r != ESP_OK) { ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(r)); return r; }

    while ((r = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) { }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(r));
        esp_https_ota_abort(h);
        return r;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "OTA download truncated - discarding");
        esp_https_ota_abort(h);
        return ESP_FAIL;
    }

    // One hash, two questions. Identity: is this the image the manifest named?
    // Authenticity: did the key-holder say so? Both answered here, in the gap
    // between "written to flash" and "will boot", which is the whole reason
    // finish() is withheld above.
    {
        int len = esp_https_ota_get_image_len_read(h);
        char got[65];
        uint8_t digest[32];
        if (len <= 0 || hash_written_image((size_t)len, got, digest) != ESP_OK) {
            ESP_LOGE(TAG, "could not hash the written image - discarding");
            esp_https_ota_abort(h);
            return ESP_FAIL;
        }

        if (expect_sha256 && expect_sha256[0]) {
            if (strcasecmp(got, expect_sha256) != 0) {
                // Never reached the boot partition, so the running firmware is
                // untouched and the next check simply tries again.
                ESP_LOGE(TAG, "sha256 MISMATCH - refusing to install");
                ESP_LOGE(TAG, "  manifest %.16s...  image %.16s...", expect_sha256, got);
                esp_https_ota_abort(h);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "image sha256 verified (%.16s...)", got);
        } else {
            ESP_LOGW(TAG, "manifest carries no sha256");
        }

        // Unsigned is REFUSED, not merely warned about. A check that can be
        // skipped by omitting a field is not a check - anyone able to serve a
        // manifest could simply leave the signature out. The escape hatch is
        // the AP upload, which reaches a different function entirely.
        if (!expect_sig || !expect_sig[0]) {
            ESP_LOGE(TAG, "manifest is UNSIGNED - refusing to install over the air");
            ESP_LOGE(TAG, "  (to install anyway: hold the button at power-on and upload it)");
            esp_https_ota_abort(h);
            return ESP_FAIL;
        }
        if (!sig_ok(digest, expect_sig)) {
            ESP_LOGE(TAG, "signature check FAILED - refusing to install");
            esp_https_ota_abort(h);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "signature verified - image is ours");
    }

    r = esp_https_ota_finish(h);             // validates the header, sets boot
    if (r == ESP_OK) ESP_LOGI(TAG, "image installed - reboot to run it");
    else             ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(r));
    return r;
}

esp_err_t ota_update_from_manifest(const char *manifest_url,
                                   const char *cur_version, bool *installed,
                                   bool verify_assets)
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

    // The device's own audio pack, if it has one. Separate URL, separate
    // document, usually a private one - the public firmware manifest can't
    // carry a personal voice bank.
    //
    // A pack failure does NOT defer the firmware, and the difference from the
    // block above is the whole point. Those assets ship in the same document as
    // the firmware, so new code may need them. A pack is somebody's personal
    // audio on somebody's own host - the firmware never references it, and it
    // is the thing most likely to be offline, moved, or paid up until last
    // Tuesday. Coupling them means one dead host freezes firmware updates
    // permanently, including the update that would fix whatever else is wrong.
    {
        device_config_t pcfg;
        appcfg_load(&pcfg);
        if (pcfg.assets_url[0]) {
            int pack_updated = 0;
            if (assets_sync_pack(pcfg.assets_url, cur_version, &pack_updated, NULL,
                                 verify_assets) != ESP_OK)
                ESP_LOGW(TAG, "audio pack did not sync - continuing (will retry next check)");
            if (pack_updated > 0)
                ESP_LOGI(TAG, "synced %d file(s) from the audio pack", pack_updated);
        }
    }

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
            rc = ota_install_from_url(m.firmware_url, m.sha256, m.sig);
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
