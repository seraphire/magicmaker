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

// Scan the filesystem for assignable sounds. Call once, after the data
// partition is mounted and before anything asks for the list.
//
// The list used to be a compiled-in array, which meant the device could only
// ever report what it was BUILT with - drop a new pack on the filesystem and
// nothing, including the web page, knew it was there.
void sounds_init(void);

// The list is of GROUPS, not files: "chime" covers chime.wav plus any chime-1,
// chime-2 beside it, and one tap picks between them. That keeps a twelve-clip
// pack from flooding the dropdown with twelve entries.
int         sound_count(void);
const char *sound_path(int i);     // "/spiffs/chime.wav", or NULL if out of range
const char *sound_id(int i);       // "chime" (stable storage key), or NULL
const char *sound_label(int i);    // "Chime" (friendly display name), or NULL
int         sound_variants(int i); // how many clips are behind the group

// Animation that goes with a sound path (ANIM_CELEBRATE if it's not listed).
anim_id_t   sound_anim(const char *path);

// id -> stored sound value: a known id yields its path; "random" yields "" (the
// random sentinel). Returns false for an unknown id (so a crafted POST can't
// inject an arbitrary path).
bool        sound_path_for_id(const char *id, char *out, size_t sz);

// Stored sound value -> id for display: "" (or NULL) yields "random"; a known
// path yields its id. Returns false for an unknown path.
//
// Both lookups strip a trailing "-N" first, so a band pinned to a specific
// variant still answers as its group - and, more importantly, keeps its
// animation. Matching the exact path meant a pinned variant fell through to the
// default, which reads as a lighting bug rather than a naming one.
bool        sound_id_for_path(const char *path, char *out, size_t sz);

// Turn a stored sound value into the clip that should actually play. The stored
// value is a LOGICAL base name - "/spiffs/chime.wav" means chime or any of its
// numbered variants - and one is chosen at random per call.
//
// Called at tap time, never at enrollment: picking once when the band is set up
// would lock it to whichever clip happened to exist that day. Writes the input
// through unchanged if nothing resolves, so a missing file fails the way it
// always did.
void        sound_pick(const char *logical, char *out, size_t sz);
