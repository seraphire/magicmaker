# Audio script — the spoken text of every clip

**This is the input document for regenerating the voice bank with TTS.**

Every audio file the device ships with is listed here against the exact words
spoken in it, with the document that states those words. It exists because the
whole bank is currently one specific person's voice and needs to be re-cut in a
different one.

**Read the rules before generating anything — see [How to regenerate](#how-to-regenerate).**

## Scope and method

- **Inventory** = `assets/audio-src/**` (the masters, the source of truth per
  `tools/build-audio.ps1`). `firmware/spiffs/` is build output: every master is
  encoded to `.mp3` there, 1:1. A name-by-name diff was run — **nothing exists in
  `firmware/spiffs/` that isn't in `assets/audio-src/`**, and nothing is orphaned
  the other way. 123 files each side.
- **Text** comes only from a document that states it. Nothing here is inferred
  from a filename. Where a doc's ordering was ambiguous, the mapping was resolved
  by **exact duration match** against the labelled splitter output in
  `assets/countdown-split/named*/` (durations match to the microsecond because
  the bank was assembled by renaming those files) — those cases are marked.
- Anything a doc does not cover is in
  [Unknown — needs a listen](#unknown--needs-a-listen). It is not guessed at.

---

## How to regenerate

Read this first; several of these change what you ask the TTS for.

### 1. `cd/` clips are spliced into one sentence — no leading or trailing silence

`firmware/main/countdown.c` composes a phrase from up to `CD_MAX_CLIPS = 6`
separate files and the decoder butt-splices them in the PCM domain
(`docs/audio-roadmap.md`, "The gapless problem"). Any silence the generator pads
onto the head or tail of a clip becomes an audible stutter mid-sentence. **Trim
every `cd/` clip to the speech and nothing else.** The existing masters are cut
this way — measured silence on the current bank is 0.05–0.35 s per file, all of
it head/tail padding from the splitter, and the decoder relies on it being small.

### 2. Everything in `cd/` must share one sample rate — 22050 Hz

Pinned in `tools/build-audio.ps1` (`"cd" = @{ Rate = 22050 }`). Retuning the I2S
clock mid-sentence is audible as a tick. `Program/` and the root sounds play
standalone and keep their source rate, so they don't care. Mono, 16-bit.

### 3. Delivery differs by pool, and it is grammatical, not stylistic

| Pool | Delivery |
|---|---|
| numbers, units, `lead-*`, `tail-*`, `tail-since-*` | **Flat and even.** These get glued either side of a number. A line that swoops down at the end sounds wrong when the next word follows it. |
| `cheeky-*`, `after-*`, `today-*`, `tomorrow-*` | **Full personality.** These play alone as complete sentences. |
| `mile-*` | Flat-ish — they replace a trailer, so they follow a unit word. |

### 4. Within a pool, the number in the filename is arbitrary

`countdown.c` probes `lead-1`, `lead-2`, … until one is missing and then picks
**at random**. So for `lead-*`, `tail-*`, `tail-since-*`, `cheeky-*`, `after-*`,
`today-*`, `tomorrow-*` and `flavor-*`, only the *set* of lines and the *count*
matter — any line can go to any index, with no gaps in the numbering.

**The index is semantic** (and must be preserved) for: `1`–`31`, the six unit
words, `today.wav`/`tomorrow.wav`, `mile-week`/`mile-month`/`mile-final`,
`after-howto`, `preamble`/`preamble-dayof`.

This is why the unresolved per-file mappings in the `after-*` and `tail-since-*`
pools are not blockers — see the note in that section.

### 5. Do not regenerate 10 of the 123 files

The 8 root reward sounds are third-party park audio, and `preamble` /
`preamble-dayof` are music, not speech. See
[Third-party audio](#third-party-audio--do-not-regenerate).

---

## `Program/` — spoken prompts (16 files)

Standalone clips, no splicing. Source rate is a mix of 16 kHz and 22050 Hz and is
deliberately preserved by the build script.

| file | spoken text | source doc |
|---|---|---|
| `start-mode.wav` | "Program mode — pick a sound, then tap a card." | audio-recordings.md §3 |
| `saved.wav` | "Saved!" ⚠ | audio-recordings.md §3 (see conflict C7) |
| `scan-now.wav` | "Scan it to try." | audio-recordings.md §3 |
| `all-done.wav` | "All done." | audio-recordings.md §3 |
| `try-again.wav` | "Try again." ⚠ | audio-recordings.md §3 (see conflict C7) |
| `random.wav` | **unknown** — doc line is "This card is now a surprise." but the recording is far too short to be it | see [Unknown](#unknown--needs-a-listen), conflict C6 |
| `release-setup.wav` | "Release the button." | audio-recordings.md §4; audio-roadmap.md "Loose ends" (both agree) |
| `wifi-setup.wav` | "Starting setup. Connect your phone to the Magic Maker network." | audio-recordings.md §4 |
| `browse-magicmaker.wav` | "If a setup page doesn't open by itself, open your browser and visit 192.168.4.1 to continue." | audio-recordings.md §4 |
| `wifi-online.wav` | "You're online!" | audio-recordings.md §4 (listed 🔵 planned, but the file exists — conflict C5) |
| `wifi-failed.wav` | "Couldn't connect — starting setup." | audio-recordings.md §4 (⚪ legacy, superseded by `wifi-trouble`) |
| `entering-setup.wav` | "Entering setup." | audio-recordings.md §4 (⚪ retired — no longer played) |
| `update-start.wav` | "Updating my magic — one moment…" | audio-recordings.md §6 |
| `update-done.wav` | "All updated!" | audio-recordings.md §6 |
| `update-failed.wav` | "Update didn't take — I'm still working." | audio-recordings.md §6 |
| `welcome-magic.wav` | "Welcome to MagicMaker!" ⚠ | physical-test-checklist.md §A; `main.c:537` comment. **Not in audio-recordings.md at all** — see conflict C4 |

### Say "Magic Maker network", not the real SSID

`audio-recordings.md` §4 is explicit: the real SSID is `<device-name>-<id>-Setup`,
so anything more specific goes stale when someone renames their reader. Likewise
`browse-magicmaker` speaks a **bare IP** on purpose (skips DNS, avoids forced
HTTPS, and can't be routed out over cellular). Keep both as written.

### Documented but never recorded — regenerate these too if you want them

These have text in the docs and no file on disk. `wifi-trouble` is the one that
matters: the firmware already plays it if it appears (`config.h:170`).

| file | spoken text | source doc |
|---|---|---|
| `Program/wifi-trouble.wav` | "I'm having trouble connecting to Wi-Fi. To change my setup, hold the button while powering me on." | audio-recordings.md §4 (🔵 wire-ready) |
| `Program/wifi-connecting.wav` | "Connecting to Wi-Fi…" | audio-recordings.md §4 (🔵 planned) |
| `Program/wifi-saved.wav` | "Got it — restarting to connect." | audio-recordings.md §4 (🔵 planned) |

> SPIFFS caps object names at 32 chars **including** the `/Program/` prefix, so
> any new prompt filename must be ≤ 23 chars.

### A regeneration pass has already been started — and it rewords things

`assets/audio-walt/script_1.txt` (untracked, alongside a generated
`script_1.mp3`) contains a TTS script covering part of `Program/`. It is **not**
one of the source docs above and it does not match them verbatim:

| line in `script_1.txt` | closest existing prompt | difference |
|---|---|---|
| "Welcome to the MagicMaker. I'm glad you're here. Let's get you setup so you can make your own Magic." | `welcome-magic.wav` | much longer than the 1.39 s original |
| "Release the button to enter setup." | `release-setup.wav` | original is just "Release the button." — audio-roadmap.md §Loose ends argues *against* naming the consequence |
| "Starting setup. Connect your phone to the MagicMaker Wi-Fi to begin setup." | `wifi-setup.wav` | "MagicMaker Wi-Fi" vs "Magic Maker network" |
| "Couldn't connect to Wi-Fi, starting setup now." | `wifi-failed.wav` | reworded; this prompt is marked ⚪ legacy |
| "If the setup page didn't open by itself, open your phone's browser and navigate to one ninty-two dot one sixty eight dot four dot one." | `browse-magicmaker.wav` | past tense, "your phone's browser", and the IP **spelled out phonetically** |

Decide deliberately whether the new voice re-reads the documented lines or adopts
these. Two things from that file are worth keeping regardless:

- **Spell the IP out in words.** A generator handed "192.168.4.1" will say
  "one hundred ninety-two point one hundred sixty-eight…" or worse. (Note the
  typo — it should be "ninety-two", not "ninty-two".)
- The audio-roadmap already warns that `release-setup` should *not* explain the
  consequence, because the solid blue ring says the hold registered and
  `wifi-setup` explains what's happening a second later. That reasoning still
  applies to the reworded version.

---

## `cd/` — the countdown bank (99 files)

### How a phrase is composed

From `firmware/main/countdown.c` (`countdown_build`), in play order:

```
[preamble]  [lead-in ~35%]  <number>  <unit>  [closer ~85%]
 music        "It's only"   "twenty three"  "days"   "until our Disney trip!"
```

- **`preamble`** plays first on every countdown (`preamble-dayof` instead on days
  0 and 1). Cheeky lines get **no** preamble — that's the joke.
- **Unit stepping** keeps every spoken number between 1 and 31, so nothing is ever
  concatenated mid-number: ≤ 31 days → `days`; 32–59 → `weeks` (`(days+3)/7`);
  60+ → `months` (`days/30.44`, rounded). A value of 1 switches to the singular
  unit word.
- **Day 1 / day 0** replace number + unit entirely with a random `tomorrow-N` /
  `today-N` full sentence, which takes **no closer**. If that pool is empty it
  falls back to the bare `tomorrow.wav` / `today.wav` (plus `mile-final` on day 1).
- **Closer** is `mile-week` at exactly 7 days, `mile-month` at exactly 30, and
  `mile-final` at ≤ 3 days (80% of the time); otherwise a random `tail-N` (85%).
- **After the trip**, 70% of the time it counts *up* — `<number> <unit>
  tail-since-N` — reusing the same number bank. Otherwise a random `after-N`, or
  the `after-howto` nudge (25% of the non-counting case).
- **Cheeky** fires on the 3rd consecutive tap of the *same* band and plays a
  single `cheeky-N` on its own, replacing the countdown.

### preamble (2 files) — music, not voice

See [Third-party audio](#third-party-audio--do-not-regenerate). Nothing to speak.

### lead-* (10 files) — openers, flat delivery, run straight into a number

| file | spoken text | source doc |
|---|---|---|
| `lead-1.wav` | "It's only" | countdown-lines.md (confirmed: duration-identical to `named/its-only-1.wav`) |
| `lead-2.wav` | "It's only" *(second take of the same line)* | countdown-lines.md (= `named/its-only-2.wav`) |
| `lead-3.wav` | "It's only" *(third take)* | countdown-lines.md (= `named/its-only-3.wav`) |
| `lead-4.wav` | "It's just" | countdown-lines.md (= `named/its-just-1.wav`) |
| `lead-5.wav` | "just" | countdown-recording-script-2.md §Lead-ins |
| `lead-6.wav` | "only" | countdown-recording-script-2.md §Lead-ins |
| `lead-7.wav` | "we're down to" | countdown-recording-script-2.md §Lead-ins |
| `lead-8.wav` | "that's" | countdown-recording-script-2.md §Lead-ins |
| `lead-9.wav` | "guess what" | countdown-recording-script-2.md §Lead-ins |
| `lead-10.wav` | "oh boy" | countdown-recording-script-2.md §Lead-ins |

> Three takes of "It's only" is deliberate variety in a *human* bank. A TTS voice
> will produce three identical files, so consider replacing `lead-2`/`lead-3` with
> two of the unused candidates in countdown-lines.md ("we're down to", "would you
> believe", "hold on to your ears") rather than generating the same line 3×.

### tail-* (12 files) — closers, follow the unit word

| file | spoken text | source doc |
|---|---|---|
| `tail-1.wav` | "to go" | countdown-lines.md (confirmed: duration-identical to `named/to-go.wav`) |
| `tail-2.wav` | "until our Disney trip" | countdown-lines.md (= `named/until-our-disney-trip.wav`) |
| `tail-3.wav` | "until the magic" | countdown-lines.md (= `named/until-the-magic.wav`) |
| `tail-4.wav` | "until we meet the mouse" | countdown-lines.md (= `named/until-we-meet-the-mouse.wav`) |
| `tail-5.wav` | "until we leave" | countdown-lines.md (= `named/until-we-leave.wav`) |
| `tail-6.wav` | "left to wait" | countdown-lines.md (= `named/left-to-wait.wav`) |
| `tail-7.wav` | "and counting" | countdown-recording-script-2.md §Trailers |
| `tail-8.wav` | "until we're there" | countdown-recording-script-2.md §Trailers |
| `tail-9.wav` | "until the castle" | countdown-recording-script-2.md §Trailers |
| `tail-10.wav` | "until we see the fireworks" | countdown-recording-script-2.md §Trailers |
| `tail-11.wav` | "so get packing" | countdown-recording-script-2.md §Trailers |
| `tail-12.wav` | "I can't wait" | countdown-recording-script-2.md §Trailers |

The exclamation marks in the recording scripts ("to go!", "until the magic!") are
delivery notes, not text differences — countdown-lines.md lists the same lines
without them.

### tail-since-* (3 files) — post-trip closers, follow the unit word

Only **one** line is documented for this pool and **three** files exist.

| file | spoken text | source doc |
|---|---|---|
| `tail-since-1.wav` | **unknown** (pool line: "since our Disney trip") | countdown-recording-script-3.md §Count-up trailer |
| `tail-since-2.wav` | **unknown** | — |
| `tail-since-3.wav` | **unknown** | — |

Because the index is arbitrary (rule 4), you can simply generate
`tail-since-1.wav` = "since our Disney trip" and delete `-2`/`-3`, or add two
more of your own that scan after a unit word ("since we came home", "since the
magic").

### today-* / tomorrow-* (8 files) — complete announcements, no closer follows

Big energy. These are whole sentences and nothing is glued after them.

| file | spoken text | source doc |
|---|---|---|
| `today-1.wav` | "Today's the day! We're going to Disney!" | countdown-recording-script-4.md §Day of the trip |
| `today-2.wav` | "It's here! It's finally here!" | countdown-recording-script-4.md |
| `today-3.wav` | "Wake up, wake up! Today is the day!" | countdown-recording-script-4.md |
| `today-4.wav` | "This is it! Today we go see the mouse!" | countdown-recording-script-4.md |
| `today-5.wav` | "The magic starts today!" | countdown-recording-script-4.md |
| `tomorrow-1.wav` | "Tomorrow's the big day!" | countdown-recording-script-4.md §The day before |
| `tomorrow-2.wav` | "Just one more sleep!" | countdown-recording-script-4.md |
| `tomorrow-3.wav` | "Tomorrow! Can you believe it?" | countdown-recording-script-4.md |

### today / tomorrow (2 files) — the bare fallback words

| file | spoken text | source doc |
|---|---|---|
| `today.wav` | "today" | countdown-recording-script.md §Specials; countdown-lines.md |
| `tomorrow.wav` | "tomorrow" | countdown-recording-script.md §Specials; countdown-lines.md |

Only used when the `today-N` / `tomorrow-N` pool is empty. Keep them flat.

### after-* (8 files) — post-trip lines, play on their own

`after-howto` is index-semantic (the firmware calls it by name); `after-1`…`after-7`
are a random pool.

| file | spoken text | source doc |
|---|---|---|
| `after-howto.wav` | "Ask a grown-up to set a new trip date, and I'll start counting again!" | countdown-recording-script-3.md §The practical one |
| `after-1.wav` … `after-7.wav` | **per-file mapping unknown** — see below | countdown-recording-script-3.md §Post-trip lines |

The session-3 script lists **six** post-trip lines but **seven** files exist, and
`after-1` holds only 0.84 s of speech — far too short for the script's first line
("Welcome home! I hope it was magical."). The labelling clearly drifted from the
script, so no per-file mapping is asserted. **The six scripted lines are still the
right set to regenerate**, in any order, plus one extra of your choosing:

```
Welcome home! I hope it was magical.
Wasn't that wonderful?
Did you have the best time?
The memories are the real magic.
Someday we'll go back!
Ready to plan the next adventure?
```

### mile-* (3 files) — landmark closers that replace the trailer

| file | spoken text | source doc | plays at |
|---|---|---|---|
| `mile-week.wav` | "One week! Start packing!" | countdown-recording-script-2.md §Milestone lines | exactly 7 days |
| `mile-month.wav` | "One month to go!" | countdown-recording-script-2.md §Milestone lines | exactly 30 days |
| `mile-final.wav` | "It's almost here!" | countdown-recording-script-2.md §Milestone lines | ≤ 3 days, and day 1 after the bare `tomorrow.wav` |

### cheeky-* (12 files) — repeat-tap comebacks, play alone with personality

The most-heard lines in the device. Warm, not snarky.

| file | spoken text | source doc |
|---|---|---|
| `cheeky-1.wav` | "Again?" | countdown-recording-script-2.md §Cheeky |
| `cheeky-2.wav` | "Still the same number!" | countdown-recording-script-2.md §Cheeky |
| `cheeky-3.wav` | "It hasn't changed since last time." | countdown-recording-script-2.md §Cheeky |
| `cheeky-4.wav` | "Tapping more won't make it sooner." | countdown-recording-script-2.md §Cheeky |
| `cheeky-5.wav` | "Okay, okay — I heard you the first time." | countdown-recording-script-2.md §Cheeky |
| `cheeky-6.wav` | "You really want to go, don't you?" | countdown-recording-script-2.md §Cheeky |
| `cheeky-7.wav` | "Patience! The mouse isn't going anywhere." | countdown-recording-script-2.md §Cheeky |
| `cheeky-8.wav` | "Save some magic for later!" | countdown-recording-script-2.md §Cheeky |
| `cheeky-9.wav` | **unknown** | — |
| `cheeky-10.wav` | **unknown** | — |
| `cheeky-11.wav` | **unknown** | — |
| `cheeky-12.wav` | **unknown** | — |

`cheeky-9`…`12` came from a different recording session than 1–8 (see
[Unknown](#unknown--needs-a-listen)). countdown-lines.md offers four never-used
candidates that would fill those four slots if you'd rather not transcribe them:

```
I promise I'm counting.
Nope, still the same.
That's the third time, you know.
Are you trying to speed it up?
```

### flavor-* (2 files) — recorded, but the firmware never plays them

| file | spoken text | source doc |
|---|---|---|
| `flavor-1.wav` | "soon" | countdown-lines.md (= `named/soon-1.wav` by duration) |
| `flavor-2.wav` | "it's coming" | countdown-lines.md (= `named/its-coming-1.wav` by duration) |

`grep flavor firmware/main/` returns nothing — no pool in `countdown.c` reads
these. countdown-lines.md says they "don't fit before a number; used on their
own", but that path was never built. **Safe to skip when regenerating.**

### numbers (31 files) — `1.wav` … `31.wav`

**Verified range: 1 through 31 inclusive, no gaps, 31 files.** Each is the
cardinal number spoken as one natural whole phrase — "one", "two", … "twenty",
"twenty one", "twenty two", … "thirty", "thirty one".

Source: `countdown-recording-script.md` §"The list — read in this order" gives the
full ordered list; `countdown-lines.md` restates it as "`1.wav`–`31.wav` = one …
thirty one". `split.html` preset `s1` carries the same order. Three sources agree.

**Critical for TTS:** each number must be **one recording, never concatenated**.
The whole unit-stepping design exists so that "twenty three" is a single natural
phrase and there is no "twenty" + "three" seam. Generate them individually and
flat — no rising or falling intonation, because a unit word always follows.
There is no `0.wav`, and nothing above 31 (`countdown.c` clamps to 31).

### units (6 files)

| file | spoken text | source doc |
|---|---|---|
| `day.wav` | "day" | countdown-recording-script.md §Units; countdown-lines.md |
| `days.wav` | "days" | countdown-recording-script.md §Units; countdown-lines.md |
| `week.wav` | "week" | countdown-recording-script.md §Units; countdown-lines.md |
| `weeks.wav` | "weeks" | countdown-recording-script.md §Units; countdown-lines.md |
| `month.wav` | "month" | countdown-recording-script.md §Units; countdown-lines.md |
| `months.wav` | "months" | countdown-recording-script.md §Units; countdown-lines.md |

The singulars only ever play when a count lands on exactly 1 in that unit, but the
firmware needs them — a missing one aborts the whole phrase (`put_count` returns
false and nothing is spoken).

---

## Root — the reward sounds (8 files)

All eight are **third-party** and none is the owner's voice. They are covered in
full under [Third-party audio](#third-party-audio--do-not-regenerate) — nothing to
transcribe or regenerate here.

| file | spoken text | source doc |
|---|---|---|
| `be-our-guest.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `chime.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `excellent.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `foolish.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `hello.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `operational.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `startours.wav` | *n/a — third-party park audio* | README.md §About the audio |
| `walt-welcome.wav` | *n/a — Walt Disney's park dedication* | README.md §About the audio; audio-recordings.md §1 |

---

## Third-party audio — do not regenerate

### The 8 root reward sounds

`README.md` §About the audio is unambiguous:

> **Reward sounds** a band can be assigned — `chime`, `excellent`, `foolish`,
> `hello`, `operational`, `startours`, `walt-welcome`, `be-our-guest` — come from
> the [Adafruit Magic Band Reader](https://learn.adafruit.com/magic-band-reader)
> project. "They're park audio bundled with that guide, so they're not
> redistributed here."

That list is **all eight** root files, with no exceptions. `walt-welcome.wav` is
additionally a recording of Walt Disney's park dedication (audio-recordings.md §1;
`config.h:131-134` notes a dedicated "To all who come to this happy place" take
was *considered* but never cut).

`audio-recordings.md` §1 does describe these in quotes ("All systems
operational.", "Hello!") — that is describing what the third-party clip says, not
a line the owner read. Do not feed them to the generator.

Note that these define the **animation pool** (`sounds.c`, audio-recordings.md §2),
so if you ever replace them the filenames must stay the same or the mapping breaks.

### The 2 preamble clips — music, not speech

| file | what it is | source |
|---|---|---|
| `cd/preamble.wav` | "warm-up music before the count" | README.md file table, line 119 |
| `cd/preamble-dayof.wav` | "a more excited cut used on days 0-1" | commit `e987c99` |

Both are duration-identical to `assets/music/preamble.wav` and
`assets/music/dayof.wav`, which sit alongside `First_Light_Departure.mp3` and
`Morning_of_the_Ascent.mp3`. There is no spoken text to regenerate. Their
provenance (licence, origin) is **not documented anywhere in the repo** — worth
settling separately if the project is ever redistributed.

---

## Conflicts

Recorded where two sources disagree. `audio-recordings.md` is the newest of the
prose docs and generally wins, except where a *file on disk* settles it.

**C1 — `audio-recordings.md` §5 describes a countdown scheme that was never built.**
It specifies `cd-today-N.wav`, `cd-week-N.wav`, `cd-2weeks-N.wav`, `cd-month-N.wav`,
`cd-faraway-N.wav`, `cd-after-N.wav` **in `/spiffs/Program/`**, plus a 5b
"number word bank" of `zero`…`nineteen`, `twenty`…`ninety`, `one-hundred`…
`three-hundred` and a joiner `and`, covering 0–399. **None of that exists.** The
shipped design is `/spiffs/cd/` with whole numbers 1–31 and unit stepping
(`countdown.c`, countdown-recording-script.md), explicitly so that nothing is ever
concatenated mid-number. **Ignore §5 entirely** — it is the stalest section in the
docs. Everything real is in countdown-lines.md and the four recording scripts.

**C2 — trailer numbering: countdown-lines.md vs countdown-recording-script.md.**
countdown-lines.md maps `tail-1`…`tail-6` positionally to "to go, until our Disney
trip, until the magic, until we meet the mouse, until we leave, left to wait".
countdown-recording-script.md lists the same six lines in a *different* recording
order, beginning "until our Disney trip!". If labels had followed recording order,
`tail-1` would be "until our Disney trip".
**Resolved:** `cd/tail-1.wav` is duration-identical (0.609977 s) to
`assets/countdown-split/named/to-go.wav`, and `tail-2`–`tail-6` match the rest of
countdown-lines.md's order exactly. **countdown-lines.md is correct.**

**C3 — lead-in wording.** countdown-recording-script.md says the lead-ins are
"It's only" / "Just" (and its extras section says "Only · Just").
countdown-lines.md says `lead-1`–`lead-4` are "it's only" ×3 and "it's just".
**Resolved:** the four masters are duration-identical to
`named/its-only-1|2|3.wav` and `named/its-just-1.wav`. **countdown-lines.md is
correct** — all four carry the "It's …" prefix.

**C4 — `Program/welcome-magic.wav` is missing from `audio-recordings.md`.**
It is the provisioned power-on greeting (`config.h:137`, `main.c:537`) and it
exists on disk, but the audio master list never mentions it. Its text here comes
from two *secondary* sources that agree — `docs/physical-test-checklist.md` §A
("welcome to MagicMaker") and the `main.c:537` comment ("welcome to
MagicMaker!"). The 1.39 s of speech fits. Treat the exact wording and punctuation
as unverified.

**C5 — `audio-recordings.md` status flags are out of date against the disk.**
- `Program/random.wav` — marked ❌ "needs recording", **but the file exists**.
- `Program/wifi-setup.wav` — marked ✅ in §4 **and** listed as ❌ missing in the
  "what's missing right now" list at the bottom of the same document. The file
  exists (4.14 s). The bottom list is wrong.
- `Program/wifi-online.wav` — marked 🔵 planned, **but the file exists** (0.78 s).
- `Program/wifi-trouble.wav` — marked 🔵 "wire-ready" and referenced by
  `config.h:170`, **but there is no file**. This is the only genuinely missing
  prompt the firmware would play today.

**C6 — `Program/random.wav`: the documented line doesn't fit the recording.**
audio-recordings.md gives "This card is now a surprise." The file holds **0.79 s**
of speech; that sentence needs roughly twice that. Combined with the ❌ flag in
C5, the safest reading is that the file is an older, shorter take. Listed as
unknown. (The documented line is still the right line to *generate*.)

**C7 — two prompts run longer than their documented lines.**
`Program/saved.wav` is **1.81 s of speech** for "Saved!", and
`Program/try-again.wav` is **1.79 s** for "Try again." Both are roughly double
what those words need. They may simply be drawn-out reads, or the takes may say
more than the doc records. Worth a confirming listen; the documented lines are
still the right ones to generate.

**C8 — sample rate: docs contradict each other and the files.**
`audio-recordings.md` §Format says spoken clips are **16 kHz** and that masters
live in `hardware/audio/`. `countdown-recording-script.md` (session 1) also says
16 kHz. Sessions 2, 3 and 4 and countdown-lines.md all say **22050 Hz** and warn
"don't switch back to 16k — mixing rates causes a retune click".
**Reality:** masters live in `assets/audio-src/`, **every** `cd/` master is
22050 Hz, and `build-audio.ps1` pins `cd/` to 22050. `Program/` is a genuine mix
(16 kHz: browse-magicmaker, entering-setup, random, release-setup, update-*,
welcome-magic, wifi-failed, wifi-online, wifi-setup; 22050: all-done, saved,
scan-now, start-mode, try-again) and the build script deliberately preserves each
file's source rate. **Generate `cd/` at 22050. `Program/` can be anything.**

**C9 — session-3 file count doesn't match its script.**
countdown-recording-script-3.md specifies 8 lines (`after-1`…`after-6`,
`after-howto`, `tail-since`). Eleven files were produced (`after-1`…`after-7`,
`after-howto`, `tail-since-1`…`3`). Since the splitter auto-labels strictly in
order, a count mismatch invalidates the positional mapping — and `after-1`'s
0.84 s of speech confirms it drifted. See the `after-*` section.

**C10 — the splitter preset disagrees with its own script (cosmetic).**
`assets/countdown-split/split.html` preset `s2` expects **25** segments with
`cheeky-1`…`cheeky-10`; countdown-recording-script-2.md specifies **23** lines with
8 cheeky. The 23 files in `named2/` match the script, so the script won. Only
matters if anyone re-runs the splitter.

**C11 — milestone filenames (cosmetic).**
countdown-lines.md proposes `one-week.wav`, `one-month.wav`, `final-countdown`.
What shipped is `mile-week`, `mile-month`, `mile-final`. Use the shipped names.

**C12 — is the audio third-party or not?**
`audio-roadmap.md` §"The constraint that matters" says the bank is "entirely our
own content — no third party is involved." `README.md` §About the audio says the
8 root reward sounds are Adafruit-guide park audio, not redistributed.
**README.md is correct for the root sounds**; the roadmap sentence is true only of
`cd/` and `Program/`. This one matters — it is the line between what may and may
not be regenerated.

---

## Unknown — needs a listen

15 files. No document states their text, so nothing is asserted. Durations are
raw / speech-only (silence trimmed at −45 dB).

| file | raw | speech | what is known |
|---|---|---|---|
| `Program/random.wav` | 0.79 s | 0.79 s | Doc line is "This card is now a surprise." but that cannot fit in 0.79 s, and the doc flags the clip as not-yet-recorded (C5, C6). 16 kHz. |
| `cd/cheeky-9.wav` | 4.11 s | 3.36 s | Master is `assets/countdown/raw/sooner.wav` (exact duration match). Not from any recording script. |
| `cd/cheeky-10.wav` | 2.53 s | 2.36 s | Master is `assets/countdown/raw/often.wav` (exact duration match). |
| `cd/cheeky-11.wav` | 2.91 s | 2.41 s | Master is `assets/countdown-split/still-here.wav` (exact duration match). |
| `cd/cheeky-12.wav` | 5.61 s | 4.95 s | Master is `assets/countdown-split/ask-doctor.wav` (exact duration match). Longest clip in the bank. |
| `cd/after-1.wav` | 1.06 s | 0.84 s | Session-3 pool; mapping drifted (C9). Too short for the script's first line. |
| `cd/after-2.wav` | 1.71 s | 1.49 s | Session-3 pool; mapping drifted (C9). |
| `cd/after-3.wav` | 1.31 s | 1.24 s | Session-3 pool; mapping drifted (C9). |
| `cd/after-4.wav` | 1.75 s | 1.47 s | Session-3 pool; mapping drifted (C9). |
| `cd/after-5.wav` | 2.10 s | 1.95 s | Session-3 pool; mapping drifted (C9). |
| `cd/after-6.wav` | 1.34 s | 1.20 s | Session-3 pool; mapping drifted (C9). |
| `cd/after-7.wav` | 1.80 s | 1.70 s | Session-3 pool; entirely undocumented — the script only lists six post-trip lines. |
| `cd/tail-since-1.wav` | 1.53 s | 1.35 s | Pool has one documented line, "since our Disney trip", and three files. |
| `cd/tail-since-2.wav` | 1.84 s | 1.72 s | Undocumented. |
| `cd/tail-since-3.wav` | 2.69 s | 2.48 s | Undocumented. |

The four `cheeky-9`…`12` masters carry suggestive names (`sooner`, `often`,
`still-here`, `ask-doctor`) but **no document states their text and none is
guessed here.** They are the four clips most worth actually playing back — they
sit in the most-heard pool in the device, and `ask-doctor` at 5.6 s is an outlier
in a pool where everything else is under 3.5 s.

### Verify by ear (text is stated, but only weakly)

Not counted as unknown — the text below is the right thing to generate either way.

| file | why |
|---|---|
| `Program/welcome-magic.wav` | Text comes from a test checklist and a code comment, not the audio master list (C4). |
| `Program/saved.wav` | 1.81 s of speech for "Saved!" (C7). |
| `Program/try-again.wav` | 1.79 s of speech for "Try again." (C7). |

---

## Counts

| | files |
|---|---|
| **Total on disk** | **123** |
| — `assets/audio-src/cd/` | 99 |
| — `assets/audio-src/Program/` | 16 |
| — `assets/audio-src/` (root) | 8 |
| **Confirmed text** | **98** |
| — in `cd/` | 83 |
| — in `Program/` | 15 |
| **Unknown — needs a listen** | **15** |
| — in `cd/` | 14 |
| — in `Program/` | 1 |
| **Do not regenerate** | **10** |
| — third-party park audio (root) | 8 |
| — music, not speech (`cd/preamble*`) | 2 |

98 + 15 + 10 = 123. ✓

**Actually to be generated in the new voice: 113 files** (98 confirmed + 15 after a
listening pass), or **111** if you skip the two unused `flavor-*` clips.

`firmware/spiffs/` mirrors `assets/audio-src/` exactly — same 123 names, `.mp3`
instead of `.wav`. There is nothing in SPIFFS that is missing from the masters.

## Where to put the regenerated files

Mono 16-bit WAV into `assets/audio-src/` (same paths, same names), then run
`tools\build-audio.ps1`. It loudness-matches everything to −16 LUFS / −3 dBTP and
encodes to MP3 into `firmware/spiffs/`. Do **not** write into `firmware/spiffs/`
directly — it is build output and gets pruned. A file left out simply plays
nothing; it never crashes the device.
