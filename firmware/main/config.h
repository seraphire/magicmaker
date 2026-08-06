// ---------------------------------------------------------------------------
// config.h - all the knobs for the MagicBand Reader in one place.
//
// This is a stopgap build: a button stands in for the PN532 NFC reader that
// hasn't arrived yet. When it does, only trigger.c changes - see trigger.h.
// ---------------------------------------------------------------------------
#pragma once

// ===========================================================================
// Board / pin map  (ESP32-S3, shared with the vintage-radio wiring)
// ===========================================================================

// WS2812 / NeoPixel data line (ring + Mickey are chained on this one pin).
#define NEOPIXEL_GPIO       GPIO_NUM_4

// I2S -> MAX98357A amplifier.  (LRC=1, BCLK=2, DIN=42 as wired on the board)
#define I2S_BCLK_GPIO       GPIO_NUM_2
#define I2S_WS_GPIO         GPIO_NUM_1
#define I2S_DOUT_GPIO       GPIO_NUM_42

// Push-button trigger (stand-in for NFC). Wire a momentary button between this
// pin and GND; the internal pull-up handles the rest, so no resistor needed.
#define BUTTON_GPIO         GPIO_NUM_6
#define BUTTON_ACTIVE_LOW   1        // pressed == pin reads LOW (button to GND)

// ===========================================================================
// Trigger source. Priority: RC522 > PN532 > button.
//   TRIGGER_USE_RC522 1 = RC522 RFID over SPI (what's wired for the gift)
//   TRIGGER_USE_NFC   1 = PN532 NFC over I2C (unused here)
//   both 0              = push-button on BUTTON_GPIO
// ===========================================================================
#define TRIGGER_USE_RC522   1
#define TRIGGER_USE_NFC     0

// RC522 wiring (SPI). RST tied to a GPIO so it can be hard-reset in software.
#define RC522_SPI_HOST      SPI2_HOST
#define RC522_SCK_GPIO      GPIO_NUM_11
#define RC522_MOSI_GPIO     GPIO_NUM_10
#define RC522_MISO_GPIO     GPIO_NUM_9
#define RC522_CS_GPIO       GPIO_NUM_12
#define RC522_RST_GPIO      GPIO_NUM_8
#define RC522_SPI_HZ        1000000   // 1 MHz - conservative and reliable
// 3V3 + GND go to the rails (NOT GPIO 3/46); IRQ unconnected.

// PN532 wiring (only used when TRIGGER_USE_NFC == 1).
// Set the module's DIP switches / jumpers to I2C mode. Only SDA + SCL + power
// are required for polled reads; IRQ/RST are optional.
#define PN532_SDA_GPIO      GPIO_NUM_10
#define PN532_SCL_GPIO      GPIO_NUM_9
#define PN532_I2C_HZ        100000   // 100 kHz is safe; PN532 also does 400 kHz

// Interrupt-driven detection keeps the idle animation perfectly smooth: the
// PN532 pulls its IRQ line LOW when a band is found, so we never busy-poll I2C
// while waiting. Wire the module's IRQ pad to this GPIO.
//   1 = use IRQ (recommended)   0 = fall back to blocking polled reads
#define PN532_USE_IRQ       1
#define PN532_IRQ_GPIO      GPIO_NUM_11

// Ignore the same band for this long after a read, so one tap = one reaction
// (instead of re-firing every poll while the band sits on the reader).
#define NFC_RETRIGGER_MS    1500

// ===========================================================================
// LED layout
//
// Your strip is two logical segments chained on one data line. Set each count
// to the real number of pixels once you know them. The chase animation runs on
// the RING; the MICKEY segment glows/pulses along with it.
//
// Order assumption: the RING pixels come first in the chain, then MICKEY.
// If yours is wired the other way round, set RING_FIRST to 0.
// ===========================================================================
// These are the DEFAULTS for a fresh device. The live values are per-device
// settings (NVS, editable on the config page) because every physical build
// differs - a strand gets cut to fit, a dead pixel gets removed - and one OTA
// image now serves several units. Compile-time counts would give unit two
// unit one's layout.
#define RING_LED_COUNT      45       // ring is first in the chain (pixels 0-44)
                                     // was 46; first pixel died and was cut off the strand
#define MICKEY_LED_COUNT    36       // face loop is pixels 45-80, closes at bottom
#define RING_FIRST          1        // confirmed: ring first, then the mouse face

// Allocation ceilings. Buffers and the strip driver are sized to these, so the
// runtime counts can be anything up to them without a rebuild. Raise if you
// build something bigger.
#define RING_LED_MAX        120
#define MICKEY_LED_MAX      120
#define TOTAL_LED_MAX       (RING_LED_MAX + MICKEY_LED_MAX)

// Global brightness ceiling, 0-255. 120 keeps it lively while trimming the
// peak LED current (~20% vs 150) for more headroom on modest USB supplies -
// full-white animation spikes were browning out weak USB-A ports. Raise back
// toward 150 once running on a solid 2A+ supply (or the PCB's 5V injection).
// Frame pacing for the animation loops (~50 fps). Shared: main.c drives the
// idle/sustain loops with it, and the CLI holds a test animation the same way
// so what you see over serial matches what a real tap does.
#define FRAME_DELAY_MS      20

// Colour of the audio-reactive pulse used for the cheeky repeat-tap lines.
// Warm amber: playful rather than ceremonial, and clearly not one of the
// celebration colours, so a repeat tap reads as an aside at a glance.
#define PULSE_COLOR         0xFFB030

#define LED_BRIGHTNESS      120

// Demo mode: 1 = ignore cards/buttons and just cycle every animation in a loop
// (for previewing/tuning them). Set back to 0 for the normal reader.
#define DEMO_MODE           0

// ===========================================================================
// Audio
// ===========================================================================
// All the Adafruit sounds are 22050 Hz / 16-bit / mono, so we run I2S there.
#define AUDIO_SAMPLE_RATE   22050

// Software volume, 0-256 (256 = unity / no attenuation). The MAX98357A also has
// a fixed hardware gain; drop this if things clip or are simply too loud.
// When USE_VOLUME_POT is on, this is just the startup value; the knob takes over.
#define AUDIO_VOLUME        256

// Optional volume knob: a ~10k pot with its outer legs on 3V3 and GND and its
// wiper on VOLUME_POT_GPIO (must be ADC-capable). Software scales playback
// 0..full and follows the knob live. Set USE_VOLUME_POT 0 to disable.
#define USE_VOLUME_POT      1
#define VOLUME_POT_GPIO     GPIO_NUM_5

// Play a sound once at boot so you know the audio path works. Set to 0 to
// stay silent until the first trigger (matches the Adafruit behaviour).
#define PLAY_BOOT_SOUND     1
#define BOOT_SOUND          "/spiffs/hello.wav"
// First boot / not-yet-provisioned greeting: Walt's dedication instead of the
// everyday "hello". (walt-welcome.wav for now - see the audio recording list;
// we may cut a dedicated "To all who come to this happy place" take.)
#define BOOT_SOUND_FIRSTRUN "/spiffs/walt-welcome.wav"
// Provisioned power-on greeting (plays immediately with the blue sparkle);
// "all systems operational" then plays once Wi-Fi actually connects.
#define BOOT_SOUND_WELCOME     "/spiffs/Program/welcome-magic.wav"
#define BOOT_SOUND_OPERATIONAL "/spiffs/operational.wav"

// ===========================================================================
// Program mode: enroll cards on the device (saved to NVS, survives power-off).
//   Long-press the button  -> enter/exit program mode
//   Short-press the button -> audition the next sound
//   Tap a card             -> assign the current sound to that card
// Wire a momentary button between PROG_BUTTON_GPIO and GND.
// ===========================================================================
#define PROG_ENABLE            1
#define PROG_BUTTON_GPIO       GPIO_NUM_6
#define PROG_BUTTON_ACTIVE_LOW 1
#define PROG_LONGPRESS_MS      3000     // hold ~3 s to enter/exit program mode
                                        // (setup/recovery = hold through power-on,
                                        //  see UPDATE_MODE_HOLD_MS)
#define PROG_DEBOUNCE_MS       30

// Voice prompts (in spiffs/Program/). A missing file just plays nothing.
#define PROMPT_START     "/spiffs/Program/start-mode.wav"   // entering program mode
#define PROMPT_SAVED     "/spiffs/Program/saved.wav"        // card assigned
#define PROMPT_SCANNOW   "/spiffs/Program/scan-now.wav"     // "scan it to try"
#define PROMPT_ALLDONE   "/spiffs/Program/all-done.wav"     // exiting program mode
#define PROMPT_TRYAGAIN  "/spiffs/Program/try-again.wav"    // read failed
#define PROMPT_RANDOM    "/spiffs/Program/random.wav"       // "this card is now a surprise"
// Plays for BOTH ways into setup: a first boot with no credentials, and the
// button-held-at-boot recovery. One instruction, one recording, so the two paths
// sound like the same device. (entering-setup.wav is retired - it said
// "programming", and only restated this more vaguely.)
#define PROMPT_WIFI_SETUP "/spiffs/Program/wifi-setup.wav"  // "starting setup, connect your phone..."
#define PROMPT_WIFI_FAILED "/spiffs/Program/wifi-failed.wav" // "couldn't connect - starting setup" (legacy)
// Provisioned but can't connect right now: we stay usable offline and keep
// retrying (see provision()). Tell them how to redo setup on purpose.
#define PROMPT_WIFI_TROUBLE "/spiffs/Program/wifi-trouble.wav" // "trouble connecting - hold the button while powering on to change setup"
#define PROMPT_RELEASE_SETUP "/spiffs/Program/release-setup.wav"  // "release the button..."
#define PROMPT_VISIT_SITE "/spiffs/Program/browse-magicmaker.wav" // "visit 192.168.4.1..."

// The address prompt repeats while nobody has actually opened the page (see
// portal_page_seen). Three times across ~40 s, then silence - if that hasn't
// landed it, more talking won't. The gap is measured from the start of the clip
// (~10 s), so it's about ten seconds of quiet between takes.
#define VISIT_PROMPT_MAX     3
#define VISIT_PROMPT_GAP_MS  20000

// OTA voice. update-start/failed play only for *user-initiated* installs (web
// upload, ota-url); update-done plays on the next boot after one. Manifest-
// driven updates (update-now / scheduled) stay silent - see quiet_boot.
#define PROMPT_UPDATE_START  "/spiffs/Program/update-start.wav"   // "updating my magic..."
#define PROMPT_UPDATE_DONE   "/spiffs/Program/update-done.wav"    // "all updated!"
#define PROMPT_UPDATE_FAILED "/spiffs/Program/update-failed.wav"  // "update didn't take"

// ===========================================================================
// Wi-Fi / provisioning  (Stage 1 of the Wi-Fi/NTP/OTA roadmap)
// Set WIFI_ENABLE 0 to build the offline device exactly as before.
// ===========================================================================
#define WIFI_ENABLE          1
#define WIFI_AP_PREFIX       "MagicMaker"        // SoftAP SSID = PREFIX-<id>-Setup
#define WIFI_AP_MAX_CONN     4
#define WIFI_STA_TIMEOUT_MS  15000               // wait this long to join home Wi-Fi

// Setup / recovery is entered by holding the program button *through power-on*
// (like a DFU combo): opens the SoftAP portal with firmware upload + manifest
// editing, keeping all saved settings. A plain boot stays locked. Reboot exits.
#define UPDATE_MODE_HOLD_MS  1500                // hold this long at boot -> setup mode

// Tiny serial command line over the console (help / forget-wifi / factory-reset
// / update-mode). The REPL only starts once a USB host is actually connected
// (see cli_start_when_connected) - an idle REPL over USB-Serial-JTAG with no
// host draining it spins and starves the animation loop, so on a plain charger
// it never runs. Set 0 to leave the CLI out entirely.
#define CLI_ENABLE           1

// Compiled-in config defaults (first boot / after an NVS wipe).
#define CFG_DEFAULT_DEVICE_NAME  "MagicMaker"
#define CFG_DEFAULT_TRIP_YEAR    2027
#define CFG_DEFAULT_TRIP_MONTH   1
#define CFG_DEFAULT_TRIP_DAY     20

// ===========================================================================
// Time / NTP  (Stage 2). Sync the clock over SNTP after joining Wi-Fi; there's
// no RTC, so time resets each power cycle and is re-synced on boot.
// ===========================================================================
#define NTP_ENABLE           1
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"
#define NTP_SERVER_3         "time.cloudflare.com"
#define NTP_SYNC_TIMEOUT_MS  6000
// If NTP (UDP 123) is blocked, fall back to the Date header of this HTTPS URL.
#define NTP_HTTP_FALLBACK_URL "https://example.com/"
// POSIX TZ: US Eastern with daylight saving (2nd Sun Mar -> 1st Sun Nov).
// (Made configurable later; compiled default for now.)
#define NTP_TZ               "EST5EDT,M3.2.0,M11.1.0"

// Background housekeeping period. There's no RTC, so the clock drifts between
// syncs; every cycle re-syncs NTP (keeps the countdown accurate over months) and
// re-checks the manifest (updates land without waiting for a reboot). Wi-Fi self-
// heals separately (see wifi.c). 6 h is plenty for drift and polite for polling.
#define HOUSEKEEP_PERIOD_MS  (6 * 60 * 60 * 1000)

// ===========================================================================
// OTA / update  (Stages 3-6). The manifest + firmware host are set later; for
// now OTA_TEST_URL is the target for the Stage-3 HTTPS smoke test (CLI: fetch).
// ===========================================================================
#define OTA_TEST_URL         "https://example.com/"

// Running firmware version (bump per release; compared against the manifest).
#define FW_VERSION           "1.4.0"
// Where the update manifest lives (override per-fetch with: update-check <url>).
#define OTA_MANIFEST_URL     "https://example.com/manifest.json"

// ===========================================================================
// Idle look while waiting for a trigger
// 0 = LEDs fully off (matches Adafruit).  1 = gentle "armed" breathing.
// ===========================================================================
#define IDLE_BREATHE        1
