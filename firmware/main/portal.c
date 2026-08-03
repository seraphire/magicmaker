#include "portal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>          // unlink() - drop a partial upload
#include <sys/stat.h>
#include "esp_littlefs.h"    // free-space check before accepting a file
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "cJSON.h"
#include "appcfg.h"
#include "assets.h"
#include "store.h"
#include "bands.h"
#include "sounds.h"
#include "countdown.h"
#include "leds.h"
#include "webota.h"
#include "wifi.h"
#include "config.h"
#include "app.h"

static const char *TAG = "portal";
static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static volatile bool  s_dns_run;

// true = SoftAP setup/recovery (setup fields live); false = normal LAN config.
static bool s_ap_mode;

// The SoftAP's own address; every DNS answer points here.
#define AP_IP_STR "192.168.4.1"

// ===========================================================================
// The config page now lives on the data filesystem at /spiffs/www/index.html
// (LittleFS) so it's editable / OTA-updatable without a firmware rebuild. The
// markup carries {{TOKEN}} placeholders; send_config_page() streams the file
// and splices in the live values below. The AP-vs-LAN security logic stays in
// C (which fragments get injected), never in the served file.
// ===========================================================================
#define PAGE_DIR  "/spiffs/www/"
#define PAGE_PATH PAGE_DIR "index.html"

// Setup-mode firmware controls vs the normal-mode locked note.
// Colours live in the page's CSS variables so these blocks follow the light/dark
// theme; anything hardcoded here would stay dark-mode on a light page.
static const char FW_LIVE[] =
"<p><a href='/update'>Upload a firmware .bin &#8594;</a></p>";
static const char FW_LOCKED[] =
"<p class='note'>Manifest &amp; firmware upload are locked in normal mode. "
"To change them, hold the button while powering the device on until it opens "
"its <b>MagicMaker Setup</b> Wi-Fi. Reboot to return to normal.</p>";

// Forget/Factory only exist in setup mode.
static const char ADMIN_FORMS[] =
"<hr class='sep'>"
"<form method='POST' action='/forget' style='margin-bottom:.6em'>"
"<button type='submit' class='btn-warn'>Forget Wi-Fi</button></form>"
"<form method='POST' action='/factory'>"
"<button type='submit' class='btn-danger'>Factory reset (erase everything)</button></form>"
"<p><small>Forget Wi-Fi keeps your cards &amp; settings. Factory reset erases all of it.</small></p>";

// ===========================================================================
// Tiny form helpers: URL-decode in place and pull one field out of an
// application/x-www-form-urlencoded body.
// ===========================================================================
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = hexval(p[1]), lo = hexval(p[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)(hi * 16 + lo); p += 2; }
            else *o++ = *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// Copy the value of `key` from a urlencoded body into out (URL-decoded).
// Returns true if the key was present.
static bool form_get(const char *body, const char *key, char *out, size_t sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= sz) n = sz - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

// ===========================================================================
// HTTP handlers
// ===========================================================================
// Build + send the setup/config page. In AP mode we serve THIS for every URL
// (the commercial captive-portal pattern - WiFiManager et al.): a phone's
// connectivity probe gets a real page as a 200 instead of a redirect, which is
// what makes the OS "Sign in" window pop on its own. No 302s, no Host games -
// modern browsers/Android fight both.
// Stream `tpl`, replacing each {{TOKEN}} with its value. Unknown tokens are
// emitted verbatim (a stray "{{" in the file is harmless). Chunked so the page
// never needs a big contiguous buffer. Finalizes the response.
static esp_err_t render_template(httpd_req_t *req, const char *tpl,
                                 const char *const keys[], const char *const vals[], int ntok)
{
    const char *p = tpl;
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) { httpd_resp_send_chunk(req, p, HTTPD_RESP_USE_STRLEN); break; }
        if (open > p) httpd_resp_send_chunk(req, p, open - p);

        const char *close = strstr(open, "}}");
        if (!close) { httpd_resp_send_chunk(req, open, HTTPD_RESP_USE_STRLEN); break; }

        size_t klen = (size_t)(close - (open + 2));
        int matched = 0;
        for (int i = 0; i < ntok; i++) {
            if (strlen(keys[i]) == klen && strncmp(open + 2, keys[i], klen) == 0) {
                if (vals[i] && vals[i][0])
                    httpd_resp_send_chunk(req, vals[i], HTTPD_RESP_USE_STRLEN);
                matched = 1;
                break;
            }
        }
        if (!matched) httpd_resp_send_chunk(req, open, close + 2 - open);  // leave as-is
        p = close + 2;
    }
    return httpd_resp_send_chunk(req, NULL, 0);   // finalize the chunked response
}

static esp_err_t send_config_page(httpd_req_t *req)
{
    device_config_t cfg;
    appcfg_load(&cfg);

    char date[16];                                 // native <input type=date> wants YYYY-MM-DD
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             cfg.trip_year, cfg.trip_month, cfg.trip_day);

    // The Wi-Fi / manifest block. In setup mode these are the whole point, so
    // they're live inputs. On the home LAN they're locked anyway, and rendering
    // them as disabled boxes was just noise - an empty password field you can't
    // read or set, and a "show password" toggle with nothing to show. Show the
    // useful part (which network, is it up) as read-only status instead.
    // " (version N)" if a pack has been applied - the answer to "did the audio
    // I sent actually land?", which a URL alone doesn't give.
    char packver[64] = "";
    {
        char v[33];
        assets_pack_version(v, sizeof(v));
        if (v[0]) snprintf(packver, sizeof(packver), " <small>version %s</small>", v);
    }

    char *wifi = malloc(1024);
    if (!wifi) return ESP_FAIL;
    if (s_ap_mode) {
        snprintf(wifi, 1024,
            "<label>Wi-Fi network</label><input name='ssid' value='%s'>"
            "<label>Wi-Fi password</label><input name='pass' type='password' placeholder='(unchanged)'>"
            "<label class='chk'><input type='checkbox' id='showpw'>Show password</label>"
            "<label>Update manifest URL</label><input name='manif' value='%s'>"
            "<label>Audio pack URL</label><input name='assetu' value='%s' placeholder='(none)'>"
            "<small>Optional, and private to this device - where its sounds come from.</small>",
            cfg.wifi_ssid, cfg.manifest_url, cfg.assets_url);
    } else {
        snprintf(wifi, 1024,
            "<div class='status'>Wi-Fi: <b>%s</b> %s<br>"
            "Updates: <b>%s</b><br>"
            "Audio pack: <b>%s</b>%s<br>"
            "<small>To change these, hold the button while powering the device on.</small></div>",
            cfg.wifi_ssid[0] ? cfg.wifi_ssid : "(none)",
            wifi_is_connected() ? "<span class='ok'>&#10003; connected</span>" : "(not connected)",
            cfg.manifest_url[0] ? cfg.manifest_url : "(none)",
            cfg.assets_url[0] ? cfg.assets_url : "(none)",
            packver);
    }

    char idlecol[10];
    snprintf(idlecol, sizeof(idlecol), "#%06lX", (unsigned long)(cfg.idle_color & 0xFFFFFF));
    char host[64], ring[8], face[8];
    snprintf(host, sizeof(host), "%s.local", wifi_hostname());
    snprintf(ring, sizeof(ring), "%d", cfg.ring_leds);
    snprintf(face, sizeof(face), "%d", cfg.mickey_leds);

    const char *keys[] = { "SUFFIX", "NAME", "DATE", "NOTRIP", "TAPER", "WIFI",
                           "FW", "ADMIN", "FWVER", "DEVID",
                           "BOOTAU", "IDLECOL", "HOST", "RING", "FACE", "RINGFIRST" };
    const char *vals[] = {
        s_ap_mode ? "Setup" : "Config",
        cfg.device_name,
        date,
        cfg.countdown_enabled ? "" : "checked",    // "No trip planned" box state
        cfg.countdown_taper   ? "checked" : "",    // taper when far from the trip
        wifi,                                      // live inputs (AP) or status (LAN)
        s_ap_mode ? FW_LIVE : FW_LOCKED,
        s_ap_mode ? ADMIN_FORMS : "",
        FW_VERSION,
        wifi_device_id(),
        cfg.boot_audio_enabled ? "checked" : "",
        idlecol,
        host,
        ring,
        face,
        cfg.ring_first ? "checked" : "",
    };
    const int ntok = sizeof(keys) / sizeof(keys[0]);

    // Slurp the template off the data FS (bounded; the page is ~1.5 KB).
    struct stat st;
    FILE *f = NULL;
    char *tpl = NULL;
    if (stat(PAGE_PATH, &st) == 0 && st.st_size > 0 && st.st_size < 32768 &&
        (f = fopen(PAGE_PATH, "r")) != NULL) {
        tpl = malloc(st.st_size + 1);
        if (tpl) {
            size_t n = fread(tpl, 1, st.st_size, f);
            tpl[n] = '\0';
        }
        fclose(f);
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");

    if (!tpl) {
        // Page file missing (bad data flash?) - still let the user save Wi-Fi so
        // the device is never bricked into an un-configurable state.
        ESP_LOGE(TAG, "cannot read %s; serving minimal fallback", PAGE_PATH);
        free(wifi);
        return httpd_resp_sendstr(req,
            "<!DOCTYPE html><meta charset='utf-8'>"
            "<body style='font-family:sans-serif;background:#101018;color:#eee;padding:2em'>"
            "<h2>MagicMaker Setup</h2><form method='POST' action='/save'>"
            "<p>Wi-Fi network<br><input name='ssid'></p>"
            "<p>Password<br><input name='pass' type='password'></p>"
            "<button>Save</button></form></body>");
    }

    esp_err_t r = render_template(req, tpl, keys, vals, ntok);
    free(tpl);
    free(wifi);
    return r;
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_config_page(req);   // ignore Host; the page is the page
}

// Stream a static file off the data FS with a content type. 404 if missing.
static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_OK; }
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// The real favicon + the corner logo, served from /spiffs/www/. Registered
// explicitly so they win over the AP catch-all.
static esp_err_t favicon_get(httpd_req_t *req) { return serve_file(req, PAGE_DIR "favicon.ico", "image/x-icon"); }

// A request for the logo means a browser is laying the page out, not just
// probing it - see portal_page_seen(). It's the only honest "they found it"
// signal we have in AP mode, where every URL answers with the page.
static volatile bool s_page_seen = false;
bool portal_page_seen(void)       { return s_page_seen; }
void portal_clear_page_seen(void) { s_page_seen = false; }

static esp_err_t logo_get(httpd_req_t *req)
{
    s_page_seen = true;
    return serve_file(req, PAGE_DIR "logo.png", "image/png");
}

// A small task that waits, then reboots - lets the HTTP response flush first.
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "Rebooting to apply new configuration");
    esp_restart();
}

// Setup mode only. The credentials are saved, the reader is about to reboot onto
// the home network, and this page - served from the SoftAP that is seconds from
// vanishing - is orphaned. Rather than "you can close this page", hand the user
// over: say what is happening, show the address to type, and quietly probe for
// the device coming back up on the LAN.
//
// The probe is an <img> load rather than fetch(): an image load is not subject
// to CORS (only reading its pixels is), so this needs nothing on the device side
// beyond the favicon already served. Cache-busted per attempt so a failed lookup
// is not remembered.
//
// The instructions are the feature and the redirect is the bonus, because three
// things here are outside our control: the phone may come back on cellular
// rather than Wi-Fi, Android's mDNS resolution of ".local" is unreliable before
// 12, and iOS's captive-portal webview tends to close itself the moment the
// network changes. In any of those cases the user still knows where to go.
#define HANDOFF_MAX 2048
static void send_handoff_page(httpd_req_t *req, const char *device_name)
{
    // Where the reader will be AFTER the reboot - derived from the name just
    // saved, which is not necessarily the one mDNS is advertising right now.
    char host[32];
    wifi_hostname_for(device_name, host, sizeof(host));

    char *page = malloc(HANDOFF_MAX);
    if (!page) {                       // no room for the nice version; say the essentials
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><meta charset='utf-8'>"
            "<h2>&#9989; Saved</h2><p>The reader is restarting and will join your network.</p>");
        return;
    }

    int len = snprintf(page, HANDOFF_MAX,
        "<!DOCTYPE html><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<body style='font-family:system-ui,-apple-system,sans-serif;background:#101018;"
        "color:#eee;padding:2em;line-height:1.55;max-width:34em;margin:0 auto'>"
        "<h2>&#9989; Saved</h2>"
        "<p>The reader is restarting and joining your Wi-Fi network.</p>"
        "<p id='s' style='color:#ffd54a'><b>Waiting for it to come back&hellip;</b></p>"
        "<p style='color:#aaa;font-size:.92em'>Your phone should rejoin your home Wi-Fi by "
        "itself once this setup network disappears. If it does not, switch back by hand "
        "&mdash; this page keeps looking, and will jump to the reader as soon as it answers.</p>"
        // Both links are written out here rather than filled in by script: a
        // captive-portal webview that blocks JS still gets a working way in.
        "<p style='color:#aaa;font-size:.92em'>You can also just open "
        "<a href='http://%s.local/' style='color:#ffd54a'>http://%s.local/</a> yourself.</p>"
        "<p style='margin-top:1.5em'><a href='http://%s.local/' "
        "style='display:inline-block;background:#ffd54a;color:#101018;padding:.75em 1.3em;"
        "border-radius:.4em;text-decoration:none;font-weight:600'>Go to MagicMaker &rarr;</a></p>"
        "<script>"
        "var h='http://%s.local/',n=0,cur=null;"
        "function tick(){"
        "if(++n===15)document.getElementById('s').innerHTML="
        "'<b>Still waiting.</b> Check that your phone is back on your home Wi-Fi.';"
        "if(cur)cur.onload=cur.onerror=null;"   // a late reply from a stale probe is noise
        "cur=new Image();"
        "cur.onload=function(){location.replace(h);};"
        "cur.src=h+'favicon.ico?'+n;}"
        "tick();setInterval(tick,2000);"
        "</script></body>", host, host, host, host);

    // Truncation would cut the <script> mid-statement and quietly kill the
    // redirect while the page still looked fine. Say so rather than wonder.
    if (len >= HANDOFF_MAX) ESP_LOGE(TAG, "handoff page truncated (%d >= %d)", len, HANDOFF_MAX);

    httpd_resp_sendstr(req, page);
    free(page);
}

static esp_err_t save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) total = 1024;
    char *body = calloc(1, total + 1);
    if (!body) return ESP_FAIL;

    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) { free(body); return ESP_FAIL; }
        off += r;
    }

    device_config_t cfg;
    appcfg_load(&cfg);                 // keep existing values as defaults

    // Always-editable config (day-to-day, allowed in normal mode too).
    char buf[131];
    int old_y = cfg.trip_year, old_m = cfg.trip_month, old_d = cfg.trip_day;
    if (form_get(body, "name", buf, sizeof(buf))) {
        bands_sanitize_name(buf);          // same rules as band names
        strncpy(cfg.device_name, buf, sizeof(cfg.device_name) - 1);
    }
    // Native <input type=date> posts "YYYY-MM-DD". (Older three-box form still
    // parsed as a fallback so a stale cached page keeps working.)
    if (form_get(body, "trip", buf, sizeof(buf)) && buf[0]) {
        int y = 0, m = 0, d = 0;
        if (sscanf(buf, "%d-%d-%d", &y, &m, &d) == 3 && y > 0 && m >= 1 && d >= 1) {
            cfg.trip_year = y; cfg.trip_month = m; cfg.trip_day = d;
        }
    } else {
        if (form_get(body, "ty", buf, sizeof(buf))) cfg.trip_year  = atoi(buf);
        if (form_get(body, "tm", buf, sizeof(buf))) cfg.trip_month = atoi(buf);
        if (form_get(body, "td", buf, sizeof(buf))) cfg.trip_day   = atoi(buf);
    }
    // "No trip planned" checkbox: only posts when ticked, so its presence means
    // no countdown. (Drives the future countdown/milestone logic.)
    cfg.countdown_enabled = !form_get(body, "notrip", buf, sizeof(buf));
    cfg.countdown_taper   = form_get(body, "taper", buf, sizeof(buf));
    cfg.boot_audio_enabled = form_get(body, "bootau", buf, sizeof(buf));
    cfg.ring_first         = form_get(body, "ringfirst", buf, sizeof(buf));

    if (form_get(body, "idlecol", buf, sizeof(buf)) && buf[0]) {
        const char *h = (buf[0] == '#') ? buf + 1 : buf;      // "#RRGGBB" from <input type=color>
        cfg.idle_color = (uint32_t)strtoul(h, NULL, 16) & 0xFFFFFF;
    }
    // LED layout is per-device: every build is wired a little differently and one
    // OTA image serves several units. Clamped to what the buffers are sized for.
    if (form_get(body, "ring", buf, sizeof(buf)) && buf[0]) {
        int v = atoi(buf);
        if (v >= 1 && v <= RING_LED_MAX) cfg.ring_leds = v;
    }
    if (form_get(body, "face", buf, sizeof(buf)) && buf[0]) {
        int v = atoi(buf);
        if (v >= 0 && v <= MICKEY_LED_MAX) cfg.mickey_leds = v;
    }

    // If the trip date moved, re-arm every band. Otherwise the once-a-day gate
    // hides the new count until tomorrow, which reads exactly like the date
    // failing to save.
    if (cfg.trip_year != old_y || cfg.trip_month != old_m || cfg.trip_day != old_d)
        countdown_reset_all();

    // Setup-only fields: honored ONLY in AP setup mode. Even if a crafted POST
    // smuggles them in over the LAN, they're ignored here (defense past the
    // disabled inputs). Empty values are skipped so a blank field never wipes.
    if (s_ap_mode) {
        if (form_get(body, "ssid", buf, sizeof(buf)) && buf[0])
            strncpy(cfg.wifi_ssid, buf, sizeof(cfg.wifi_ssid) - 1);
        if (form_get(body, "pass", buf, sizeof(buf)) && buf[0])
            strncpy(cfg.wifi_password, buf, sizeof(cfg.wifi_password) - 1);
        if (form_get(body, "manif", buf, sizeof(buf)) && buf[0])
            strncpy(cfg.manifest_url, buf, sizeof(cfg.manifest_url) - 1);
        // Setup-mode only, and deliberately NOT relaxed to the LAN the way
        // /api/sound was. That endpoint can add a file beside the reward
        // sounds; this one is a redirect - point it elsewhere and the next
        // sync rewrites Program/ and cd/ wholesale, in the device's own voice.
        // Contained blast radius is what earned upload its LAN access, and a
        // pack URL has none.
        //
        // It is the one field here that must be clearable, though: "stop
        // syncing audio from anywhere" has to be expressible, and an empty box
        // is how someone would say it.
        if (form_get(body, "assetu", buf, sizeof(buf))) {
            strncpy(cfg.assets_url, buf, sizeof(cfg.assets_url) - 1);
            cfg.assets_url[sizeof(cfg.assets_url) - 1] = '\0';
        }
    }
    free(body);

    appcfg_save(&cfg);

    // Apply the look immediately - the buffers are sized to the *_MAX ceilings,
    // so changing counts or colour needs no reboot. Makes dialling in the LED
    // layout for a new build a live edit rather than a flash-and-see cycle.
    leds_set_idle_color(cfg.idle_color);
    leds_set_layout(cfg.ring_leds, cfg.mickey_leds, cfg.ring_first);

    httpd_resp_set_type(req, "text/html");
    if (s_ap_mode) {
        send_handoff_page(req, cfg.device_name);
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);   // apply Wi-Fi change
    } else {
        // Normal-mode config change (name / trip date) - no reboot needed.
        // Re-point mDNS so "<name>.local" follows the new name immediately.
        wifi_start_mdns(cfg.device_name);
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><meta charset='utf-8'>"
            "<body style='font-family:sans-serif;background:#101018;color:#eee;padding:2em'>"
            "<h2>&#9989; Saved</h2><p>Settings updated. "
            "<a href='/' style='color:#ffd54a'>Back</a></p>"
            "<p style='color:#999;font-size:.9em'>If you renamed the device, it may take a "
            "moment for the new <b>.local</b> name to resolve (your computer caches the old one).</p></body>");
    }
    return ESP_OK;
}

// Setup-only: refuse destructive actions over the normal-mode LAN server.
static bool setup_only_or_403(httpd_req_t *req)
{
    if (s_ap_mode) return true;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "not allowed in normal mode - enter setup mode "
                            "(hold the button while powering on)");
    return false;
}

// "Forget Wi-Fi": clear just the credentials, keep enrolled cards + settings.
static esp_err_t forget_post(httpd_req_t *req)
{
    if (!setup_only_or_403(req)) return ESP_OK;
    appcfg_clear_wifi();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><meta charset='utf-8'>"
        "<body style='font-family:sans-serif;background:#101018;color:#eee;padding:2em'>"
        "<h2>Wi-Fi forgotten</h2><p>Restarting into setup - reconnect to the "
        "MagicMaker setup network.</p></body>");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// "Factory reset": wipe everything (Wi-Fi, enrolled cards, flags) back to box.
static esp_err_t factory_post(httpd_req_t *req)
{
    if (!setup_only_or_403(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><meta charset='utf-8'>"
        "<body style='font-family:sans-serif;background:#101018;color:#eee;padding:2em'>"
        "<h2>Factory reset</h2><p>Everything erased. Restarting fresh.</p></body>");
    store_factory_reset();
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Captive-portal catch-all: in AP mode EVERY unmatched URL - including the OS
// connectivity probes (/generate_204, /hotspot-detect.html, /ncsi.txt, ...) -
// gets the setup page as a 200. A non-204 / non-"Success" response is exactly
// what makes Android/iOS/Windows decide "captive portal" and pop their own
// sign-in window; serving the page in place (no redirect, no IP in the bar)
// avoids the HTTPS/redirect fights modern clients put up.
static esp_err_t captive_serve(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return send_config_page(req);
}

// ===========================================================================
// Band manager JSON API. Day-to-day config, so it works in normal (LAN) mode
// too. A band's "action" is a sound id from sounds.c, incl. "random".
// ===========================================================================
static int read_body(httpd_req_t *req, char *buf, size_t sz)
{
    int total = req->content_len;
    if (total <= 0) { buf[0] = '\0'; return 0; }
    if (total > (int)sz - 1) total = sz - 1;
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) return -1;
        off += r;
    }
    buf[off] = '\0';
    return off;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return r;
}

static void uid_to_hex(const uint8_t u[4], char out[9])
{
    static const char *hx = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) { out[i*2] = hx[u[i] >> 4]; out[i*2+1] = hx[u[i] & 0xF]; }
    out[8] = '\0';
}

static bool hex_to_uid(const char *s, uint8_t u[4])
{
    if (!s || strlen(s) < 8) return false;
    for (int i = 0; i < 4; i++) {
        int hi = hexval(s[i*2]), lo = hexval(s[i*2 + 1]);
        if (hi < 0 || lo < 0) return false;
        u[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

// store_list callback -> append one band object to the JSON array.
static void band_to_json(const char *uid_hex, const char *sound, uint8_t anim, void *ctx)
{
    cJSON *arr = (cJSON *)ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "uid", uid_hex);
    char nm[BAND_NAME_MAX + 1]; bands_get_name(uid_hex, nm, sizeof(nm));
    cJSON_AddStringToObject(o, "name", nm);
    char sid[24]; sound_id_for_path(sound, sid, sizeof(sid));   // "" -> "random"
    cJSON_AddStringToObject(o, "sound", sid);
    cJSON_AddNumberToObject(o, "anim", anim);
    cJSON_AddNumberToObject(o, "cdmode", countdown_get_mode(uid_hex));
    cJSON_AddItemToArray(arr, o);
}

// GET /api/bands -> { bands:[...], sounds:[...], last:{...} }
static esp_err_t api_bands_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *bands = cJSON_AddArrayToObject(root, "bands");
    store_list(band_to_json, bands);

    cJSON *sounds = cJSON_AddArrayToObject(root, "sounds");
    for (int i = 0; i < sound_count(); i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "id", sound_id(i));
        cJSON_AddStringToObject(s, "label", sound_label(i));
        cJSON_AddItemToArray(sounds, s);
    }
    cJSON *rnd = cJSON_CreateObject();
    cJSON_AddStringToObject(rnd, "id", SOUND_RANDOM_ID);
    cJSON_AddStringToObject(rnd, "label", "\xF0\x9F\x8E\xB2 Random");   // 🎲 Random
    cJSON_AddItemToArray(sounds, rnd);

    cJSON *last = cJSON_AddObjectToObject(root, "last");
    char lh[9];
    if (bands_last_scan(lh, sizeof(lh))) {
        cJSON_AddStringToObject(last, "uid", lh);
        char nm[BAND_NAME_MAX + 1]; bands_get_name(lh, nm, sizeof(nm));
        cJSON_AddStringToObject(last, "name", nm);
        uint8_t u[4]; char buf[48]; uint8_t a;
        bool registered = hex_to_uid(lh, u) && store_lookup(u, buf, sizeof(buf), &a);
        char sid[24] = "";
        if (registered) sound_id_for_path(buf, sid, sizeof(sid));
        cJSON_AddStringToObject(last, "sound", sid);
        cJSON_AddBoolToObject(last, "registered", registered);
    }
    return send_json(req, root);
}

// POST /api/band  (uid, name, sound) -> create/update a band.
static esp_err_t api_band_post(httpd_req_t *req)
{
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    char uidhex[24], name[80], sid[24];
    if (!form_get(body, "uid", uidhex, sizeof(uidhex))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no uid"); return ESP_OK; }
    if (!form_get(body, "name", name, sizeof(name)))  name[0] = '\0';
    if (!form_get(body, "sound", sid, sizeof(sid)))   strcpy(sid, SOUND_RANDOM_ID);
    bands_sanitize_name(name);            // strip control/HTML chars, cap length

    uint8_t u[4];
    char path[64];
    if (!hex_to_uid(uidhex, u))                       { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uid"); return ESP_OK; }
    if (!sound_path_for_id(sid, path, sizeof(path)))  { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad sound"); return ESP_OK; }

    char canon[9]; uid_to_hex(u, canon);
    store_save(u, path, (uint8_t)sound_anim(path));   // path "" (random) -> anim unused
    bands_set_name(canon, name);
    char modebuf[8];
    if (form_get(body, "cdmode", modebuf, sizeof(modebuf)))   // only when the form sends it
        countdown_set_mode(canon, atoi(modebuf));
    ESP_LOGI(TAG, "band saved: %s '%s' -> %s", canon, name, sid);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/sound?name=<file>  -> store an uploaded reward sound.
//
// Raw octet-stream, deliberately not multipart, so there is no boundary parsing
// of untrusted input - the same choice webota.c made for firmware.
//
// Allowed on the LAN rather than gated to setup mode, because the blast radius
// is contained instead: a name is a bare filename with no path, so writes can
// only ever land beside the reward sounds. Program/, cd/ and www/ are
// unreachable from here, meaning the worst a stranger on the Wi-Fi can do is add
// a silly noise - not overwrite a voice prompt, the countdown bank, or the page
// itself. Firmware upload and factory reset stay locked; they are a different
// risk class.
#define SND_MAX_BYTES  (1024 * 1024)   // a reward clip is a few seconds; the
                                       // largest in the bank is ~25 KB
#define SND_CHUNK      2048
#define SND_FREE_SLACK (256 * 1024)    // never fill the last of the partition -
                                       // LittleFS needs room to compact

// A bare filename: letters, digits, dash, underscore, then .mp3 or .wav. No
// dots elsewhere, so ".." cannot form; no separators, so no directory can be
// named. Rejecting is much cheaper than sanitising something into safety.
static bool sound_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len < 5 || len > 28) return false;

    const char *dot = strrchr(n, '.');
    if (!dot || dot == n) return false;
    if (strcasecmp(dot, ".mp3") != 0 && strcasecmp(dot, ".wav") != 0) return false;

    for (const char *p = n; p < dot; p++) {
        if (*p >= 'a' && *p <= 'z') continue;
        if (*p >= 'A' && *p <= 'Z') continue;
        if (*p >= '0' && *p <= '9') continue;
        if (*p == '-' || *p == '_')  continue;
        return false;                       // covers '.', '/', '\\' and the rest
    }
    return true;
}

// Enough of a check to reject something that is plainly not audio. A full decode
// at the door would cost seconds; this catches the common mistakes - a text
// file, an image, a truncated download - and the decoder still refuses the rest.
static bool looks_like_audio(const uint8_t *b, int n, const char *name)
{
    if (n < 4) return false;
    const char *dot = strrchr(name, '.');
    if (dot && strcasecmp(dot, ".wav") == 0)
        return memcmp(b, "RIFF", 4) == 0;
    if (memcmp(b, "ID3", 3) == 0) return true;              // tagged MP3
    return b[0] == 0xFF && (b[1] & 0xE0) == 0xE0;           // bare MPEG sync
}

static esp_err_t api_sound_post(httpd_req_t *req)
{
    char query[96] = { 0 }, name[32] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name"); return ESP_OK;
    }
    url_decode(name);
    if (!sound_name_ok(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "name must be letters/digits/-/_ ending .mp3 or .wav, no folders");
        return ESP_OK;
    }

    int total = req->content_len;
    if (total <= 0)             { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_OK; }
    if (total > SND_MAX_BYTES)  { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big (1 MB max)"); return ESP_OK; }

    size_t fs_total = 0, fs_used = 0;
    if (esp_littlefs_info("storage", &fs_total, &fs_used) == ESP_OK &&
        (fs_used + (size_t)total + SND_FREE_SLACK) > fs_total) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not enough free space");
        return ESP_OK;
    }

    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open"); return ESP_OK; }

    char *buf = malloc(SND_CHUNK);
    if (!buf) { fclose(f); unlink(path); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }

    int  remaining = total;
    bool ok = true, checked = false;
    while (remaining > 0) {
        int want = (remaining < SND_CHUNK) ? remaining : SND_CHUNK;
        int rd = httpd_req_recv(req, buf, want);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (rd <= 0) { ok = false; break; }
        if (!checked) {                       // judge it on the first bytes, so a
            checked = true;                   // bad file costs one chunk, not 1 MB
            if (!looks_like_audio((const uint8_t *)buf, rd, name)) { ok = false; break; }
        }
        if (fwrite(buf, 1, (size_t)rd, f) != (size_t)rd) { ok = false; break; }
        remaining -= rd;
    }
    free(buf);
    fclose(f);

    if (!ok) {
        unlink(path);                         // never leave a half file behind:
                                              // a truncated clip would be found
                                              // by discovery and played
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload failed or not audio");
        return ESP_OK;
    }

    sounds_init();                            // rescan so it appears in the list
                                              // straight away, no reboot
    ESP_LOGI(TAG, "sound uploaded: %s (%d bytes)", name, total);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "bytes", total);
    cJSON_AddNumberToObject(root, "sounds", sound_count());
    return send_json(req, root);
}

// POST /api/sound/delete?name=<file>  -> remove a reward sound.
//
// The other half of upload, and it has to exist: a device that can be given a
// file it cannot be rid of is a device someone needs a USB cable to rescue.
//
// Same containment as upload - a bare filename, so only the reward-sound root is
// reachable and the prompts, the countdown bank and the page cannot be touched.
// A built-in can be deleted, which is deliberate: "I don't want that one" is a
// fair thing to want, and a reflash restores it. A band still pointing at a
// deleted sound simply plays nothing, which is the same outcome as a bank that
// never had the clip.
static esp_err_t api_sound_delete(httpd_req_t *req)
{
    char query[96] = { 0 }, name[32] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name"); return ESP_OK;
    }
    url_decode(name);
    if (!sound_name_ok(name)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_OK; }

    // The stored name is logical: a clip uploaded as .wav may sit on flash as
    // the .mp3 the build produced, so try both rather than report a false miss.
    char path[64];
    bool gone = false;
    const char *exts[] = { "", ".mp3", ".wav" };
    char stem[32];
    snprintf(stem, sizeof(stem), "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = '\0';
    for (int i = 0; i < 3 && !gone; i++) {
        if (i == 0) snprintf(path, sizeof(path), "/spiffs/%s", name);
        else        snprintf(path, sizeof(path), "/spiffs/%s%s", stem, exts[i]);
        if (unlink(path) == 0) gone = true;
    }

    sounds_init();                            // rescan so the list follows at once
    ESP_LOGI(TAG, "sound delete %s: %s", name, gone ? "removed" : "not found");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", gone);
    cJSON_AddNumberToObject(root, "sounds", sound_count());
    return send_json(req, root);
}

// POST /api/band/play  (uid) -> behave as though that band had just been tapped.
//
// Not a "play this file" endpoint on purpose. It queues the UID for the main
// loop, so the moment runs where every moment runs and takes the whole path:
// the scan counter moves, the countdown gates the same way, a third press in a
// row still earns a cheeky line, and variants rotate. Playing audio straight
// from this handler would have tested none of that, and would have started a
// clip from the HTTP task while the LED loop was mid-frame.
static esp_err_t api_band_play(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    char uidhex[24];
    uint8_t u[4];
    if (!form_get(body, "uid", uidhex, sizeof(uidhex))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no uid");  return ESP_OK; }
    if (!hex_to_uid(uidhex, u))                         { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uid"); return ESP_OK; }

    bool queued = app_play_band(u);
    ESP_LOGI(TAG, "web play %s: %s", uidhex, queued ? "queued" : "busy");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", queued);
    return send_json(req, root);
}

// POST /api/band/delete  (uid) -> remove a band (enrollment + name).
static esp_err_t api_band_delete(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    char uidhex[24];
    uint8_t u[4];
    if (!form_get(body, "uid", uidhex, sizeof(uidhex)) || !hex_to_uid(uidhex, u)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uid");
        return ESP_OK;
    }
    char canon[9]; uid_to_hex(u, canon);
    store_erase(u);
    bands_clear_name(canon);
    ESP_LOGI(TAG, "band deleted: %s", canon);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// POST /api/reboot -> restart the device (saves a trip to the plug).
static esp_err_t api_reboot(httpd_req_t *req)
{
    ESP_LOGW(TAG, "reboot requested from the web page");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    esp_err_t r = send_json(req, root);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);   // after the reply flushes
    return r;
}

// POST /api/countdown/replay (uid) -> let this band play the countdown again today.
// Change-detection poll. Deliberately tiny: this is the one endpoint that runs
// every couple of seconds, so it answers "has anything happened?" in ~40 bytes
// and the page only re-fetches the 500-odd byte band list when the sequence
// moves. Hand-built rather than cJSON - allocating an object per poll to emit
// three fields is more work than the answer.
//
// `busy` lets the page stay out of the way while a moment is playing. The HTTP
// task runs at priority 5 against the animation loop's 1, so a poll preempts a
// show; a few hundred bytes won't be visible, but there's no reason to find out
// during the one second someone is watching the lights.
static esp_err_t api_scan(httpd_req_t *req)
{
    char last[9];
    if (!bands_last_scan(last, sizeof(last))) last[0] = '\0';

    char body[96];
    int n = snprintf(body, sizeof(body), "{\"n\":%u,\"uid\":\"%s\",\"busy\":%s}",
                     (unsigned)bands_scan_seq(), last,
                     app_moment_busy() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

static esp_err_t api_cd_replay(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;
    char uidhex[24];
    uint8_t u[4];
    if (!form_get(body, "uid", uidhex, sizeof(uidhex)) || !hex_to_uid(uidhex, u)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uid");
        return ESP_OK;
    }
    char canon[9]; uid_to_hex(u, canon);
    countdown_reset_today(canon);
    ESP_LOGI(TAG, "countdown reset-today: %s", canon);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

// ===========================================================================
// DNS catch-all: answer every A query with the SoftAP address.
// ===========================================================================
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS catch-all up (all names -> " AP_IP_STR ")");

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < (int)sizeof(uint16_t) * 6) continue;   // too small to be a query

        // Turn the query into an answer: set QR + fixed one-answer counts.
        buf[2] |= 0x80;          // QR = response
        buf[3] |= 0x80;          // RA
        buf[6] = 0; buf[7] = 1;  // ANCOUNT = 1
        buf[8] = 0; buf[9] = 0;  // NSCOUNT = 0
        buf[10] = 0; buf[11] = 0;// ARCOUNT = 0

        int len = n;             // append the answer after the original question
        uint8_t ans[] = {
            0xC0, 0x0C,          // name -> pointer to the question
            0x00, 0x01,          // type A
            0x00, 0x01,          // class IN
            0x00, 0x00, 0x00, 0x05, // TTL 5s (short: don't let clients cache us
                                    // as the resolver after they leave the AP)
            0x00, 0x04,          // RDLENGTH 4
            192, 168, 4, 1,      // RDATA = 192.168.4.1
        };
        if (len + (int)sizeof(ans) <= (int)sizeof(buf)) {
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
            sendto(sock, buf, len, 0, (struct sockaddr *)&from, flen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

// ===========================================================================
void portal_start(bool ap_mode)
{
    s_ap_mode = ap_mode;

    // DNS catch-all + captive redirect are an AP concern only. On the home LAN
    // we're a plain config server that must not hijack other hostnames.
    if (ap_mode) {
        s_dns_run = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 5, &s_dns_task);
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.max_uri_handlers = 16;          // page/assets/config/api + webota, w/ headroom
    if (ap_mode) hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }

    httpd_uri_t root    = { .uri = "/",            .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = favicon_get };
    httpd_uri_t logo    = { .uri = "/logo.png",    .method = HTTP_GET,  .handler = logo_get };
    httpd_uri_t save    = { .uri = "/save",        .method = HTTP_POST, .handler = save_post };
    httpd_uri_t forget  = { .uri = "/forget",      .method = HTTP_POST, .handler = forget_post };
    httpd_uri_t factory = { .uri = "/factory",     .method = HTTP_POST, .handler = factory_post };
    httpd_uri_t api_bands = { .uri = "/api/bands",        .method = HTTP_GET,  .handler = api_bands_get };
    httpd_uri_t api_save  = { .uri = "/api/band",         .method = HTTP_POST, .handler = api_band_post };
    httpd_uri_t api_del   = { .uri = "/api/band/delete",  .method = HTTP_POST, .handler = api_band_delete };
    httpd_uri_t api_play  = { .uri = "/api/band/play",    .method = HTTP_POST, .handler = api_band_play };
    httpd_uri_t api_snd   = { .uri = "/api/sound",        .method = HTTP_POST, .handler = api_sound_post };
    httpd_uri_t api_snddel= { .uri = "/api/sound/delete", .method = HTTP_POST, .handler = api_sound_delete };
    httpd_uri_t api_rep   = { .uri = "/api/countdown/replay", .method = HTTP_POST, .handler = api_cd_replay };
    httpd_uri_t api_scan_ = { .uri = "/api/scan",             .method = HTTP_GET,  .handler = api_scan };
    httpd_uri_t api_rbt   = { .uri = "/api/reboot",           .method = HTTP_POST, .handler = api_reboot };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &favicon);
    httpd_register_uri_handler(s_httpd, &logo);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &forget);
    httpd_register_uri_handler(s_httpd, &factory);
    httpd_register_uri_handler(s_httpd, &api_bands);
    httpd_register_uri_handler(s_httpd, &api_save);
    httpd_register_uri_handler(s_httpd, &api_del);
    httpd_register_uri_handler(s_httpd, &api_play);
    httpd_register_uri_handler(s_httpd, &api_snd);
    httpd_register_uri_handler(s_httpd, &api_snddel);
    httpd_register_uri_handler(s_httpd, &api_rep);
    httpd_register_uri_handler(s_httpd, &api_scan_);
    httpd_register_uri_handler(s_httpd, &api_rbt);

    // Upload routes always exist; the gate decides if they do anything. In setup
    // mode we enable them; in normal mode they stay locked (403) until the
    // serial console explicitly opens them (CLI 'update-mode').
    webota_register(s_httpd);
    webota_set_upload_enabled(ap_mode);

    if (ap_mode) {
        // Any other path (OS probes, typos) -> just serve the setup page (200).
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_serve);
        ESP_LOGI(TAG, "Setup portal ready at http://" AP_IP_STR "/");
    } else {
        ESP_LOGI(TAG, "Config server ready at http://%s.local/  (or the LAN IP)", wifi_hostname());
    }
}

void portal_stop(void)
{
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}
