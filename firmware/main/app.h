// ---------------------------------------------------------------------------
// app.h - the few signals the main loop exposes to other modules.
//
// main.c owns the tap loop and the "moment" (sound + light show). Everything
// else here is a module with its own header; this is the small surface main.c
// offers back, rather than scattering extern declarations at call sites.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

// True while a moment is on screen - a sound playing with its animation, from
// the tap until the fade finishes.
//
// Used to keep heavy work off the animation: the network task waits for this
// before its startup update check (a TLS handshake at priority 5 lands on top
// of an animation loop at priority 1 and stutters it), and the web page's poll
// endpoint reports it so the page can back off while the lights are running.
bool app_moment_busy(void);

// Make the reader behave as though this band had just been tapped. Returns
// false if a tap is already queued, so a double-click cannot stack two.
//
// This hands the UID to the same slot a real tap uses when one arrives during a
// moment, which is the point: the show then runs on the main loop, in order,
// through the identical path - scan counter, countdown gating, cheeky on the
// third repeat, variant choice. A "play this file" shortcut would have tested
// none of that, and would have been running audio from the HTTP task while the
// LED loop was mid-frame.
bool app_play_band(const uint8_t uid[4]);
