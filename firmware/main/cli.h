// ---------------------------------------------------------------------------
// cli.h - tiny serial command line over the console (for bench/testing).
//
// Starts an esp_console REPL with a handful of commands:
//   help           list commands
//   forget-wifi    clear Wi-Fi credentials (keep cards), reboot to setup
//   factory-reset  erase everything (Wi-Fi + cards + flags), reboot fresh
// ---------------------------------------------------------------------------
#pragma once

// Start the console REPL immediately (assumes a USB host is present).
void cli_start(void);

// Preferred: spawn a tiny supervisor that starts the REPL only once a USB host
// is actually connected. On a plain power supply (no host draining the console)
// an idle USB-Serial-JTAG REPL spins and starves the app, so we simply never
// start it until someone plugs in. Safe to call at boot regardless of CLI use.
void cli_start_when_connected(void);
