# Audio Recordings — MagicBand Reader

The master list of every voice/sound clip the device uses or will use. Work
against this when recording. Edit freely as features land.

## Format

- **Mono, 16-bit PCM WAV.**
- **Spoken clips: 16 kHz** (voice needs no more; ~30% smaller than 22050).
- **Musical stingers / chimes: 22050 Hz** (keep them crisp).
- Keep them short and trim leading/trailing silence — the silence-keeper in
  `audio.c` handles gaps.
- Deployed files live in `firmware/spiffs/` (rewards) and
  `firmware/spiffs/Program/` (prompts). Source/master takes live in
  `hardware/audio/`.

## Status legend

| Flag | Meaning |
|---|---|
| ✅ | Recorded **and** referenced by current firmware |
| ❌ | Referenced by firmware but **file missing** (plays nothing until recorded) |
| 🔵 | Planned — not wired into code yet (future stage) |

---

## 1. Boot & greetings

| File | Line / content | Flag | Notes |
|---|---|---|---|
| `/spiffs/operational.wav` | "All systems operational." | ✅ | `BOOT_SOUND_OPERATIONAL` — **standard-run** boot greeting (provisioned + normal power-on). Also in the random reward pool. |
| `/spiffs/walt-welcome.wav` | Walt's dedication | ✅ | `BOOT_SOUND_FIRSTRUN` — first boot / unprovisioned (followed by the setup prompt). Also stays in the random reward pool. *(Using the existing take for now.)* |
| `/spiffs/hello.wav` | everyday "Hello!" greeting | ✅ | `BOOT_SOUND` — fallback greeting when Wi-Fi is compiled out (`WIFI_ENABLE 0`); not used on the normal Wi-Fi build. |

**Quiet boot (intent-based, not time-based):** a **manifest-driven** update is self-initiated, so the reboot into the new image is **silent** — `ota_update_from_manifest` sets a one-shot `quiet_boot` NVS flag and the boot path skips the greeting (then clears it). Everything that means "unexpectedly online" still greets: power outage, unplug/replug, plain reboot, *and* manual firmware pushes (web upload, `ota-url`). *(Planned companion: a persistent "skip boot audio" toggle on the config page — see §7.)*

---

## 2. Reward / celebration sounds (the random pool)

These eight are the tap-reward pool, each mapped to an animation. All ✅.

| File | Animation |
|---|---|
| `/spiffs/chime.wav` | CELEBRATE |
| `/spiffs/excellent.wav` | FIREWORKS |
| `/spiffs/foolish.wav` | ENCHANTED |
| `/spiffs/hello.wav` | WELCOME |
| `/spiffs/operational.wav` | CELEBRATE |
| `/spiffs/startours.wav` | RAINBOW |
| `/spiffs/walt-welcome.wav` | WELCOME |
| `/spiffs/be-our-guest.wav` | BE-OUR-GUEST |

---

## 3. Program mode prompts

| File | Line / content | Flag |
|---|---|---|
| `/spiffs/Program/start-mode.wav` | "Program mode — pick a sound, then tap a card." | ✅ |
| `/spiffs/Program/saved.wav` | "Saved!" | ✅ |
| `/spiffs/Program/scan-now.wav` | "Scan it to try." | ✅ |
| `/spiffs/Program/all-done.wav` | "All done." | ✅ |
| `/spiffs/Program/try-again.wav` | "Try again." (read failed / storage full) | ✅ |
| `/spiffs/Program/random.wav` | "This card is now a surprise." (un-program → random) | ❌ **needs recording** |

---

## 4. Wi-Fi provisioning voice

Only `wifi-setup.wav` is wired today; the rest are candidates — record the ones
you want and we flip them on in code. `home` = plays when a join succeeds;
`not home` = plays when setup/AP mode comes up.

| File | Line / content | Flag | When |
|---|---|---|---|
| `/spiffs/Program/wifi-setup.wav` | "Starting setup. Connect your phone to the MagicMaker Setup network." | ✅ | first-boot setup (after the Walt welcome) |
| `/spiffs/Program/entering-setup.wav` | "Entering setup." | ✅ | button held through power-on |
| `/spiffs/Program/release-setup.wav` | "Release the button to enter setup." | ✅ | plays when the hold hits the threshold (ring turns blue) |
| `/spiffs/Program/browse-magicmaker.wav` | "Visit magicmaker.com on your phone's browser to continue setup." | ✅ | plays when a phone joins the setup AP |

> **SPIFFS gotcha:** object names cap at **32 chars including the `/Program/` path** (9 chars), so keep prompt filenames ≤ 23 chars or `spiffsgen` fails the build.
| `/spiffs/Program/wifi-connecting.wav` | "Connecting to Wi-Fi…" | 🔵 planned | at boot while joining (home) |
| `/spiffs/Program/wifi-online.wav` | "You're online!" | 🔵 planned | at boot, join succeeded (home) |
| `/spiffs/Program/wifi-failed.wav` | "Couldn't connect — starting setup." | ⚪ legacy | superseded by wifi-trouble (we no longer force setup on a failed join) |
| `/spiffs/Program/wifi-trouble.wav` | "I'm having trouble connecting to Wi-Fi. To change my setup, hold the button while powering me on." | 🔵 **wire-ready** | at boot, provisioned but join failed → stay offline + keep retrying (plays now if the file exists) |
| `/spiffs/Program/wifi-saved.wav` | "Got it — restarting to connect." | 🔵 planned | after saving creds on the portal page (optional; the page already says this on screen) |

---

## 5. Trip countdown  *(LOGIC CODED — just needs these recordings)*

The countdown engine is built and hardware-verified (`countdown.c`; test with the
`countdown` / `countdown <days>` CLI). It fires on the **first tap of the day per
band** (any UID; button = one pseudo-band), gated on the `countdown_enabled`
config + a synced clock. It stays silent until the clips below exist — drop them
in and they play; ship more later over the air (media OTA). Two layers were
requested — **milestone phrases** *and* **exact day count**; milestones are wired,
exact-count is the stretch (5b).

### 5a. Milestone phrases (the practical set)

**Multiple takes per tier:** name them `cd-<tier>-1.wav`, `cd-<tier>-2.wav`,
`cd-<tier>-3.wav`, … all in `/spiffs/Program/`. The device counts however many
exist and picks one at **random** each time (never the same one twice running for
a given band). Record **at least one** per tier to start; add variety anytime via
OTA. (A single un-numbered `cd-<tier>.wav` also works as a fallback.)

| Tier files | Line ideas | Trigger (nearest wins) | Animation |
|---|---|---|---|
| `cd-today-N.wav` | "Welcome to your Disney day!" | 0 days | fireworks |
| `cd-tomorrow-N.wav` | "Tomorrow is the big day!" | 1 day | fireworks |
| `cd-week-N.wav` | "One week to go!" | ≤ 7 days | rainbow |
| `cd-2weeks-N.wav` | "Two weeks until Disney!" | ≤ 14 days | be-our-guest |
| `cd-month-N.wav` | "Less than a month to go!" | ≤ 30 days | classic celebrate |
| `cd-faraway-N.wav` | "The magic is coming…" | > 30 days | warm gold welcome |
| `cd-after-N.wav` | "Hope your trip was magical!" | after the trip | enchanted (or disable) |

### 5b. Exact day count  *(stretch — concatenative "number bank")*

To speak an arbitrary count like "thirty-seven days until Disney," record a bank
of word clips and stitch them at runtime. Suggested lead-in + trailer:

- Lead-in: `cd-count-intro.wav` → "…days until Disney" trailer `cd-count-days.wav`
  ("day" singular: `cd-count-day.wav`).

**Number word bank** (record each once, mono/16 kHz):

- Ones/teens: `zero one two three four five six seven eight nine ten eleven
  twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen`
- Tens: `twenty thirty forty fifty sixty seventy eighty ninety`
- Hundreds: `one-hundred two-hundred three-hundred`
- Joiner: `and` (for "one hundred *and* five")

That covers 0–399, which is more than a year out. Runtime logic (later): decompose
the day count → play hundreds [+ "and"] → tens → ones → "days". Keep cadence tight
so the stitched line doesn't drag.

> Alternative if the word bank feels like too much: skip 5b and ship 5a only.
> The milestones alone read as polished.

---

## 6. OTA update voice  *(future — not coded yet)*

| File | Line / content | When |
|---|---|---|
| `/spiffs/Program/update-start.wav` | "Updating my magic — one moment…" | OTA begins |
| `/spiffs/Program/update-done.wav` | "All updated!" | first boot after a successful OTA |
| `/spiffs/Program/update-failed.wav` | "Update didn't take — I'm still working." | OTA failed (optional; can reuse `try-again`) |

---

## 7. Config-page settings  *(future — not coded yet)*

Not recordings, but tracked here so the idea isn't lost. A **persistent "skip
boot audio"** toggle on the config page would let the owner silence the power-on
greeting for good (distinct from the one-shot `quiet_boot` used by OTA). Pairs
naturally with the trip-date config the page already edits.

---

## Quick "what's missing right now" list

Referenced by current firmware but not yet recorded:

1. ❌ `Program/random.wav`
2. ❌ `Program/wifi-setup.wav`

Everything else above is either already recorded (✅) or intentionally future
(🔵). A missing file simply plays nothing — it never crashes the device.
