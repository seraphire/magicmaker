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
- **#36 Tap rhythm: one spoken thing per tap** — a tap is always the band's
  sound, then *at most one* spoken thing: the nearest occasion that hasn't
  spoken today, else a cheeky line on the third consecutive tap, else nothing.
  Keeping the "spoken today" gate **per occasion** makes repeat taps drain the
  queue instead of competing. Deliberately not a bands × occasions matrix — two
  optional per-band fields (`occasion`, `quiet`), both defaulting to sensible.
- **#31 Help band** — one clip, one enrollment, no firmware. Now that
  `magicmaker.local` is permanent the script can name it and stay true. Wants
  the `quiet` flag from #36 or it ends with "…and it's 42 days to go!"
- Physical: a power brick in the box; swap *test tag* / *Tomorrow Transit* for
  the real cards.

## The theme system

- **#37 Let a theme own the card sounds** — unblocked now that bands store an id
  rather than a path. `sounds_init` scans the active set as well as core and
  masks by id. This is what makes a Halloween `chime` a creaking door.
- **#40 Per-theme `set.json`** — optional, overrides only. The label mostly
  solves itself (a card is assigned to a *slot*, and the slot is `chime` in
  every season); the **animation** does not — a theme's chime would fall through
  to `ANIM_CELEBRATE`, bright and cheerful and exactly wrong for a door.

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
- **#9 "Working" beat** — animation first, audio a moment later.

## Countdown behaviour

- **#21 Compound: "two weeks and three days"** — better phrasing *and* it
  retires `d8`–`d13`. The phrasing is the reason; the six files are a rounding
  error.
- **#14 "Sometimes" mode** alongside Daily/Always/Off
- **#15 Cheeky as a parenthetical** — play the band's own sound after it
- **#16 Per-band chance** of playing something other than its own sound

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
