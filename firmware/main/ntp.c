#include "ntp.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "ntp";
static bool s_synced = false;

// --- HTTP Date fallback -----------------------------------------------------
// Some networks block outbound NTP (UDP 123) but allow HTTPS. Every HTTP
// response carries a "Date:" header (GMT); we can set the clock from it to
// ~1 s accuracy - plenty for a day countdown - over the port 443 that already
// works. Used only when SNTP times out.
static char s_date_hdr[48];

static esp_err_t date_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER &&
        e->header_key && strcasecmp(e->header_key, "Date") == 0) {
        strncpy(s_date_hdr, e->header_value, sizeof(s_date_hdr) - 1);
        s_date_hdr[sizeof(s_date_hdr) - 1] = '\0';
    }
    return ESP_OK;
}

// Convert a UTC struct tm to time_t (newlib has no timegm; mktime assumes
// local time). Proleptic Gregorian, seconds since 1970-01-01 UTC.
static time_t tm_to_utc(const struct tm *tm)
{
    static const int mdays[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
    long y    = tm->tm_year + 1900;
    long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
    days += mdays[tm->tm_mon];
    if (tm->tm_mon > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        days += 1;                          // leap day, this year, past February
    days += tm->tm_mday - 1;
    return ((time_t)days * 24 + tm->tm_hour) * 3600L + tm->tm_min * 60 + tm->tm_sec;
}

static bool http_date_sync(const char *url)
{
    s_date_hdr[0] = '\0';
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_HEAD,
        .event_handler     = date_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 8000,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return false;
    esp_err_t r = esp_http_client_perform(cl);
    esp_http_client_cleanup(cl);
    if (r != ESP_OK || !s_date_hdr[0]) {
        ESP_LOGW(TAG, "HTTP date fallback failed (%s)", esp_err_to_name(r));
        return false;
    }

    // RFC 1123: "Wed, 24 Jul 2026 21:00:07 GMT"
    struct tm tm = { 0 };
    if (!strptime(s_date_hdr, "%a, %d %b %Y %H:%M:%S", &tm)) {
        ESP_LOGW(TAG, "Couldn't parse Date '%s'", s_date_hdr);
        return false;
    }
    time_t t = tm_to_utc(&tm);              // header is GMT/UTC
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return true;
}

bool ntp_sync(int timeout_ms)
{
    // Timezone first, so localtime() is right the moment we get UTC.
    setenv("TZ", NTP_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST(NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3));
    esp_netif_sntp_init(&cfg);

    ESP_LOGI(TAG, "Syncing time (timeout %d ms)...", timeout_ms);
    esp_err_t r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    esp_netif_sntp_deinit();

    if (r != ESP_OK) {
        // NTP blocked/unreachable - fall back to the Date header over HTTPS.
        ESP_LOGW(TAG, "NTP timed out; trying HTTPS Date fallback...");
        if (!http_date_sync(NTP_HTTP_FALLBACK_URL)) {
            ESP_LOGW(TAG, "No time source - running without a clock");
            return false;
        }
        ESP_LOGI(TAG, "Time set from HTTPS Date header");
    }

    s_synced = true;
    struct tm now_tm;
    if (ntp_localtime(&now_tm)) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &now_tm);
        ESP_LOGI(TAG, "Time synced: %s", buf);
    }
    return true;
}

bool ntp_is_synced(void)
{
    return s_synced;
}

bool ntp_localtime(struct tm *out)
{
    if (!s_synced) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}
