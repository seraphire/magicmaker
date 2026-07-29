# Shipping media (and web assets) over the air

Firmware and media now update from **one manifest**. `esp_https_ota` only swaps
the app slot, so audio/animations/web files ride along as a separate **assets**
section that syncs onto the LittleFS data partition. Only files whose `sha256`
differs from what's already installed are downloaded — the device tracks that in
`/spiffs/installed.json`.

## Manifest format

```json
{
  "manifest_url": "https://newhost/magicmaker/manifest.json",
  "firmware": {
    "version": "1.0.2",
    "url": "http://<host>:8000/firmware.bin"
  },
  "assets": {
    "base_url": "http://<host>:8000/assets/",
    "files": [
      { "path": "hello.wav",                  "sha256": "…", "bytes": 12030 },
      { "path": "Program/welcome-magic.wav",  "sha256": "…", "bytes": 47554 }
    ],
    "remove": [ "ota-test.wav", "Program/retired-prompt.wav" ]
  }
}
```

- A file lands on flash at `/spiffs/<path>`. `<path>` is sanitized (no `..`, no
  leading `/`, restricted charset) so a manifest can't escape the filesystem.
- `remove` (optional) deletes files that are no longer needed — including baked-in
  ones. It's an **explicit list**, deliberately *not* "delete anything not in
  `files`", so a partial or wrong manifest can never wipe the device's audio.
  Keep a retirement listed until every device has synced once. Deleting a file a
  card still points at just means that card plays nothing.
- `sha256` is used for **both** change-detection and download integrity (media
  isn't code-signed like the firmware image, so the hash is the guard).
- `bytes` is optional; used for a free-space pre-check.
- The legacy flat firmware manifest (`{"version":…,"firmware_url":…}`) still
  parses — the `firmware` object just takes precedence when present.
- `manifest_url` (optional, top-level) **relocates the device**: on the next
  `update-now`/scheduled check, if this differs from the URL the device is
  configured with, the device saves it and every future check goes there. Use it
  to migrate the manifest host with no reflashing — point the old host's manifest
  at the new one, let devices sync once, then retire the old host. It's persisted
  only (not re-fetched the same run), so an A↔B mistake can't loop. `update-check`
  shows a pending relocation but doesn't apply it.

## How it behaves

- **Assets sync first, firmware installs last.** New firmware may reference a
  new sound, so the media lands before the app slot switches. If any asset fails
  to fetch/verify, the firmware install is **deferred** (old firmware ignoring a
  new file is harmless; new firmware missing its media is not).
- Each file streams to `<name>.new`, is hash-verified, then **atomically
  renamed** over the original — a power cut never leaves a half-written asset.
- `installed.json` is updated per file, so an interrupted sync just resumes.
- Assets-only changes need **no reboot** — the new sound plays on the next tap.

## Triggering an update

- `update-now [manifest-url]` — full update: sync assets, then install newer
  firmware and reboot (silent / quiet-boot). Defaults to the configured
  manifest URL.
- `sync-media [manifest-url]` — sync **only** the assets, no firmware, no reboot.
  Handy for shipping/testing new sounds without bumping the firmware version.

## Building a manifest (PowerShell)

```powershell
$dir = "C:\path\to\host"           # served by: python -m http.server 8000 --directory $dir
$assets = "$dir\assets"
$files = Get-ChildItem $assets -Recurse -File | ForEach-Object {
  $rel = $_.FullName.Substring($assets.Length + 1) -replace '\\','/'
  '    { "path": "' + $rel + '", "sha256": "' +
    (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() +
    '", "bytes": ' + $_.Length + ' }'
}
@"
{
  "firmware": { "version": "1.0.1", "url": "http://<host>:8000/firmware.bin" },
  "assets": {
    "base_url": "http://<host>:8000/assets/",
    "files": [
$($files -join ",`n")
    ]
  }
}
"@ | Set-Content "$dir\manifest.json" -Encoding UTF8
```

Then serve it and run `sync-media http://<host>:8000/manifest.json` (or
`update-now …`) on the device.

## Notes / limits

- Max 64 files per manifest; a `bytes` free-space guard keeps ~64 KB headroom.
- Deferred optimization: a build-time step could bake `installed.json` into the
  flashed image so a factory-fresh unit already "knows" its assets and pulls
  nothing on the first check. For now the first asset sync re-pulls the listed
  files once.
