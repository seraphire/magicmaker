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
#include "ota.h"          // ota_http_get - one HTTP fetch helper, not two
#include "verscmp.h"
#include "needhelp.h"

static const char *TAG = "assets";

// The firmware version the last DEFERRED sync asked for; "" when nothing is
// being held back. RAM only - it is a fact about the last sync, not a setting,
// and the next sync is the authority on whether it is still true.
static char s_requires[24] = "";

// The "needs a person" state is raised and cleared HERE rather than by each
// caller, because there is more than one caller - the periodic network task and
// the `sync-media` console command - and the first version only did it in one.
// The console sync then succeeded while the reader carried on asking for help
// about a problem it had just confirmed was gone. A fact belongs to the code
// that establishes it, not to whoever remembers to ask.
//
// A FAILED fetch deliberately changes nothing: unreachable is not "needs a
// person", and a previously known deferral is still true while the host is
// down. Only a sync that actually read the manifest gets an opinion.

#define MOUNT          "/spiffs"
#define INSTALLED_PATH MOUNT "/installed.json"
#define INSTALLED_TMP  MOUNT "/installed.json.new"
// Sanity cap on a manifest's file list. 128 rather than a rounder, smaller
// number because a complete bank is bigger than it looks: the countdown alone
// is a clip per number per unit, and the real device already carries 111 files.
// A cap a full pack can't fit under isn't a safety net, it's a bug.
#define MAX_FILES      128
// Pack bookkeeping shares installed.json rather than adding a second state file
// - one atomic write, one thing to lose. It hangs off a key no file path can
// take (path_ok rejects it), so it can't collide with a tracked asset.
#define PACK_KEY       "_pack"
// A pack manifest carries no firmware section, but it carries every file: at
// ~105 bytes per compact entry, 128 files is ~13 KB. The buffer is freed before
// the downloads start, so this doesn't stack with the TLS session.
#define PACK_MAX       16384
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
    if (strcmp(p, PACK_KEY) == 0) return false;            // reserved: pack state lives under this key
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
        .timeout_ms        = 20000,
        // Same reason as the firmware download: a redirect to a signed CDN link
        // (GitHub releases, S3, ...) overflows the default 512-byte TX buffer.
        .buffer_size       = 2048,
        .buffer_size_tx    = 2048,
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

// Hash a file already on flash. Returns false if it can't be read at all.
//
// This is what lets installed.json be a *cache* rather than the only truth.
// The record and the files can disagree - `idf.py flash` rewrites the storage
// partition, so a reflash wipes the record while leaving every file intact, and
// the device would otherwise re-download a bank it already has, byte for byte.
// The bytes on disk are the fact; the record is just a way to avoid re-reading
// them.
static bool file_sha256(const char *fullpath, char out_hex[65])
{
    FILE *f = fopen(fullpath, "rb");
    if (!f) return false;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    // 512 at a time: this runs on the network task's stack, and the point is to
    // be cheap enough that checking is never the expensive option.
    unsigned char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        mbedtls_sha256_update(&sha, buf, n);

    bool ok = !ferror(f);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    fclose(f);

    if (ok) to_hex(digest, 32, out_hex);
    return ok;
}

// --- one file --------------------------------------------------------------
// Returns: 1 = downloaded, 0 = already up to date (skipped), -1 = failed.
static int sync_one(const char *base_url, cJSON *entry, cJSON *installed, bool force)
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

    // Already have this exact content, according to the record?
    //
    // Under `force` this fast path is skipped entirely, and that is the whole
    // difference between the two modes. The record can be wrong in BOTH
    // directions: it can forget files that are present (a reflash wipes it),
    // and it can remember files that are gone (deleted through the web UI, or
    // lost to a corrupt filesystem). The version short-circuit alone only
    // covered the first. Trusting this entry is what made a deleted sound
    // invisible to a sync run specifically to get it back.
    cJSON *cur = cJSON_GetObjectItem(installed, path);
    if (!force && cJSON_IsString(cur) && strcasecmp(cur->valuestring, sha) == 0) {
        ESP_LOGD(TAG, "up to date: %s", path);
        return 0;
    }

    // Ask the disk. The bytes are the fact; the record is only a way to avoid
    // re-reading them.
    //
    // Bounded by construction in the normal case: only files that were *about*
    // to be downloaded get hashed, so the check costs a local read where the
    // alternative was a TLS handshake and a transfer. Under `force` every file
    // is hashed - measured at about five seconds for the full 111-file bank,
    // against seven minutes to re-download it.
    char full[192];
    snprintf(full, sizeof(full), MOUNT "/%s", path);
    char have_hex[65];
    if (file_sha256(full, have_hex) && strcasecmp(have_hex, sha) == 0) {
        if (!cJSON_IsString(cur) || strcasecmp(cur->valuestring, sha) != 0) {
            ESP_LOGI(TAG, "already on flash, adopting: %s", path);
            installed_set(installed, path, sha);
        }
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
// Apply the "assets" section of an already-parsed manifest. Does NOT take
// ownership of `root` - the caller frees it. Split out so the pack path can
// drop the raw JSON buffer before the downloads (and their TLS session) begin.
static esp_err_t sync_root(cJSON *root, int *n_updated, bool force)
{
    if (n_updated) *n_updated = 0;

    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsObject(assets))                           // firmware-only manifest: nothing to do
        return ESP_OK;
    cJSON *base  = cJSON_GetObjectItem(assets, "base_url");
    cJSON *files = cJSON_GetObjectItem(assets, "files");

    // files is optional (a "remove"-only manifest is valid); base_url is only
    // required when there are files to fetch.
    int count = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
    if (count > 0 && !cJSON_IsString(base)) {
        ESP_LOGW(TAG, "assets has files but no base_url");
        return ESP_FAIL;
    }
    if (count > MAX_FILES) {
        ESP_LOGW(TAG, "manifest lists %d assets; capping at %d", count, MAX_FILES);
        count = MAX_FILES;
    }

    cJSON *installed = installed_load();
    int updated = 0, failed = 0, skipped = 0, removed = 0;

    for (int i = 0; i < count; i++) {
        int r = sync_one(base->valuestring, cJSON_GetArrayItem(files, i), installed, force);
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

    ESP_LOGI(TAG, "asset sync: %d updated, %d up-to-date, %d removed, %d failed",
             updated, skipped, removed, failed);
    if (n_updated) *n_updated = updated;
    return failed ? ESP_FAIL : ESP_OK;
}

esp_err_t assets_sync_json(const char *manifest_json, int *n_updated)
{
    if (n_updated) *n_updated = 0;
    cJSON *root = cJSON_Parse(manifest_json);
    if (!root) { ESP_LOGW(TAG, "manifest is not valid JSON"); return ESP_FAIL; }
    esp_err_t r = sync_root(root, n_updated, false);
    cJSON_Delete(root);
    return r;
}

// --- audio packs (a manifest of their own, at their own URL) ----------------
static void pack_state_get(char *url, size_t usz, char *ver, size_t vsz)
{
    if (url && usz) url[0] = '\0';
    if (ver && vsz) ver[0] = '\0';

    cJSON *inst = installed_load();
    cJSON *p = cJSON_GetObjectItem(inst, PACK_KEY);
    if (cJSON_IsObject(p)) {
        cJSON *u = cJSON_GetObjectItem(p, "url");
        cJSON *v = cJSON_GetObjectItem(p, "version");
        if (url && usz && cJSON_IsString(u)) { strncpy(url, u->valuestring, usz - 1); url[usz - 1] = '\0'; }
        if (ver && vsz && cJSON_IsString(v)) { strncpy(ver, v->valuestring, vsz - 1); ver[vsz - 1] = '\0'; }
    }
    cJSON_Delete(inst);
}

// Read-modify-write, deliberately separate from the sync's own save: the file
// list must be persisted per file (so an interrupted sync resumes), whereas the
// version may only be claimed once the whole pack landed. Merging the two would
// mean recording "version 4 applied" partway through applying it.
static void pack_state_set(const char *url, const char *ver)
{
    cJSON *inst = installed_load();
    cJSON_DeleteItemFromObject(inst, PACK_KEY);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "url", url);
    cJSON_AddStringToObject(p, "version", ver);
    cJSON_AddItemToObject(inst, PACK_KEY, p);
    installed_save(inst);
    cJSON_Delete(inst);
}

const char *assets_pack_requires(void) { return s_requires; }

void assets_pack_version(char *out, size_t sz)
{
    pack_state_get(NULL, 0, out, sz);
}

// Resolve an include against its parent's URL and apply it.
//
// Relative paths are resolved against the parent manifest's directory, so a
// pack moves host without editing every include. An absolute URL is honoured as
// given - occasionally useful, and refusing it would be arbitrary since the
// parent document is already trusted to name files.
static esp_err_t sync_include(const char *parent_url, const char *ref, int *n_updated)
{
    if (n_updated) *n_updated = 0;
    if (!ref || !ref[0]) return ESP_OK;
    if (strstr(ref, "..")) { ESP_LOGW(TAG, "include escapes its base: %s", ref); return ESP_FAIL; }

    char sub[256];
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(sub, sizeof(sub), "%s", ref);
    } else {
        const char *slash = strrchr(parent_url, '/');
        int dirlen = slash ? (int)(slash - parent_url) : (int)strlen(parent_url);
        snprintf(sub, sizeof(sub), "%.*s/%s", dirlen, parent_url, ref);
    }

    char *json = malloc(PACK_MAX);
    if (!json) return ESP_FAIL;
    int n = ota_http_get(sub, json, PACK_MAX);
    if (n <= 0) { ESP_LOGW(TAG, "sub-pack fetch failed: %s", sub); free(json); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) { ESP_LOGW(TAG, "sub-pack is not valid JSON: %s", sub); return ESP_FAIL; }

    // Always force inside a sub-pack: the version short-circuit belongs to the
    // top-level document, and a sub-pack carries no version of its own to
    // compare. Its files are checked against flash, which is cheap.
    esp_err_t r = sync_root(root, n_updated, true);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "sub-pack %s: %d file(s) updated", ref, n_updated ? *n_updated : 0);
    return r;
}

esp_err_t assets_sync_pack(const char *url, const char *cur_fw,
                           int *n_updated, bool *deferred, bool force)
{
    if (n_updated) *n_updated = 0;
    if (deferred) *deferred = false;

    // No pack configured: nothing can be held back, so nothing to ask about.
    if (!url || !url[0]) { needhelp_clear(); return ESP_OK; }

    char *json = malloc(PACK_MAX);
    if (!json) return ESP_FAIL;
    int n = ota_http_get(url, json, PACK_MAX);
    if (n <= 0) { ESP_LOGW(TAG, "pack fetch failed: %s", url); free(json); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(json);
    free(json);                                            // parsed: the raw text is dead weight
    json = NULL;
    if (!root) { ESP_LOGW(TAG, "pack is not valid JSON"); return ESP_FAIL; }

    // Firmware floor. DEFERRED, not failed - see assets.h.
    cJSON *req = cJSON_GetObjectItem(root, "requires_fw");
    if (cJSON_IsString(req) && cur_fw && version_cmp(cur_fw, req->valuestring) < 0) {
        ESP_LOGW(TAG, "pack needs firmware %s (running %s) - skipping until we qualify",
                 req->valuestring, cur_fw);
        snprintf(s_requires, sizeof(s_requires), "%s", req->valuestring);
        if (deferred) *deferred = true;
        needhelp_set(HELP_PACK_FW, s_requires);
        cJSON_Delete(root);
        return ESP_OK;
    }
    s_requires[0] = '\0';      // we qualify: nothing is being held back
    needhelp_clear();

    // Version short-circuit. Only meaningful paired with the URL it came from,
    // and only sound when nothing has changed on the device: it says "the
    // manifest is the same one", not "the files still match it". Skipped under
    // `force`, so a person who noticed a missing sound gets a real check.
    char ver[33] = "";
    cJSON *v = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(v)) {
        strncpy(ver, v->valuestring, sizeof(ver) - 1);
        char had_url[129], had_ver[33];
        pack_state_get(had_url, sizeof(had_url), had_ver, sizeof(had_ver));
        if (!force && had_ver[0] && strcmp(had_ver, ver) == 0 && strcmp(had_url, url) == 0) {
            ESP_LOGI(TAG, "pack %s already applied (version %s)", url, ver);
            cJSON_Delete(root);
            return ESP_OK;
        }
    }

    int updated = 0;
    esp_err_t r = sync_root(root, &updated, force);

    // Sub-packs. Three themed occasions take the bank past 270 files, against a
    // 128-entry cap and a 16 KB parse buffer - so split the document rather
    // than grow the parser. Each include is ~54 entries / ~6 KB and is applied
    // on its own, so PEAK MEMORY DOESN'T MOVE however many sets exist.
    //
    // Depth 1 only: an include inside an include is ignored, because a bounded
    // fetch graph is worth more here than the generality. Nothing about audio
    // sets needs a tree.
    cJSON *inc = cJSON_GetObjectItem(root, "include");
    if (cJSON_IsArray(inc)) {
        cJSON *it;
        cJSON_ArrayForEach(it, inc) {
            if (!cJSON_IsString(it)) continue;
            int sub_updated = 0;
            if (sync_include(url, it->valuestring, &sub_updated) != ESP_OK) {
                // One bad sub-pack costs its own set, not the bank. Carry on so
                // the others still land, and fail the whole run so the version
                // isn't recorded and the next check retries.
                ESP_LOGW(TAG, "sub-pack failed: %s", it->valuestring);
                r = ESP_FAIL;
            }
            updated += sub_updated;
        }
    }

    cJSON_Delete(root);
    if (n_updated) *n_updated = updated;

    // Claim the version only on a clean run. A failed file means the next check
    // must try again rather than short-circuit on a pack it didn't fully apply.
    if (r == ESP_OK && ver[0]) pack_state_set(url, ver);
    if (r == ESP_OK) ESP_LOGI(TAG, "pack applied: %s%s%s (%d file(s) downloaded)",
                              url, ver[0] ? " version " : "", ver, updated);
    return r;
}
