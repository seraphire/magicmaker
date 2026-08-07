# Roadmap

Grouped by what things are *for*, because a flat list hides the shape. Numbers
are stable — don't renumber, just strike through.

> Where this came from: it lived in Claude Code's task list, which is keyed by
> **session**, so it would have evaporated at the end of the conversation that
> produced it. Backlogs belong in the repo.

---

## Before the gift leaves

Everything else on this page rides OTA. These don't.

- **#35 Occasions: several countdowns, each with its own window** — replaces the
  single trip date with up to 8 records. `lead_days` rather than a start date:
  Christmas with `lead_days: 24` wakes on 1 December *every year* with nobody
  touching it. The active theme is **derived** from the calendar, not stored, so
  there's nothing to fall out of sync. `flags` carries managed-vs-user, so a
  pushed schedule never stomps something the owner added.
  **Changes stored data — that's why it's here rather than later.**
  *Done in firmware (`occasions.c`, `occasion` CLI) as of 87ec94f. Still to
  do: the web page only edits the trip, so slots 1–7 are console-only.*
- **#36 Tap rhythm: one spoken thing per tap** — a tap is always the band's
  sound, then *at most one* spoken thing: the nearest occasion that hasn't
  spoken today, else a cheeky line on the third consecutive tap, else nothing.
  Keeping the "spoken today" gate **per occasion** makes repeat taps drain the
  queue instead of competing. Deliberately not a bands × occasions matrix — two
  optional per-band fields (`occasion`, `quiet`), both defaulting to sensible.
- **#31 Help band** — one clip, one enrollment, no firmware. Now that
  `magicmaker.local` is permanent the script can name it and stay true. Wants
  the `quiet` flag from #36 or it ends with "…and it's 42 days to go!"
- **#44 Export / import the settings as one JSON file** — `GET /export` serves
  everything the owner chose (config, enrollments, per-band countdown modes,
  occasions); `POST /import` puts it back. An **endpoint, not a scrape of the
  rendered page**: the page shows only what it happens to draw, and it breaks
  every time the HTML does. Carries a schema version so import can migrate
  rather than guess, and carries **no Wi-Fi at all** — not the password, and
  not the SSID: it could never be imported (a network name without its password
  takes the device offline, the one state the page can't undo), so carrying it
  bought nothing and put the name of somebody's home network in a file that
  travels. This is the escape hatch that lets a breaking stored-data
  change ship without un-enrolling anybody, which is exactly why it belongs
  *here* and not in the nice-to-haves.
  *Done and live in v1.4.0 — `GET /api/export`, `POST /api/import`, and the
  Backup section on the Advanced tab.*
- **#49 A blocked update should ask for help out loud** — an update the device
  cannot apply on its own (one needing a migration, or a `requires_fw` it can't
  reach) currently fails into the log, where nobody is looking. It should say
  so: a clip on the next tap, plus a notice on the config page carrying the
  release's own `help_url` from the manifest.
  **The spoken clip must never contain a URL.** It says "open magicmaker dot
  local" — a name the device already answers to and which cannot change — and
  the page carries the link. A clip that names a URL has to be re-recorded
  every time the URL moves, and a spoken URL is unusable anyway.
  Two things to get right: rate-limit it, or a device that can't update nags at
  every tap until it's unplugged; and render `help_url` as visible text that
  the owner clicks, never as an automatic redirect — it arrives from the
  network, and the manifest signature covers the firmware hash, not that field.
  Pairs with #44: the notice's actual advice is "export your settings first".
  *Done and live in v1.4.0.*

  **The clips** — recorded and installed as `Program/help-1..3.mp3`.
  `Program/`, not `cd/`: the countdown bank is split into shared numbers and
  per-occasion sets, and this belongs to neither — it isn't about an occasion
  and it would have had to squat in `cd/num/` to be found. `Program/` is
  already where the device's own voice lives, next to `update-failed` and
  `browse-magicmaker`.
  A pool of three for the same reason every other pool exists: this plays after
  *every* scan until it's dealt with, and one identical line repeated is
  nagging where three is a character asking for help.

  | | |
  |---|---|
  | 1 | "I could use a little help! Please have an adult visit Magic Maker dot local, so we can keep the magic alive." |
  | 2 | "Psst — could a grown-up visit Magic Maker dot local? I need a hand to keep the magic going." |
  | 3 | "I'm still working, but I need a grown-up at Magic Maker dot local before I can do something new." |

  Recorded as "Magic Maker dot local", spoken as two words — read as one it
  comes out "magicmakerdotlocal" and nobody can type it back.

  Note what these DON'T say: no version number, no what's-wrong, no URL beyond
  the device's own permanent name. All of that changes; the clip can't. It goes
  on the page, where it can be rewritten in a release instead of re-recorded.
- Physical: a power brick in the box; swap *test tag* / *Tomorrow Transit* for
  the real cards.

- **#45 Card and set artwork in the web UI** — storage is *not* the blocker:
  the partition holds 1.9 MB of 9.9 MB, so ~8 MB free is room for hundreds of
  thumbnails at ~20 KB each. The cost is plumbing — upload, a size ceiling
  enforced on the device, serving, and a resize step that has to happen off the
  device because an ESP32 will not scale a phone photo. Ships alongside the
  emoji (#46), which stays as the zero-cost default: an emoji is drawn by the
  viewer's phone, so it never needs uploading, caching or serving at all.
- **#47 Occasions beyond what the device can hold** — sixteen slots covers
  Christmas, Halloween, the trip and two birthdays with room to spare, but a
  fixed list on the device is the wrong long-term shape: the answer is a
  *pushed schedule* the device caches, holding only what's near-term and
  taking the rest from #38. `OCC_F_MANAGED` already exists as the hook, and
  #44's export/import is the offline half of the same need. Until then, raising
  a constant is the honest stopgap and should be called one.
- **#48 Slot 0 should stop being special** — it's synthesised from appcfg so
  nothing had to migrate, which was right for shipping and is wrong forever:
  the one occasion every device definitely has is the only one that can't be
  deleted, reordered, or given a lead window. Fold it into a real record once
  #44 can carry owners across the change. Its *label* and *emoji* are already
  settings (`trip label` / `trip icon`), so a reader that outlives this trip
  can at least count down to something else without a rebuild.

## The theme system

**Settled design — build this before recording into it.** Agreed in full; none
of it is built yet. Doing it while `hallow` holds one file is the whole point.

### Layout

```
/spiffs/
├── cues/               chime, foolish, startours …   core / fallback
├── Program/            the device's own voice
├── cd/                 THE SHARED BANK
│   ├── d1…d13  w1…w4  m1…m18  and
│   ├── lead-1…3        "There's only" · "Just" · "Only"
│   ├── tail-1…3        "and counting" · "to go" · "left"
│   └── today  tomorrow "Today's the day!"
└── sets/<id>/          THE THEME ROOT
    ├── set.json
    ├── cues/           masks core cues BY ID (merge, not replace)
    ├── Program/        themed prompts
    └── cd/             themed framing; masks the shared bank per family
```

- **`cues/`, not the partition root.** Short beats descriptive; a creaking door
  isn't an "attraction", so the generic word wins in a public firmware.
- **Safe to move because bands store ids, not paths** — what #17 bought.
  `BOOT_SOUND_OPERATIONAL` moves with the rest: `operational` is both the boot
  chime *and* an assignable cue, which is easy to miss.
- **Cues merge per id; countdown families mask wholesale.** Deliberately
  opposite rules. Cues are independent, so a small theme can override `chime`
  and still offer core `startours`; full replacement would mean re-recording
  eight clips before any theme worked. Countdown families are not independent —
  a birthday closing with a Disney line reads as a glitch.
- **The shared bank gains neutral framing.** Without it a themeless countdown
  emits only `"six days"` — a fragment — and worse, `today`/`tomorrow` resolve
  to nothing so it goes **completely silent on the day it must speak**. That's
  a bug, not polish.

### `set.json`

```json
{ "label": "Halloween", "icon": "🎃",
  "strict": true, "countdown": true,
  "hide": ["be-our-guest"],
  "anim": { "chime": "ENCHANTED" } }
```

- **`strict`** — never borrow framing from the shared bank (numbers excepted).
  Neutral shared framing is right for a themeless countdown, but it quietly
  re-introduces mixed voices: a caretaker with no `tail-*` would borrow the
  narrator's *"and counting"*. Strict makes a partial character set sound
  sparse but consistently **him**, which is the better failure. Default
  lenient — the casual case is "I made some music".
- **`countdown: false`** — a theme with no countdown at all: cues and
  animations only. An **occasion answers *when*; a set answers *what*,**
  including whether it counts. Explicit rather than inferred from a missing
  `cd/`, because "doesn't count" and "not recorded yet" must not look alike.
- **`hide`** — let a character refuse a core cue that breaks the spell.
- **`anim`** — the reason set.json exists at all: a themed chime otherwise
  falls through to `ANIM_CELEBRATE`, bright and cheerful and exactly wrong for
  a creaking door.

### Occasions need a trailing window

`occasion_t` gains **`trail_days`**: the window is `[date − lead, date + trail]`.
Christmas is `lead 24, trail 7` — 1 December to 1 January. Without it an annual
occasion rolls to next year the morning after, so **Santa vanishes on Boxing
Day**, which is exactly when you still want him.

Consequence worth writing down: *"annual occasions never need `after-*` or
`tail-since`"* was only true at `trail = 0`. Inside a trailing window the count
goes negative, and those families are precisely what handles it — Santa's
*"three days since Christmas"*. Christmas needs them; Halloween at `trail: 1`
barely does.

### Counting

| Days | Says | Why |
|---|---|---|
| 0 / 1 | full lines | today / tomorrow |
| 2–13 | exact days — *"ten days"* | nobody says "one week and three days" for ten |
| 14–27 | *"two weeks and three days"* | exactly where one unit lies; exact multiples drop the tail |
| 28–59 | weeks, rounded | the remainder stops being interesting |
| 60+ | months | |

Today 25 days says *"four weeks"* — 28, a three-day lie, repeated across 25, 26
and 27. **That, not the phrasing, is the bug.** Compound needs no new clips:
`w2` + `and` + `d3`, and `and` already exists. The 13→14 boundary reads
correctly counting down — yesterday *"two weeks"*, today *"thirteen days"*
feels like progress.

A caretaker/Santa bank is therefore **`d1`–`d13`, `w1`–`w4`, `and`** = 18 clips,
plus framing. Numbers are the one deliberate compromise: shared means the
character says *"Only…"* and the narrator says *"six days"*, so a character
theme should ship its own.

### Turning a published event off

`OCC_F_ENABLED` exists but is not enough. A pushed schedule that rewrites the
record erases the owner's choice, so it needs a **second bit, `OCC_F_USER_OFF`,
that a push may never clear.** Off wins. One boolean owned by two authorities
always loses the owner. If Joe doesn't want Halloween, that has to survive every
future sync or the switch isn't worth having.

- **#37 Let a theme own the card sounds** — unblocked now that bands store an id
  rather than a path. `sounds_init` scans the active set as well as core and
  masks by id. This is what makes a Halloween `chime` a creaking door.
- **#40 Per-theme `set.json`** — optional, overrides only, living at the **root
  of the theme** so one directory is the whole season. The label mostly solves
  itself (a card is assigned to a *slot*, and the slot is `chime` in every
  season); the **animation** does not — a theme's chime would fall through to
  `ANIM_CELEBRATE`, bright and cheerful and exactly wrong for a door.
- **#43 Disney becomes a theme set like any other** — today it's the bare root
  and everything else is a special case, which means the *default* is the one
  path that never gets exercised by the theme code. Make it `sets/disney/` and
  the seasons stop being exceptions. Root stays as the fallback so an
  Adafruit-sounds build with no themes at all still works.

## Managing it from afar

- **#38 Push the schedule, get a heartbeat back** — pull for config (the device
  already polls that document), push for state (new). Both device-initiated;
  never an inbound path. Report the *shape* of the device, never its contents —
  no band names, no SSID, no IP. Start with a Worker appending to KV; D1 earns
  its keep at "which of my devices is missing the Christmas pack?", which needs
  a second device to be a real question.
- **#24 Inventory API** — largely absorbed by #38.

## Animation

- **#29 Trim the chime animation to fit its audio** — drop the white chase from
  that cycle, single-colour sparkle. It outlasts the sound by a wide margin.
- **#30 Ring as a progress meter** — do it *with* #29, not after. A ring that
  means "this is how much is left" makes a long animation read as intentional
  rather than slow, which is a better fix than shortening.
- **#41 Palettes: the same animations, re-coloured** — most of the existing
  shows are the right *motion* for Halloween and wrong only in hue. Lift the
  hard-coded colours out of `leds.c` into a palette a theme can name, so
  `ANIM_CELEBRATE` in October is the identical choreography in orange and
  purple. Cheapest large win in the theme system: no new animation code, and
  a season gets a look from a few lines of `set.json`.
- **#42 Custom scenes, and a way to describe the face** — the blocker under
  #41's ceiling. Some looks aren't a re-colour of anything: Halloween's Mickey
  with an orange face and *green ears*, Christmas's green ring with red
  "berries" scattered round it and a sparkling face. Static is fine to start —
  the hard part isn't animating it, it's that the firmware has no vocabulary
  for *regions* of the strip beyond "ring" and "face". Needs a layout
  description (ears as their own segments, ring positions addressable) before
  any of it can be authored as data. **No settled design yet — think before
  building.** Note it also touches `leds_set_layout`, which is per-device.
- **#9 "Working" beat** — animation first, audio a moment later. In the real device, this looks like a white trailing dot racing around the ring until the device replies. Would be worthwhile to create this chasing animation code with a variable color.

## Countdown behaviour

- **#21 Compound: "two weeks and three days"** — better phrasing *and* it
  retires `d8`–`d13`. The phrasing is the reason; the six files are a rounding
  error.
- **#50 Pressure, not gates** — replaces the daily mark, the taper and the
  cheeky streak with one mechanism: anything that *didn't* play gets more
  likely on the next tap, and resets when it fires. Written up in full in
  [sound-selection.md](sound-selection.md).
  The bug that forced it: at 167 days out `is_speaking_day` falls to
  fortnightly, `167 % 14 = 13`, so the countdown could not be demonstrated at
  all — invisible thirteen days in fourteen, and most invisible when the trip
  is furthest away. A hard gate has no middle setting.
  **Absorbs #14 and #16**, which become a `base` and a per-band cue chance.
  Ships with `countdown why`, without which a quiet device and a broken one
  are indistinguishable.
- **#15 Cheeky as a parenthetical** — play the band's own sound after it

## Interface and odds and ends

- **#18** Make it obvious which card was tapped last; let any card be forced
- **#26** Show variant count; let a band play its variants in order
- **#11** Separate idle and animation brightness
- **#25** Unlock setup actions over the LAN with a button press, no AP mode
- **#23** Publish the generation scripts as the shareable artifact, not the audio
- **#20** Investigate NFC push to a phone tapped on the reader

---

## Done, and worth remembering why

- **#39 Signed OTA at the manifest layer** — not the image, because IDF's
  built-in signing hooks `esp_ota_end()` and would have caught the AP upload
  too, destroying the recovery rail. Signing the *hash* keeps the manifest
  editable. Unsigned is refused, not warned about: a check you can skip by
  omitting a field is not a check.
- **#17 Stored-data compatibility** — `nvs_get_blob` returns `INVALID_LENGTH` on
  any size mismatch, and the caller turned that into "not enrolled". One added
  field would have un-enrolled every band in the house, silently.
- **#33 / #34 Split bank, `include` sub-packs** — whichever directory answers
  for a name owns the *whole family*, or a birthday countdown would occasionally
  close with a Disney line at random.
- **#6 / #22 Filesystem-discovered sounds**, **#32 three mDNS names**,
  **#27 no-repeat countdown**, **#13 fade-out**, **#8 gapless MP3**,
  **#28 (resolved) audio ripple was Wi-Fi TX on a weak supply — use a real PSU**
