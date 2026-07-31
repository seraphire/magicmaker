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

// True from the moment audio_play() is called until the file finishes.
bool audio_is_playing(void);

// Cut the current clip short and drop anything queued behind it. The streaming
// loops check this between buffer writes, so a long clip stops promptly instead
// of running to the end. A later audio_play() is unaffected.
void audio_stop(void);
