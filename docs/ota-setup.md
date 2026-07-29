# Turning OTA on for real (GitHub)

Everything in the firmware is built and verified — it's just pointed at the
`https://example.com/manifest.json` placeholder, which is what keeps it dormant.
This is how to give it a real home.

---

## The one hard constraint: it must be publicly readable

The device fetches over plain HTTPS with **no credentials** — there's no way to
give it a GitHub token. So whatever hosts the manifest, the firmware and the
audio has to be reachable **without logging in**, i.e. a **public repo**.

**That means your voice recordings become publicly downloadable.** They're
harmless clips (numbers, "it's only", the cheeky lines) at an obscure URL, and
nobody will ever look — but it should be a decision, not a surprise.

Two ways to go:

| Option | What's public | Effort |
|---|---|---|
| **A. Make this repo public** | code, docs, 3D models, all audio | simplest |
| **B. Separate public `magicmaker-ota` repo** | only what you copy into it | keeps the main repo private |

Option A is what the tooling assumes. For B, create the second repo, copy
`firmware/spiffs/` into it, and pass that repo's name to `release.ps1`.

---

## Hosting model (no duplication)

| Thing | Where | Why |
|---|---|---|
| `firmware.bin` | GitHub **release asset** | build output, doesn't belong in the repo |
| audio / web assets | the repo, via **raw.githubusercontent.com** | `firmware/spiffs/` is already committed — serve it directly |
| `manifest.json` | repo root, via raw | tiny, versioned with everything else |

Nothing is duplicated and no firmware change is needed: the device builds asset
URLs as `base_url + path`, and `raw.githubusercontent.com/<repo>/<branch>/firmware/spiffs/`
+ `cd/cheeky-13.wav` resolves exactly right.

---

## One-time setup

**1. Create the repo on GitHub** (public — see above), then:

```
git remote add origin https://github.com/<you>/<repo>.git
git push -u origin main
```

> Check `.gitignore` first — the Audacity project (~100 MB) and the raw takes are
> already excluded, so only the finished clips go up.

**2. Cut the first release:**

```
tools\release.ps1 -Version 1.0.2 -Repo <you>/<repo>
```

It bumps `FW_VERSION`, builds, hashes, and writes `manifest.json` to the repo
root plus `release\firmware.bin`.

**3. Publish it:**

```
git add -A; git commit -m "release v1.0.2"; git push
gh release create v1.0.2 release\firmware.bin --title "v1.0.2"
```

(No `gh`? Create the release on the website and drag `release\firmware.bin` in.
The tag **must** be `v<version>` — that's what the manifest URL points at.)

**4. Point the device at it — once.** Over USB:

```
update-now https://raw.githubusercontent.com/<you>/<repo>/main/manifest.json
```

The manifest names itself in its `manifest_url` field, so the device **saves that
address permanently**. You never have to do this again, and you never have to
enter setup mode to change the update URL.

---

## Cutting a release after that

```
tools\release.ps1 -Version 1.0.3 -Repo <you>/<repo> -Assets cd/cheeky-13.wav
git add -A; git commit -m "release v1.0.3"; git push
gh release create v1.0.3 release\firmware.bin --title "v1.0.3"
```

- `-Assets` — only the files that **changed**. Everything unlisted is left alone
  on the device; it never re-downloads what it already has.
- `-AllAssets` — ship the whole `firmware/spiffs` tree (first release, or after a
  big audio session).
- `-Remove cd/old-line.wav` — retire a file.
- `-SkipBuild` — reuse the current build.

The device picks it up **at boot and every 6 hours**, installs silently, and
reboots itself. Firmware is rollback-protected: a bad image reverts on its own.

### Audio-only updates need no version bump
New clips land through the assets section without touching the firmware — the
device just downloads them and plays them on the next tap. Handy once the gift
is wrapped: think of a new cheeky line in December, push it, and it appears.

---

## Checking it worked

Over USB:

```
update-check      # what the device thinks is available
sync-media        # assets only, no firmware, no reboot
update-now        # the full check, using the saved URL
```

Or watch for `startup update check:` / `housekeeping:` in the boot log.

## Gotchas

- **Tag must match** `v<version>` or the firmware URL 404s.
- **raw.githubusercontent caches for ~5 minutes** — a just-pushed manifest may
  take that long to appear.
- **Version must increase** for firmware to install (`1.0.10` > `1.0.9`);
  assets sync regardless of version.
- **Don't rewrite history** on the branch the device points at.
