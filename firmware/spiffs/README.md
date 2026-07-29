# Data partition contents

Everything in this folder is packed into a LittleFS image and flashed to the
`storage` partition. It's what the device reads at runtime.

## What's here

- `www/` — the setup and band-manager web page, its favicon and logo. These are
  served straight off the filesystem, so the UI can be changed (or updated over
  the air) without rebuilding the firmware.

## What's deliberately missing: the audio

**No WAV files are committed** — `.gitignore` excludes them. Two reasons:

1. The countdown clips are a specific person's **voice**.
2. The original reward sounds come from a Disney-derived project and aren't ours
   to redistribute.

**The firmware runs fine without them.** A missing clip is treated as "skip", so
the device boots, reads bands, serves the web UI and keeps time with an empty
bank — it's just quiet. Add files and it gets its voice back, one pool at a time.

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
