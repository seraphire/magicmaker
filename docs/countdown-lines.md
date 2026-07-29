# Countdown lines — what's recorded, what to add

The composer builds a sentence from parts, so **variety comes from adding files,
not changing firmware**. Drop a new `lead-5.wav` in and it joins the rotation
automatically — including over the air.

## Sentence shape

```
[lead-in]  <count>  <unit>  [trailer]
"It's only"  "twenty three"  "days"  "until our Disney trip!"
```

- **count + unit** always plays.
- **lead-in** plays sometimes (~1 in 3) so it doesn't get predictable.
- **trailer** plays most times, picked at random.
- `today` / `tomorrow` replace count+unit entirely, and still take a trailer.

## Already recorded (51 clips)

| Pool | Files | Content |
|---|---|---|
| numbers | `1.wav`–`31.wav` | one … thirty one |
| units | `day` `days` `week` `weeks` `month` `months` | |
| specials | `today` `tomorrow` | |
| lead-ins | `lead-1`–`lead-4` | "it's only" ×3, "it's just" |
| trailers | `tail-1`–`tail-6` | "to go", "until our Disney trip", "until the magic", "until we meet the mouse", "until we leave", "left to wait" |
| flavor | `flavor-1` `flavor-2` | "soon", "it's coming" — don't fit before a number; used on their own |

---

## Worth adding — more lead-ins  (`lead-5.wav`, `lead-6.wav`, …)

Anything that reads naturally straight into a number:

```
just                       →  "Just twenty three days to go!"
only                       →  "Only three days!"
we're down to              →  "We're down to five days!"
that's                     →  "That's twelve days until the magic!"
would you believe          →  "Would you believe nine days?"
guess what                 →  "Guess what — four days to go!"
oh boy                     →  "Oh boy! Six days!"
hold on to your ears       →  "Hold on to your ears — two days!"
```

## More trailers  (`tail-7.wav`, …)

Anything that reads naturally after a unit word:

```
to go
and counting
until we're there
until the castle
until we see the fireworks
until our trip
so get packing!
can you wait?
I can't wait!
almost time!
```

## Cheeky repeat-scan lines  (`cheeky-1.wav`, `cheeky-2.wav`, …)

Played when the **same band** is tapped several times in a row — instead of the
normal countdown, as a little wink. Keep them short and good-natured.

```
Again?
Still the same number!
It hasn't changed since last time.
Tapping more won't make it sooner.
Okay, okay — I heard you the first time.
You really want to go, don't you?
Patience! The mouse isn't going anywhere.
I promise I'm counting.
Nope, still the same.
That's the third time, you know.
Are you trying to speed it up?
Save some magic for later!
```

Record 4–8 of these; more = less repetition. They're the most-heard lines in the
whole device, so keep them warm rather than snarky.

## Optional: milestone reactions

Extra spice for the big moments — these replace the trailer when the count hits
a landmark:

```
one-week.wav      →  played at exactly 7 days:  "One week! Start packing!"
one-month.wav     →  at ~30 days
final-countdown   →  at 3, 2, 1 days: "It's almost here!"
```

## Recording rules (unchanged)

One take · ~1 s pause between lines · each line alone · flat, even delivery ·
**22050 Hz, mono, 16-bit** (matches the existing bank — don't switch back to 16k,
mixing rates causes a retune click between words).

Drop the take at `assets/countdown-split/source.wav`, open the splitter, label,
Generate.
