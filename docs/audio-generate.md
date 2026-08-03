# Voice generation script — what's staged, what's left

Companion to `audio-script.md` (which documents the *existing* human bank). This
one is the worklist for the new generated voice.

**Staged so far: 101 files** — the bank is complete. in `assets/audio-staged/`, split out of
`countdown-1.mp3`, `oneday-18months.mp3`, `countdown-2.mp3`, `countdown-3.mp3`,
`cheeky.mp3`, `countdown-4.mp3`, `countdown-5.mp3`, `countdown-6.mp3`,
`setup.mp3` and `script_2.mp3`.
**Still to generate: nothing.**

**Every composed countdown phrase now works end to end in the new voice** —
lead-in, count and closer — for any distance from 1 day to 563 days (18½
months), with no gaps. Every prompt, day-of line, milestone and after-the-trip
line is in. **No reachable code path falls back to the old bank** — see the merge
rules below, which is where that stops being automatic.

---

## Format that works

`countdown-1.txt` split **25 for 25**. Use that shape:

```
It's only -
- four days -
- five days -
before the adventure begins.
```

First line ends with ` -`, middle lines are wrapped `- like this -`, last line is
plain. The earlier `--double dash--` form did **not** separate reliably — it
dropped the pause before the final item and ran two clips together.

**Rules**

- **One clip per line.** A line containing two sentences splits into two clips —
  that is how `"Starting setup. Connect your phone…"` broke in half.
- **22050 Hz mono** for everything in `cd/`. Retuning I2S mid-sentence is an
  audible tick, and these get butt-spliced into one another.
- **Trim hard.** Up to six clips are glued in the PCM domain with no fade. Silence
  the generator adds becomes a stutter *inside* a sentence.
- **Number pools contiguously from 1.** `countdown.c` sizes a pool by probing
  until the first gap, so `tail-1, tail-2, tail-4` silently loses everything from
  4 on. Generating *fewer* is fine; a hole is not.
- Delivery is **flat** for anything glued either side of a number, and **full
  personality** for clips that play alone (`cheeky-*`, `after-*`, `today-*`,
  `tomorrow-*`).

---

## The firmware already supports these names ✅

Staged counts use **baked number+unit** names — `d4` = "four days", `w2` =
"2 weeks", `m6` = "6 months". `put_count()` prefers those and falls back to the
old `4` + `days` split pair, so both bank shapes work and either can be dropped
in without the other breaking.

**The bank decides the resolution.** `put_count()` starts at the tier that suits
the distance and steps to a coarser unit when the clip is missing, so a bank
holding `d1`–`d13` says "two weeks" at seventeen days out while one holding
`d1`–`d31` says "seventeen days" — no constant to keep in sync with the
recordings. It only ever steps *coarser*, and never lets three days round down
into "one week".

With the bank as staged today:

| Days out | Speaks |
|---|---|
| 1–13 | `d1` … `d13` |
| 14–31 | `w2`, `w3`, `w4` |
| 32–563 | `m1` … `m18` |
| 564+ | nothing — past 18½ months, beyond what was recorded |

No holes anywhere below that. Day 1 and day 0 don't actually reach a count clip
anyway — they're replaced by a whole `tomorrow-*` / `today-*` sentence — but
`d1` is there because the after-the-trip count-up uses the same bank.

`w1` is currently unreachable: days 4–10 stay in the days tier, so `(days+3)/7`
never lands on 1. It earns its place if the compound form ships ("one month and
one week").

Once the bank is complete the unit words `day/days/week/weeks/month/months` can
leave it — nothing asks for them unless a split bank is present.

---

## Already staged (101)

| Name | Line |
|---|---|
| `cd/lead-1` … `cd/lead-8` | It's only · There's just · just · only · we're down to · that's · guess what · oh boy |
| `cd/lead-9` | Countdown time! There's — *see note below* |
| `cd/and` | and — connector for the compound form |
| `cd/d1` … `cd/d13` | one day … 13 days *(13)* |
| `cd/w1` … `cd/w4` | 1 week … 4 weeks *(4)* |
| `cd/m1` … `cd/m18` | one month … eighteen months *(18)* |
| `cd/tail-1` … `cd/tail-13` | before the adventure begins. · between you and the magic. · until our Disney trip. · until the magic. · until we meet the mouse. · until we leave. · left to wait. · and counting. · until we're there. · until the castle. · until we see the fireworks. · so, get packing. · I can't wait. |
| `cd/mile-week` · `mile-month` · `mile-final` | the 7-day, 30-day and final-stretch markers |
| `cd/after-1` … `cd/after-7` | after the trip *(7)* |
| `cd/after-howto` | how to set a new date |
| `cd/tail-since-1` | "since our Disney trip" — flat, glues to a number |
| `cd/today-1` … `cd/today-5` | the day-of announcements *(5)* |
| `cd/tomorrow-1` … `cd/tomorrow-3` | the day-before announcements *(3)* |
| `cd/cheeky-1` … `cd/cheeky-10` | the softened repeat-tap lines *(10)* |
| `Program/start-mode` · `saved` · `scan-now` | from `script_2.mp3` |
| `Program/all-done` · `try-again` · `random` · `release-setup` · `wifi-setup` · `browse-magicmaker` · `update-start` · `update-done` · `update-failed` · `welcome-magic` | from `setup.mp3` *(10)* |

---

## ~~Batch 1 — remaining counts~~ ✅ done

Delivered by `oneday-18months.mp3` (14 lines, split 14/14), which also brought
`lead-2` "There's just" and `tail-2` "between you and the magic."

Only `and` was left out — it's optional, and needed solely for the compound form
on a repeat tap ("one month **and** one week"). One line whenever you want it:

```
and
```

## ~~Batch 2 — lead-ins~~ ✅ done

Delivered by `countdown-2.mp3` (8 lines, split 8/8) as `lead-3` … `lead-8`, plus
the `and` connector.

Reading them with a trailing comma was the right instinct — it gives the
continuing intonation a lead needs when a number follows it.

**`lead-9` is "Countdown time! There's", and it's yours to keep or bin.** It reads
as scaffolding to prime the generator's prosody, but it also works as a genuine
lead — *"Countdown time! There's four days before the adventure begins."* It's
deliberately the **highest** index: `pool_size()` counts up to the first gap, so
deleting the last entry just shrinks the pool, while deleting a middle one would
silently hide everything after it.

It is much longer than the others (1.8 s against 0.25–0.7 s), so it will stand
out when it comes up — roughly one countdown in nine.

## ~~Batch 3 — closers~~ ✅ done

Delivered by `countdown-3.mp3` (11 lines, split 11/11) as `tail-3` … `tail-13`.

Leading each with a comma was the same good instinct as the trailing commas on
the lead-ins — it gives the pickup intonation a closer needs when it follows a
number.

"to go" was dropped, which is right: `tail-1` and `tail-2` already cover the
plain ending.

## ~~Batch 4 — day-of and day-before~~ [done]

Delivered by `countdown-4.mp3` as `today-1` … `today-5` and `tomorrow-1` …
`tomorrow-3`.

**The bare `today` / `tomorrow` were dropped, and are not needed.** They are an
empty-pool fallback only — `countdown.c` prefers a random `today-N` and reaches
the bare word solely when that pool has nothing in it. With five and three
recorded, they are unreachable. (A fork that skips these pools would want them.)

This file showed **both** split faults at once, which is why the segment count
looked plausible but no mapping worked: lines 3 and 4 each broke at an internal
exclamation mark, *and* lines 9 and 10 ran together into one segment. 11 = 10 +
2 − 1. Duration matching could not settle it and it took a listen.

> When over- and under-splits appear in the same file they cancel in the count.
> Never take a matching total as confirmation — check the spans.


## ~~Batches 5 and 6 — milestones and after the trip~~ [done]

Delivered by `countdown-5.mp3` and `countdown-6.mp3`.

`countdown-5` line 3 ran three sentences together — `It's almost here!` with
`Welcome home! I hope it was magical.` — and `countdown-6` re-cut them cleanly,
so `mile-final` and `after-1` come from there and the mashed segments are simply
unused. `countdown-6` also brought a seventh after-the-trip line.

One ambiguity was ruled out rather than guessed: segments 1 and 2 of
`countdown-5` could have been line 1 splitting. They are not — that reading would
leave line 3's three sentences as a single 1.50 s segment, impossible at this
voice's pace. The mapping is forced, not chosen.

## ~~Batch 7 — cheeky~~ [done]

Delivered by `cheeky.mp3` as `cheeky-1` … `cheeky-10`, rewritten warmer than the
originals — they nudge rather than scold, which suits the most-heard lines in the
device.

**Two needed rejoining.** The `- -` separators worked, but five lines hold two
sentences and two of those split at the full stop: the short emphatic openers
("Don't worry." / "Still counting!") left a pause almost as long as a separator.
The margin was **0.30 s inside a line against 0.37 s between lines** — 70 ms.
Both were re-cut spanning the pair from the source so the internal beat survives.

> **For future batches:** make the separator unmistakably longer than any pause
> inside a line — several blank separator lines rather than one. Seventy
> milliseconds is not a margin, and the failure is silent: you get a clip that
> ends mid-thought and another that starts mid-thought, both of which sound
> plausible on their own.


## ~~Batch 8 — spoken prompts~~ [done]

Delivered by `setup.mp3`. This one did **not** split cleanly and needed the line
lengths to disambiguate, because gap size alone was actively misleading: a real
separator measured **0.18 s** while a pause *inside* a line measured **0.34 s**.
Ranking gaps by duration would have cut in the wrong places.

Four clips span an internal pause rather than cut at it — `wifi-setup` (two
sentences), `update-start` and `update-failed` (em-dashes), and
`browse-magicmaker`, which spans seven, being the IP read with a beat between
digit groups. Those beats are wanted: they are what make the address
transcribable. They just cannot be told apart from separators automatically.

> This is the general case of the `cheeky.mp3` problem. Any batch whose lines
> contain internal punctuation needs either one clip per generation, or a
> separator long enough that no sentence can imitate it.


---

## Decisions waiting on you

**`tail-1`/`tail-2` and `lead-1`/`lead-2` are your new lines, not the old bank's.**
Staged tails are "before the adventure begins." and "between you and the magic.";
staged leads are "It's only" and "There's just". The old bank's `tail-1` was "to
go", which batch 3 renumbers to `tail-3`. Both sets work — just don't end up with
two files claiming the same index.

**`script_1.mp3` rewords five prompts** against choices made after it was
generated — "MagicMaker Wi-Fi" where "Magic Maker network" was chosen so it
survives a device rename, and a longer `release-setup` than the take you decided
to keep. Batch 8 above uses the **decided** wording. Nothing from `script_1` is
staged.

**`countdown-onetwothree.mp3` split into 5, not 4.** Needs an ear before batch 1.

---

## Merging: five files must be deleted, not just overwritten

`pool_size()` counts **up to the first gap**, so an old clip sitting past the end
of a regenerated pool is still in it. Leave these and the device picks a human
voice out of an otherwise-new pool — about one cheeky tap in six:

| Delete | Because |
|---|---|
| `cheeky-11`, `cheeky-12` | new pool is 1–10; these extend it to 12 |
| `lead-10` | new pool is 1–9 |
| `tail-since-2`, `tail-since-3` | new pool is 1 |

Also drop, though they are already unreachable:

- **`1`–`31` and the unit words** — `put_count()` prefers baked and the baked bank
  is complete to 563 days. If one ever *did* fire you would get a human-voiced
  number inside a new-voice sentence, which is worse than silence.
- **`flavor-1`, `flavor-2`** — no code path reads them.
- **bare `today` / `tomorrow`** — empty-pool fallback only; both pools are full.

`preamble` and `preamble-dayof` are music rather than speech, so they carry no
voice clash. Keeping them is a taste call, not a correctness one.

## Do not generate

| | Why |
|---|---|
| 8 root reward sounds | Adafruit-guide park audio — third party, not ours to regenerate |
| `walt-welcome` | A recording of Walt Disney's park dedication |
| `preamble`, `preamble-dayof` | Music, not speech |
| `flavor-1`, `flavor-2` | No code path reads them |
| `entering-setup` | Retired — that branch plays `wifi-setup` now |
| `wifi-failed` | `PROMPT_WIFI_FAILED` is defined but referenced by no `.c` file |
| `wifi-online` | No define, no references anywhere |
| `day` `days` `week` `weeks` `month` `months` | Superseded — the unit is baked into the count clip |

---

## When the files land

Copy `assets/audio-staged/**` into `assets/audio-src/`, then:

```
pwsh -File tools/build-audio.ps1
```

It encodes to MP3, matches every clip to −16 LUFS so levels sit together, and
writes into `firmware/spiffs/`. Then flash. Remember the `cd/` names need the
`put_count()` change first.
