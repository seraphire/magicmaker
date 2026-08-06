// ---------------------------------------------------------------------------
// ota_pubkey.h - the public half of the OTA signing key.
//
// PUBLIC ON PURPOSE. This is what lets a device tell "the firmware Brian
// published" from "a valid firmware that arrived at this URL". The private half
// lives in BitWarden and, as a working copy, outside every repository - never in
// git, because history is permanent and the mirror to the public repo is a file
// copy done by hand.
//
// ECDSA P-256, DER SubjectPublicKeyInfo, as produced by
// System.Security.Cryptography.ECDsa.ExportSubjectPublicKeyInfo().
//
// Rotating this means shipping firmware, not changing a setting - which is also
// the recovery path if the private key is ever lost: hold the button, join the
// setup AP, upload a build carrying a new key from a phone. That upload is not
// signature-checked, deliberately, and cannot be: it goes through webota.c's
// ota_begin/write/finish, a different function from the manifest path entirely.
// The exemption is structural rather than a flag somebody must remember.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

static const uint8_t OTA_PUBKEY_DER[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
    0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
    0x42, 0x00, 0x04, 0xba, 0xb2, 0xc3, 0x40, 0xc3, 0xad, 0x67, 0x96, 0x6a,
    0x62, 0x69, 0xf6, 0xa1, 0x88, 0xdd, 0xd9, 0x14, 0xd6, 0x8b, 0xd2, 0x55,
    0x86, 0x95, 0xdc, 0x07, 0xbe, 0x7e, 0xbb, 0xc9, 0x6a, 0xa2, 0xe8, 0x55,
    0xa4, 0xe1, 0xe7, 0x67, 0x1a, 0x33, 0xb7, 0x0d, 0x8b, 0xc4, 0xce, 0x96,
    0x05, 0xd6, 0x91, 0xda, 0xed, 0xac, 0x24, 0x77, 0xe5, 0x04, 0xca, 0xed,
    0x29, 0x6d, 0x97, 0xa9, 0x06, 0x9e, 0x7f,

};
