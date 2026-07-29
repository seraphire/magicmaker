# MagicBand Reader — ESP32-S3 firmware

Firmware for the MakerWorld *Walt Disney World Inspired MagicBand Reader*.

**Stopgap build:** a push-button stands in for the PN532 NFC reader until it
arrives. Behaviour mirrors the Adafruit `code.py`:

> wait for a trigger → play a random Disney sound + run a green NeoPixel chase → repeat.

## Hardware / wiring (ESP32-S3, matches the vintage-radio board)

| Function            | GPIO | Notes                                            |
|---------------------|------|--------------------------------------------------|
| NeoPixel data       | 8    | Ring + Mickey chained on one line                |
| I2S BCLK → MAX98357A| 5    |                                                  |
| I2S WS  → MAX98357A | 4    |                                                  |
| I2S DOUT→ MAX98357A | 6    |                                                  |
| Button              | 7    | Wire between GPIO7 and **GND** (internal pull-up)|

All pins live in [`main/config.h`](main/config.h).

## Before it looks right: set your LED counts

The strip is two segments on one data line. Set the real pixel counts in
`main/config.h`:

```c
#define RING_LED_COUNT      24   // your ring's pixel count
#define MICKEY_LED_COUNT    8    // your Mickey outline's pixel count
#define RING_FIRST          1    // 1 = ring first in the chain, 0 = Mickey first
```

If the segments animate in the wrong order, flip `RING_FIRST`.

## Build & flash

```powershell
& C:\esp\v5.5.2\esp-idf\export.ps1        # activate ESP-IDF (this machine)
idf.py -C C:\DEV\Disney-Magic-Band\firmware build
idf.py -C C:\DEV\Disney-Magic-Band\firmware -p COMx flash monitor
```

`flash` writes the app **and** the sound files (SPIFFS `storage` partition) in
one shot. Press the button → you should hear a random sound and see the ring
chase. There's also a boot sound so you know the audio path works on power-up
(disable with `PLAY_BOOT_SOUND 0` in config.h).

## Swapping in the NFC reader later

The PN532 driver ([`main/pn532.c`](main/pn532.c)) and its wiring into
[`main/trigger.c`](main/trigger.c) are already written. To switch over:

1. In `config.h`, set `TRIGGER_USE_NFC 1`.
2. Wire the module in **I2C mode**: SDA→`PN532_SDA_GPIO` (10), SCL→`PN532_SCL_GPIO`
   (9), IRQ→`PN532_IRQ_GPIO` (11), plus 3V3/GND. Adjust those `#define`s to your
   actual pins.
3. Rebuild + flash. `main.c`, `audio.c`, and `leds.c` don't change.

Detection is **interrupt-driven** by default (`PN532_USE_IRQ 1`): the PN532
pulls IRQ low when a band is found, so the idle loop never busy-polls I2C. Set
`PN532_USE_IRQ 0` for a simpler polled read if you don't wire the IRQ pin.

Still hardware-untested (the reader is in transit) — expect to shake out a
wiring / mode-switch detail on first contact.

## File map

| File          | Role                                                          |
|---------------|---------------------------------------------------------------|
| `main.c`      | Orchestration loop (trigger → sound + animation)              |
| `config.h`    | All pins, LED counts, audio + idle options                    |
| `trigger.c/h` | Trigger source — button now, PN532 later                      |
| `audio.c/h`   | I2S playback of WAVs from SPIFFS + silence-keeper task        |
| `leds.c/h`    | Ring comet + Mickey pulse (reward), idle breathe              |
| `spiffs/`     | The 6 Disney WAVs (mono/16-bit/22050 Hz), flashed to SPIFFS   |
