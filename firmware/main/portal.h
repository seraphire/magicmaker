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
