// ---------------------------------------------------------------------------
// sounds.h - the assignable sound "actions" in one place, shared by the tap
// loop (main.c) and the web band-manager (portal.c) so they never drift.
//
// Each sound has a short id ("chime"), a file path, and the animation that
// plays with it. "random" is a first-class action too: a band assigned the
// random action is stored with an EMPTY sound value and plays a random pick on
// each tap (while still being a named band with its own countdown settings).
// ---------------------------------------------------------------------------
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "leds.h"          // anim_id_t

#define SOUND_RANDOM_ID "random"   // the "surprise me" action; stored as ""

int         sound_count(void);
const char *sound_path(int i);     // "/spiffs/chime.wav", or NULL if out of range
const char *sound_id(int i);       // "chime" (stable storage key), or NULL
const char *sound_label(int i);    // "Chime" (friendly display name), or NULL

// Animation that goes with a sound path (ANIM_CELEBRATE if it's not listed).
anim_id_t   sound_anim(const char *path);

// id -> stored sound value: a known id yields its path; "random" yields "" (the
// random sentinel). Returns false for an unknown id (so a crafted POST can't
// inject an arbitrary path).
bool        sound_path_for_id(const char *id, char *out, size_t sz);

// Stored sound value -> id for display: "" (or NULL) yields "random"; a known
// path yields its id. Returns false for an unknown path.
bool        sound_id_for_path(const char *path, char *out, size_t sz);
