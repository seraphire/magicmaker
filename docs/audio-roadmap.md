# Audio roadmap

Two phases and a set of optimisations.

| | Item | Status |
|---|---|---|
| **1** | [MP3 encoding](#1-mp3) | next — flash is 84% full |
| **2** | [Custom audio](#2-custom-audio) | planned, not scheduled |
| — | [Phrase staging and sound caching](#latency-optimisations) | optional; measure before building |

Phase 2 is what lets someone who isn't us put audio on a device — a child's name,
a custom greeting — without rebuilding firmware. It's the reason the resource
rules below exist: the moment a web page accepts an audio file, size stops being
something we control.

---

## The constraint that matters

**Flash.** The storage partition is 9.88 MB and we're using 8.3 MB:

| | size | what |
|---|---|---|
| `cd/` | 5.7 MB | 99 countdown clips |
| `Program/` | 1.6 MB | system prompts |
| root `*.wav` | 1.0 MB | the 8 assignable reward sounds |
| `www/` | 56 KB | the config page |

At 22 kHz mono 16-bit (44.1 KB/s), **about three minutes of audio is filling 84%
of the partition.** Every recording session pushes closer to the wall. This is
entirely our own content — no third party is involved.

### Free space isn't just headroom — LittleFS needs it to work

Running a copy-on-write filesystem near full degrades it in ways that aren't
obvious from a capacity number:

- **Asset OTA needs room for a second copy.** `assets.c` streams to `<name>.new`,
  verifies the hash, then renames — deliberately, so a failed download can't
  corrupt a good file. That means replacing a file temporarily needs **both
  copies resident**. With ~1.58 MB free, any single asset larger than that fails
  outright. Today's biggest is `be-our-guest.wav` at 320 KB, so it works — but
  the margin is thinner than it looks, and it's a live constraint on the update
  path.
- **Metadata compaction needs free blocks.** LittleFS never overwrites in place;
  it allocates, then frees. Near full, operations that look like they should fit
  can return `LFS_ERR_NOSPC` because compaction has nowhere to go.
- **Wear levelling needs blocks to rotate through.** With few free blocks the
  same ones get rewritten, wearing flash faster. On a device meant to run for
  years and take OTA updates, that compounds.

Going from 8.3 MB to ~1.2 MB takes the partition from **16% free to about 88%
free**, which retires this whole class of problem rather than deferring it.

---

## 1. MP3

### Standardise `cd/` on 16 kHz mono, ~40 kbps

16 kHz captures to 8 kHz, ample for voice, and allows ~40 kbps where 22 kHz would
need ~64.

| | now | after |
|---|---|---|
| `cd/` | 5.7 MB | ~650 KB |
| `Program/` | 1.6 MB | ~250 KB |
| reward sounds | 1.0 MB | ~150 KB |
| **total** | **8.3 MB** | **~1.2 MB** |

A uniform rate across `cd/` is **required, not an optimisation** — spliced clips
must share a rate. `firmware/spiffs/README.md` already documents why from the
other direction: retuning the I2S clock mid-sentence is audible as a tick.

Decoder: `chmorgan/esp-libhelix-mp3` (fixed-point, ~30 KB RAM, proven on ESP32).

**Sniff the header rather than trusting the extension.** Paths in `sounds.c` are
hardcoded `.wav`; replacing `chime.wav` with MP3 content would otherwise hit the
RIFF parser and yield silence. This also means a fork can supply either format
without editing paths.

### The gapless problem, and why it isn't one

MP3 carries encoder/decoder delay and frame padding — roughly 24 ms of silence at
the head of a decoded file and up to ~50 ms at the tail. The countdown
*concatenates* clips (`[lead] number unit [trailer]`), so that would inject up to
~150 ms of stutter into a spoken phrase.

The fix is to decode a whole phrase into a buffer and **splice in the PCM
domain** — trim the 529-sample decoder delay and the padding off each piece and
butt the samples together. Exact sample alignment, no gaps.

Measured: a realistic phrase is **377 KB PCM (8.7 s)** at 22 kHz; the ceiling with
`CD_MAX_CLIPS = 6` is **1.08 MB (25 s)**. Less at 16 kHz. PSRAM is 8 MB, so this
is comfortable — but keep a bounds check, since a long recorded preamble grows
the buffer, and fall back to sequential playback (today's behaviour, gaps and
all) if a phrase ever exceeds it.

This is tractable because *we* encode the clips that concatenate. Standalone
sounds never concatenate, so they don't care.

### While re-encoding

Re-encoding from `assets/countdown/raw/` is the moment to fix the
trailing-consonant truncation in recording sessions 1–2, and to level-match
across all four sessions.

### No OTA changes needed

`assets.c` is format-agnostic, and `.gitignore` already excludes `*.mp3`.

---

## Latency optimisations

**Build these only if decode latency is actually perceptible.** Helix runs
~15–25× realtime, so a single clip is ~100–250 ms and a full phrase ~350–600 ms.
The LED show fires instantly and may well cover it. Measure first.

### Phrase staging

The next phrase is knowable ahead: trip date + today fixes the tier, number and
unit. The only free variable is the random lead/trailer — choose it at stage time
instead of tap time. Same randomness, decided earlier.

**Double-buffer:** hold the old buffer until the new render completes, then swap.
Interruptible playback makes rapid taps a designed-for case, and the stale buffer
is still *correct* for the same day — only the lead/trailer repeats.

**Re-render immediately after each play**, not only on day rollover, or every band
on a given day hears the identical lead and trailer, losing the per-band variety
we have now. Other invalidation: `countdown_reset_all()` (already the date-change
hook), and the first render must wait for NTP.

### Sound caching

**Random is always reachable and can't be bounded by registered bands.**
`main.c:626-638` falls through to a random pick for *any* unknown tag — tap a
hotel key card and it plays.

At the current 8-sound pool, **cache the whole pool** (~1 MB PCM). Random becomes
instant with no staging machinery, the existing no-repeat rule
(`main.c:631-637`) keeps working untouched, and program-mode audition is instant
too — the one case that would otherwise always eat decode latency.

Warm it at boot in the dead time while Wi-Fi connects and the sparkle runs, so
the *first* tap is instant. Rebuild on band add/edit/delete and on asset OTA.

---

## 2. Custom audio

The goal: someone who didn't build the firmware records a clip — a child's name,
a personal greeting — and gets it onto a device from the web page.

Today audio arrives exactly two ways: compiled into the LittleFS image, or asset
OTA from a manifest. Both are us. There is no upload endpoint
(`portal.c:704-714`), and a recipient can't rebuild firmware or run
`release.ps1`. **That missing endpoint is the feature.**

*(Someone forking the project is a different, already-solved case: they supply
audio at build time, documented in
[`firmware/spiffs/README.md`](../firmware/spiffs/README.md) for format, layout
and sourcing, and in the main [README](../README.md) for the file names every
pool expects.)*

### The data model already fits

Band records store a **path string**, not a pool index — `store_lookup()` returns
the path and `""` is the random sentinel. So a per-band custom clip
(`custom/emma.mp3`) needs no schema change.

The blocker is deliberate: `sound_path_for_id()` validates against the known pool
specifically *"so a crafted POST can't inject an arbitrary path"*. Custom audio
means widening that to accept paths inside an uploads directory — carefully, as
it's a real injection guard rather than incidental strictness.

This means per-band custom greetings may not need a filesystem-discovered pool at
all. An uploads directory and a widened validator could be enough; discovery
(replacing the compile-time table at `sounds.c:13`) is the larger version, for
when custom clips should appear as assignable pool entries too.

### Resource rules — required before any upload ships

The moment a form accepts audio, size is user-controlled. A kid's name is 1–2
seconds; the same box accepts a song. As decoded PCM a 3-minute song is **5.5 MB**
at 16 kHz mono and **30 MB** at 44.1 kHz stereo, against 8 MB of PSRAM — one
upload can exceed the entire budget.

- **Streaming decode becomes the baseline** — frame by frame into a ~64 KB ring
  buffer, constant RAM regardless of length, `s_stop_req` checked at the frame
  boundary.
- **Caching becomes a pure accelerator**, safe to leave empty, fill, or disable
  with playback still working. Never load-bearing.
- **Per-item cap ~512 KB PCM** (16 s at 16 kHz). Our largest reward sound is
  ~232 KB at 16 kHz — 2× headroom while excluding songs by construction. Over the
  cap streams regardless of free budget.
- **No eviction, no LRU** — with a per-item cap, overflow just means "stream it",
  a path that has to work anyway.
- **Surface file size in the upload path**: a 4-minute MP3 at 128 kbps is ~3.8 MB,
  40% of the partition, reachable with no cache involved.
- **Validate on upload, not on play** — reject or transcode at the door so a bad
  file can't be discovered mid-tap.

---

## Loose ends

Two clips need re-recording:

- `Program/release-setup.wav` — doesn't say what releasing the button actually does.
- `Program/entering-setup.wav` — says "programming" when it means Wi-Fi setup.

Raw takes are archived in `assets/countdown/raw/`. Sessions 1–2 were cut before
the trailing-consonant truncation was caught, so they want a listening pass.
