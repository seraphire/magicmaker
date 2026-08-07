# How the reader decides what to say

Every tap plays a **cue** — the card's own sound. On top of that it may add **at
most one spoken thing**: a countdown, or a cheeky line. This document is how it
chooses.

---

## Why it isn't a set of rules any more

It used to be. Three of them, each sensible alone:

- a **daily mark** — one countdown per band per day
- a **taper** — far from the trip, only speak every so often
- a **streak** — a cheeky line on the third consecutive tap of one band

Then someone tried to demonstrate the reader to friends, 167 days from the trip,
and could not make it count down at all. Not a bug: `is_speaking_day(167)` fell
through to the fortnightly branch, `167 % 14 = 13`, and the next speaking day
was **thirteen days away**. The device's most demonstrable feature was invisible
thirteen days out of fourteen, and *most* invisible when the trip was furthest —
exactly when a countdown is the only interesting thing it does.

A hard gate has no middle setting. Silent is silent all day, and no amount of
tapping can coax it. So the gates are gone, replaced by one mechanism:

> **Nothing is ever off the table. Things you didn't hear become more likely.**

---

## Pressure

Each candidate carries a **pressure** that rises on every tap where it *didn't*
play, and drops to nothing when it does.

```
p = min(cap, base + bonuses + pressure)

roll candidates in priority order — the first hit wins
  winner:  pressure = 0
  losers:  pressure += step
```

At 167 days out, the countdown sits at `base 15, step 20, cap 85`:

| tap | chance |
|---|---|
| 1 | 15% |
| 2 | 35% |
| 3 | 55% |
| 4 | 75% |
| 5 | 85% |

Two or three taps to hear it. Then it resets to 15% and climbs again — so a
second hearing costs another two or three taps.

That reset-then-climb is the point, and it is deliberately not a decay. If the
countdown simply became *less* likely after playing, someone who wanted to hear
it again would have no way to get it, which is the frustration this replaces.
Persistent tapping gets you everything; casual use doesn't get spammed.

**`base` carries the taper's old job**, without the cliff:

| distance | base |
|---|---|
| 60+ days | 15 |
| 32–59 | 35 |
| 8–31 | 60 |
| 2–7 | 90 |

---

## What resets, and when

The scope of a pressure follows what its candidate is *about*.

| | scope | resets on |
|---|---|---|
| **cheeky** | per band | a different band, or when it fires |
| **countdown** | **global** | when it fires, or midnight |
| first-of-day bonus | per band | midnight |

**Cheeky is about the band.** "You keep tapping *this* card" — tap a different
one and the premise is gone.

**The countdown is about the day.** How long until the trip does not depend on
which card is in your hand. This matters more than it sounds: passing cards
around a room means the band changes on *every tap*. If countdown pressure reset
on a band change it could never accumulate at all, and the demo that prompted
all of this would fail exactly as before.

The reverse is just as bad — cheeky pressure surviving a band change would aim
"you again?" at four different people holding four different cards.

**The per-band first-of-day bonus** closes the remaining gap. Joe taps at
breakfast and hears the countdown, draining the global pressure. Charissa taps
at lunch — she wasn't there, and her own untouched bonus still gives her a good
chance on her first tap of the day.

---

## Priority

Rolled in this order; the first hit wins, and nothing else speaks:

1. **forced** — milestones (below)
2. **countdown** for the active occasion
3. **cheeky**
4. nothing — the cue plays alone, which is a perfectly good outcome

---

## The hard rules that remain

Two, and only two.

**Forced to certainty** — never a roll, because they get exactly one chance to
land: *today*, *tomorrow*, the one-week and one-month milestones, and the first
day an occasion enters its window.

**Forced to zero** — a band set to countdown **Off**. A stated preference is not
a probability.

---

## The cue itself

The card's own sound plays. But an assigned card still keeps a **small
non-zero chance of a random cue** — 5% — so no card is ever entirely
predictable and nothing is completely off the table.

This is the one place where chance overrides something the owner chose, which
sits awkwardly beside the "Off is a hard rule" principle above. It is
deliberately small enough to read as a surprise rather than a malfunction, and
it is a stopgap: once a per-band chance exists (#16) this becomes that band's
own setting, defaulting to 5%, and the inconsistency goes away.

---

## `countdown why`

Making this probabilistic makes "it didn't play" unfalsifiable — which is the
position the demo failure left us in, and it would get *worse*, not better,
without a way to see the arithmetic.

```
magicmaker> countdown why
occasion    trip (The trip), 167 days, in window
countdown   base 15  +day 0  +pressure 40  ->  55%   [fired 2 taps ago]
cheeky      base  0  +pressure 0           ->   0%   [1st tap of this band]
forced      none
last tap    countdown rolled 71 -> no
```

A quiet device and a broken one look identical from the outside. This is what
tells them apart.

---

## What this deleted

Worth recording, because the simplification is the real win:

- `is_speaking_day()` and the `% 14` arithmetic — now just a lower `base`
- the per-band daily gate — now just "pressure was reset"
- `CHEEKY_AFTER 3` — now just cheeky's `step`

Three special cases became one mechanism with two numbers each. The per-occasion
day marks added to `cd_rec_t` in 1.4.0 are still there and still used, but they
stopped being a gate and became the first-of-day bonus — so nothing stored had
to change.
