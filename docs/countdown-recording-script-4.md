# Countdown recording — session 4 (the big days)

Day zero is the payoff of a six-month countdown, and right now it just says
"Today." This session replaces that with **complete announcements**.

These are whole sentences, so the device plays them on their own — no trailer
glued on afterwards. That means you can be as exuberant as you like; nothing has
to line up grammatically with anything else.

**8 lines, one take, read in order.** ~1 s pause between lines,
**22050 Hz mono 16-bit**. Big energy on these — especially the first five.

---

## Read in this order

### Day of the trip (5) — `today-1` … `today-5`
```
1. Today's the day! We're going to Disney!
2. It's here! It's finally here!
3. Wake up, wake up! Today is the day!
4. This is it! Today we go see the mouse!
5. The magic starts today!
```

### The day before (3) — `tomorrow-1` … `tomorrow-3`
```
6. Tomorrow's the big day!
7. Just one more sleep!
8. Tomorrow! Can you believe it?
```

---

## How they're used

| Day | Plays |
|---|---|
| 1 | `preamble-dayof` + a random `tomorrow-N` |
| 0 | `preamble-dayof` + a random `today-N` |

Both get the excited `preamble-dayof` music first, and the fireworks animation.

If a pool is empty the device falls back to the old bare `today.wav` /
`tomorrow.wav`, so nothing breaks if you only record some of these.

> Five variants for day zero may look like overkill for a day that happens once
> — but it's the day the band gets tapped over and over by an excited kid, and
> the 3rd tap onward is cheeky lines. Variety here is worth it.

## After recording

1. Save the take over `assets\countdown-split\source.wav`
2. Splitter → preset **Session 4** → **Auto-label in order** (expect 8)
3. **Generate**, paste me the script
