# ---------------------------------------------------------------------------
# make-pack.ps1 - build an audio pack manifest from a folder of assets.
#
# A pack is what `assets_url` on a device points at: a manifest of its own,
# holding only assets, hosted wherever that device's owner wants. It exists
# because the firmware manifest is public - anything listed there is published,
# with no unlisted state - so a personal voice bank cannot live in it.
#
#   .\make-pack.ps1 -Source ..\firmware\spiffs -Out C:\host\joe -BaseUrl https://example/joe/assets/
#
# Then serve C:\host\joe and point the device at <base>/pack.json.
#
# The version is DERIVED from the content, never typed. A hand-bumped number is
# a number someone forgets, and a forgotten bump means the device silently skips
# a real update - the manifest says "nothing changed" while the files say
# otherwise. Here it cannot go stale: change any file and the version changes.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    # Folder whose contents become the pack. Every file under it is included,
    # at the same relative path it will occupy on the device's filesystem.
    [Parameter(Mandatory)] [string] $Source,

    # Where to write pack.json and the assets/ tree that goes with it.
    [Parameter(Mandatory)] [string] $Out,

    # Public URL of that assets/ folder. Must end in '/'.
    [Parameter(Mandatory)] [string] $BaseUrl,

    # Firmware floor. A device below this defers the pack rather than applying
    # a bank whose clips its code never asks for.
    [string] $RequiresFw = "1.1.0",

    # Files to retire from devices, relative paths, e.g. -Remove chime-1.mp3
    # Keep a retirement listed until every device has synced once.
    [string[]] $Remove = @(),

    # Skip files matching these wildcards. www/ is the config page, which comes
    # from the firmware build, not from someone's audio folder.
    [string[]] $Exclude = @('www/*', 'installed.json')
)

$ErrorActionPreference = 'Stop'

if (-not $BaseUrl.EndsWith('/')) { $BaseUrl += '/' }
$Source = (Resolve-Path $Source).Path
$assetsOut = Join-Path $Out 'assets'
New-Item -ItemType Directory -Force -Path $assetsOut | Out-Null

$entries = @()
$copied = 0

Get-ChildItem $Source -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($Source.Length + 1) -replace '\\', '/'
    foreach ($pat in $Exclude) { if ($rel -like $pat) { return } }

    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()

    $dst = Join-Path $assetsOut ($rel -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item $_.FullName $dst -Force
    $copied++

    $entries += [pscustomobject]@{ path = $rel; sha256 = $hash; bytes = $_.Length }
}

# A remove-only pack is legitimate - "retire these, send nothing" - and the
# device supports it, so the tool must be able to express it.
if ($entries.Count -eq 0 -and $Remove.Count -eq 0) { throw "No files found under $Source" }
if ($entries.Count -gt 128) { throw "$($entries.Count) files - the device caps a manifest at 128" }

# Version = hash of (path, hash) pairs plus the retirement list, in sorted
# order. Same content, same version, on any machine; one byte different
# anywhere, different version.
#
# The removals are in the fingerprint deliberately. Leave them out and adding a
# retirement to an otherwise unchanged pack doesn't move the version - so the
# device short-circuits, and the file it was told to delete stays forever.
$fingerprint = (
    ($entries | ForEach-Object { "$($_.path):$($_.sha256)" }) +
    ($Remove  | Sort-Object   | ForEach-Object { "-$_" })
) -join "`n"
$sha = [System.Security.Cryptography.SHA256]::Create()
$version = ([BitConverter]::ToString(
    $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fingerprint))
) -replace '-', '').ToLower().Substring(0, 12)

$pack = [ordered]@{
    version     = $version
    requires_fw = $RequiresFw
    assets      = [ordered]@{
        base_url = $BaseUrl
        files    = @($entries)   # @() so one file still serializes as an array
    }
}
if ($Remove.Count -gt 0) { $pack.assets.remove = $Remove }

# Compressed, not pretty-printed: the device reads this into a fixed 16 KB
# buffer, and indentation on a hundred entries is kilobytes of nothing.
$json = $pack | ConvertTo-Json -Depth 6 -Compress
if ($json.Length -gt 15500) {
    throw "pack.json is $($json.Length) bytes - the device's buffer is 16 KB. Fewer files, or shorter paths."
}
Set-Content (Join-Path $Out 'pack.json') $json -Encoding UTF8

$bytes = [int](($entries | Measure-Object bytes -Sum).Sum)
Write-Host "pack $version - $($entries.Count) file(s), $([math]::Round($bytes/1KB)) KB, requires fw $RequiresFw"
if ($Remove.Count -gt 0) { Write-Host "  retiring: $($Remove -join ', ')" }
Write-Host "  wrote $(Join-Path $Out 'pack.json') and $copied file(s) to $assetsOut"
