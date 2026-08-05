// ---------------------------------------------------------------------------
// audio.h - I2S playback of WAV and MP3 files, MAX98357A friendly.
//
// A dedicated task owns the I2S channel. When nothing is playing it streams
// silence continuously (this is what keeps the amp from crackling - the same
// trick as the vintage-radio noise test). A play request interrupts the
// silence, streams the file, then returns to silence.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mounts SPIFFS, sets up I2S, and starts the audio task. Call once at boot.
void audio_init(void);

// Request playback of a clip (e.g. "/spiffs/chime.wav"). Non-blocking: it
// returns immediately and the file plays on the audio task. Marks the engine
// "playing" synchronously so audio_is_playing() is true on return.
//
// The extension in the path is a *logical* name. If that exact file isn't
// there, the other audio extension is tried - so a clip can be re-encoded from
// WAV to MP3 (or back) by the build script without touching the firmware, and
// without rewriting the paths already stored in NVS against enrolled bands.
void audio_play(const char *path);

// Resolve a logical clip path to the file actually on disk, trying the given
// path first and then the same name with the other audio extension. Writes the
// winner to `out` and returns true; returns false if neither exists. Callers
// that need to know whether a clip is present (rather than play it) use this
// so their idea of "missing" matches the player's.
bool audio_resolve(const char *path, char *out, size_t out_sz);

// ---------------------------------------------------------------------------
// Variant selection - the one place that knows "a name is a family of files".
//
// Three separate things were doing this and two of them were wrong in the same
// way. A logical name stands for every clip behind it:
//
//     chime.mp3, chime-1.mp3, chime-2.mp3     ->  "chime" picks one of three
//     cd/tail-1.mp3 ... cd/tail-13.mp3        ->  "cd/tail" picks one of thirteen
//     chime-2.mp3                             ->  "chime-2" is a family of ONE
//
// That last line is why the trailing "-N" is NOT stripped: a stored path
// pointing at a specific variant means "play exactly this", and pinning has to
// keep working. Nor does it confuse a name that merely contains hyphens -
// "be-our-guest" only splits on a suffix that is entirely digits.
//
// Enumerates by READING THE DIRECTORY, not by probing name-1, name-2 until
// something is missing. Probing silently truncates a family at the first hole:
// retire cd/tail-7 and tail-8 through tail-13 become unreachable, six of
// thirteen closers gone with no error anywhere. Packs can now retire files, so
// that hole is a thing a person can punch from another machine.
// ---------------------------------------------------------------------------
#define AUDIO_VARIANT_MAX 32

// Variant numbers present for `logical`, ascending, where 0 means the bare name
// (no -N suffix). Returns how many were written to `out`. Zero means nothing on
// disk answers to that name at all.
int audio_variants(const char *logical, uint8_t *out, int max);

// Concrete path for one variant number, resolved to whichever extension is
// actually on disk. False if that variant isn't there.
bool audio_variant_path(const char *logical, uint8_t variant, char *out, size_t out_sz);

// Pick one variant at random, avoiding `avoid` when the family has more than
// one member (pass 0xFF for "no preference"). Writes the concrete path.
// Returns the variant number chosen, or -1 if the family is empty.
int audio_pick_variant(const char *logical, uint8_t avoid, char *out, size_t out_sz);

// True from the moment audio_play() is called until the file finishes.
bool audio_is_playing(void);

// Loudness of what's coming out right now, 0-255, for driving LEDs from the
// sound. This is a peak-hold that decays a little on every read, so it tracks
// the rhythm of a voice rather than the waveform - poll it once per animation
// frame. Raw sample amplitude would strobe; syllables are what you want to see.
// Reads as 0 when nothing is playing.
uint8_t audio_level(void);

// Cut the current clip short and drop anything queued behind it. The streaming
// loops check this between buffer writes, so a long clip stops promptly instead
// of running to the end. A later audio_play() is unaffected.
void audio_stop(void);
