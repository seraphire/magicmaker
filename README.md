# MagicMaker

A tap-point for theme-park style RFID bands, built as a countdown gift. Tap a
band and it plays a sound, runs a light show — and tells you, out loud, how long
is left until the trip.

> ### ⚠️ Alpha — public so the device can update itself
>
> This repo is public because the device fetches its firmware manifest over
> HTTPS, and that needs somewhere publicly readable to fetch it *from*. It isn't
> public because it's finished.
>
> It's under daily revision and **stored data is not stable yet**: NVS layouts,
> enrolled bands and saved settings may be changed destructively between
> commits, and an update can reset them. That freedom ends before the device is
> given away — after that, configuration has to survive updates — but until then
> the better design wins over the compatible one.
>
> By all means build one, read it, take ideas from it. Just don't expect a
> pinned version, a migration path, or a stable API yet.

The countdown isn't a stack of pre-rendered clips. It's **assembled at play
time** from a bank of recorded words:

```
[warm-up music]  "It's only"  "twenty three"  "days"  "until our Disney trip!"
```

Every number 1–31 is recorded as one natural phrase, and the unit steps up to
weeks and then months as the date gets further out — so a number is never
stitched together mid-word, and it never says "one hundred fifty two". As the
trip approaches it tightens from months, to weeks, to counting every single day,
until it's shouting *"Today's the day!"* over fireworks.

Tap the same band a few times in a row and it gets a little cheeky about it.

---

## What it does

- **Tap to play** — an RC522 reads a 13.56 MHz band/card; each one can be given
  its own sound and animation, or set to surprise you.
- **Spoken countdown** — composes a sentence from a clip bank; tiers by
  today / tomorrow / days / weeks / months, with landmark lines at a week and a
  month out, and a set of "after the trip" lines so it never goes quiet.
- **Repeat-tap comebacks** — tapping one band repeatedly earns a random retort.
- **Web UI** on the local network: name your bands, pick their sounds, set the
  trip date, control per-band countdown behaviour, reboot the device.
- **Phone setup** — first boot raises a Wi-Fi access point with a captive portal.
- **Over-the-air updates** — firmware *and* audio, hash-verified, with automatic
  rollback if an image doesn't boot.
- **Keeps itself honest** — re-syncs the clock every 6 hours (no RTC, so it
  drifts), reconnects to Wi-Fi indefinitely, and re-checks for updates.

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 (N16R8) | 16 MB flash — the partition layout assumes it |
| **RC522 RFID reader (mini)** | SPI, 13.56 MHz / ISO 14443A. The *mini* board is the one that fits — [Amazon](https://a.co/d/8RjPJ3M) · [AliExpress](https://a.aliexpress.com/_mMmHC4V) |
| MAX98357A | I2S amp, mono |
| WS2812 / NeoPixel | 81 px here: a 45-px ring + 36-px face loop, one data line |
| 10 k potentiometer | volume (optional) |
| Momentary button | program mode / setup |

Pins, counts and behaviour all live in [`firmware/main/config.h`](firmware/main/config.h),
and the wiring is in [`docs/wiring-magicband.xlsx`](docs/wiring-magicband.xlsx).

**Enclosure:** [Walt Disney World Inspired MagicBand Reader](https://makerworld.com/en/models/2020419-walt-disney-world-inspired-magicband-reader)
on MakerWorld. Print notes and the mistakes worth avoiding are in
[`docs/hardware-rev-notes.md`](docs/hardware-rev-notes.md).

**Cards:** any ISO 14443A card works, but **MIFARE Classic 1K** is the best fit —
its 4-byte UID is read whole. 7-byte-UID cards (NTAG21x) work, but this reader
only takes the first 4 bytes. Avoid 125 kHz cards entirely; the RC522 can't see
them.

## Build

Needs **ESP-IDF v5.5.x**.

```bash
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
idf.py -C firmware -p <PORT> flash
```

`firmware/setup.ps1` sets up the toolchain on Windows.

## 🔊 About the audio

**No audio files are included in this repository**, and the device will be silent
until you add some. That's deliberate — and each kind has a different answer:

| Sounds | Where they come from |
|---|---|
| **Reward sounds** a band can be assigned — `chime`, `excellent`, `foolish`, `hello`, `operational`, `startours`, `walt-welcome`, `be-our-guest` | The [Adafruit Magic Band Reader](https://learn.adafruit.com/magic-band-reader) project — **download them from there.** They're park audio bundled with that guide, so they're not redistributed here. |
| **Countdown clips** (`cd/`) and **spoken prompts** (`Program/`) | Recorded by you. The originals are one specific person's voice, so you'll want your own anyway — arguably the best part of the build. |

Everything is designed around that — the firmware treats a missing clip as
"skip", so it runs fine with a partial bank and gets richer as you add files.
Grab the Adafruit sounds and it has reward audio immediately; record the
countdown bank whenever you like.

To give it a voice, put mono 16-bit WAVs into **`assets/audio-src/`** and run
`tools\build-audio.ps1`, which encodes and loudness-matches them into the data
partition. (Not into `firmware/spiffs/` — that's build output, and anything
placed there directly is pruned on the next build.)

| Path | What |
|---|---|
| `cd/1.wav` … `cd/31.wav` | the numbers, each a whole natural phrase |
| `cd/day.wav` `days` `week` `weeks` `month` `months` | unit words |
| `cd/today-N.wav` `cd/tomorrow-N.wav` | full announcements for the big days |
| `cd/lead-N.wav` | optional openers — *"it's only"*, *"guess what"* |
| `cd/tail-N.wav` | optional closers — *"to go!"*, *"until the castle"* |
| `cd/cheeky-N.wav` | repeat-tap comebacks |
| `cd/after-N.wav` | after the trip has passed |
| `cd/preamble.wav` | warm-up music before the count |
| `chime.wav`, `excellent.wav`, … (partition root) | the reward sounds — from the [Adafruit guide](https://learn.adafruit.com/magic-band-reader) |

The reward-sound list is defined in
[`firmware/main/sounds.c`](firmware/main/sounds.c) — edit it to use whatever
files you actually have, and the web UI picks up the change.

Pools are **data-driven**: the firmware probes `lead-1`, `lead-2`, … until one is
missing, so adding variety is just adding files — no code change, and it can be
shipped over the air.

The `.wav` in those paths is a *logical* name. Clips ship as MP3 (8.1 MB of
masters become 1.58 MB, taking the partition from 84% to 16% used), and the
firmware tries the other extension automatically — so `cd/lead-1.wav` finds
`cd/lead-1.mp3` and nothing in the table above has to change.

[`docs/countdown-recording-script.md`](docs/countdown-recording-script.md) is the
script used to record the original bank, with the reasoning behind the ordering,
and there's a browser-based splitter for cutting one long take into clips.

## Documentation

| Doc | About |
|---|---|
| [ota-setup.md](docs/ota-setup.md) | hosting updates on GitHub |
| [media-ota.md](docs/media-ota.md) | the manifest format |
| [countdown-recording-script.md](docs/countdown-recording-script.md) | recording the clip bank |
| [countdown-lines.md](docs/countdown-lines.md) | what's in each pool, and line ideas |
| [roadmap-wifi-ota.md](docs/roadmap-wifi-ota.md) | how the networking stack was built |
| [hardware-rev-notes.md](docs/hardware-rev-notes.md) | assembly lessons the hard way |

## Credits

- **[Adafruit Magic Band Reader](https://learn.adafruit.com/magic-band-reader)** —
  the project this grew out of. The reward sounds come from that guide (download
  them there), and its CircuitPython logic was the starting point before this was
  rewritten in C on ESP-IDF.
- **Enclosure:** [Walt Disney World Inspired MagicBand Reader](https://makerworld.com/en/models/2020419-walt-disney-world-inspired-magicband-reader)
  on MakerWorld, by its original author — print it from there; no model files are
  redistributed here.
- Standing on: [ESP-IDF](https://github.com/espressif/esp-idf),
  [littlefs](https://github.com/littlefs-project/littlefs),
  and Espressif's `led_strip` and `mdns` components.

## Legal

Not affiliated with, endorsed by, or connected to The Walt Disney Company. No
Disney artwork, audio, or trademarks are included in this repository. It's a
hobby project that happens to count down to a family holiday.

## License

[MIT](LICENSE) — for the code in this repository. Any audio, artwork or 3D
models you supply are your own and are not covered by it.
