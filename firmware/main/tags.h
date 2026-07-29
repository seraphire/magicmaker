// ---------------------------------------------------------------------------
// tags.h - map specific RFID/band UIDs to their own sound + animation.
//
// HOW TO ADD A TAG:
//   1. Build + flash, open the serial monitor, and tap the tag.
//   2. The monitor prints a ready-to-paste line, e.g.:
//        { {0x04,0xA1,0xB2,0xC3}, "/spiffs/hello.wav", ANIM_CELEBRATE },
//   3. Paste it into TAG_PROFILES below; change the .wav (must exist in
//      spiffs/) and the animation to taste.
//   4. Rebuild + flash. That band now has its own moment.
//
// Sounds: any 16-bit / 22050 Hz / mono .wav in the spiffs/ folder.
// Animations: ANIM_CELEBRATE (green), ANIM_WELCOME (gold),
//             ANIM_FIREWORKS (bursts), ANIM_RAINBOW (spin).
//
// Any tag NOT listed here plays a RANDOM sound + ANIM_CELEBRATE.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include "leds.h"          // anim_id_t

typedef struct {
    uint8_t     uid[4];    // the tag's 4-byte UID (as printed in the monitor)
    const char *sound;     // "/spiffs/xxx.wav"
    anim_id_t   anim;      // which animation to run
} tag_profile_t;

static const tag_profile_t TAG_PROFILES[] = {
    // --- Add your enrolled bands here. Examples (replace the UIDs!): ---
    // { {0x04,0xA1,0xB2,0xC3}, "/spiffs/walt-welcome.wav", ANIM_WELCOME   },
    // { {0x11,0x22,0x33,0x44}, "/spiffs/be-our-guest.wav", ANIM_RAINBOW   },
    // { {0xDE,0xAD,0xBE,0xEF}, "/spiffs/startours.wav",    ANIM_FIREWORKS },
};

#define NUM_TAG_PROFILES (sizeof(TAG_PROFILES) / sizeof(TAG_PROFILES[0]))
