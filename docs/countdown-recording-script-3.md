# Countdown recording — session 3 (after the trip)

Once the trip date passes, the device has nothing to say — and that's its
**permanent state** until someone sets a new date. This session fills that in.

**8 lines, one take, read in order.** Same rules: ~1 s pause between lines,
each line alone, **22050 Hz mono 16-bit**. These play on their own (not glued to
a number), so give them warmth and personality.

---

## Read in this order

### Post-trip lines (6) — `after-1` … `after-6`
```
1. Welcome home! I hope it was magical.
2. Wasn't that wonderful?
3. Did you have the best time?
4. The memories are the real magic.
5. Someday we'll go back!
6. Ready to plan the next adventure?
```

### The practical one (1) — `after-howto`
```
7. Ask a grown-up to set a new trip date, and I'll start counting again!
```
> This is the one that keeps the gift alive — it tells the family *how* to
> restart the countdown instead of leaving them with a device that's gone quiet.

### Count-up trailer (1) — `tail-since`
```
8. since our Disney trip
```
> This one is clever: it reuses the **numbers you already recorded** so the device
> can say *"twelve days since our Disney trip"* — a countdown that runs both ways
> for free, no new number recordings.

---

## How it'll behave after the trip

| Days since | Says |
|---|---|
| 1–31 | "*N* days **since our Disney trip**" + a post-trip line |
| 32–59 | "*N* weeks since our Disney trip" |
| 60+ | mostly the post-trip lines, with the "set a new date" nudge mixed in |

The nudge (`after-howto`) plays occasionally rather than every time, so it
reminds without nagging.

## After recording

1. Save the take over `assets\countdown-split\source.wav`
2. Splitter → set preset to **Session 3** → **Auto-label in order** (expect 8)
3. **Generate**, paste me the script, I'll cut it at the right rate
