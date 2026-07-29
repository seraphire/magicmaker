# MagicBand Reader — V1 Roadmap: Wi-Fi, NTP, and OTA

## Goal

Finish the first device for its recipients with a reliable foundation that
allows future software updates **without reopening the enclosure**.

V1 preserves all existing NFC, audio, and LED behavior while adding:

- Wi-Fi provisioning
- Recoverable Wi-Fi configuration
- NTP date/time synchronization (no RTC on the board)
- Configurable Disney trip date + countdown
- OTA firmware updates (internet **and** local web upload)
- A physical USB-C recovery path

Anything beyond this scope is deferred until after the device is delivered.

---

## 0. Starting facts (measured, not assumed)

These ground every sizing decision below. Confirmed against the current build:

| Fact | Value | Why it matters |
|---|---|---|
| App binary size | **~342 KB** | Tons of OTA headroom — even with Wi-Fi/TLS/HTTP/OTA added, expect ~1.2–1.5 MB. |
| Audio storage | **SPIFFS partition**, 1.7 MB used of 8 MB | Audio is **NOT compiled into the app** — it lives in the `storage` partition. |
| Flash | 16 MB | Comfortably fits two ~3 MB OTA slots **and** a 10 MB data partition. |
| PSRAM | 8 MB | Plenty for Wi-Fi + HTTP server + TLS buffers. |

> **Correction from the original draft:** earlier notes assumed audio was
> *embedded in the application*, forcing large OTA slots. It is not. The app is
> tiny and OTA on this device is comfortable, not tight.

---

## 1. Freeze the V1 Hardware

The current hardware is considered complete. Components:

- ESP32-S3 (N16R8: 16 MB flash / 8 MB PSRAM)
- RC522 RFID reader (SPI)
- WS2812 LED ring + Mickey face loop
- MAX98357A amplifier + speaker
- Volume potentiometer
- One rear pushbutton
- USB-C power and data (via Adafruit USB-C breakout, native USB)

No microphone, display, or extra sensors/controls are added in V1.

The external USB-C port is verified to provide serial/data access to the ESP32
with the enclosure assembled. This remains the **last-resort recovery path** if
networking and web upload both fail.

---

## 2. Preserve Existing Device Behavior

Must keep working, before and after the networking changes:

- Scan NFC/MagicBand UID
- Look up UID in device storage (NVS)
- Select the assigned sound + animation
- Play audio
- Run the corresponding LED animation
- Short press toggles the idle blue pulse
- Long press enters band-programming mode
- Volume knob controls playback level

MagicBands/cards remain **read-only**; UID→sound mappings are stored on the
ESP32 in NVS, never written to the tags.

---

## 3. Button Interaction

One rear button drives all physical configuration and recovery. Because the
modes are distinguished only by hold time, **the LEDs must give a visible cue at
each threshold** so the user knows what they are about to enter and can release
in time. (Audio announcements alone fire *after* entry — too late to abort.)

| Action | Hold | LED cue while holding | Result |
|---|---|---|---|
| Short press | tap | — | Toggle idle blue pulse |
| Program mode | ~3 s | Amber at 3 s | Enter band-programming mode (existing spoken prompt) |
| Wi-Fi setup | ~10 s | Blue at 10 s | Start setup AP + config page |
| Factory reset *(future)* | ~15–20 s | Red pulse at 15 s | Full reset — **only with spoken warning + confirm** |

**Wi-Fi setup** should:

1. Announce that Wi-Fi setup is starting.
2. Disconnect from the current network if connected.
3. Start a temporary access point.
4. Start the local configuration webpage.

Wi-Fi setup must **not** erase NFC mappings, audio, the trip date, or other
settings.

The future factory reset is intentionally not built yet — it needs a clear
spoken warning and a confirmation gesture first.

---

## 4. Persistent Configuration Storage (NVS)

Store configuration in NVS. Suggested shape (individual keys are fine — often
easier to migrate than one packed struct):

```c
typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];

    char device_name[33];
    char ota_manifest_url[256];

    int  trip_year;
    int  trip_month;
    int  trip_day;

    bool countdown_enabled;
    bool idle_led_enabled;

    uint32_t config_version;   // bump to migrate older settings
} device_config_t;
```

Rules:

- **Never** erase NFC mappings when resetting only Wi-Fi.
- Do not store temporary runtime state in persistent config.
- Include `config_version` so future firmware can migrate old settings.
- Provide compiled-in defaults for **every** setting.
- An invalid saved setting must fall back safely, never block startup.

> **NVS sizing:** the current table gives NVS only 24 KB. With ~100 card
> enrollments *plus* Wi-Fi creds and config, that gets tight. Bump NVS to
> **64 KB** during the partition migration (§9) while the table is changing
> anyway. OTA updates never touch NVS, so enrollments survive every future
> update.

---

## 5. Wi-Fi Setup Mode

When Wi-Fi credentials are absent or unusable, run as a temporary access point.

- Suggested AP name: `MagicBand-Setup` (a per-device suffix like
  `MagicBand-Setup-001` can be added later).

Initial setup page fields:

- Wi-Fi network name
- Wi-Fi password
- Trip date
- Countdown enabled/disabled
- Device name
- **Save and connect** button

Keep the page simple and phone-friendly.

**Setup flow:**

1. User joins the device's temporary Wi-Fi network.
2. The config page opens automatically (captive portal — see below).
3. User enters home Wi-Fi credentials + trip date.
4. Device saves to NVS.
5. Device attempts to join the home network.
6. Device reports success, or returns to setup mode on failure.

### Captive portal (in 1.0)

The recipients are non-technical, and the main recovery scenario (changed home
Wi-Fi password) happens months later when nobody's there to help. Requiring them
to manually browse to `192.168.4.1` would make recovery theoretical, not real.
So the captive portal is **core provisioning, not polish** — built as three
tiers, floor first:

1. **Typed IP (floor, always works).** `192.168.4.1` reaches the page no matter
   what.
2. **DNS catch-all (the guarantee).** A tiny DNS server resolves *every*
   hostname to `192.168.4.1`, so anything the user types (`google.com`,
   anything) lands on the setup page. Rock-solid, ~30 lines, no OS-specific
   quirks. **This is the piece that makes it usable.**
3. **OS auto-popup (the upgrade).** Because the DNS catch-all already routes the
   OS connectivity-probe hostnames to us, we only need HTTP handlers that answer
   those probes so the phone auto-opens the page on join. Known probe endpoints
   to handle:
   - Apple/iOS: `captive.apple.com` (expects a specific `Success` page; return
     a redirect instead to trigger the sheet)
   - Android: `connectivitycheck.gstatic.com/generate_204`,
     `www.google.com/generate_204` (return non-204 to trigger)
   - Windows: `www.msftconnecttest.com/connecttest.txt`,
     `dns.msftncsi.com`
   - Firefox: `detectportal.firefox.com/canonical.html`

   Adding these handlers is cheap; the honest caveat is that auto-popup
   *reliability* still varies by OS/version/MDM. That variability is why it's the
   top tier — but the catch-all underneath means the user always gets to the page
   regardless.

---

## 6. Normal Startup Sequence

1. Initialize hardware + existing application services.
2. Load persistent configuration from NVS.
3. Play the normal startup greeting.
4. If no Wi-Fi credentials exist → enter setup mode.
5. Attempt to connect to the configured Wi-Fi (allow ~10–20 s).
6. On failure → announce it, enter setup mode.
7. On success → synchronize time via NTP.
8. Check for a firmware update.
9. Calculate the Disney countdown.
10. Enter normal NFC scanning mode.

**A network or server failure must never prevent scanning from starting.** The
scanner still operates if NTP fails, the update server is down, or no new
firmware exists.

---

## 7. NTP Time Synchronization

No RTC on the board, so time is lost on every power cycle and re-synced at boot.
Use public NTP servers rather than trusting the local router:

- `pool.ntp.org`
- `time.google.com`
- `time.cloudflare.com`

Steps:

1. Connect to Wi-Fi.
2. Start SNTP.
3. Wait for a valid time (reasonable timeout).
4. Retain the synchronized system time.
5. Continue startup even if sync fails.

**Order matters:** NTP must succeed *before* any HTTPS OTA check, so TLS
certificate validation has a valid clock. *(Bring-up note: in practice ESP-IDF's
mbedtls has cert **date**-checking OFF by default (`CONFIG_MBEDTLS_HAVE_TIME_DATE`),
so HTTPS/OTA works even without a synced clock — the ordering is still nice but
not strictly required.)*

**Time zone:** the countdown uses local time — initially US Eastern. Account for
daylight-saving via a POSIX TZ string, not a fixed UTC offset.

### Hardware bring-up findings (2026-07-24)

- **SNTP = NTP over UDP 123.** SNTP is just the lightweight NTP client; same
  port. Some networks block it (the test network here does).
- **HTTP-Date fallback added.** When SNTP times out, read the `Date:` header of
  an HTTPS HEAD (port 443) and set the clock from it (~1 s accuracy). Verified
  working — clock came up correct in EDT. Design is SNTP-first (accurate on open
  nets), HTTP-Date fallback (works wherever HTTPS does).
- **NTS considered, rejected for this problem.** Cloudflare NTS authenticates
  time but its transfer is still NTP over UDP 123 — it doesn't traverse the
  firewall, and ESP-IDF's SNTP has no NTS support. Revisit only if we later need
  *authenticated* time (TLS date-checking, signed-OTA anti-rollback).

### Flagged refinements (not doing now)

- **Time-sync blocks boot.** `ntp_sync()` runs synchronously in `provision()`,
  so on a firewalled network the SNTP timeout (~6 s) delays "ready to scan."
  Fix later: move time-sync to a background task, or shorten the timeout.
- **Captive-portal auto-popup didn't fire** on the test phone (had to type
  `192.168.4.1`). The DNS catch-all floor works; the OS-probe tier (§5) needs a
  look. Not blocking — the page is always one typed address away.

---

## 8. Disney Trip Countdown

Store the trip date in **config**, never a `#define`. Initial target:
**January 20, 2027**.

On startup or scan, compute calendar days remaining. Possible tone:

- \> 30 days: occasional calm countdown message
- ≤ 30 days: more enthusiastic
- ≤ 7 days: special animation/audio
- 1 day: "Tomorrow is the big day"
- 0 days: trip-day celebration
- After the trip: disable, or play a completed-trip response

The countdown need not play on every scan. It can be limited to: first scan
after startup, first scan of the day, a random subset, a specific assigned band,
or a config option. **V1 can simply play it once after startup or on the first
scan.**

---

## 9. OTA Partition Layout

Switch the partition table from a single `factory` app to A/B OTA slots.

**Proposed table (16 MB, grounded in the ~342 KB app):**

| Name | Type | SubType | Size | Notes |
|---|---|---|---|---|
| `nvs` | data | nvs | 0x10000 (64 KB) | bumped from 24 KB (see §4) |
| `otadata` | data | ota | 0x2000 | A/B boot selection |
| `phy_init` | data | phy | 0x1000 | |
| `ota_0` | app | ota_0 | 0x300000 (3 MB) | huge headroom vs. 342 KB app |
| `ota_1` | app | ota_1 | 0x300000 (3 MB) | |
| `storage` | data | spiffs | ~0x9EB000 (~10 MB) | bigger than today's 8 MB |

> **⚠ One-time cost — do this before final card enrollment.** Changing the
> partition layout requires **one flash over USB**, and that reflash **wipes
> NVS** (the enrolled cards). So migrate to the OTA layout *first*, then enroll
> the cards. After that, every future update rides OTA and NVS is preserved.

The application must check partition sizes before attempting an update.

---

## 10. OTA Manifest

One small manifest describes the available firmware.

**V1 needs only:**

```json
{
  "version": "1.0.1",
  "firmware_url": "https://example.com/magicband/firmware-1.0.1.bin"
}
```

**Future fields (optional):**

```json
{
  "version": "1.0.1",
  "firmware_url": "https://example.com/magicband/firmware-1.0.1.bin",
  "minimum_version": "1.0.0",
  "sha256": "...",
  "release_notes": "Adds Wi-Fi recovery and countdown improvements",
  "channel": "stable"
}
```

The device: requests the manifest → parses the version → compares with the
running firmware → begins OTA only when the manifest is **newer**.

Version comparison must be **semantic/numeric**, not string compare — `1.10.0`
must be treated as newer than `1.9.0`.

---

## 11. Recoverable Manifest URL

- Compile a **default** manifest URL into the firmware, e.g.
  `https://example.github.io/magicband-updates/manifest.json`.
- Store an optional **override** URL in NVS.
- Use the override when present, else the compiled-in default.

A **developer-only** section of the config page exposes:

- Current firmware version
- Current manifest URL
- Manifest URL override
- Reset URL to firmware default
- Check for update now

Because changing the URL allows arbitrary firmware installation, this section
must **not** appear in the ordinary user-facing page. For this two-device pilot,
gate it by requiring the rear button to be held while opening the developer
page. This keeps the update host movable without making the device permanently
dependent on one account/provider.

---

## 12. Local Firmware Upload — Web-Based Recovery OTA

**New in this revision. Recommended as a first-class V1 feature.**

In addition to pulling firmware from the internet, the device's own config page
accepts a firmware `.bin` **uploaded directly from the phone/browser** and
writes it to the inactive OTA slot. This reuses the same `esp_ota_begin/write/
end` machinery as internet OTA — it's actually *simpler* because it needs no
DNS, no TLS, no manifest, and no valid clock.

**Why it's worth it — a tiered recovery ladder:**

1. Internet OTA (automatic, from the manifest).
2. **If that fails:** email the owner a `.bin`; they hold the dev-page button and drags
   the file into the setup page. No PC tools, enclosure stays sealed.
3. **If even that fails:** USB reflash (the un-brickable floor).

**Implementation notes:**

- Serve the upload behind the same button-gated developer section as §11.
- **Stream** the POST body straight to `esp_ota_write` in chunks — never buffer
  the whole ~1.5 MB image in RAM.
- Validate it's a real ESP32 app image (ESP-IDF OTA validation) before switching
  the boot partition.
- Same rollback protection as §15 applies.

Build this alongside Stage 5 (§17) — once the OTA partition machinery exists,
the local-upload path is a small addition on top.

---

## 13. OTA Runtime Behavior

For V1, check for updates:

- Once during startup, **after** NTP succeeds.
- When the user selects "Check for update now" on the developer page.

Do **not** poll throughout the day.

The update process should:

- Avoid starting while audio is playing or an NFC operation is active.
- Announce that an update is beginning.
- Set the LEDs to a distinct "updating" state.
- Prevent normal scans during flash writing.
- Reboot automatically after success.
- Play a success/startup message once the new firmware boots.

If the check fails → log and continue normally. If the download fails → keep the
running firmware and continue normally.

---

## 14. Initial OTA Hosting

Keep the source repo private if desired; host only the compiled artifacts
publicly:

- `manifest.json`
- `firmware-1.0.0.bin`
- `firmware-1.0.1.bin`

Options: GitHub Pages, GitHub Releases, Cloudflare Pages, Cloudflare R2, or any
static HTTPS host. No custom API or dashboard needed; the ESP32 only needs
outbound HTTPS. (And with §12, even a total hosting outage is recoverable.)

---

## 15. Security Scope for V1

Minimum protections:

- Use HTTPS for the internet update path.
- Validate the server certificate; **never** disable TLS verification in the
  shipping build.
- Do not expose Wi-Fi passwords on any status page.
- Keep the manifest URL + local-upload behind the developer-only section.
- Require physical access (button-gated dev page) for developer config.
- Validate that any downloaded/uploaded file is a real ESP32 app image; use
  ESP-IDF OTA image validation.

**Signed firmware vs. Secure Boot — an important distinction:**

- **App signature checking on OTA images: safe to add later, no downside.** This
  lets the manifest + binary stay publicly hosted while blocking substituted
  firmware. Design the update system so signing can be dropped in later.
- **Full Secure Boot / flash encryption burns eFuses — irreversible, and must
  be decided before sealing.** For a two-device gift, **do not enable Secure
  Boot.** Nobody should burn a fuse thinking it can be undone.

Do not delay the first working OTA prototype to implement every advanced
security feature — just don't design yourself into a corner.

---

## 16. Audio Storage & Format

> Recording checklist (every clip the device uses or will use, flagged
> used/missing/planned): **[audio-recordings.md](audio-recordings.md)**.


**V1: stay with WAV.** Current audio is mono 16-bit PCM WAV in SPIFFS, streamed
straight to I2S. It is glitch-free with the silence-keeper pattern, needs zero
decode CPU, and is only 1.7 MB of an 8 MB partition — you are **not** space-
constrained yet.

**Do not** introduce an MP3/compressed pipeline as part of the OTA milestone:

- It requires a decoder component (more code + CPU).
- It risks reintroducing the crackle/underrun already solved in `audio.c`.
- §16's own rule: don't combine the OTA milestone with an audio-storage rewrite.

**Cheap win available now, no new code:** voice prompts don't need 22050 Hz —
re-export spoken lines at **16 kHz mono** to trim ~30% with no audible loss.

**V2 (with LittleFS content packs):** *then* move to a compressed codec (MP3 via
minimp3/libhelix is simplest to produce and the S3 + PSRAM decode it easily).
That is the right home for personalized greetings, uploaded names, seasonal
audio, and holiday packs — kept as a **separate concept from firmware OTA**.

---

## 17. Implement in Stages

Do not build the whole pipeline at once.

- **Stage 1 — Reliable Wi-Fi + captive portal:** save creds, connect, handle
  failure, re-enter setup mode, verify reconnect after reboot. Includes the
  SoftAP config page, the **DNS catch-all**, and the **OS-probe handlers** (§5) —
  the catch-all is required, the auto-popup is best-effort.
- **Stage 2 — NTP:** resolve NTP names, sync time, apply time zone, confirm
  countdown date math.
- **Stage 3 — Basic HTTPS download:** fetch a small text file, print to serial.
  Verifies DNS, connectivity, HTTPS, TLS certs, valid clock, HTTP handling.
- **Stage 4 — Manifest:** download + parse JSON; print running vs. available
  version and whether an update is required. **Do not install yet.**
- **Stage 5 — OTA install:** ESP-IDF HTTPS OTA — download → write inactive slot
  → validate → select next boot → reboot. **Build §12 local upload here too.**
- **Stage 6 — Rollback:** enable rollback; new app boots, inits critical
  hardware, confirms itself, marks valid. If it can't init, the bootloader
  returns to the previous image.

**Recommended first move:** Stage 1 (Wi-Fi provisioning + recovery) **plus** the
§9 partition migration. Those two are the foundation everything else sits on, and
the partition change is the only thing that touches the already-working build.
NTP/countdown and OTA then layer on with no further flash-layout churn.

---

## 18. Testing Checklist

**Existing functionality**
- [ ] Every registered UID still triggers the correct audio.
- [ ] Every registered UID still triggers the correct animation.
- [ ] Unknown UIDs behave correctly.
- [ ] Volume control works.
- [ ] Short press toggles the idle LED.
- [ ] Band-programming mode still works.

**Wi-Fi provisioning**
- [ ] Fresh device starts setup mode.
- [ ] Phone can connect to the setup AP.
- [ ] Config page loads at `192.168.4.1` (floor).
- [ ] DNS catch-all: any typed hostname lands on the setup page.
- [ ] OS auto-popup fires on join (iOS / Android / Windows — best-effort).
- [ ] Credentials save correctly.
- [ ] Device connects after configuration.
- [ ] Incorrect password returns to setup mode.
- [ ] Changed home Wi-Fi recoverable with the 10-second hold.
- [ ] Wi-Fi reset does not erase band mappings.

**NTP and countdown**
- [ ] NTP synchronizes successfully.
- [ ] Correct local date is calculated.
- [ ] Countdown correct before and after midnight.
- [ ] Countdown handles trip day correctly.
- [ ] Device still scans when NTP fails.

**OTA (internet + local upload)**
- [ ] Device retrieves the manifest.
- [ ] Same-version firmware does not trigger an update.
- [ ] Newer firmware triggers an update.
- [ ] Firmware downloads into the inactive partition.
- [ ] Device reboots into the new firmware.
- [ ] New firmware marks itself valid.
- [ ] Interrupted download does not damage the running firmware.
- [ ] Unavailable manifest server does not block startup.
- [ ] Invalid firmware image is rejected.
- [ ] Manifest URL override works; reset-to-default works.
- [ ] **Local web upload writes and boots a valid image.**
- [ ] **Local web upload rejects an invalid image.**

**Physical recovery**
- [ ] External USB-C port enumerates on the computer.
- [ ] Serial monitor works through the assembled enclosure.
- [ ] Firmware can be flashed through the external USB-C port.
- [ ] Device operates normally after USB recovery flashing.

---

## 19. Definition of Done

V1 is complete when:

- The recipients can configure their Wi-Fi from a phone.
- The device recovers from wrong/changed Wi-Fi credentials without opening the
  enclosure.
- The device syncs the current date and calculates the Disney countdown.
- The device checks a hosted manifest for a newer firmware version.
- The device installs and boots a valid OTA image — **from the internet or a
  local web upload.**
- A failed update does not destroy the working firmware.
- Existing NFC, audio, LED, button, and volume functions remain stable.
- The external USB-C connection remains available as a recovery path.
- The device can be sealed, shipped, and updated later without physical access.

---

## Deferred Until After Delivery

Intentionally outside V1:

- Device status/check-in API
- Remote command service
- Bluetooth presence / phone proximity features
- Browser microphone recording
- Personalized uploaded greetings
- Separate downloadable audio packs / seasonal scheduling
- Firmware signing + full Secure Boot
- Alexa / Google Assistant / Spotify integration
- Home Assistant integration
- Additional physical controls or sensors
- **OEM / pre-provisioning** (see below)

These stay possible because V1 establishes Wi-Fi, persistent configuration,
LittleFS planning, and OTA updates.

### OEM / pre-provisioning  *(future idea)*

Ship a unit that's ready-to-gift out of the box, configured before it ever
powers on for the recipient. Because everything already routes through `appcfg`
(NVS) with compiled-in defaults, this is mostly a matter of *seeding* those
values at flash time rather than new runtime code. A pre-configured unit could
carry:

- a custom **device name** (e.g. "<name>'s MagicMaker") and recipient/user names
- a **theme / brand skin** — the loadable "edition" idea (Disney, Halloween,
  birthday): default sounds, animation palette, greetings, maybe a logo on the
  setup page
- a pre-set **trip date / countdown**
- optionally pre-seeded Wi-Fi (only sensible when we control the destination
  network)

Likely mechanisms (decide later): a build-time config profile compiled into the
defaults, an NVS partition image flashed alongside the app, or a one-time
"provisioning file" the device ingests on first boot. Theming pairs naturally
with the LittleFS content-pack work (§16) — a theme is essentially a content
pack plus a few settings. Keep it a nice-to-have; nothing in V1 depends on it.

### Data-driven animations  *(future idea)*

Today the six animations are **procedural C** in `leds.c` (comet math, sine
breathe, random sparkle). A recurring thought is to move them out of code into
**data** that a runtime "player" renders onto the LEDs. Worth capturing why —
and why the motivation is *not* memory:

- **It would not save memory; likely the opposite.** The procedural code is
  tiny (~5-15 KB for all six). Pre-rendered frame data (RGB per pixel per frame)
  is far bigger — one ~4.5 s animation at 50 fps is ~54 KB (`81 px x 3 B x 225
  frames`), so six ≈ ~325 KB to replace ~10 KB of code. And we're not
  constrained anyway (~2 MB free in the app slot, ~8 MB free in SPIFFS).
- **So never pre-bake frames.** If we do this, use a **compact animation DSL /
  keyframe script** (e.g. "green comet, 2 laps, speed X" -> "fade ring to green
  over N frames + sparkle face") interpreted by a small player. That's a few
  hundred bytes per animation, but it needs an interpreter whose fixed cost only
  pays off across *many* animations.
- **The real payoff is updatability + theming, not size:** ship new looks as
  data files via OTA content (no firmware reflash); let a **theme** carry its
  own animations (Disney vs Halloween editions); allow authoring without a
  compile (a future web UI or JSON).

Conclusion: this belongs to the LittleFS content-pack / theming stage (§16), as
a companion to per-theme sounds and settings — a theme = sounds + palette +
**animations** + settings. Not a V1 concern; the procedural animations work and
are cheap.

### Near-term hardening: non-blocking boot + periodic housekeeping  *(next up)*

Two related gaps surfaced during real-world testing (unit run upstairs, on a
weak USB-A supply, between two APs):

- **Non-blocking boot.** Today `provision()` runs in `app_main` *before* the main
  loop, so `wifi_connect_sta` (up to 15 s) and `ntp_sync` (up to 6 s) **block the
  device from becoming responsive** — on weak Wi-Fi it's "slow to come up," and
  the LED idle loop isn't animating yet. Fix: move Wi-Fi join + NTP + the LAN
  server onto a background task so the device is interactive (taps, animation)
  **immediately**, and connectivity fills in when it's ready.
- **Periodic time re-sync (drift).** NTP is a **one-shot at boot**; with no RTC
  the clock free-runs on the crystal and drifts over days. Add a background
  re-sync every few hours. Note this network **blocks NTP (UDP 123)**, so the
  re-sync must also drive the HTTP-Date fallback, not just SNTP.

Both belong to a single **periodic "housekeeping" task** that also carries the
deferred **scheduled OTA check** and the **notification poll** (below) - one
timer, three jobs (re-sync time, check for updates, check for notifications),
all respecting the quiet rules.

### Notification glow + pushed messages  *(future idea, next release-ish)*

A "you have something waiting" light. When a **notification** is pending, the
idle glow switches from its calm **blue** breathe to an attention-getting
**yellow/gold** breathe (reuse the warm-gold palette already in `anim_welcome`).
Tapping the band (or a card) plays the notification's message + a little
animation, then the glow returns to blue. Persist the pending notification in
NVS so a power cycle doesn't lose it; auto-clear once heard.

The nice realization: the glow is just the **surfacing layer**, and *two*
sources can feed it —

1. **Local countdown milestones (§8).** The device already has NTP time + the
   trip date, so it can compute days-to-go and raise a milestone notification
   ("One week to go!" -> `cd-week.wav`) on its own. The glow is how the
   countdown *announces itself* without nagging — it waits quietly in yellow
   until the recipient taps to hear it.
2. **Remote push.** Let us "push" an arbitrary message like *"It's one week til
   Disney!"* without a firmware update. Cleanest mechanism: **piggyback on the
   OTA manifest poll** — add an optional `notification` object to the manifest
   JSON, e.g. `{"id": "...", "text": "...", "sound": "..."}`. When the scheduled
   poll sees a new `id`, it stores the notification and lights the glow yellow.
   No new server infrastructure — it rides the channel we already built.

Design notes for when we build it:
- **Quiet rules still apply:** picking up a pushed notification must NOT make
  noise on its own (same spirit as `quiet_boot`). It lights the glow silently;
  sound only plays when the recipient taps. A 3 a.m. push just means a yellow
  glow waiting in the morning.
- **De-dupe by `id`** so the same push isn't re-raised every poll (same pattern
  as `version_cmp` gating OTA installs).
- **Sound source:** a pushed notification can name a sound already in SPIFFS, or
  (later, with content packs / §16) ship a small audio clip alongside the push.
- Depends on: the countdown logic (§8, deferred) for source #1, and the
  scheduled manifest poll (deferred) for source #2. The glow + NVS state + tap-
  to-hear is the small new piece that unifies them.
