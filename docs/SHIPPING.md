# SHIPPING — how to get a change onto a device

Read this before publishing anything. It exists because the two halves of this
system behave **oppositely**, and getting them the wrong way round is the
mistake that's easy to make six months from now.

---

## The one thing to remember

> ### Firmware needs a go-live switch. Audio does not.
>
> Pushing firmware source does **nothing** until you publish the manifest.
> Deploying audio **is** the change — devices take it within 6 hours, with no
> version bump and no second step.
>
> Once a reader is in someone else's house, `publish-pack.ps1` edits a live
> object sitting on their counter. `-WhatIf` exists for exactly that.

---

## What did you change?

| You changed | Run | Reaches devices |
|---|---|---|
| **Audio** (a clip, the countdown bank) | `tools\publish-pack.ps1` | within 6 h, automatically |
| **Firmware** (any `.c` / `.h`) | `tools\release.ps1` + 3 publish steps | within 6 h, **after** step 3 |
| **The config page** (`spiffs/www/`) | ship it as a firmware release asset | with the release |
| **Nothing yet — just testing** | flash over USB | that device only |

---

## A. Audio only

The common case. New clip, re-recorded line, retiring a sound.

```powershell
# 1. put masters in assets\audio-src\   (never edit firmware\spiffs by hand)
.\tools\build-audio.ps1          # normalise + encode -> firmware\spiffs\

# 2. hear it before anyone else does
.\tools\publish-pack.ps1 -WhatIf # builds, deploys nothing

# 3. go
.\tools\publish-pack.ps1
```

To retire a file from every device: `-Remove old-clip.mp3`. Keep it listed until
every reader has synced once — removals are explicit, so a device that misses
the window keeps the file forever.

**No version to bump.** The pack version is derived from the content, including
the removal list. Change anything and it changes.

**Where the work happens:** `assets\audio-src\` in this repo. `magicmaker-host\`
is generated output and is overwritten on every publish — there's a README there
saying so.

---

## B. Firmware

Four steps, and **the order matters**. Steps 1 and 2 are inert; step 3 is the
switch.

```powershell
# 0. bump FW_VERSION in firmware\main\config.h  (release.ps1 will do it for you)

# 1. source to the public repo's main
#    (mirror from workshop first - see "Two repos" below)

# 2. build + stage the manifest
.\tools\release.ps1 -Version X.Y.Z -Repo seraphire/magicmaker `
                    -Assets www/index.html -ManifestOut ..\MagicMaker-ota

# 3. the binary — this ALSO creates the vX.Y.Z tag the manifest points at
gh release create vX.Y.Z release\firmware.bin --title "vX.Y.Z"

# 4. LAST — the go-live switch
git -C ..\MagicMaker-ota add manifest.json
git -C ..\MagicMaker-ota commit -m "publish vX.Y.Z"
git -C ..\MagicMaker-ota push
```

Before step 4, verify the things a device is about to trust:

```powershell
# both should be 200, and the hashes must match the manifest
curl.exe -sL -o fw.bin https://github.com/<repo>/releases/download/vX.Y.Z/firmware.bin
(Get-FileHash fw.bin -Algorithm SHA256).Hash.ToLower()
```

A hash mismatch means every device refuses the update. Cheaper to find here.

---

## Two repos, and what must never cross

| Repo | What it is |
|---|---|
| `workshop\` | **Private.** Source of truth. Real names, SSID, live URLs. |
| `MagicMaker\` | **Public mirror.** What the world sees. Audio is gitignored. |
| `MagicMaker-ota\` | Public, orphan `ota` branch. Holds only `manifest.json`. |

Mirroring is a **copy, not a push** — the two have different history. Copy the
changed files, then scan **every staged file**, not just the ones you expect to
be risky:

```powershell
# from C:\DEV\Disney-Magic-Band — audits the ENTIRE public repo, not just a diff
$pat = Get-Content workshop\docs\private-patterns.txt | Where-Object {$_}
Get-ChildItem MagicMaker -Recurse -File -Include *.md,*.c,*.h,*.ps1,*.html |
  ForEach-Object {
    $h = Select-String -Path $_.FullName -Pattern $pat -List -ErrorAction SilentlyContinue
    if ($h) { "LEAK  $($_.Name) : $($h.Line.Trim())" }
  }
```

Scanning the whole repo rather than the staged diff costs nothing and catches
what a diff can't: something that leaked in an earlier session and has been
sitting there since.

The pattern list is in `private-patterns.txt`, which is **never mirrored** — a
denylist of names, SSIDs and secret URLs publishes every one of them the moment
it goes public.

The list holds only strings that genuinely identify a person or a secret. `COM8`
was in it briefly and came out: it identifies nothing, it's been public for
months, and a check that fires on something harmless trains you to skim past the
one time it matters.

**Never copy these two:**

- `docs\roadmap-wifi-ota.md` — the public copy is deliberately sanitised of the
  SSID and real names. Overwriting it leaks both.
- `docs\deployment-live.md` — the pack host and secret path. That URL **is** the
  access control.
- `docs\private-patterns.txt` — the denylist itself. Publishing it publishes
  everything on it.

---

## Traps we already fell into

**`idf.py flash` wipes the audio.** It rewrites the storage partition. The files
come back from `firmware\spiffs`, but `installed.json` doesn't — the device then
re-verifies against flash and adopts what's there (~5 s). Use `app-flash` when
you want to keep the filesystem as-is.

**Assets are served from the tag, not `main`.** A manifest pins each file by
sha256; branches move, tags don't. If you ever hand-edit a manifest, don't
"helpfully" point it at `main`.

**Don't touch DTR/RTS on the serial port.** They're the ESP32-S3's bootloader
straps. Asserting them drops the chip into the ROM bootloader and it goes dark.

**A sync during a moment cuts the audio off mid-word.** Handled now — the CLI
waits — but it's why a sync fired seconds after boot used to truncate the
greeting.

**Publishing reaches your bench device too.** Its manifest URL is the real one,
so it will install and reboot on its own. "Publish" and "test on the bench" are
no longer separable.

---

## Checking what a device actually has

Over serial (USB cable in the socket):

```
assets-url            # which pack it points at, and the applied version
sync-media            # full verify against flash; restores anything missing
update-check          # is newer firmware published?
status                # version, Wi-Fi, time, free heap
```

`sync-media` with no argument uses the configured pack. It **forces** a full
verify rather than trusting the recorded version, so it's the right thing to run
when a sound has gone missing.

---

## Setting up a new device

1. Hold the button while powering on → setup mode (its own Wi-Fi network)
2. Join it, set the home Wi-Fi, save — it reboots onto the network
3. Point it at its audio, over serial:
   ```
   assets-url https://<host>/<secret>/pack.json
   sync-media
   ```
4. **Decide about firmware updates.** `manifest_url` defaults to a placeholder,
   which means the device never self-updates firmware. Leave it dormant and you
   must be physically present to change firmware, ever. Set it (setup mode only)
   and they get fixes automatically. Audio syncs either way.
5. Enrol the real bands, and remove any test enrolments.

Real values for step 3 are in `deployment-live.md` — private, never mirrored.
