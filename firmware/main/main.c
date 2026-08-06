// ---------------------------------------------------------------------------
// Walt Disney World inspired MagicBand Reader - ESP32-S3
//
// Normal mode:  tap a card/'band -> play its sound + animation (or a random
//               one if the card isn't enrolled).
// Program mode: long-press the button to enter; short-press to audition the
//               next sound; tap a card to assign the current sound to it.
//               Assignments persist in NVS (survive power-off).
// ---------------------------------------------------------------------------
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"

#include "config.h"
#include "app.h"
#include "audio.h"
#include "leds.h"
#include "trigger.h"
#include "tags.h"
#include "store.h"
#include "sounds.h"
#include "bands.h"
#include "ota.h"
#include "assets.h"      // pack sync runs even when firmware updates are dormant
#include "needhelp.h"
#include "countdown.h"
#if WIFI_ENABLE
#include "appcfg.h"
#include "wifi.h"
#include "portal.h"
#include "webota.h"
#if NTP_ENABLE
#include "ntp.h"
#endif
#endif
#if CLI_ENABLE
#include "cli.h"
#endif

static const char *TAG = "magicband";

// Cross-task boot state. Networking now runs on its own task so the device is
// interactive immediately, so the main loop reads these to react.
static volatile bool s_setup_mode    = false;  // came up in SoftAP setup mode -> LED cue
static volatile bool s_connecting    = false;  // joining home Wi-Fi -> keep the sparkle going
static volatile bool s_online_pending = false; // network task -> loop: play the "online" moment
static bool          s_quiet_boot    = false;  // this boot is a silent (manifest-OTA) reboot
static bool          s_boot_audio    = true;   // owner's "wake up audibly?" preference

// Repeating the setup address to a phone that has joined but not found the page.
static int     s_visit_said   = 0;   // times said since this phone joined
static int64_t s_visit_due_us = 0;   // earliest time to say it again

// The assignable sound pool + per-sound animation now live in sounds.c (shared
// with the web band-manager so the two never drift). These shims keep the rest
// of this file reading exactly as before.
#define NUM_SOUNDS (sound_count())
static inline anim_id_t anim_for_sound(const char *sound) { return sound_anim(sound); }

// --- interrupting a moment --------------------------------------------------
// A moment (countdown clip and/or a band's sound + animation) used to run to
// completion with the main loop blocked, so a tap during a 7 s clip did nothing
// until it ended. Now the animations and the audio streamer both poll for a new
// tap and bail out.
//
// Guard: NFC_RETRIGGER_MS is a plain 1.5 s cooldown, not "the card left the
// field", so a band resting on the reader re-reads every 1.5 s. We therefore
// ignore the band whose moment is currently playing - only a *different* band
// interrupts. Otherwise a card left sitting there would cut off its own show.
static uint8_t s_current_uid[4];            // band whose moment is playing
static uint8_t s_current_len = 0;
// A tap waiting to be handled: either one that arrived mid-moment, or one the
// web page injected. Volatile because the HTTP task now writes here too - it
// publishes the UID first and the flag last, and the main loop reads the flag
// first, so a half-written UID is never acted on.
static volatile uint8_t s_pending_uid[4];
static volatile uint8_t s_pending_len = 0;
static volatile bool    s_pending     = false;

// Only the main loop owns the reader. leds_play() is also called from the CLI
// (the `countdown` tester), and polling the RC522 from that task would drive SPI
// concurrently with the scan loop - which wedges the bus. So the abort check is
// a no-op anywhere but the main task; CLI-triggered animations simply run to
// completion.
static TaskHandle_t s_main_task = NULL;

static bool moment_interrupted(void)
{
    if (s_main_task && xTaskGetCurrentTaskHandle() != s_main_task) return false;
    if (s_pending) return true;             // already latched one this moment

    uint8_t uid[4], len = 0;
    if (!trigger_poll(uid, &len)) return false;
    if (len >= 4 && s_current_len >= 4 && memcmp(uid, s_current_uid, 4) == 0)
        return false;                       // same band still on the reader

    for (int i = 0; i < (len < 4 ? len : 4); i++) s_pending_uid[i] = uid[i];
    s_pending_len = len;
    s_pending     = true;
    audio_stop();                           // cut the clip; animation returns next frame
    ESP_LOGI(TAG, "Moment interrupted by a new tap");
    return true;
}

// True while a moment (sound + show) is on screen. Heavy work elsewhere waits on
// it: a TLS handshake at priority 5 landing on this loop's priority 1 stutters
// the animation, and the web page's poll backs off while it's set. See app.h.
static volatile bool s_moment_busy = false;

bool app_moment_busy(void) { return s_moment_busy; }

// Queue a tap from the web page. Deliberately the same slot a real mid-moment
// tap uses, so the show runs on the main loop and takes the identical path -
// including being interrupted, or interrupting, exactly as a card would.
bool app_play_band(const uint8_t uid[4])
{
    if (s_pending) return false;              // one already waiting; don't stack
    for (int i = 0; i < 4; i++) s_pending_uid[i] = uid[i];
    s_pending_len = 4;
    s_pending     = true;                     // publish last - see the declaration
    return true;
}

// Play a sequence of clips back-to-back (a composed countdown phrase) with one
// animation over the whole thing. The audio queue streams them with no gap, and
// audio_is_playing() stays true until the last one ends.
static void celebrate_seq(char paths[][CD_PATH_MAX], int n, anim_id_t anim)
{
    s_moment_busy = true;
    for (int i = 0; i < n; i++) audio_play(paths[i]);

    if (anim == ANIM_PULSE) {
        // Driven by the sound rather than a script: no opening choreography,
        // just brightness following the voice for as long as it talks. The
        // level is read here rather than inside leds.c so the LED code stays
        // independent of the audio engine.
        leds_pulse_reset();
        for (int guard = 0; audio_is_playing() && guard < 1500; guard++) {
            if (moment_interrupted()) break;
            leds_pulse_step(audio_level(), PULSE_COLOR);
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
        }
    } else {
        leds_play(anim);
        for (int guard = 0; audio_is_playing() && guard < 1500; guard++) {
            if (moment_interrupted()) break;
            leds_sustain_step(anim);
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
        }
    }
    // Eased down rather than cut. A hard clear followed by the idle glow
    // breathing up from nothing reads as two separate events; the fade lets one
    // moment end. It's also the join between a band's sound and the countdown
    // that follows it, so this is what keeps that from being a blink.
    leds_fade_out();
    s_moment_busy = false;
}

// Play a sound and run a chosen animation alongside it.
//
// The expansion lives here rather than at the call sites so every route into a
// moment gets it - a band's own sound, a random pick, and the boot greetings -
// and none can be forgotten later.
//
// Takes an ID ("chime") or a path ("/spiffs/Program/hello.wav"), told apart by
// the slash. Enrollments store ids so a theme can move the file underneath them;
// the boot prompts and the compiled defaults in tags.h are still literal paths,
// and there's no reason to churn them. Resolving both here means neither caller
// has to know which kind it holds.
//
// Then the variant expansion: "chime" plays chime or any chime-N beside it,
// chosen fresh on each tap. A name with no variants comes back untouched.
static void celebrate(const char *sound, anim_id_t anim)
{
    char resolved[CD_PATH_MAX];
    if (sound && sound[0] && !strchr(sound, '/')) {
        if (!sound_path_for_id(sound, resolved, sizeof(resolved)) || !resolved[0]) {
            // The id names nothing this device holds - a theme that never
            // arrived, or a clip retired out from under the band. Say so rather
            // than playing silence, which is indistinguishable from a dead
            // reader.
            ESP_LOGW(TAG, "no sound for id '%s' - is its theme installed?", sound);
            return;
        }
        sound = resolved;
    }

    char pick[CD_PATH_MAX];
    sound_pick(sound, pick, sizeof(pick));
    sound = pick;

    ESP_LOGI(TAG, "Playing %s", sound);
    s_moment_busy = true;
    audio_play(sound);
    leds_play(anim);       // the choreographed show; sound plays on the audio task
    // If the clip is longer than the show, hold/continue the look until it ends
    // (e.g. be-our-guest ~7s vs a ~4.5s animation). Capped so a stuck "playing"
    // flag can't hang here.
    for (int guard = 0; audio_is_playing() && guard < 1500; guard++) {   // ~30s cap
        if (moment_interrupted()) break;
        leds_sustain_step(anim);
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }
    // Eased down rather than cut. A hard clear followed by the idle glow
    // breathing up from nothing reads as two separate events; the fade lets one
    // moment end. It's also the join between a band's sound and the countdown
    // that follows it, so this is what keeps that from being a blink.
    leds_fade_out();
    s_moment_busy = false;
}

// Compiled-in default enrollment for a UID (from tags.h); NULL if none.
static const tag_profile_t *find_tag(const uint8_t *uid, uint8_t len)
{
    if (len < 4) return NULL;
    const tag_profile_t *end = TAG_PROFILES + NUM_TAG_PROFILES;
    for (const tag_profile_t *p = TAG_PROFILES; p < end; p++)
        if (memcmp(p->uid, uid, 4) == 0) return p;
    return NULL;
}

// Resolve a tapped UID to a sound + animation. NVS (program-mode enrollments)
// wins over the compiled defaults in tags.h. Returns false if unenrolled.
static bool resolve_tag(const uint8_t *uid, uint8_t len,
                        const char **sound, anim_id_t *anim,
                        char *nvsbuf, size_t nvsbuf_sz)
{
    if (len < 4) return false;
    uint8_t a;
    if (store_lookup(uid, nvsbuf, nvsbuf_sz, &a)) {
        if (nvsbuf[0] == '\0') return false;   // enrolled as the RANDOM action ->
                                               // fall through to a random pick
        *sound = nvsbuf;
        *anim  = (anim_id_t)a;
        return true;
    }
    const tag_profile_t *p = find_tag(uid, len);
    if (p) { *sound = p->sound; *anim = p->anim; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Program-mode button: long-press = enter/exit, short-press = next sound.
// ---------------------------------------------------------------------------
#if PROG_ENABLE
#define PROG_PRESSED (PROG_BUTTON_ACTIVE_LOW ? 0 : 1)
typedef enum { BTN_NONE, BTN_SHORT, BTN_LONG, BTN_VLONG } btn_evt_t;

static void prog_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PROG_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = PROG_BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE    : GPIO_PULLUP_DISABLE,
        .pull_down_en = PROG_BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "Program button on GPIO %d", PROG_BUTTON_GPIO);
}

// Current hold stage, for the LED cue: 0 = not holding past a threshold,
// 1 = past program threshold (amber).
static int s_hold_stage = 0;

// Classify a button press by how long it was held, on release: >3 s = BTN_LONG
// (program mode), a quick tap = BTN_SHORT. Classifying on release keeps the two
// actions from overlapping. While held, s_hold_stage tracks the threshold so
// the loop can paint a cue. (Setup/recovery is a separate gesture - the button
// held *through power-on*; see update_mode_held_at_boot.)
static btn_evt_t prog_button_poll(void)
{
    static bool    held = false;
    static int64_t t_down = 0;

    bool    pressed = (gpio_get_level(PROG_BUTTON_GPIO) == PROG_PRESSED);
    int64_t now     = esp_timer_get_time();

    if (pressed && !held) {
        held = true; t_down = now; s_hold_stage = 0;
    } else if (pressed && held) {
        int64_t dur = now - t_down;
        s_hold_stage = (dur > (int64_t)PROG_LONGPRESS_MS * 1000) ? 1 : 0;
    } else if (!pressed && held) {
        held = false; s_hold_stage = 0;
        int64_t dur = now - t_down;
        if (dur > (int64_t)PROG_LONGPRESS_MS * 1000) return BTN_LONG;   // program
        if (dur > (int64_t)PROG_DEBOUNCE_MS  * 1000) return BTN_SHORT;  // idle/audition
    } else {
        s_hold_stage = 0;
    }
    return BTN_NONE;
}

#if WIFI_ENABLE
// Setup/recovery gesture: the program button held *through power-on* (like a DFU
// combo). Sampled once at boot, before anything else touches the button. Blocks
// only while the button is actually down, and waits for release so the main loop
// doesn't then read the same press as a tap. Returns true if it was held past
// the threshold. Physical-presence gated: a plain boot never triggers it.
static bool update_mode_held_at_boot(void)
{
    if (gpio_get_level(PROG_BUTTON_GPIO) != PROG_PRESSED) return false;

    int64_t t0 = esp_timer_get_time();
    bool reached = false;
    // Wait out the hold, but cap the total time so a physically stuck/shorted
    // button can't hang boot forever. Past the cap we just stop waiting and boot
    // (into the harmless, recoverable setup mode if the threshold was reached).
    while (gpio_get_level(PROG_BUTTON_GPIO) == PROG_PRESSED) {
        int64_t held = esp_timer_get_time() - t0;
        if (!reached && held > (int64_t)UPDATE_MODE_HOLD_MS * 1000) {
            reached = true;
            leds_hold_cue(2);          // solid blue: "you've held long enough"
            audio_play(PROMPT_RELEASE_SETUP);   // "release the button to enter setup"
        }
        if (held > 10LL * 1000 * 1000) break;   // safety cap (~10 s): stuck button
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (reached) leds_off();
    return reached;
}
#endif
#endif // PROG_ENABLE

#if WIFI_ENABLE
// Boot-time provisioning, three outcomes (see net_state_t):
//   NET_ONLINE   - joined the saved home network; LAN config server running.
//   NET_SETUP_AP - button held at boot, or no creds: SoftAP + full setup portal.
//   NET_OFFLINE  - provisioned but couldn't join right now: run offline, keep
//                  retrying in the background (auto-recovers). NOT the setup AP.
// Never blocks the scanner from starting.
// How the network came up at boot.
typedef enum { NET_ONLINE, NET_SETUP_AP, NET_OFFLINE } net_state_t;

// Bring up the LAN services once we hold an IP: the .local name, the config
// server, and a clock sync. Called on an immediate join, or later by the network
// task if the device joined only after a delayed (background) reconnect.
static void online_setup(const device_config_t *cfg)
{
    ESP_LOGI(TAG, "Online as '%s'.", cfg->device_name);
    wifi_start_mdns(cfg->device_name);         // <name>.local
    portal_start(false);                       // LAN config only (upload locked)
#if NTP_ENABLE
    ntp_sync(NTP_SYNC_TIMEOUT_MS);             // set the clock (best-effort)
#endif
}

static net_state_t provision(bool setup_mode)
{
    device_config_t cfg;
    appcfg_load(&cfg);
    wifi_init();

    if (!setup_mode && appcfg_has_wifi(&cfg)) {
        if (wifi_connect_sta(cfg.wifi_ssid, cfg.wifi_password, WIFI_STA_TIMEOUT_MS)) {
            online_setup(&cfg);
            return NET_ONLINE;
        }
        // Provisioned but couldn't join right now (router down at power-on, out of
        // range, ...). Don't trap in the setup AP over a possibly-transient miss:
        // stay usable OFFLINE, keep retrying in the background (auto-recovers when
        // Wi-Fi returns), and tell them how to redo setup on purpose.
        ESP_LOGW(TAG, "Home Wi-Fi join failed - staying offline and retrying");
        audio_play(PROMPT_WIFI_TROUBLE);
        wifi_keep_retrying();
        return NET_OFFLINE;
    }

    ESP_LOGI(TAG, "%s - opening setup access point",
             setup_mode ? "Setup requested (button held at boot)"
                        : "No Wi-Fi configured");

    // AP SSID follows the device name (rename it with the `set-name` CLI cmd);
    // the -<id> keeps two same-named units distinct. Changing the name yields a
    // brand-new SSID, which is also the reliable way to dodge a phone's cached
    // "this network has no captive portal" verdict.
    char ap_ssid[33];   // Wi-Fi SSIDs cap at 32 bytes; bound the name portion
    snprintf(ap_ssid, sizeof(ap_ssid), "%.20s-%s-Setup", cfg.device_name, wifi_device_id());
    wifi_start_ap(ap_ssid);
    portal_start(true);                            // full setup portal + firmware upload
    return NET_SETUP_AP;
}

// Provisioning runs on its own task so app_main can start the interactive loop
// immediately (no more blocking ~15 s on a weak join). Needs a generous stack:
// the NTP HTTP-Date fallback does TLS. On a home-network join it flags the loop
// to play the "online" moment; otherwise provision() opens the setup AP.
static bool s_net_setup_arg = false;

// Auto-update on boot is DORMANT until a real manifest URL is configured: the
// compiled default points at example.com, so skip that (and an empty URL). This
// makes "set a real manifest URL in setup" the on-switch - the gift never
// self-updates until you're ready to publish for it.
static bool manifest_configured(const char *url)
{
    return url && url[0] && !strstr(url, "example.com");
}

// Fetch the manifest and apply any newer firmware + media. Silent, and safe
// unattended because a bad image auto-reverts (rollback, ota_mark_valid). Reboots
// (quietly) if firmware was installed. No-op unless a real manifest host is set.
// `verify` asks for the audio pack to be checked against what is really on
// flash rather than trusting the recorded pack version. True at boot, false on
// the routine cycle.
//
// The version alone can only answer "is this the same manifest?", never "do the
// files still match it" - so a sound deleted or corrupted on the device is
// invisible to it, forever. On a reader in someone else's house nobody is going
// to type `sync-media`, so the only chance to notice is a check the device
// makes itself. Boot is the right place: reboots are rare, it costs about nine
// seconds of hashing, and it lets the thing quietly repair itself.
static void check_for_updates(const char *why, bool verify)
{
    device_config_t cfg;
    appcfg_load(&cfg);                     // re-read each time: picks up a newly-set URL

    if (manifest_configured(cfg.manifest_url)) {
        bool installed = false;
        ESP_LOGI(TAG, "%s update check: %s", why, cfg.manifest_url);
        // Syncs the audio pack too, on its way past.
        ota_update_from_manifest(cfg.manifest_url, FW_VERSION, &installed, verify);
        if (installed) {
            ESP_LOGW(TAG, "firmware updated (%s) - rebooting (silent)", why);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();                 // quiet_boot was set by the update path
        }
        return;
    }

    // Firmware updates are dormant (manifest still the example.com placeholder),
    // but the audio pack has its OWN on-switch and must not inherit this one.
    //
    // These are two separate documents at two separate URLs, with two separate
    // reasons to be turned on. Leaving the pack behind the firmware gate meant a
    // device could have assets_url set and a reachable host and still never
    // sync a thing - the exact configuration a gift ships in, since dormant
    // firmware updates are the point of that default.
    if (cfg.assets_url[0]) {
        int  n = 0;
        bool deferred = false;
        ESP_LOGI(TAG, "%s pack check: %s", why, cfg.assets_url);
        // assets_sync_pack raises and clears the "needs a person" state itself,
        // so every caller of it agrees - see the note at the top of assets.c.
        if (assets_sync_pack(cfg.assets_url, FW_VERSION, &n, &deferred, verify) != ESP_OK)
            ESP_LOGW(TAG, "pack sync incomplete - will retry next cycle");
        else if (deferred)
            ESP_LOGW(TAG, "pack held back: needs firmware %s", assets_pack_requires());
        else if (n > 0)
            ESP_LOGI(TAG, "pack: %d file(s) updated", n);
    }
}

static void network_task(void *arg)
{
    (void)arg;
    net_state_t st = provision(s_net_setup_arg);
    s_setup_mode = (st == NET_SETUP_AP);
    s_connecting = false;                  // done attempting -> stop the sparkle

    if (st == NET_SETUP_AP) { vTaskDelete(NULL); return; }   // AP: no housekeeping

    // Online now (services up), or offline-and-retrying (services deferred until a
    // background reconnect lands).
    bool services_up = (st == NET_ONLINE);
    if (services_up && !s_quiet_boot && s_boot_audio) s_online_pending = true;
    bool startup_checked = false;

    // Monitor + housekeeping for the long haul (months to the trip). While
    // offline we poll often for a (re)connect; once up we relax to the 6h period,
    // re-syncing the drifting clock (no RTC) and re-checking for updates each cycle.
    for (;;) {
        if (wifi_is_connected()) {
            if (!services_up) {            // joined only after a delayed reconnect
                device_config_t cfg;
                appcfg_load(&cfg);
                online_setup(&cfg);
                services_up = true;
                if (!s_quiet_boot && s_boot_audio) s_online_pending = true;  // "online" moment
            }
            if (!startup_checked) {
                // Wait for the boot moment to actually finish, rather than
                // guessing at it. The old fixed 4 s was tuned to the audio -
                // operational.wav is ~2.1 s - but the "online" moment is
                // animation-bound: ANIM_CELEBRATE runs ~4.5 s. So the sleep
                // expired mid-show and the TLS handshake landed on top of it,
                // stuttering the green ring. Capped so a wedged moment can't
                // hold the update check off forever.
                // Pending counts as busy. This task is what SETS
                // s_online_pending, and the main loop only turns that into a
                // moment on its next pass - so checking "busy" alone finds
                // nothing running yet and sails straight through, which is
                // exactly what it did: the check fired 480 ms into the show.
                for (int i = 0; i < 120 && (s_online_pending || app_moment_busy()); i++)
                    vTaskDelay(pdMS_TO_TICKS(100));
                vTaskDelay(pdMS_TO_TICKS(500));    // let the last frame land
                check_for_updates("startup", true);   // verify the bank while we're here
                startup_checked = true;
            } else {
                ESP_LOGI(TAG, "housekeeping: re-sync clock + check updates");
#if NTP_ENABLE
                ntp_sync(NTP_SYNC_TIMEOUT_MS);     // correct clock drift
#endif
                check_for_updates("periodic", false); // routine: trust the pack version
            }
        }
        vTaskDelay(pdMS_TO_TICKS((services_up && startup_checked) ? HOUSEKEEP_PERIOD_MS : 30000));
    }
}
#endif // WIFI_ENABLE

void app_main(void)
{
    ESP_LOGI(TAG, "=== MagicMaker ===");

    store_init();

    // Per-device settings that the drivers need before they start: the physical
    // LED layout (every build is wired a bit differently, and one OTA image
    // serves several units), the idle colour, and whether to wake up audibly.
    {
        device_config_t c;
        appcfg_load(&c);
        leds_set_layout(c.ring_leds, c.mickey_leds, c.ring_first);
        leds_set_idle_color(c.idle_color);
        countdown_set_audio_set(c.audio_set);   // which occasion's framing to speak
        s_boot_audio = c.boot_audio_enabled;
    }

    audio_init();
    sounds_init();   // needs the data partition, which audio_init() mounts
    leds_init();
    trigger_init();
    s_main_task = xTaskGetCurrentTaskHandle();  // only this task may poll the reader
    leds_set_abort_check(moment_interrupted);   // a new tap cuts a moment short
#if PROG_ENABLE
    prog_button_init();
#endif

    // Setup/recovery gesture: button held through power-on. Sampled first, while
    // the button is guaranteed idle-state, before the CLI or main loop read it.
    bool setup_mode = false;
#if PROG_ENABLE && WIFI_ENABLE
    setup_mode = update_mode_held_at_boot();
#endif
#if WIFI_ENABLE
    // One-shot: the `set-name` CLI command sets this to bring us up in setup mode
    // (with the freshly-renamed AP) without needing the physical button.
    if (store_get_flag("force_setup", 0)) { store_set_flag("force_setup", 0); setup_mode = true; }
#endif

#if CLI_ENABLE
    cli_start_when_connected();   // serial CLI - starts only once a USB host attaches
#endif

    // Core hardware is up -> if this is a freshly-OTA'd image, confirm it so the
    // bootloader keeps it. A bad image that crashes before here auto-reverts.
    // The return says whether we just updated, so boot audio can say "all updated!"
    bool just_updated = ota_mark_valid();

#if DEMO_MODE
    // Preview/tune loop: cycle every animation forever, no cards/buttons.
    ESP_LOGI(TAG, "DEMO MODE - cycling animations (set DEMO_MODE 0 for normal use)");
    // Sized by ANIM_COUNT, so adding an id without a name here is a compile
    // error rather than a read off the end.
    static const char *ANIM_NAMES[ANIM_COUNT] = { "CELEBRATE", "WELCOME", "FIREWORKS",
                                                  "RAINBOW", "ENCHANTED", "BE-OUR-GUEST",
                                                  "PULSE" };
    for (;;) {
        for (int a = 0; a < ANIM_COUNT; a++) {
            ESP_LOGI(TAG, ">>> Animation %d: %s", a, ANIM_NAMES[a]);
            leds_play((anim_id_t)a);
            leds_off();
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
#endif

    // Quiet-boot: a manifest-driven OTA (ota_update_from_manifest) sets this
    // one-shot flag before rebooting, so a self-initiated update lands silently.
    // "Unexpectedly online" - power-loss, unplug/replug, plain reboot, and manual
    // firmware pushes (web upload, ota-url) - never set it, so those still greet.
    s_quiet_boot = store_get_flag("quiet_boot", 0);
    if (s_quiet_boot) store_set_flag("quiet_boot", 0);  // one-shot: clear on use

#if WIFI_ENABLE
    // Kick off Wi-Fi/NTP/servers on their own task so the device is interactive
    // right away. The "online" moment (operational sound + animation) fires from
    // the main loop when this reports a successful home-network join.
    device_config_t bootcfg;
    appcfg_load(&bootcfg);
    bool provisioned = appcfg_has_wifi(&bootcfg);
    s_net_setup_arg = setup_mode;
    s_connecting = provisioned && !setup_mode;   // will attempt a join -> sparkle until it lands
    xTaskCreate(network_task, "net", 10240, NULL, 5, NULL);
#endif

#if PLAY_BOOT_SOUND
    // s_quiet_boot suppresses one specific reboot (a self-initiated OTA);
    // boot_audio_enabled is the owner's standing "wake up quietly" preference.
    if (!s_quiet_boot && s_boot_audio) {
#if WIFI_ENABLE
        if (just_updated) {
            audio_play(PROMPT_UPDATE_DONE);             // "all updated!" after a user install
        } else if (setup_mode) {
            // Button held at boot. Same instruction as a first boot, deliberately:
            // this branch used to say only "entering setup", which left the one
            // thing the user has to do next - join the setup network - unsaid.
            // browse-magicmaker.wav only speaks AFTER they've joined it.
            audio_play(PROMPT_WIFI_SETUP);
        } else if (!provisioned) {
            celebrate(BOOT_SOUND_FIRSTRUN,              // first boot: Walt's dedication...
                      anim_for_sound(BOOT_SOUND_FIRSTRUN));
            audio_play(PROMPT_WIFI_SETUP);              // ...then the setup prompt
        } else {
            audio_play(BOOT_SOUND_WELCOME);             // "welcome to MagicMaker!"; the loop then
                                                        // sparkles until Wi-Fi connects
        }
#else
        celebrate(BOOT_SOUND, anim_for_sound(BOOT_SOUND));
#endif
    }
#endif

    ESP_LOGI(TAG, "Ready - tap a card or 'band.");

    uint8_t uid[4];
    uint8_t uid_len = 0;
    char    nvsbuf[48];

    // Idle glow on/off, remembered across power-offs (default = IDLE_BREATHE).
    bool idle_on = store_get_flag("idle", IDLE_BREATHE);
    leds_set_idle_enabled(idle_on);
#if PROG_ENABLE
    bool prog_mode = false;
    int  audition  = -1;      // -1 = nothing auditioned yet
#endif

    for (;;) {
#if WIFI_ENABLE
        if (s_online_pending) {              // network task joined home Wi-Fi
            // Spoken, not celebrated. Boot already has a look - the sparkle runs
            // through the Wi-Fi join - and following it with a full green
            // celebration made switching on feel like an achievement rather than
            // a device waking up. The line alone lands better.
            //
            // Claim before releasing, so the two flags are never both clear: the
            // network task waits on "pending or busy" before its update check,
            // and clearing pending first leaves a gap where it sees neither.
            s_moment_busy    = true;
            s_online_pending = false;
            audio_play(BOOT_SOUND_OPERATIONAL);
            // Keep the boot look alive while it speaks. Waiting on a bare delay
            // froze the strip for the length of the clip - the sparkle didn't
            // end, it stalled, which looks like the device hanging rather than
            // talking. Dropping the celebration meant dropping the animation
            // with it; only the celebration was unwanted.
            while (audio_is_playing()) {
                leds_sparkle_step();
                vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
            }
            leds_fade_out();          // settle into the idle glow rather than snap
            s_moment_busy    = false;
        }
        if (s_setup_mode && wifi_ap_client_joined()) {  // phone joined the setup AP
            portal_clear_page_seen();                  // this one hasn't found it yet
            s_visit_said   = 0;
            s_visit_due_us = 0;                        // speak as soon as audio is free
        }
        // Say the address again while nobody has actually opened the page. Not on
        // a plain timer: portal_page_seen() goes true the moment a browser lays
        // the page out, so this shuts up on its own the instant they arrive -
        // whether the captive sheet popped by itself or they typed the IP.
        // Reciting an address at someone already reading it is worse than silence.
        if (s_setup_mode && s_visit_said < VISIT_PROMPT_MAX && !portal_page_seen()) {
            int64_t now = esp_timer_get_time();
            if (now >= s_visit_due_us && !audio_is_playing()) {
                audio_play(PROMPT_VISIT_SITE);         // "visit 192.168.4.1 to continue"
                s_visit_said++;
                s_visit_due_us = now + (int64_t)VISIT_PROMPT_GAP_MS * 1000;
            }
        }
#endif
#if PROG_ENABLE
        btn_evt_t be = prog_button_poll();

        if (be == BTN_LONG) {                     // toggle program mode
            s_pending = false;                    // drop any latched tap; it isn't
                                                  // meant for the mode we're entering
            prog_mode = !prog_mode;
            if (prog_mode) {
                audition = -1;
                ESP_LOGI(TAG, "Program mode ON - press to audition, tap a card to assign");
                audio_play(PROMPT_START);
            } else {
                ESP_LOGI(TAG, "Program mode OFF");
                audio_play(PROMPT_ALLDONE);
                leds_off();
            }
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
            continue;
        }

        if (prog_mode) {
            // Audition cycles all sounds plus one extra "make random" slot at
            // the end (index == NUM_SOUNDS), which un-programs a card.
            if (be == BTN_SHORT) {
                audition = (audition + 1) % (NUM_SOUNDS + 1);
                if (audition == NUM_SOUNDS) {
                    ESP_LOGI(TAG, "Audition: RANDOM (un-program a card)");
                    audio_play(PROMPT_RANDOM);
                } else {
                    ESP_LOGI(TAG, "Audition %d/%d: %s", audition + 1, NUM_SOUNDS, sound_path(audition));
                    audio_play(sound_path(audition));
                }
            }

            if (trigger_poll(uid, &uid_len)) {    // tap a card to assign / un-program
                if (uid_len >= 4 && audition == NUM_SOUNDS) {
                    store_erase(uid);             // this one card -> back to random
                    ESP_LOGI(TAG, "Card un-programmed -> random");
                    audio_play(PROMPT_RANDOM);
                    audio_play(PROMPT_SCANNOW);   // "scan it to try"
                    prog_mode = false;
                    leds_off();
                } else if (uid_len >= 4 && audition >= 0) {
                    esp_err_t r = store_save(uid, sound_id(audition),
                                             (uint8_t)anim_for_sound(sound_path(audition)));
                    if (r == ESP_OK) {
                        audio_play(PROMPT_SAVED);
                        audio_play(PROMPT_SCANNOW);   // "scan it to try"
                    } else {
                        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(r));
                        audio_play(PROMPT_TRYAGAIN);  // e.g. storage full
                    }
                    prog_mode = false;            // auto-exit so the try-tap works
                    leds_off();
                } else {
                    audio_play(PROMPT_TRYAGAIN);  // no sound picked yet / bad read
                }
            }

            if (s_hold_stage) leds_hold_cue(s_hold_stage);   // holding for Wi-Fi setup
            else              leds_prog_step();
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
            continue;
        }

        // Normal mode: a short press toggles the idle glow (and remembers it).
        if (be == BTN_SHORT) {
            idle_on = !idle_on;
            leds_set_idle_enabled(idle_on);
            store_set_flag("idle", idle_on ? 1 : 0);
            ESP_LOGI(TAG, "Idle glow %s", idle_on ? "on" : "off");
        }
#endif // PROG_ENABLE

        // --- normal mode ---
        // A tap latched while the previous moment was playing jumps the queue;
        // otherwise poll the reader as usual.
        bool tapped;
        if (s_pending) {
            for (int i = 0; i < 4; i++) uid[i] = s_pending_uid[i];
            uid_len   = s_pending_len;
            s_pending = false;
            tapped    = true;
        } else {
            tapped = trigger_poll(uid, &uid_len);
        }

        if (tapped) {
            // Remember who's playing so a card left on the reader can't interrupt
            // its own moment (see moment_interrupted).
            memcpy(s_current_uid, uid, uid_len < 4 ? uid_len : 4);
            s_current_len = uid_len;

            bands_note_scan(uid, uid_len);       // remember it for the web page

            // First tap of the day for this band -> the composed countdown
            // phrase; repeated taps of one band -> a cheeky line. (No-op if the
            // countdown's off, the clock isn't set, or this band was already
            // greeted today.)
            static char cdpaths[CD_MAX_CLIPS][CD_PATH_MAX];
            uint8_t cdanim = ANIM_CELEBRATE;
            int cdn = countdown_due(uid, uid_len, cdpaths, CD_MAX_CLIPS, &cdanim);

            // The band's own sound comes FIRST, and the countdown follows it -
            // they no longer replace each other. A band set to "always" used to
            // never play its own sound at all, which is backwards: the scan is
            // the thing you chose, and the countdown is an addition to it.
            //
            // Two shows rather than one merged sequence, so each keeps its own
            // animation - a Star Tours band shouldn't turn into the countdown's
            // colours just because a countdown is due. The fade at the end of
            // the first is the beat between them; letting them run together
            // would crash two thoughts into one sentence.
            //
            // Cheeky lines are the exception. They're already a reply to tapping
            // the same band repeatedly, so the band's sound has just played -
            // saying it again would be the joke stepping on itself.
            bool cheeky = (cdn > 0 && cdanim == ANIM_PULSE);
            if (!cheeky && !s_pending) {
                const char *sound;
                anim_id_t   anim;
                if (resolve_tag(uid, uid_len, &sound, &anim, nvsbuf, sizeof(nvsbuf))) {
                    celebrate(sound, anim);             // this band's own moment
                } else {
                    if (uid_len >= 4) {
                        ESP_LOGI(TAG, "Unknown tag %02X%02X%02X%02X - long-press to program it",
                                 uid[0], uid[1], uid[2], uid[3]);
                    }
                    // Random pick, but never the same sound twice in a row (enrolled
                    // cards above always play their own sound; only randoms de-dupe).
                    static int last_rnd = -1;
                    int ridx = esp_random() % NUM_SOUNDS;
                    if (NUM_SOUNDS > 1)
                        while (ridx == last_rnd) ridx = esp_random() % NUM_SOUNDS;
                    last_rnd = ridx;
                    celebrate(sound_path(ridx), anim_for_sound(sound_path(ridx)));
                }
            }

            // ...then the countdown, as a second thought rather than a
            // replacement. Skipped if a new tap arrived during the first half -
            // that tap is what someone is waiting on now, and the countdown will
            // still be due next time.
            if (cdn > 0 && !s_pending && !moment_interrupted())
                celebrate_seq(cdpaths, cdn, (anim_id_t)cdanim);

            // ...and last, if the device can't finish an update on its own, it
            // asks. Last so it never displaces what someone tapped for: the
            // scan still does its own thing first, and this is appended.
            //
            // Every scan, deliberately - it is the only channel the device has
            // to reach somebody, and a condition that needs a person doesn't
            // resolve by being mentioned once. It stops the moment the next
            // sync succeeds, so the way to make it quiet is to fix it.
            if (needhelp_active() && !s_pending && !moment_interrupted()) {
                char clip[CD_PATH_MAX];
                if (needhelp_clip(clip, sizeof(clip))) {
                    static char seq[1][CD_PATH_MAX];
                    snprintf(seq[0], CD_PATH_MAX, "%s", clip);
                    celebrate_seq(seq, 1, ANIM_PULSE);   // a voice, not a show
                }
            }

            s_current_len = 0;                          // moment over
            // The re-trigger cooldown is measured from the last read, so a
            // multi-second show outlives it: without this, a card still on the
            // reader re-fires immediately and taps appear to queue up.
            if (!s_pending) trigger_flush();
        } else {
#if PROG_ENABLE
            if (s_hold_stage) leds_hold_cue(s_hold_stage);   // holding for program mode
            else
#endif
            if (s_setup_mode)      leds_setup_step();         // sitting in setup/AP mode (cyan)
            else if (s_connecting) leds_sparkle_step();       // joining Wi-Fi -> blue twinkle
            else                   leds_idle_step();          // idle (calm blue breathe)
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
        }
    }
}
