#include "sounds.h"
#include <string.h>
#include <stdio.h>

// The assignable sound pool. System prompts in Program/ are intentionally NOT
// here - these are the "reward" sounds a band can be set to. Keep the ids short
// and stable (they're what the web stores/shows).
// id = stable storage key (never change it), label = friendly display name.
// NOTE: labels are best-guess from the filenames - tweak them to match what the
// clips actually say.
typedef struct { const char *id; const char *label; const char *path; anim_id_t anim; } sound_t;

static const sound_t S[] = {
    { "chime",      "Chime",              "/spiffs/chime.wav",        ANIM_CELEBRATE  },
    { "excellent",  "Excellent!",         "/spiffs/excellent.wav",    ANIM_FIREWORKS  },
    { "foolish",    "Foolish Mortals",    "/spiffs/foolish.wav",      ANIM_ENCHANTED  },
    { "hello",      "Hello World",        "/spiffs/hello.wav",        ANIM_WELCOME    },
    { "operational","All Systems Operational", "/spiffs/operational.wav", ANIM_CELEBRATE },
    { "startours",  "Star Tours",         "/spiffs/startours.wav",    ANIM_RAINBOW    },
    { "walt",       "Walt's Welcome",     "/spiffs/walt-welcome.wav", ANIM_WELCOME    },
    { "beourguest", "Be Our Guest",       "/spiffs/be-our-guest.wav", ANIM_BEOURGUEST },
};
#define N ((int)(sizeof(S) / sizeof(S[0])))

int         sound_count(void)  { return N; }
const char *sound_path(int i)  { return (i >= 0 && i < N) ? S[i].path  : NULL; }
const char *sound_id(int i)    { return (i >= 0 && i < N) ? S[i].id    : NULL; }
const char *sound_label(int i) { return (i >= 0 && i < N) ? S[i].label : NULL; }

anim_id_t sound_anim(const char *path)
{
    for (int i = 0; i < N; i++)
        if (strcmp(S[i].path, path) == 0) return S[i].anim;
    return ANIM_CELEBRATE;
}

bool sound_path_for_id(const char *id, char *out, size_t sz)
{
    if (!id) return false;
    if (strcmp(id, SOUND_RANDOM_ID) == 0) { if (sz) out[0] = '\0'; return true; }
    for (int i = 0; i < N; i++)
        if (strcmp(S[i].id, id) == 0) { snprintf(out, sz, "%s", S[i].path); return true; }
    return false;
}

bool sound_id_for_path(const char *path, char *out, size_t sz)
{
    if (!path || path[0] == '\0') { snprintf(out, sz, "%s", SOUND_RANDOM_ID); return true; }
    for (int i = 0; i < N; i++)
        if (strcmp(S[i].path, path) == 0) { snprintf(out, sz, "%s", S[i].id); return true; }
    return false;
}
