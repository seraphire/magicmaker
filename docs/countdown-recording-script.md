# Countdown recording script

One take, read in order. The splitter cuts it up and labels in order, so **the
order matters more than anything else**.

Design: the count switches units as the trip gets closer, so every spoken number
stays between 1 and 31 and **nothing is ever concatenated mid-number**. No
"forty" + "one" seams - each number is one natural recording.

| Days out | Says | Numbers used |
|---|---|---|
| 0 | "today" | - |
| 1 | "tomorrow" | - |
| 2-31 | "*N* days" | 2-31 |
| 32-59 | "*N* weeks" | 5-8 |
| 60+ | "*N* months" | 2-12 |

## How to record

- **One continuous take.** Don't stop/export per word - that's what broke last
  time. Exporting the whole timeline is fine now.
- **Pause ~1 second between words.** Long, clean gaps are what let the splitter
  find boundaries without guessing. Silence, not "umm".
- **Say each item alone.** Just "twenty three" - *not* "twenty three days". The
  device adds the unit.
- **Flat, even delivery.** Read it like a list. A word that swoops down at the
  end sounds wrong when the unit follows it.
- **Fluffed one?** Pause, say it again, carry on - keep the order. You'll see
  both takes in the splitter and can drop the bad one.
- Format: **16 kHz, mono, 16-bit WAV** (same as before - that part was right).

## The list — read in this order

**Numbers 1-31 (31 items)** — each as one natural phrase, e.g. "twenty one"
```
one, two, three, four, five, six, seven, eight, nine, ten,
eleven, twelve, thirteen, fourteen, fifteen, sixteen, seventeen, eighteen, nineteen, twenty,
twenty one, twenty two, twenty three, twenty four, twenty five,
twenty six, twenty seven, twenty eight, twenty nine, thirty, thirty one
```

**Units (6)**
```
day, days, week, weeks, month, months
```

**Specials (2)**
```
today, tomorrow
```

```
until our Disney trip!
until the magic!
until we meet the mouse!
to go!
until we leave!
left to wait!
```

```
It's only
Just
```

That's **39 clips** and it covers the whole countdown with no seams.

> `day`, `week` and `month` (singular) are only used if a count of exactly 1 ever
> lands in that unit. Record them anyway - they're two seconds and it avoids a
> silent gap in an edge case.

## Optional extras (record after the 39, same rules)

The number + unit alone gives "twenty three days" with nothing around it. These
turn it into a sentence, and give the tiers something to say. Record a few
trailers so it varies:

- **Trailers** (after the count): `until our Disney trip!` · `to go!` ·
  `until we leave!` · `left to wait!`
- **Lead-ins** (before the count): `Only` · `Just`




Label these in the splitter with those exact names; anything you skip simply
isn't used.

## After recording

1. Drop the take at `assets/countdown-split/source.wav`
2. Open the splitter, hit **Auto-label in order**, spot-check a few, **Generate**
3. Run the printed script -> `named/*.wav`

The splitter will tell you if it found a number of segments other than 39, which
is your cue that a gap was too short or there's a retake to drop.
