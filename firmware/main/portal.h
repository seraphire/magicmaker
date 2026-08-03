// ---------------------------------------------------------------------------
// portal.h - captive-portal provisioning (Stage 1).
//
// Brings up an HTTP config page plus a DNS catch-all so a phone joining the
// SoftAP is steered straight to the setup form. Three tiers, per the roadmap:
//   1. typed IP  -> 192.168.4.1 always works
//   2. DNS catch-all -> any hostname resolves to us (the guarantee)
//   3. OS-probe handlers -> phone auto-opens the page (best-effort)
//
// Saving the form writes the new config to NVS and reboots the device so it
// comes back up and tries the freshly entered network.
// ---------------------------------------------------------------------------
#pragma once
#include <stdbool.h>

// Start the HTTP config server. Serves the same page in both modes.
//   ap_mode = true  (setup/recovery over the SoftAP): DNS catch-all + captive
//             redirect, and the setup-only fields (Wi-Fi creds, manifest URL,
//             firmware upload, Forget/Factory) are live.
//   ap_mode = false (normal run over the home LAN): plain config server - those
//             setup fields are disabled in the page and rejected server-side.
// Call after wifi_start_ap() (AP) or after joining the home network (STA).
void portal_start(bool ap_mode);

// Stop both servers (rarely needed; a save reboots instead).
void portal_stop(void);

// True once a browser has actually RENDERED the setup page, so the spoken
// "visit 192.168.4.1" prompt knows to stop repeating.
//
// Deliberately not "the page was served": in AP mode every URL returns the page
// (that's what makes the captive sheet pop), so a phone's connectivity probe
// fetches it within a second of joining - before the human has seen anything. A
// probe reads the response and stops; only a browser laying the page out goes
// back for /logo.png. That second request is the one that means someone is
// looking.
bool portal_page_seen(void);

// Forget that, so a newly joined phone gets told where to go even if an earlier
// one already found the page.
void portal_clear_page_seen(void);
