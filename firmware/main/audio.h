// ---------------------------------------------------------------------------
// audio.h - I2S playback of WAV files from SPIFFS, MAX98357A friendly.
//
// A dedicated task owns the I2S channel. When nothing is playing it streams
// silence continuously (this is what keeps the amp from crackling - the same
// trick as the vintage-radio noise test). A play request interrupts the
// silence, streams the file, then returns to silence.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

// Mounts SPIFFS, sets up I2S, and starts the audio task. Call once at boot.
void audio_init(void);

// Request playback of a WAV file (e.g. "/spiffs/chime.wav"). Non-blocking:
// it returns immediately and the file plays on the audio task. Marks the
// engine "playing" synchronously so audio_is_playing() is true on return.
void audio_play(const char *path);

// True from the moment audio_play() is called until the file finishes.
bool audio_is_playing(void);

// Cut the current clip short and drop anything queued behind it. The streaming
// loops check this between buffer writes, so a long clip stops promptly instead
// of running to the end. A later audio_play() is unaffected.
void audio_stop(void);
