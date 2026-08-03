# ---------------------------------------------------------------------------
# publish-pack.ps1 - build the audio pack and push it live, in one step.
#
# Wraps make-pack.ps1 + `wrangler deploy`. Use this rather than running them by
# hand: doing it in two steps invites the failure where the manifest is
# regenerated but never deployed, or deployed from the wrong folder, and the
# device then fetches a manifest whose files aren't there yet.
#
#   .\publish-pack.ps1                 # build + deploy, using saved settings
#   .\publish-pack.ps1 -WhatIf         # build only, show what WOULD deploy
#   .\publish-pack.ps1 -Remove old.mp3 # also retire a file from every device
#
# THIS PUSHES TO THE PUBLIC INTERNET. Everything under firmware/spiffs becomes
# fetchable by anyone holding the URL. That is the licence line for the voice
# bank (personal use, not published) - an unguessable URL nobody has been given
# is one thing, a link that circulates is another.
#
# It also reaches devices in the field WITHOUT a version bump or any go-live
# step: they take new audio within 6 hours. Unlike firmware, there is no second
# switch to throw. Deploying is the change.
#
# --- settings ---------------------------------------------------------------
# The host and secret path are per-owner and MUST NOT live in this repo (this
# file is mirrored publicly; the URL is the only access control). They come
# from a settings file outside the repo:
#
#   <deploy dir>\pack.settings.json
#   { "worker": "<worker name>", "secret": "<random path segment>",
#     "host": "<worker>.<account>.workers.dev" }
#
# See docs/deployment-live.md (private) for the real values.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    # Where the deploy tree and pack.settings.json live. Outside the repo, so a
    # megabyte and a half of audio can never wander into a commit.
    [string] $Deploy = "C:\DEV\magicmaker-host",

    # What becomes the pack. Defaults to exactly what the device runs.
    [string] $Source = "",

    # Minimum firmware. A device below this defers the pack rather than applying
    # a bank whose clips its code never asks for.
    [string] $RequiresFw = "1.2.0",

    # Files to retire from every device, e.g. -Remove chime-1.mp3
    [string[]] $Remove = @(),

    # Build and report, but don't deploy.
    [switch] $WhatIf
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Source) { $Source = Join-Path $root "firmware\spiffs" }

$settingsPath = Join-Path $Deploy "pack.settings.json"
if (-not (Test-Path $settingsPath)) {
    throw @"
No settings at $settingsPath

Create it with:
  { "worker": "<worker name>", "secret": "<random path segment>",
    "host": "<worker>.<account>.workers.dev" }

The real values are in docs/deployment-live.md, which is NOT mirrored publicly.
"@
}
$s = Get-Content $settingsPath -Raw | ConvertFrom-Json
foreach ($k in 'worker','secret','host') {
    if (-not $s.$k) { throw "pack.settings.json is missing '$k'" }
}

$packDir = Join-Path $Deploy "public\$($s.secret)"
$baseUrl = "https://$($s.host)/$($s.secret)/assets/"

Write-Host "building pack from $Source" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "make-pack.ps1") `
    -Source $Source -Out $packDir -BaseUrl $baseUrl -RequiresFw $RequiresFw -Remove $Remove

# robots.txt at the DEPLOY ROOT, not inside the secret segment - a crawler that
# somehow learns the host should be told to go away before it learns the path.
Set-Content (Join-Path $Deploy "public\robots.txt") "User-agent: *`nDisallow: /" -Encoding UTF8

$pack = Get-Content (Join-Path $packDir "pack.json") -Raw | ConvertFrom-Json

if ($WhatIf) {
    Write-Host ""
    Write-Host "-WhatIf: nothing deployed." -ForegroundColor Yellow
    Write-Host "  would publish pack $($pack.version) to https://$($s.host)/$($s.secret)/pack.json"
    return
}

Write-Host ""
Write-Host "deploying to $($s.host) ..." -ForegroundColor Cyan
Push-Location $Deploy
try {
    # Wrangler writes progress to stderr; don't let that read as failure.
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { npx wrangler deploy 2>&1 | ForEach-Object { "  $_" } } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "wrangler deploy failed (exit $LASTEXITCODE)" }
} finally { Pop-Location }

# Verify from outside rather than trusting the deploy's own word: fetch the
# manifest back and confirm it's the one just built. A deploy that reported
# success while serving a stale pack is the failure this catches.
$url = "https://$($s.host)/$($s.secret)/pack.json"
Start-Sleep -Seconds 2
try {
    $live = (Invoke-WebRequest $url -TimeoutSec 20).Content | ConvertFrom-Json
} catch {
    throw "deployed, but $url did not fetch back: $_"
}

Write-Host ""
if ($live.version -eq $pack.version) {
    Write-Host "LIVE  pack $($live.version) - $($live.assets.files.Count) file(s), requires fw $($live.requires_fw)" -ForegroundColor Green
    if ($Remove.Count) { Write-Host "      retiring: $($Remove -join ', ')" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "Devices pick this up within 6 hours. To take it now, on the device:" -ForegroundColor Cyan
    Write-Host "  sync-media"
} else {
    throw "served version is $($live.version), expected $($pack.version) - deploy did not take"
}
