// ---------------------------------------------------------------------------
// verscmp.h - dotted numeric version comparison (for OTA manifest checks).
//
// Pure, dependency-free (no ESP-IDF) so it can be unit-tested off-target too.
// ---------------------------------------------------------------------------
#pragma once

// Compare two dotted versions component-by-component, numerically.
//   version_cmp("1.10.0", "1.9.0")  > 0   (10 > 9, not string order)
//   version_cmp("1.0.0",  "1.0")   == 0   (missing trailing parts read as 0)
//   version_cmp("v1.2.3", "1.2.3") == 0   (a leading 'v' is ignored)
// Returns <0 if a<b, 0 if equal, >0 if a>b.
int version_cmp(const char *a, const char *b);
