<#
  release.ps1 - cut an OTA release for the MagicMaker.

  Hosting model (no duplication, no firmware changes):
    * firmware.bin -> uploaded as a GitHub *release asset* (build output, not in
      the repo)
    * audio/web assets -> served straight out of the repo via
      raw.githubusercontent.com, because firmware/spiffs is already committed
    * manifest.json -> committed to the repo, fetched by the device via raw

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
  [string]$Branch = "main",
  [string[]]$Assets = @(),
  [switch]$AllAssets,
  [string[]]$Remove = @(),
  [string]$ManifestUrl = "",     # set to relocate devices to a new manifest home
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root   = Split-Path $PSScriptRoot -Parent
$fw     = Join-Path $root "firmware"
$spiffs = Join-Path $fw "spiffs"
$out    = Join-Path $root "release"

$rawBase  = "https://raw.githubusercontent.com/$Repo/$Branch/firmware/spiffs/"
$relBase  = "https://github.com/$Repo/releases/download/v$Version"
$selfUrl  = "https://raw.githubusercontent.com/$Repo/$Branch/manifest.json"

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
  & "C:\esp\v5.5.2\esp-idf\export.ps1" *> $null
  idf.py -C $fw build 2>&1 | Select-String -Pattern 'error:|Project build complete' | ForEach-Object { "  $_" }
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

# --- 6. report -------------------------------------------------------------
Write-Host ""
Write-Host "firmware $Version  ($([math]::Round($fwLen/1KB)) KB)  sha $($fwSha.Substring(0,16))..." -ForegroundColor Green
if ($entries.Count) { Write-Host "$($entries.Count) asset(s) listed (served from the repo, not re-uploaded)" }
if ($Remove.Count)  { Write-Host "$($Remove.Count) file(s) marked for removal" }
Write-Host ""
Write-Host "manifest.json written to the repo root" -ForegroundColor Cyan
Write-Host ""
Write-Host "next:" -ForegroundColor Cyan
Write-Host "  git add -A; git commit -m `"release v$Version`"; git push"
Write-Host "  gh release create v$Version release\firmware.bin --title `"v$Version`"   (or upload it on the website)"
Write-Host ""
Write-Host "device manifest URL:" -ForegroundColor Cyan
Write-Host "  https://raw.githubusercontent.com/$Repo/$Branch/manifest.json"
