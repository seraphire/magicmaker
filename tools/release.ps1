<#
  release.ps1 - cut an OTA release for the MagicMaker.

  Hosting model (no duplication, no firmware changes):
    * firmware.bin -> uploaded as a GitHub *release asset* (build output, not in
      the repo)
    * audio/web assets -> served straight out of the repo via
      raw.githubusercontent.com, because firmware/spiffs is already committed
    * manifest.json -> committed to the ORPHAN `ota` branch and fetched by the
      device via raw

  Why the manifest lives on its own branch: it is a deployment artifact, not
  source. Devices poll it, so committing it to `main` means any routine code
  push is one bad merge away from telling every unit in the field to install
  something. Keeping it on a branch that holds nothing else makes publishing a
  deliberate act. Note the two branches are used for different things:
    -Branch         where the ASSETS are read from  (main - has firmware/spiffs)
    -ManifestBranch where the MANIFEST is published (ota  - holds only itself)

  Usage:
    tools\release.ps1 -Version 1.0.2 -Repo yourname/magicmaker
    tools\release.ps1 -Version 1.0.2 -Repo yourname/magicmaker -Assets cd/cheeky-13.wav
    tools\release.ps1 -Version 1.0.2 -Repo yourname/magicmaker -AllAssets

  -Assets paths are relative to firmware/spiffs (e.g. "cd/cheeky-13.wav").
  List only what CHANGED; the device keeps everything else it already has.
  Unlisted files are never touched - use -Remove to retire one.
#>
param(
  [Parameter(Mandatory=$true)][string]$Version,
  [Parameter(Mandatory=$true)][string]$Repo,      # "user/repo"
  [string]$Branch = "main",           # branch the assets are served from
  [string]$ManifestBranch = "ota",    # branch the manifest is published to
  [string[]]$Assets = @(),
  [switch]$AllAssets,
  [string[]]$Remove = @(),
  [string]$ManifestUrl = "",     # set to relocate devices to a new manifest home
  [string]$IdfPath = "C:\esp\v5.5.2\esp-idf",
  [string]$ManifestOut = "",     # also drop manifest.json here (e.g. the public repo)
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root   = Split-Path $PSScriptRoot -Parent
$fw     = Join-Path $root "firmware"
$spiffs = Join-Path $fw "spiffs"
$out    = Join-Path $root "release"

# Assets read from $Branch (that's where firmware/spiffs is committed); the
# manifest publishes to $ManifestBranch, which holds nothing else.
$rawBase  = "https://raw.githubusercontent.com/$Repo/$Branch/firmware/spiffs/"
$relBase  = "https://github.com/$Repo/releases/download/v$Version"
$selfUrl  = "https://raw.githubusercontent.com/$Repo/$ManifestBranch/manifest.json"

# By default the manifest points at itself. A device told to check this URL once
# (CLI: update-now <url>) then adopts it permanently, so you never have to enter
# setup mode just to set the update address.
if (-not $ManifestUrl) { $ManifestUrl = $selfUrl }

# --- 1. stamp the version into config.h -----------------------------------
$cfgPath = Join-Path $fw "main\config.h"
$cfg = Get-Content $cfgPath -Raw
if ($cfg -notmatch '#define\s+FW_VERSION\s+"([^"]+)"') { throw "FW_VERSION not found in config.h" }
$old = $Matches[1]
if ($old -ne $Version) {
  Write-Host "FW_VERSION $old -> $Version"
  ($cfg -replace '(#define\s+FW_VERSION\s+")[^"]+(")', "`${1}$Version`${2}") | Set-Content $cfgPath -NoNewline
} else { Write-Host "FW_VERSION already $Version" }

# --- 2. build --------------------------------------------------------------
if (-not $SkipBuild) {
  Write-Host "building..."
  # ESP-IDF's export.ps1 (and idf.py) write progress to stderr. With
  # $ErrorActionPreference = "Stop" that becomes a terminating NativeCommandError,
  # so drop to Continue for the duration of the toolchain calls.
  $prevEAP = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
      $export = Join-Path $IdfPath "export.ps1"
      if (-not (Test-Path $export)) { throw "ESP-IDF not found at $IdfPath (pass -IdfPath)" }
      & $export 2>&1 | Out-Null
    }
    $log = & idf.py -C $fw build 2>&1
    $log | Select-String -Pattern 'error:|Project build complete' | ForEach-Object { "  $_" }
    if (-not ($log | Select-String -Quiet 'Project build complete')) {
      $log | Select-Object -Last 15 | ForEach-Object { "  $_" }
      throw "build failed"
    }
  } finally { $ErrorActionPreference = $prevEAP }
}
$bin = Join-Path $fw "build\magic_band_reader.bin"
if (-not (Test-Path $bin)) { throw "firmware binary missing - build failed?" }

# --- 3. stage --------------------------------------------------------------
if (Test-Path $out) { Get-ChildItem $out -File | ForEach-Object { [System.IO.File]::Delete($_.FullName) } }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item $bin (Join-Path $out "firmware.bin") -Force

function Sha($p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() }
$fwSha = Sha (Join-Path $out "firmware.bin")
$fwLen = (Get-Item (Join-Path $out "firmware.bin")).Length

# --- 4. assets (hashed in place; served from the repo) ---------------------
if ($AllAssets) {
  $Assets = Get-ChildItem $spiffs -Recurse -File |
            ForEach-Object { $_.FullName.Substring($spiffs.Length+1).Replace('\','/') }
}
$entries = @()
foreach ($rel in $Assets) {
  $src = Join-Path $spiffs ($rel -replace '/','\')
  if (-not (Test-Path $src)) { Write-Warning "missing asset: $rel"; continue }
  $entries += [pscustomobject]@{ path=$rel; sha=(Sha $src); bytes=(Get-Item $src).Length }
}

# --- 5. manifest -----------------------------------------------------------
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("{")
if ($ManifestUrl) { [void]$sb.AppendLine("  `"manifest_url`": `"$ManifestUrl`",") }
[void]$sb.AppendLine("  `"firmware`": {")
[void]$sb.AppendLine("    `"version`": `"$Version`",")
[void]$sb.AppendLine("    `"url`": `"$relBase/firmware.bin`",")
[void]$sb.AppendLine("    `"sha256`": `"$fwSha`"")
[void]$sb.Append("  }")
if ($entries.Count -or $Remove.Count) {
  [void]$sb.AppendLine(",")
  [void]$sb.AppendLine("  `"assets`": {")
  [void]$sb.AppendLine("    `"base_url`": `"$rawBase`",")
  [void]$sb.AppendLine("    `"files`": [")
  for ($i=0; $i -lt $entries.Count; $i++) {
    $e = $entries[$i]
    $comma = if ($i -lt $entries.Count-1) { "," } else { "" }
    [void]$sb.AppendLine("      { `"path`": `"$($e.path)`", `"sha256`": `"$($e.sha)`", `"bytes`": $($e.bytes) }$comma")
  }
  [void]$sb.Append("    ]")
  if ($Remove.Count) {
    [void]$sb.AppendLine(",")
    [void]$sb.AppendLine("    `"remove`": [ " + (($Remove | ForEach-Object { "`"$_`"" }) -join ", ") + " ]")
  } else { [void]$sb.AppendLine() }
  [void]$sb.Append("  }")
}
[void]$sb.AppendLine()
[void]$sb.AppendLine("}")
$manifest = $sb.ToString()
$manifest | Set-Content (Join-Path $root "manifest.json") -Encoding UTF8 -NoNewline
$manifest | Set-Content (Join-Path $out  "manifest.json") -Encoding UTF8 -NoNewline
# The device fetches the manifest from the PUBLIC repo's ota branch, which isn't
# where the build happens - drop a copy straight in so a release isn't a
# copy-paste job. Point -ManifestOut at the `ota` worktree (see docs/ota-setup.md).
if ($ManifestOut) {
  if (-not (Test-Path $ManifestOut)) { throw "-ManifestOut path not found: $ManifestOut" }
  $manifest | Set-Content (Join-Path $ManifestOut "manifest.json") -Encoding UTF8 -NoNewline
  Write-Host "manifest also written to $ManifestOut" -ForegroundColor Cyan
}

# --- 6. report -------------------------------------------------------------
Write-Host ""
Write-Host "firmware $Version  ($([math]::Round($fwLen/1KB)) KB)  sha $($fwSha.Substring(0,16))..." -ForegroundColor Green
if ($entries.Count) { Write-Host "$($entries.Count) asset(s) listed (served from the repo, not re-uploaded)" }
if ($Remove.Count)  { Write-Host "$($Remove.Count) file(s) marked for removal" }
Write-Host ""
Write-Host "manifest.json written to the repo root" -ForegroundColor Cyan
Write-Host ""
Write-Host "next - publish in this order:" -ForegroundColor Cyan
Write-Host "  1. source + assets to $Branch (devices read assets from here)"
Write-Host "       git add -A; git commit -m `"release v$Version`"; git push"
Write-Host "  2. the binary as a release asset"
Write-Host "       gh release create v$Version release\firmware.bin --title `"v$Version`""
Write-Host "  3. LAST - the manifest to $ManifestBranch. This is the go-live switch:" -ForegroundColor Yellow
Write-Host "       every device in the field acts on it within 6 hours." -ForegroundColor Yellow
Write-Host "       git -C <ota-worktree> add manifest.json"
Write-Host "       git -C <ota-worktree> commit -m `"publish v$Version`"; git -C <ota-worktree> push"
Write-Host ""
Write-Host "device manifest URL:" -ForegroundColor Cyan
Write-Host "  $selfUrl"
