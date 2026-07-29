# MagicMaker — Physical Test Checklist

Re-test pass after the big rework (security modes, non-blocking boot, console fix,
audio pitch, boot voice/visuals, sparkle-until-Wi-Fi, `.local` follows name).
Board on **COM8**. Trigger is the **RC522 reader** — "tap a card" = a real card on
the reader (the button is program/setup/idle-toggle).

**Legend:** `[USB]` = needs a serial monitor on the PC · `[CHARGER]` = just watch
the device on a plain charger, no PC · **[CRITICAL]** = most likely to catch a
regression.

## ⭐ The 4 that matter most
- [x] **[CRITICAL][CHARGER] Smooth idle on a charger** — on wall/battery power (no
  computer), the idle breathe is perfectly smooth, no stutter. (Proves the CLI
  REPL doesn't start without a USB host.)
- [x] **[CRITICAL][CHARGER] Interactive immediately** — tap a card during the first
  seconds of boot; it reacts right away, not frozen ~15 s while Wi-Fi joins.
- [x] **[CRITICAL][CHARGER] 16 kHz voice at correct pitch** — the voice clips are
  not chipmunked/sped up.
- [ ] **[CRITICAL][USB] `update-now` stays silent** — a manifest install reboots
  with NO update voice and NO greeting (quiet_boot); user pushes stay voiced.

---

## A. Boot & greeting
- [x] **[CHARGER] Provisioned boot:** "welcome to MagicMaker" + **blue sparkle**,
  and the **sparkle keeps twinkling until Wi-Fi connects** (no longer reverts to
  the calm breathe early). Face stays dark.
- [x] **[CHARGER] Online moment:** once joined, `operational.wav` + green celebrate,
  then it settles to the calm blue idle breathe.
- [x] **[USB] First boot / no Wi-Fi:** Walt dedication + welcome animation, then
  the setup prompt, then SoftAP `MagicMaker-<id>-Setup`.
- [ ] **connect to wifi autoloads setup screen**
- [X] **[CHARGER] Hold-through-power-on = setup:** hold the button while applying
  power → ~1.5 s solid **blue** cue → release → setup prompt + SoftAP, saved
  settings kept.
- [x] **[CHARGER] Setup idle look:** slow **cyan** breathe while in any AP/setup mode.

## B. Stability (console + non-blocking rework)
- [x] **[CRITICAL][CHARGER] Smooth idle on a charger** (see top).
- [x] **[CRITICAL][CHARGER] Interactive immediately at boot** (see top).
- [ ] **[CHARGER] Brown-out check:** run full-white animations (fireworks/celebrate)
  back to back on a modest USB port — no reset (brightness is now 120).

## C. Audio (per-file sample-rate retune)
- [X] **[CRITICAL][CHARGER] 16 kHz clips correct pitch:** `welcome-magic`,
  `wifi-setup`, `wifi-failed`, `update-start/done/failed`, `random`.
- [x] **[CHARGER] Rate switches back:** play a 16 kHz voice clip, then immediately a
  22050 Hz stinger (e.g. `startours`/`excellent`) — the stinger is not slowed.
- [x] **[CHARGER] Volume knob** follows smoothly mid-playback.

## D. LEDs
- [x] **[CHARGER] Idle toggle + persistence:** short-press toggles the blue breathe
  off/on; power-cycle remembers it.
- [x] **[CHARGER] Per-tag animations:** enrolled cards play their mapped show;
  unknown card = random (never same twice in a row).
- [x] **[CHARGER] Program mode:** ~3 s hold → amber breathe; short-press auditions;
  tap a card to assign → `saved` + `scan-now`, auto-exit.

## E. Wi-Fi & web config
- [ ] **[CRITICAL][USB] Normal page is config-ONLY:** at `http://<name>.local/`,
  device name + trip date editable; Wi-Fi/manifest **disabled** + locked note;
  `GET /update` → **403**.
- [ ] **[USB] `.local` follows the name (NEW):** set the device name on the page,
  reboot, and confirm it's reachable at `http://<new-name>.local/`. (Unnamed /
  default units stay `magicmaker-<id>.local`.)
- [ ] **[USB] Normal-mode save:** change name/trip date → "Settings updated", no
  reboot; smuggled ssid/pass/manif fields ignored.
- [ ] **[CHARGER] SoftAP captive portal:** join `MagicMaker-<id>-Setup`; phone pops
  the sign-in sheet; full portal shows Wi-Fi + manifest + firmware upload + Forget/
  Factory.
- [ ] **[USB] Save Wi-Fi in setup → joins,** then plays the operational moment.
- [ ] **[CRITICAL][CHARGER] Bad Wi-Fi → `wifi-failed.wav` → setup:** wrong password
  or out of range → after the join times out, `wifi-failed` + drop to setup.

## F. OTA
- [ ] **[USB] Web upload (setup mode):** upload a `.bin` at `/update` → `update-start`
  during upload → reboot → `update-done` next boot.
- [ ] **[USB] `ota-url <url>`:** `update-start` → reboot → `update-done`.
- [ ] **[CRITICAL][USB] `update-now` silent** (see top).
- [ ] **[USB] `update-mode` unlocks LAN upload:** run it, then `/update` serves (not
  403); reboot re-locks.
- [ ] **[USB] Rollback:** push an image that crashes early → bootloader auto-reverts.

## G. CLI (all `[USB]` — REPL only exists with a host attached)
- [x] **[USB] CLI starts ~0.5 s after USB connect:** log "USB host detected —
  starting serial CLI"; `magicmaker>` prompt works; `help`.
- [x] **[USB] `status` / `time`** (note: test network blocks NTP-123 → HTTP-Date).
- [x] **[USB] `selftest`** all pass.
- [x] **[USB] `forget-wifi`** keeps cards → setup;
- [x] **`factory-reset`** wipes all.

---
### Suggested order
1. Charger-only stability (B + smooth idle) — highest risk, no PC needed.
2. Audio pitch (C).
3. Boot-mode matrix (A).
4. PC/USB session for E, F, G.
