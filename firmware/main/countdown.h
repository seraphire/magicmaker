// ---------------------------------------------------------------------------
// countdown.h - Disney trip countdown, spoken by composing recorded clips.
//
// The clip bank lives in /spiffs/cd/. Rather than one recording per possible
// count, a phrase is assembled from parts:
//
//     [lead-in]   <number>   <unit>   [trailer]
//     "It's only"  "twenty three"  "days"  "until our Disney trip!"
//
// Every spoken number is 1-31 and recorded whole ("twenty three" is one file),
// so nothing is ever concatenated mid-number - the unit switches to weeks and
// then months as the trip gets further out to keep it that way.
//
// Pools are DATA-DRIVEN: the code probes lead-1.wav, lead-2.wav, ... until one
// is missing. Drop in another clip (or ship one over the air) and it joins the
// rotation with no firmware change.
//
//   1..31.wav                  numbers, each a whole natural phrase
//   day/days/week/weeks/       unit words
//     month/months.wav
//   today.wav tomorrow.wav     replace number+unit entirely
//   lead-N.wav                 optional openers ("it's only", "guess what")
//   tail-N.wav                 optional closers ("to go!", "until the castle")
//   tail-since-N.wav           closers for counting UP after the trip
//   after-N.wav                post-trip lines; after-howto.wav = set-a-new-date
//   mile-week/month/final.wav  landmark closers that replace the trailer
//   cheeky-N.wav               played when one band is tapped repeatedly
//
// Needs the clock set (no RTC); with no clock everything here is a no-op.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CD_PATH_MAX  64
#define CD_MAX_CLIPS 6      // longest phrase: lead + number + unit + trailer

// Assemble the phrase for a given day count (negative = trip is in the past).
// Fills `paths` and the animation to run. Returns the number of clips, or 0 if
// the bank is missing the pieces. No gating - used by the CLI tester.
int countdown_build(int days, char paths[][CD_PATH_MAX], int max, uint8_t *anim_out);

// The full per-tap decision. Returns the number of clips to play (0 = nothing):
//   - repeated taps of the same band -> a random cheeky line
//   - otherwise, once per day per band (or per the band's mode) -> the countdown
// Records the greeting, so calling it twice in a day yields 0 the second time.
// `uid_len` 0 (the button) counts as a single pseudo-band.
int countdown_due(const uint8_t *uid, uint8_t uid_len,
                  char paths[][CD_PATH_MAX], int max, uint8_t *anim_out);

// Days from local-today to the trip date. INT_MIN if the clock isn't set;
// negative means the trip has passed.
int countdown_days_remaining(void);

// Human-readable tier for a day count ("today", "days", "weeks", "after", ...).
const char *countdown_tier_name(int days);

// Per-band countdown mode: 0 = daily (once per day, default), 1 = always (every
// tap), 2 = off. Keyed by 8-hex UID.
int  countdown_get_mode(const char *uid_hex);
void countdown_set_mode(const char *uid_hex, int mode);

// Clear a band's "already greeted today" mark so the next tap re-fires it.
void countdown_reset_today(const char *uid_hex);

// Same, for every band. Call when the trip date changes so the new count is
// heard on the next tap instead of being hidden by the daily gate until tomorrow.
void countdown_reset_all(void);
