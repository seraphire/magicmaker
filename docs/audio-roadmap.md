# Audio

How sound gets from a recording onto the device, what's already done, and what's
still open.

## The pipeline

```
assets/audio-src/        masters, 16-bit PCM WAV, never modified
        |
        |  tools/build-audio.ps1
        v
firmware/spiffs/         build output - encoded, levelled, pruned
        |
        |  idf.py build   (LittleFS image)
        v
storage partition
```

**`firmware/spiffs` is generated.** Delete the audio in it any time; the script
puts it back. Only `www/` is real source and is never touched.

```
tools\build-audio.ps1              # build what changed
tools\build-audio.ps1 -WhatIf      # show what would happen, touch nothing
tools\build-audio.ps1 -Force       # rebuild everything
tools\build-audio.ps1 -NoNormalize # leave levels exactly as recorded
```

Clips whose source has been deleted are pruned from the output, so retiring a
recording is just deleting the master.

### What the script does

**Encodes to MP3.** 8.1 MB of masters ship as 1.58 MB, taking the storage
partition from 84% used to 16%.

**Keeps each file's own sample rate** — except `cd/`, which is pinned. Only
`cd/` gets concatenated, and retuning the I2S clock mid-sentence is audible as a
tick, so those clips have to agree with each other. Everything else plays
standalone and keeps what was recorded; forcing a rate on those only throws away
bandwidth.

**Loudness-matches everything** to −16 LUFS with a −3 dBTP ceiling (EBU R128,
two-pass, `linear=true` so it's one constant gain per file — nothing is
compressed or pumped). This matters more than it sounds:

- Recordings made months apart, or by different people, otherwise never sit
  together. Measured spread before this existed: 7.6 LUFS across the prompts,
  15.7 across the countdown bank.
- It's worst in `cd/`, where clips are butted together to build one sentence, so
  a level step lands *between words*.
- The true-peak ceiling also prevents clipping. A lossy round trip overshoots
  the original peaks by 2–3 dB on transient speech, so a source sitting at
  0 dBFS decodes past full scale. Measured on one clean recording: 0 dB in gave
  728 clipped samples out, −1 dB gave 450, −3 dB gave 1. The usual "leave 1 dB"
  advice is nowhere near enough.

Peak-matching is **not** a substitute — a dense clip and a transient one can
share a peak and still be several dB apart to the ear.

## Playback

**WAV and MP3, chosen by sniffing the header, not the extension.** Contributed
audio is often an MP3 named `.wav`; guessing from the name turns that into
silence with no explanation.

**The extension in a stored path is a logical name.** `audio_resolve()` falls
back to the other one, so re-encoding a clip from WAV to MP3 never invalidates a
path — including ones sitting in NVS against enrolled bands. `sounds.c` can keep
saying `/spiffs/chime.wav` while the file on disk is `chime.mp3`.

**Decoding is streamed frame by frame**, so RAM use is constant regardless of
clip length. A three-minute song costs the same as a one-second one.

### Gapless

An MP3 doesn't hold a whole number of frames of audio, so the encoder pads it
either end. A decoder that ignores that plays the padding — a 0.61 s clip comes
out 0.68 s. Standalone sounds don't care, but the countdown butts clips together
and 74–102 ms of silence at every join is an audible stutter, about a third of a
second across a four-part phrase.

`play_mp3()` reads the LAME/Xing tag in the first frame, skips `delay + 529`
samples off the front (529 being the decoder's own pipeline delay), and stops at
the real sample count so trailing padding never reaches I2S. The tag lives
*inside* a real frame, so the decoder hands back a frame of silence for it —
dropped whole rather than counted against the lead-in.

Verified sample-exact rather than by ear: 13450 samples in, 13450 out.

> **Don't check this with `ffprobe`.** It reads the very header the decoder
> ignores, so it reports 0 ms added either way. Compare decoded sample counts.

## Hardware notes

**PSRAM is deliberately off.** Nothing needs it — streamed decoding is
constant-RAM by construction. Requiring it would narrow the project to
`R`-suffix modules, and octal PSRAM reserves GPIO 33–37, which is a poor thing
to spend on RAM you aren't using. If something ever does need it, detect at
runtime and degrade: `heap_caps_malloc(MALLOC_CAP_SPIRAM)` returning NULL is a
fine signal.

**16 MB flash is comfortable, not required.** With audio at ~1.2 MB an 8 MB
board fits: two 1.75 MB app slots for A/B OTA plus ~4.3 MB of storage. That
matters more than the free space itself — it's what lets someone build one
without hunting for a specific module.

## Still open

**Custom audio from the web page.** Recording a child's name or a personal
greeting means someone who didn't build the firmware needs to get a clip onto a
device. There's no upload endpoint today, and a recipient can't rebuild firmware
— so that missing endpoint *is* the feature.

Band records already store a path string, so a per-band custom clip needs no
schema change. The obstacle is `sound_path_for_id()`, which validates against
the known pool specifically so a crafted POST can't inject an arbitrary path;
widening it to accept an uploads directory has to be done carefully.

**Resource rules, required before any upload ships.** Once a form accepts audio,
size is user-controlled: a name is 1–2 seconds, but the same box accepts a song.
As decoded PCM a 3-minute song is 5.5 MB at 16 kHz mono and 30 MB at 44.1 kHz
stereo. Streaming decode already handles that; any *caching* added later must
stay a pure accelerator — safe to leave empty or disabled with playback still
working — with a per-item cap so one upload can't swallow the budget.

**A filesystem-discovered sound pool.** `sounds.c` holds a compile-time table,
so adding a sound means editing source and rebuilding. Not needed for a fork
(you're building from source anyway), and possibly not needed for custom audio
either — only for uploaded clips to appear as pool entries selectable by *any*
band.
