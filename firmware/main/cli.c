#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "driver/usb_serial_jtag.h"
#include "appcfg.h"
#include "store.h"
#include "config.h"
#include "audio.h"
#include "ntp.h"
#include "ota.h"
#include "assets.h"
#include "countdown.h"
#include "leds.h"
#include "verscmp.h"
#if WIFI_ENABLE
#include "wifi.h"
#include "webota.h"
#endif

static const char *TAG = "cli";

static int cmd_forget_wifi(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Clearing Wi-Fi credentials (cards kept). Rebooting into setup...\n");
    appcfg_clear_wifi();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("FACTORY RESET: erasing Wi-Fi, enrolled cards, and settings. Rebooting...\n");
    store_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct tm tm;
    if (ntp_localtime(&tm)) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        printf("%s\n", buf);
    } else {
        printf("Time not synced yet.\n");
    }
    return 0;
}

static int cmd_fetch(int argc, char **argv)
{
    const char *url = (argc > 1) ? argv[1] : OTA_TEST_URL;
#if WIFI_ENABLE
    if (!wifi_is_connected()) { printf("Not connected to Wi-Fi.\n"); return 1; }
#endif
    if (!ntp_is_synced())
        printf("(warning: clock not synced - TLS cert check may fail)\n");

    static char buf[1024];
    printf("GET %s ...\n", url);
    int n = ota_http_get(url, buf, sizeof(buf));
    if (n < 0) { printf("Fetch failed (see log).\n"); return 1; }
    printf("--- %d bytes ---\n%s\n--- end ---\n", n, buf);
    return 0;
}

static int cmd_update_check(int argc, char **argv)
{
    device_config_t cfg;
    appcfg_load(&cfg);
    const char *url = (argc > 1) ? argv[1] : cfg.manifest_url;
#if WIFI_ENABLE
    if (!wifi_is_connected()) { printf("Not connected to Wi-Fi.\n"); return 1; }
#endif
    ota_manifest_t m;
    printf("Checking %s ...\n", url);
    if (ota_fetch_manifest(url, &m) != 0) {
        printf("Manifest fetch/parse failed (see log).\n");
        return 1;
    }
    int c = version_cmp(m.version, FW_VERSION);
    printf("running   : %s\n", FW_VERSION);
    printf("available : %s\n", m.version);
    printf("firmware  : %s\n", m.firmware_url);
    printf("update    : %s\n", (c > 0) ? "YES - newer available" : "no (up to date)");
    if (m.manifest_url[0] && strcmp(m.manifest_url, url) != 0)
        printf("relocates : %s (adopted on next update-now)\n", m.manifest_url);
    return 0;
}

static int cmd_ota_url(int argc, char **argv)
{
    if (argc < 2) { printf("usage: ota-url <firmware-url>\n"); return 1; }
#if WIFI_ENABLE
    if (!wifi_is_connected()) { printf("Not connected to Wi-Fi.\n"); return 1; }
#endif
    printf("Downloading + installing %s ...\n", argv[1]);
    audio_play(PROMPT_UPDATE_START);                 // user-initiated -> announce
    if (ota_install_from_url(argv[1]) != ESP_OK) {
        audio_play(PROMPT_UPDATE_FAILED);
        printf("failed (see log).\n");
        return 1;
    }
    printf("Installed - rebooting into new firmware...\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return 0;
}

static int cmd_update_now(int argc, char **argv)
{
    device_config_t cfg;
    appcfg_load(&cfg);
    const char *url = (argc > 1) ? argv[1] : cfg.manifest_url;
#if WIFI_ENABLE
    if (!wifi_is_connected()) { printf("Not connected to Wi-Fi.\n"); return 1; }
#endif
    bool installed = false;
    printf("Checking %s ...\n", url);
    if (ota_update_from_manifest(url, FW_VERSION, &installed) != ESP_OK) {
        printf("update failed (see log).\n");
        return 1;
    }
    if (installed) {
        printf("Updated - rebooting into new firmware...\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        printf("Already up to date (running %s).\n", FW_VERSION);
    }
    return 0;
}

// Sync just the media/web assets to the manifest (no firmware, no reboot). Handy
// for testing asset OTA on its own; new sounds are usable on the next tap.
static int cmd_sync_media(int argc, char **argv)
{
    device_config_t cfg;
    appcfg_load(&cfg);
    const char *url = (argc > 1) ? argv[1] : cfg.manifest_url;
#if WIFI_ENABLE
    if (!wifi_is_connected()) { printf("Not connected to Wi-Fi.\n"); return 1; }
#endif
    char *json = malloc(8192);
    if (!json) { printf("out of memory\n"); return 1; }
    printf("Fetching %s ...\n", url);
    int n = ota_http_get(url, json, 8192);
    if (n <= 0) { printf("manifest fetch failed (see log).\n"); free(json); return 1; }

    int updated = 0;
    esp_err_t r = assets_sync_json(json, &updated);
    free(json);
    printf("media sync: %d file(s) downloaded, %s\n",
           updated, (r == ESP_OK) ? "all verified" : "some failed (see log)");
    return (r == ESP_OK) ? 0 : 1;
}

// Run an animation and then hold its sustain look until the audio finishes, the
// way a real tap does (main.c celebrate_seq). Without the hold, an animation
// ends the moment its choreography does while the phrase plays on - which is
// not what the device actually does. Keeping the CLI honest matters: a test
// path that behaves differently from the real one is how the LED refresh race
// stayed hidden.
static void cli_show(anim_id_t anim)
{
    // Held across the animation AND the sustain that follows it. leds_play()
    // locks itself, but releasing between the two lets the main loop's idle step
    // slip in between sustain frames - the strip then alternates between the
    // sustain look and idle blue, which is a visible flicker that starts exactly
    // when the animation ends. Doesn't arise on a real tap, where the main loop
    // is the one running the show and nothing else is painting.
    leds_acquire();
    leds_play(anim);
    for (int guard = 0; audio_is_playing() && guard < 1500; guard++) {   // ~30 s cap
        leds_sustain_step(anim);
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }
    leds_off();
    leds_release();
}

// `countdown`         -> report days remaining + which tier fires.
// `countdown <days>`  -> force-play that tier's clip + animation (test any tier
//                        now, without waiting months or faking the clock).
static int cmd_countdown(int argc, char **argv)
{
    // `countdown due [uid-hex]` - exercise the real once-per-day-per-band gate
    // (uid defaults to the button pseudo-band). Fires at most once per day per
    // uid; run it twice to see the second call skip.
    if (argc >= 2 && strcmp(argv[1], "due") == 0) {
        uint8_t uid[4] = { 0, 0, 0, 0 };
        uint8_t ulen = 0;
        if (argc >= 3 && sscanf(argv[2], "%2hhx%2hhx%2hhx%2hhx",
                                &uid[0], &uid[1], &uid[2], &uid[3]) == 4) ulen = 4;
        static char paths[CD_MAX_CLIPS][CD_PATH_MAX];
        uint8_t anim = 0;
        int n = countdown_due(uid, ulen, paths, CD_MAX_CLIPS, &anim);
        if (n > 0) {
            printf("due: FIRED -> %d clip(s)\n", n);
            for (int i = 0; i < n; i++) { printf("   %s\n", paths[i]); audio_play(paths[i]); }
            cli_show((anim_id_t)anim);
        } else {
            printf("due: not due (countdown off / no clock / already greeted today)\n");
        }
        return 0;
    }
    if (argc > 1) {
        int days = atoi(argv[1]);
        static char paths[CD_MAX_CLIPS][CD_PATH_MAX];
        uint8_t anim = 0;
        int n = countdown_build(days, paths, CD_MAX_CLIPS, &anim);
        if (n <= 0) {
            printf("%d day(s) -> tier '%s' -> no clips (is /spiffs/cd/ populated?)\n",
                   days, countdown_tier_name(days));
            return 1;
        }
        printf("%d day(s) -> tier '%s' -> %d clip(s):\n", days, countdown_tier_name(days), n);
        for (int i = 0; i < n; i++) printf("   %s\n", paths[i]);
        for (int i = 0; i < n; i++) audio_play(paths[i]);
        cli_show((anim_id_t)anim);
        return 0;
    }
    int d = countdown_days_remaining();
    if (d == INT_MIN) {
        printf("countdown: clock not set yet (needs NTP) - can't compute.\n");
        return 0;
    }
    printf("days until trip : %d\n", d);
    printf("tier            : %s\n", countdown_tier_name(d));
    printf("(use 'countdown <days>' to hear any tier now)\n");
    return 0;
}

#if WIFI_ENABLE
// Rename the device - drives the setup AP SSID ("<name>-<id>-Setup") and
// "<name>.local" - then reboot straight into setup mode. Useful over USB when a
// phone won't surface the captive page: a brand-new SSID also dodges the OS
// caching the old network as "no portal".
static int cmd_set_name(int argc, char **argv)
{
    if (argc < 2) { printf("usage: set-name <name>\n"); return 1; }
    device_config_t cfg;
    appcfg_load(&cfg);
    cfg.device_name[0] = '\0';
    for (int i = 1; i < argc; i++) {                 // join tokens so spaces work
        if (i > 1) strncat(cfg.device_name, " ", sizeof(cfg.device_name) - 1 - strlen(cfg.device_name));
        strncat(cfg.device_name, argv[i], sizeof(cfg.device_name) - 1 - strlen(cfg.device_name));
    }
    appcfg_save(&cfg);
    store_set_flag("force_setup", 1);                // come back up in setup mode
    printf("Name set to '%s'. Rebooting into setup - AP will be '%s-%s-Setup'.\n",
           cfg.device_name, cfg.device_name, wifi_device_id());
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return 0;
}

// Open the firmware-upload endpoints on the *current* network for this session.
// Safe from here because reaching this console already implies physical/serial
// access (which could hard-flash the chip anyway) - so it adds no exposure, and
// it lets a developer push from a desk without the button-at-boot dance.
static int cmd_update_mode(int argc, char **argv)
{
    bool on = !(argc > 1 && strcmp(argv[1], "off") == 0);
    if (on && !wifi_is_connected()) {
        printf("Not on Wi-Fi - join a network first (or use the setup access point).\n");
        return 1;
    }
    webota_set_upload_enabled(on);
    if (on) {
        printf("Firmware upload ENABLED for this session.\n");
        printf("  Web page : http://magicmaker-%s.local/update\n", wifi_device_id());
        printf("  Or POST the .bin to  /ota  on that host.\n");
        printf("  Reboot the device to lock it again.\n");
    } else {
        printf("Firmware upload disabled.\n");
    }
    return 0;
}
#endif

// On-device unit test for the pure version-compare logic.
static int cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct { const char *a, *b; int want; } cases[] = {
        { "1.10.0", "1.9.0",  +1 },   // numeric, not string order
        { "1.9.0",  "1.10.0", -1 },
        { "1.0.0",  "1.0.0",   0 },
        { "1.0.1",  "1.0.0",  +1 },
        { "2.0.0",  "1.9.9",  +1 },
        { "1.0",    "1.0.0",   0 },   // missing parts = 0
        { "v1.2.3", "1.2.3",   0 },   // 'v' prefix ignored
        { "1.0.0",  "1.0.1",  -1 },
    };
    int n = sizeof(cases) / sizeof(cases[0]), pass = 0, total = 0;
    for (int i = 0; i < n; i++, total++) {
        int got = version_cmp(cases[i].a, cases[i].b);
        int norm = (got > 0) - (got < 0);         // clamp to -1/0/+1
        bool ok = (norm == cases[i].want);
        printf("  %-8s vs %-8s -> %+d  %s\n", cases[i].a, cases[i].b, norm,
               ok ? "ok" : "FAIL");
        if (ok) pass++;
    }

    // manifest parse: happy path + garbage rejection
    ota_manifest_t m;
    bool m_ok = (ota_parse_manifest(
                    "{\"version\":\"1.2.3\",\"firmware_url\":\"https://x/fw.bin\"}", &m) == 0)
                && strcmp(m.version, "1.2.3") == 0
                && strcmp(m.firmware_url, "https://x/fw.bin") == 0;
    printf("  manifest parse       -> %s\n", m_ok ? "ok" : "FAIL");
    total++; if (m_ok) pass++;

    bool bad_ok = (ota_parse_manifest("<html>not json</html>", &m) != 0);
    printf("  bad-json rejected    -> %s\n", bad_ok ? "ok" : "FAIL");
    total++; if (bad_ok) pass++;

    printf("selftest: %d/%d passed%s\n", pass, total,
           (pass == total) ? " ✓" : " -- FAILURES");
    return (pass == total) ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("firmware  : %s\n", FW_VERSION);
    const esp_app_desc_t  *d   = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("build     : %s  (%s)\n", d->version, d->date);   // git-describe + date
    printf("partition : %s\n", run ? run->label : "?");       // ota_0 / ota_1
#if WIFI_ENABLE
    device_config_t cfg;
    appcfg_load(&cfg);
    printf("name      : %s\n", cfg.device_name);         // friendly / .local / AP
    printf("device id : %s\n", wifi_device_id());
    printf("network   : %s\n", cfg.wifi_ssid[0] ? cfg.wifi_ssid : "(none configured)");
    printf("wifi      : %s\n", wifi_is_connected() ? "connected" : "not connected");
#endif
    struct tm tm;
    if (ntp_localtime(&tm)) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        printf("time      : %s\n", buf);
    } else {
        printf("time      : not synced\n");
    }
    // Heap: the MP3 decoder and any future clip caching live here, and the
    // low-water mark is the number that actually matters - a comfortable
    // "free now" can still hide a near-miss during a burst.
    printf("heap      : %u free, %u low-water, %u largest block\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return 0;
}

// Play one file through the normal audio path, so a clip can be checked without
// enrolling a band or reaching for the reader. Decoder is chosen by sniffing the
// file, so this exercises exactly what a tap would.
static int cmd_play(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: play <path>   e.g. play /spiffs/chime.wav\n");
        return 1;
    }
    printf("playing %s\n", argv[1]);
    audio_play(argv[1]);
    // Wait it out so the prompt doesn't come back mid-clip and invite a second
    // command while I2S is still busy.
    vTaskDelay(pdMS_TO_TICKS(120));
    while (audio_is_playing()) vTaskDelay(pdMS_TO_TICKS(50));
    printf("done\n");
    return 0;
}

void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "magicmaker>";
    // Long enough for a real URL: `update-now <manifest url>` is comfortably
    // past 64, and silently truncating a URL is a miserable thing to debug.
    rc.max_cmdline_length = 256;
    rc.task_stack_size = 10240;  // commands do TLS + OTA (fetch/update/ota-url);
                                 // TLS is a heavy stack user - default ~4K blows up.

    // Match the REPL to whatever console this build uses (native USB vs UART).
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#endif

    esp_console_register_help_command();

    const esp_console_cmd_t forget = {
        .command = "forget-wifi",
        .help    = "Clear Wi-Fi credentials (keep enrolled cards), reboot to setup",
        .func    = &cmd_forget_wifi,
    };
    const esp_console_cmd_t factory = {
        .command = "factory-reset",
        .help    = "Erase EVERYTHING (Wi-Fi + cards + settings), reboot fresh",
        .func    = &cmd_factory_reset,
    };
    const esp_console_cmd_t time_cmd = {
        .command = "time",
        .help    = "Show the current (NTP-synced) local time",
        .func    = &cmd_time,
    };
    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help    = "Show device id, Wi-Fi state, time, and free heap",
        .func    = &cmd_status,
    };
    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help    = "Play an audio file: play /spiffs/chime.wav",
        .hint    = "<path>",
        .func    = &cmd_play,
    };
    const esp_console_cmd_t fetch_cmd = {
        .command = "fetch",
        .help    = "HTTPS GET a URL (default OTA_TEST_URL) and print it",
        .func    = &cmd_fetch,
    };
    const esp_console_cmd_t update_cmd = {
        .command = "update-check",
        .help    = "Fetch the manifest and report if a newer version exists",
        .func    = &cmd_update_check,
    };
    const esp_console_cmd_t otaurl_cmd = {
        .command = "ota-url",
        .help    = "Download + install firmware from a URL, then reboot",
        .func    = &cmd_ota_url,
    };
    const esp_console_cmd_t updatenow_cmd = {
        .command = "update-now",
        .help    = "Fetch manifest; if newer, download + install + reboot",
        .func    = &cmd_update_now,
    };
    const esp_console_cmd_t syncmedia_cmd = {
        .command = "sync-media",
        .help    = "Sync media/web assets to the manifest (no firmware, no reboot)",
        .func    = &cmd_sync_media,
    };
    const esp_console_cmd_t countdown_cmd = {
        .command = "countdown",
        .help    = "Days to the trip; 'countdown <days>' force-plays that tier",
        .func    = &cmd_countdown,
    };
    const esp_console_cmd_t selftest_cmd = {
        .command = "selftest",
        .help    = "Run on-device unit tests (version_cmp)",
        .func    = &cmd_selftest,
    };
#if WIFI_ENABLE
    const esp_console_cmd_t updatemode_cmd = {
        .command = "update-mode",
        .help    = "Enable firmware web-upload on the current network (reboot to lock)",
        .func    = &cmd_update_mode,
    };
    const esp_console_cmd_t setname_cmd = {
        .command = "set-name",
        .help    = "Rename the device (AP SSID + .local) and reboot into setup mode",
        .func    = &cmd_set_name,
    };
#endif
    ESP_ERROR_CHECK(esp_console_cmd_register(&forget));
    ESP_ERROR_CHECK(esp_console_cmd_register(&factory));
    ESP_ERROR_CHECK(esp_console_cmd_register(&time_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&fetch_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&update_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&otaurl_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&updatenow_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&syncmedia_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&countdown_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&selftest_cmd));
#if WIFI_ENABLE
    ESP_ERROR_CHECK(esp_console_cmd_register(&updatemode_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&setname_cmd));
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI ready - type 'help'");
}

// Wait for a USB host to attach, then start the REPL once. On a charger no host
// ever connects, so the REPL never runs (and never starves the animation loop).
static void cli_wait_task(void *arg)
{
    (void)arg;
    while (!usb_serial_jtag_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "USB host detected - starting serial CLI");
    cli_start();
    vTaskDelete(NULL);
}

void cli_start_when_connected(void)
{
    xTaskCreate(cli_wait_task, "cli_wait", 3072, NULL, 2, NULL);
}
