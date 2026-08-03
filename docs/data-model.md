# Data model — what we store, where, and why

Status: design. Nothing here is implemented yet except where marked *(today)*.

The device has two places to put things, and they are good at opposite jobs.
This document fixes the boundary between them, the rules that keep stored data
extensible, and the shape the countdown grows into when it stops being about one
trip.

---

## 1. How NVS actually works

Worth knowing before deciding what goes in it, because the intuitions people
carry about "flash wear" turn out not to be the constraint here.

**Geometry.** Our `nvs` partition is `0x10000` — **64 KB, 16 pages**. Each page
is 4096 bytes, laid out as a 32-byte header, a 32-byte entry-state bitmap, and
**126 entries of 32 bytes each**. One page is held back for compaction, so
usable capacity is roughly **15 × 126 ≈ 1890 entries**.

**What an entry costs.** Approximate, and enough to budget with:

| Value | Entries |
|---|---|
| `u8`, `i32`, `u32` … | 1 (the key rides along) |
| string | 1 + `ceil((len+1) / 32)` |
| blob | 1 index + 1 chunk header + `ceil(len / 32)` |

So today a single enrolled band costs roughly:

- `tags` — 49-byte blob (`sound[48]` + `anim`) → ~4 entries
- `bandname` — short string → ~3 entries
- `cd` — 8-byte blob → ~3 entries

**≈ 10 entries per band**, which puts the ceiling near **180 bands** with config
alongside. The partition comment's "~100 cards" is the right order.

**Wear.** NVS never updates in place. A write appends a new entry and marks the
old one dead; when a page runs out of entries it is compacted into a free page
and erased. Flash sectors are good for ~100,000 erase cycles, and NVS spreads
those across all 16 pages.

Run the numbers on the worst offender we have — a per-tap write of the countdown
record, ~3 entries. A page holds 126, so ~40 writes fill one; a full rotation
across 15 pages is ~600 writes and costs each page one erase. At 100,000 cycles
that is on the order of **tens of millions of writes** before wear matters. At
twenty taps a day, the flash outlives everyone reading this.

**So wear is not the reason to avoid writing on every tap.** Two better reasons
are:

1. **Latency on the tap path.** A flash write is milliseconds; a compaction can
   block for tens of them. That lands exactly when audio is streaming and the
   LED loop is trying to hold ~50 fps — the same class of problem as the RMT
   contention that caused the flicker. The tap path should not touch flash.
2. **It is a bug surface.** The "Again today" failure was three separate layers,
   two of which existed only because a *transient* fact was being persisted:
   a conditional write that silently no-op'd, and a stale record whose size no
   longer matched the struct. State that lives in RAM cannot rot.

---

## 2. The rule: settings persist, state does not

> **If losing it on a power cycle would only cost someone a repeated sound,
> it lives in RAM.**

With one refinement, found while designing per-band variant order:

> **Persisting is nearly free if the record is already being written on that
> occasion. It costs a new flash write otherwise.**

The first draft of this document leaned on tap-path latency to argue everything
transient belongs in RAM. That was overstated. `countdown_due()` opens NVS on
every tap but returns early *without writing* whenever it decides not to speak,
so the write only lands when the countdown actually fires — rare at months out
with the taper thinning it.

So the question is not "is this state?" but "is something already writing here?"

- The countdown record is written the moment it speaks. Remembering which
  lead-in and closer it used rides along free — and `cd_rec_t.idx` already
  exists for it, initialised to -1, carried across every write, and **never
  read or assigned**. A stub nobody finished. Wiring it up costs no bytes, no
  size change and no migration, and buys real quality: never the same closer
  twice running.
- A reward sound's variant position is different. That band record is written
  only at *enrollment*, so persisting a per-tap position would add a flash write
  to every tap — on the record holding the enrollment itself. RAM, and a reboot
  restarting a sequence costs one repeated clip on a device that is permanently
  powered.

A reboot forgetting "this band already heard the countdown today" means it might
speak twice in one day. Nobody will file that. A reboot forgetting *which sound
a band is assigned to* is data loss.

| | Kind | Where |
|---|---|---|
| Wi-Fi credentials, device name, trip date, LED layout, idle colour | setting | NVS |
| Band → sound + animation | setting | NVS |
| Band friendly name | setting | NVS |
| Countdown mode (Daily / Always / Sometimes / Off) | setting | NVS |
| Per-band chance | setting | NVS |
| **Last day this band was greeted** | state | **RAM** |
| **Which phrase variant it last used** | state | **RAM** |
| **"Again today" force flag** | state | **RAM** |
| **Last band scanned, scan counter** | state | **RAM** *(today)* |

The RAM table is small enough to be free:

```c
typedef struct {
    uint8_t uid[4];
    int32_t day;      // last greeted; 0 = never, CD_FORCE = forced
    int16_t idx;      // last variant used, so it doesn't repeat itself
} band_state_t;

static band_state_t s_state[32];   // ~384 bytes
```

Thirty-two entries with oldest-out replacement. Overflowing just means a band
might repeat itself — the same cost as a reboot, which we have already accepted.

This also deletes the per-tap flash write entirely: a tap becomes **read-only**
against NVS.

---

## 3. NVS or LittleFS

> **NVS holds pointers and settings. LittleFS holds content, and the description
> of that content.**

| | Store | Why |
|---|---|---|
| Which occasions exist, their dates, which audio set each uses | NVS | Fixed-shape, few, written one at a time from a form — per-record atomicity for free, and no parser |
| Per-band settings | NVS | Read on every tap |
| Audio | LittleFS | Obviously |
| **Which clips a set contains, and which slot each fills** | LittleFS, `/sets/<id>/set.json` | Variable-length, list-shaped, arrives as a bundle, authored by someone who is not us |

That last row is the one that matters. If the slot map is *data*, the firmware
never needs to learn what a birthday is.

---

## 4. Extensibility rules

These apply to every NVS blob. Breaking one of them is how records rot.

**1. Size is the version.** Read into a zeroed struct, tolerate a stored record
that is shorter (missing tail = defaults) *or* longer (extra tail = written by
newer firmware, take the prefix). No version byte — one at the front would shift
every existing field's offset and corrupt everything.

Forward tolerance is not theoretical. Install a build that appends a field, roll
back, and without it every band reads `ESP_ERR_NVS_INVALID_LENGTH` and the device
looks factory-reset. On a project with public OTA, that is a Tuesday.

**2. Append only.** Never reorder, resize, or repurpose a field. If a layout
genuinely must break, rename the namespace (`cd` → `cd2`) and migrate once —
cleaner than an in-record version byte, and it fails loudly instead of silently.

**3. Zero must mean the old behaviour.** Every appended field reads as `0` on
every record already on flash. This is the rule that will actually bite: a
`chance` field stored as "percent likely to play" makes `0` mean *never*, and
every existing band goes silent on upgrade. Store it as `skip_pct` instead, where
`0` = never skip = today's behaviour. Same for `occ_mask`: `0` has to mean *all
occasions*, not *none*.

**4. Never `#pragma pack` an existing struct.** `cd_rec_t` is 8 bytes *because*
of padding; packing it to 7 makes every record on flash the wrong size.

**5. Indices into variable lists are advisory.** `idx` points into a set's
variant list. Re-upload a set with fewer `tail-*` clips and it dangles — treat
out-of-range as "start over".

---

## 5. Audio sets

Today's countdown bank is 99 files in `/cd/`. Splitting it by what actually
varies:

- **31 numbers + 6 unit words** (`day/days`, `week/weeks`, `month/months`) —
  occasion-agnostic. "Seventeen days" is "seventeen days" whether it is a trip or
  a birthday.
- **~62 framing clips** — `preamble`, `lead-*`, `tail-*`, `today-*`,
  `tomorrow-*`, `after-*`, `mile-*`, `flavor-*`, `cheeky-*`. *These* are what
  make it a Disney trip.

So a set is **the framing only**, not a copy of the bank:

```
/shared/num/17.mp3  days.mp3  weeks.mp3 …    one copy, every set uses it
/sets/trip/set.json + framing clips
/sets/bday/set.json + framing clips
```

A birthday set is ~40 clips rather than 99, and someone forking the project
records a fifth of what we did. That is the difference between "supported in
principle" and "someone actually does it".

`set.json` sketch:

```json
{
  "id": "trip",
  "name": "Disney Trip",
  "voice": "Brian",
  "numbers": "shared",
  "slots": {
    "preamble": ["preamble"],
    "lead":     ["lead-1", "lead-2", "…"],
    "tail":     ["tail-1", "tail-2", "…"],
    "dayof":    ["today-1", "…"],
    "after":    ["after-1", "…"]
  }
}
```

A set missing a slot degrades to skipping it — never a failed tap. The
`audio_resolve()` fallback already models this shape.

---

## 6. Occasions

```c
typedef struct {
    uint8_t kind;        // trip | birthday | holiday
    uint8_t recur;       // 0 = one-shot, 1 = annual
    uint8_t taper;       // which speaking-day rule
    uint8_t flags;
    int16_t year;        // 0 for annual — month/day only
    uint8_t month, day;
    char    set[16];     // -> /sets/<id>/
    char    label[32];   // "Disney trip", "a birthday"
} occasion_t;            // append only, forever
```

Stored as `occ0`…`occ7` in an `occ` namespace. Eight is plenty and keeps
enumeration trivial.

Per-band, `occ_mask` as a bitmask over those slots is how "Joe's band hears Joe's
birthday" works without a second table.

**Decide the collision rule before writing the code**, or the code will decide it
accidentally: when a trip and a birthday are both due on the same tap, the
proposal is *soonest wins, never two in one tap*.

---

## 7. Staging

The window where breaking stored data is free closes at handover. What has to
happen inside it is small:

1. **Size-tolerant reader** for `store` and `countdown` (~30 lines, shared).
2. **Move state to RAM** — `day`, `idx`, and the force flag out of `cd_rec_t`.
   Since that changes field offsets, take the clean break now: new namespace,
   settings-only record. This is the last moment that is a one-line decision
   rather than a migration.
3. **Design near-term fields with zero-means-legacy** (`skip_pct`, `occ_mask`),
   even before anything reads them.
4. **Move the number bank** to `/shared/num/`. Cheap now; tedious once sets exist
   in the wild.

Everything else — `set.json`, the occasion array, the web UI for both — can land
whenever, because by then appending is free.

`tags` keeps its current layout and simply appends; enrollments are the one thing
worth preserving.
