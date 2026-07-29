# `ota` — the live release channel

This branch is not source code. It holds exactly one meaningful file:

    manifest.json

Every MagicMaker in the field fetches that file on boot and every six hours
after. **A commit here goes out to real devices.** Treat a push to this branch
the way you'd treat a deploy, because that's what it is.

## Why it's a separate branch

The manifest is a deployment artifact, not source. If it lived on `main`,
publishing a firmware update and pushing a README typo fix would be the same
gesture — and one careless merge could tell every unit to install something
half-finished. An orphan branch that contains nothing else makes going live a
deliberate, separate act.

It's an *orphan* branch (no shared history with `main`) so that merges between
the two are never even offered.

## What lives where

| Thing | Where | Why |
|---|---|---|
| Firmware source, web page, audio files | `main` | it's source; assets are served raw from `main/firmware/spiffs/` |
| `firmware.bin` | GitHub **release asset** on the `v*` tag | build output, too big and too churny for git |
| `manifest.json` | **here** | the switch that makes a release live |

Note the split: the manifest points *at* assets on `main`, but doesn't live
there. Only this file decides what devices actually do.

## Publishing

Cut the release from the workshop repo, which writes the manifest straight into
this worktree:

```
tools\release.ps1 -Version 1.0.5 -Repo seraphire/magicmaker -ManifestOut ..\MagicMaker-ota
```

Then, **in this order** — the manifest goes last, because it's the trigger:

1. Push source and assets to `main`.
2. Upload the binary: `gh release create v1.0.5 release\firmware.bin`.
3. Commit and push *this* file. Devices pick it up within six hours.

Doing it in any other order points live devices at a binary or an audio file
that isn't there yet.

## Rolling back

Restore the previous manifest and push. Devices only install when the manifest
version is *newer* than what they're running, so a rollback stops the spread but
does not downgrade units that already updated — for that, publish a higher
version number containing the older, known-good build.

## Moving the manifest somewhere else

`manifest.json` carries its own address in the top-level `manifest_url` field.
Change that value and devices adopt the new location on their next check, then
look there forever after. That's how they were migrated here from `main`, and
it means this branch is never a permanent commitment.
