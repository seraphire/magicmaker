# Shipping media (and web assets) over the air

`esp_https_ota` only swaps the app slot, so audio, animations and web files sync
separately onto the LittleFS data partition. Only files whose `sha256` differs
from what's already installed are downloaded — the device tracks that in
`/spiffs/installed.json`.

## Two manifests, not one

This design originally put firmware and assets in a single manifest. That is
wrong for this project, for a reason that has nothing to do with convenience:

**the firmware manifest is public, so anything listed in it is published.** There
is no unlisted state. The voice bank was generated on a free-tier account that
permits personal use only (see `audio-roadmap.md`), so it cannot go in that file
— and neither can a clip recorded for one particular child's birthday.

So there are two, with two settings:

| Setting | Points at | Default |
|---|---|---|
| `manifest_url` | the public firmware manifest — same for every unit | the project's `ota` branch |
| `assets_url` | an audio manifest, **per device** | empty — no asset sync at all |

Per-device is the point. One reader can carry a birthday set that no other reader
and no public repository ever sees, and the owner of each unit chooses whether
any of this happens at all.

Both URLs are **setup-mode only** — hold the button while powering on. That's a
stricter gate than the custom-audio upload endpoint gets, and deliberately:
upload can add a file beside the reward sounds, whereas a pack URL is a
*redirect*. Point it somewhere else and the next sync rewrites `Program/` and
`cd/` wholesale, in the device's own voice. A contained blast radius is what
earned upload its LAN access; a pack URL has none.

Adding the setting is free: `appcfg` stores one NVS key per field, so a new key
is a line with a default and older devices simply do not have it. That is the
property `data-model.md` argues for, and this is its first use.

## The coupling the split gives away, and how to get it back

One manifest was accidentally protecting us. A device could not take new assets
without also taking the firmware that understood them — which matters, because a
bank built for 1.1.0 has no `6.mp3` or `months.mp3`, and older code asking for
those finds nothing and goes quiet with no error.

Split them and that protection is gone. So **declare the dependency instead of
implying it**: the assets manifest names a minimum firmware, and the device skips
the sync until it qualifies.

```json
{ "requires_fw": "1.1.0", "assets": { … } }
```

This is strictly better than what it replaces. The old coupling was invisible and
worked by accident; a version gate is explicit, testable, and can say *why* it
declined rather than leaving a silent device.

A device that is behind should log and retry later, never partially apply — half
a bank is worse than none.

## Manifest format

The firmware manifest (`manifest_url`) is unchanged and stays public. The assets
manifest (`assets_url`) is its own document, fetched separately, and is the one
that may hold content nobody else should see.

```json
{
  "requires_fw": "1.1.0",
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
- `requires_fw` gates the whole document. Behind it, log and retry later; never
  apply part of a pack.

### `version` — so the common case costs one comparison

Per-file `sha256` is enough to *decide* what to download, but on its own it means
every housekeeping cycle fetches the list and compares a hundred hashes to
conclude nothing happened. A manifest version short-circuits that, and gives the
inventory endpoint something to report — "which pack is on this device?" should
not be answerable only as a hash dump.

Two rules make it safe rather than a new way to get stuck:

**The tooling sets it, derived from the content.** A hand-bumped number is a
number someone forgets, and a forgotten bump means the device silently skips a
real update. `build-audio.ps1` already hashes every file; deriving the version
from those hashes means it cannot go stale.

**It only means anything paired with the URL.** Once each owner supplies their
own audio, version 3 on one reader and version 3 on another are unrelated
documents. A device must store *(assets_url, version)* and treat a URL change as
an automatic invalidation — otherwise repointing `assets_url` at a different
pack that happens to be version 1 would be skipped by a device that last applied
version 3, and the bank would never arrive.
- A `firmware` object appearing here is ignored. Firmware comes from
  `manifest_url` only, so a compromised or mistyped `assets_url` cannot talk a
  device into installing an image.

## Building a pack

`tools/make-pack.ps1` turns a folder of audio into a pack: it copies the files
into an `assets/` tree, hashes each one, and writes `pack.json` beside it.

```powershell
.\make-pack.ps1 -Source ..\firmware\spiffs -Out C:\host\joe `
                -BaseUrl https://example/joe/assets/ -RequiresFw 1.2.0
```

Serve `C:\host\joe` and point that device's **Audio pack URL** (setup mode, on
the config page) at `.../pack.json`.

The version is derived from the content, never typed — see above for why. It
covers the `remove` list as well as the files: leave retirements out of the
fingerprint and adding one to an otherwise-unchanged pack doesn't move the
version, so the device short-circuits and the file it was told to delete lives
forever.

A **remove-only pack** — no files, just retirements — is valid and the tool will
build one. It's how you take something back off a device you can't reach.

Limits the tool enforces so the device doesn't have to fail at 3 AM instead:
128 files, and a `pack.json` under 16 KB (the device reads it into a fixed
buffer). The real bank is 111 files and ~13 KB of manifest, so there is room but
not unlimited room.

## Hosting a pack on Cloudflare

A **Worker with static assets**, which is where Cloudflare now steers new static
projects (classic Pages still exists, behind a link on the create screen).
Not R2: R2 wants a payment method on file even inside the free tier, and this
doesn't. Workers, Pages, R2 and DNS all live under the same Cloudflare login, so
a domain already on Cloudflare needs no new account.

An assets-only Worker is served at no charge and has no Worker script at all -
no `main` entry, just a directory.

### Put the secret in the path, not the hostname

Every hostname issued a public certificate is published in Certificate
Transparency logs - public, searchable, permanent. A subdomain called
`joe-audio` announces itself the moment the cert is issued. Paths appear
nowhere.

So: a dull hostname, and a long random segment beneath it.

```
https://media.<domain>/<random>/pack.json
https://media.<domain>/<random>/assets/...
```

This is obscurity, not access control - the device sends no credentials, so
anything reachable is reachable by anyone holding the URL. Nothing in a pack
should be anything you'd mind a stranger hearing.

### Layout and deploy

Build the pack *into* the random segment, so the deploy root holds it:

```powershell
$deploy = "C:\host\joe"          # the deploy root
$secret = "<random segment>"
.\make-pack.ps1 -Source ..\firmware\spiffs -Out "$deploy\$secret" `
                -BaseUrl "https://<host>/$secret/assets/" -RequiresFw 1.2.0
Set-Content "$deploy\robots.txt" "User-agent: *`nDisallow: /"
npx wrangler deploy
```

with a `wrangler.jsonc` beside it:

```jsonc
{
  "name": "<worker name>",
  "compatibility_date": "2026-08-03",
  "preview_urls": false,
  "assets": { "directory": "./public" }
}
```

`preview_urls: false` matters here. Left on — and it defaults on — every
deployed version gets its own extra public hostname serving the same files.
More front doors is the wrong direction when an unguessable path is the only
thing guarding the content.

No `not_found_handling`: the default is a plain 404, which is what we want. The
alternatives both serve *HTML* for a missing file, and an error page arriving
where an MP3 was expected is the failure mode most likely to be mistaken for a
working sync. The device hashes what it downloads so it would be caught either
way, but failing at the fetch is faster and reads better in the log.

`make-pack.ps1` already emits exactly the tree the deploy wants, so there is no
packaging step between the two. `robots.txt` goes at the deploy root, not
inside the secret segment.

The free `<name>.<account>.workers.dev` hostname is enough, and has one accidental
virtue: it's covered by a Cloudflare wildcard certificate, so unlike a custom
subdomain it puts nothing in Certificate Transparency at all. Bind a custom
domain from the Worker's **Domains** tab if you'd rather; the DNS record is
created for you when the zone is already on Cloudflare.

### Then point the device at it

Over serial, if you have a cable on it:

```
assets-url https://<host>/<secret>/pack.json
sync-media
```

`assets-url none` clears it. Serial is allowed here where the LAN isn't,
because a USB cable in the socket is the same "you are holding it" evidence the
button gives — stronger than merely being on the Wi-Fi, not weaker.

Otherwise the web form, which is setup-mode only (see above for why): hold the
button while powering on, join the device's own network, set **Audio pack URL**,
save.

Verify over serial with `sync-media` and no argument - it uses the configured
pack URL, and prints the version it applied.

### What a good first sync looks like

Measured on a real 111-file, 1.5 MB pack: about seven minutes, one TLS
handshake per file, ending in

```
assets: asset sync: 111 updated, 0 up-to-date, 0 removed, 0 failed
assets: pack applied: https://<host>/<secret>/pack.json version <v> (111 file(s) downloaded)
```

Run it a second time and it should download nothing at all - that's the version
short-circuit doing its job, and it's the cheapest proof the state was written
correctly.

(The live host, path and results are in `deployment-live.md`, which is not
mirrored publicly - the URL is the access control.)

### Why no private git repo

It works - `raw.githubusercontent.com` plus a bearer token - but fine-grained
PATs expire within about a year. Set one now and it dies around the trip, on a
device in someone else's house, where the fix is talking them through a
power-on-with-button and retyping a 90-character token. Pages URLs don't
expire.

Keep the audio in a private repo if you want it versioned, and deploy from a
checkout. Git for history, dumb static hosting for delivery; the device never
holds a credential.

## Which tool for which job

There are two ways to get custom audio onto a device and they are not competing.

**Uploading through the config page** (`custom audio`, see the roadmap) suits a
one-off: a clip recorded for one child's birthday, added by hand, needing no
hosting and no manifest at all. That is almost certainly the right tool for
"put this on Joe's reader".

**An assets manifest** suits keeping a whole bank in sync, repeatably, across
more than one unit — and re-syncing after a factory reset without redoing the
uploads. It costs somewhere to host the files.

Where to host is an open question, and it is not only technical. Serving one
household from an unguessable URL sits much closer to the personal use the voice
bank's licence allows than a public repository does, but "not indexed" is not a
licence — worth deciding deliberately rather than by default.
- `manifest_url` (optional, top-level) **relocates the device**: on the next
  `update-now`/scheduled check, if this differs from the URL the device is
  configured with, the device saves it and every future check goes there. Use it
  to migrate the manifest host with no reflashing — point the old host's manifest
  at the new one, let devices sync once, then retire the old host. It's persisted
  only (not re-fetched the same run), so an A↔B mistake can't loop. `update-check`
  shows a pending relocation but doesn't apply it.

## When the pack actually syncs

| Moment | Pack fetched? | Verified against flash? |
|---|---|---|
| AP setup mode | **no** | — |
| Boot, once online | yes | **yes** |
| Every 6 h thereafter | yes | no — trusts the pack version |
| `sync-media` / `update-now` | yes | **yes** |

**AP setup mode never syncs**, and that's structural rather than a check: the
network task exits immediately in setup mode, so no housekeeping runs at all.
Which is right — in AP mode the device is its own island with no route to the
host. The sync happens after the reboot into the home network.

**The pack is not gated on firmware updates.** They're separate documents at
separate URLs with separate reasons to be switched on, and `manifest_url`
defaults to a placeholder so a gift never self-updates firmware until you
publish for it. Leaving the pack behind that gate meant a device with
`assets_url` set and a reachable host would sync nothing, forever — the exact
configuration a gift ships in.

**Boot verifies; the routine cycle doesn't.** A pack version can only answer "is
this the same manifest?", never "do the files still match it" — so a sound
deleted through the web UI, or lost to a bad write, is invisible to it. On a
reader in someone else's house nobody is going to type `sync-media`, so the
device has to be the one that notices. Boot is the right place: reboots are
rare, hashing the bank costs ~9 s, and the reader quietly repairs itself.

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
- `sync-media [url]` — sync **only** the assets, no firmware, no reboot. Handy
  for shipping/testing new sounds without bumping the firmware version. With no
  argument it uses `assets_url` if one is set, falling back to `manifest_url`.
  It reports `DEFERRED` when a pack's `requires_fw` turns it away — otherwise
  "0 files, no errors" would read exactly like "already up to date", which is
  the one thing a deferral must not look like.

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

- `installed.json` is a **cache, not the truth**. Before downloading anything
  the device hashes the file already on flash; if it matches the manifest it
  adopts it and skips the transfer. That matters because the record can be
  wrong both ways — `idf.py flash` rewrites the storage partition and wipes it
  while leaving every file intact, and deleting a sound through the web UI
  leaves an entry claiming it's still there.
- A **human-initiated** `sync-media` passes `force`, skipping both
  short-circuits (manifest version *and* per-file record) and verifying every
  file against flash. Measured: ~9 s for a 111-file bank, versus ~7 minutes to
  re-download it. The scheduled path doesn't force — that's where the saved
  round trips are the point.
- Max 128 files per manifest; a `bytes` free-space guard keeps ~64 KB headroom.
  128 rather than something rounder and smaller because the real bank is already
  111 files — the countdown alone is a clip per number per unit — and a cap a
  full pack can't fit under isn't a safety net, it's a bug.
- ~~Deferred optimization: bake `installed.json` into the flashed image so a
  factory-fresh unit already "knows" its assets.~~ Superseded, and by something
  simpler: hashing what's on flash gets the same result with no build step, no
  generated file to keep in sync, and it also covers the cases a baked-in
  record never could — a partial sync, a deleted file, a corrupted one.
