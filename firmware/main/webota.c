#include "webota.h"
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "ota.h"
#include "wifi.h"
#include "audio.h"
#include "config.h"

static const char *TAG = "webota";

// Firmware upload is gated: off until setup mode (or the console) turns it on.
static bool s_upload_ok = false;

void webota_set_upload_enabled(bool on)
{
    s_upload_ok = on;
    ESP_LOGW(TAG, "firmware upload %s", on ? "ENABLED" : "disabled");
}

#define OTA_CHUNK 4096

// Shown (with 403) when someone reaches /update while upload is locked.
static const char LOCKED_PAGE[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Updates locked</title>"
"<body style='font-family:sans-serif;background:#101018;color:#eee;padding:1.2em'>"
"<h1>&#128274; Firmware updates are locked</h1>"
"<p>To upload custom firmware, put the device in <b>setup mode</b>: hold the "
"button while powering it on until it opens its <b>MagicMaker Setup</b> Wi-Fi, "
"then connect to that network and open this page again.</p>"
"<p>Reboot normally to return to normal mode.</p></body>";

// --- the upload page (raw-bytes POST via fetch; no multipart) ---------------
static const char UPLOAD_PAGE[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Update MagicMaker</title>"
"<body style='font-family:sans-serif;background:#101018;color:#eee;padding:1.2em'>"
"<h1>&#128260; Firmware update</h1>"
"<input type=file id=f accept='.bin'><br><br>"
"<button id=b onclick='up()' style='padding:.7em 1.2em;border:0;border-radius:.4em;"
"background:#ffd54a;color:#101018;font-weight:bold'>Upload &amp; install</button>"
"<pre id=log style='white-space:pre-wrap;margin-top:1em;color:#9cf'></pre>"
"<script>"
"function log(m){document.getElementById('log').textContent=m;}"
"async function up(){"
" var f=document.getElementById('f').files[0];"
" if(!f){log('Pick a .bin file first.');return;}"
" document.getElementById('b').disabled=true;"
" log('Uploading '+f.name+' ('+f.size+' bytes)\\u2026 do not unplug.');"
" try{var r=await fetch('/ota',{method:'POST',body:f});log(await r.text());}"
" catch(e){log('Connection closed \\u2013 if it said installing, it is rebooting into the new firmware.');}"
"}"
"</script></body>";

static esp_err_t update_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    if (!s_upload_ok) {                       // gate: locked outside setup mode
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, LOCKED_PAGE);
    }
    return httpd_resp_sendstr(req, UPLOAD_PAGE);
}

// POST /ota: stream the raw body into the inactive slot. Defensive throughout -
// bounded reads, hard size cap in ota_write, validate before switching boot,
// abort (leaving current firmware intact) on any error.
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!s_upload_ok) {                        // gate: refuse the bytes outright
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "firmware upload is locked - enter setup mode "
                                "(hold the button while powering on)");
        return ESP_OK;
    }
    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        httpd_resp_sendstr(req, "Content-Length required");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "firmware upload: %d bytes", total);

    esp_err_t r = ota_begin((size_t)total);
    if (r != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, (r == ESP_ERR_INVALID_STATE)
                           ? "another update is already in progress"
                           : "image rejected (too large or no OTA slot)");
        audio_play(PROMPT_UPDATE_FAILED);
        return ESP_OK;
    }

    // User-initiated push -> announce it (plays over the multi-second upload).
    audio_play(PROMPT_UPDATE_START);

    char *buf = malloc(OTA_CHUNK);
    if (!buf) { ota_abort_session(); audio_play(PROMPT_UPDATE_FAILED); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    int  remaining = total;
    bool ok = true;
    while (remaining > 0) {
        int want = (remaining < OTA_CHUNK) ? remaining : OTA_CHUNK;
        int rd = httpd_req_recv(req, buf, want);
        if (rd == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (rd <= 0) { ok = false; break; }
        if (ota_write(buf, rd) != ESP_OK) { ok = false; break; }
        remaining -= rd;
    }
    free(buf);

    if (!ok) {
        ota_abort_session();
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "upload interrupted - firmware unchanged");
        audio_play(PROMPT_UPDATE_FAILED);
        return ESP_OK;
    }
    if (ota_finish() != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "invalid firmware image - firmware unchanged");
        audio_play(PROMPT_UPDATE_FAILED);
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OK: installed. Rebooting into the new firmware...");
    ESP_LOGW(TAG, "rebooting into new firmware");
    vTaskDelay(pdMS_TO_TICKS(700));        // let the response flush
    esp_restart();
    return ESP_OK;
}

void webota_register(httpd_handle_t srv)
{
    httpd_uri_t upd = { .uri = "/update", .method = HTTP_GET,  .handler = update_get };
    httpd_uri_t ota = { .uri = "/ota",    .method = HTTP_POST, .handler = ota_post  };
    httpd_register_uri_handler(srv, &upd);
    httpd_register_uri_handler(srv, &ota);
}
