# Data partition contents

Everything in this folder is packed into a LittleFS image and flashed to the
`storage` partition. It's what the device reads at runtime.

## What's here

- `www/` — the setup and band-manager web page, its favicon and logo. These are
  served straight off the filesystem, so the UI can be changed (or updated over
  the air) without rebuilding the firmware.

## What's deliberately missing: the audio

**No WAV files are committed** — `.gitignore` excludes them. They split into two
groups with different answers:

**Reward sounds** (partition root) — `chime.wav`, `excellent.wav`, `foolish.wav`,
`hello.wav`, `operational.wav`, `startours.wav`, `walt-welcome.wav`,
`be-our-guest.wav`. These came from the
[Adafruit Magic Band Reader](https://learn.adafruit.com/magic-band-reader)
project — **download them from that guide.** They're park audio bundled with it,
so they aren't redistributed here. The list lives in
[`main/sounds.c`](../main/sounds.c); edit it to match whatever you have.

**Countdown clips (`cd/`) and prompts (`Program/`)** — recorded by the project's
author, so they're one person's voice. Record your own; that's most of the charm.

**The firmware runs fine without any of it.** A missing clip is treated as
"skip", so the device boots, reads bands, serves the web UI and keeps time with
an empty bank — it's just quiet. Add files and it finds its voice, one pool at a
time.

See the main [README](../../README.md) for the file names each pool expects, and
[`docs/countdown-recording-script.md`](../../docs/countdown-recording-script.md)
for the recording script.

### Format

**22050 Hz, mono, 16-bit PCM WAV.**

Mixed sample rates work, but the I2S clock has to be retuned between clips, and
that retune is audible as a tick in the middle of a composed sentence. Keeping
everything at one rate avoids it.

### Layout

```
spiffs/
├── www/                 web UI (committed)
├── cd/                  countdown clip bank        (yours)
├── Program/             spoken prompts for setup / program mode  (yours)
└── *.wav                the reward sounds a band can be assigned (yours)
```

## Note on over-the-air asset updates

The firmware can fetch individual files into this partition at runtime, which is
how new sounds reach a device that's already been given away.

That needs the files to be hosted somewhere **publicly readable** — the device
has no way to authenticate. Since this repository doesn't carry audio, OTA here
ships **firmware only** out of the box. If you host your own audio somewhere,
point the manifest's `assets.base_url` at it; see
[`docs/ota-setup.md`](../../docs/ota-setup.md).
