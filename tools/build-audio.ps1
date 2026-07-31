<#
  build-audio.ps1 - encode the master recordings into the data partition.

  assets\audio-src\  is the source of truth: 16-bit PCM WAV, never overwritten.
  firmware\spiffs\   is build output. Delete the audio in it any time; this
                     script puts it back.

  Every clip is loudness-matched on the way through, so recordings made months
  apart - or by someone else entirely - end up sitting at the same level.

  Usage:
    tools\build-audio.ps1               # build what's changed
    tools\build-audio.ps1 -WhatIf       # show what would happen, touch nothing
    tools\build-audio.ps1 -Force        # rebuild everything
    tools\build-audio.ps1 -NoNormalize  # leave levels exactly as recorded

  Only audio is managed here. www\ is real source and is never touched.
#>
param(
  [string]$Source = "",
  [string]$Dest   = "",
  [switch]$Force,
  [switch]$WhatIf,
  [switch]$NoNormalize,
  [string]$FFmpeg = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Source) { $Source = Join-Path $root "assets\audio-src" }
if (-not $Dest)   { $Dest   = Join-Path $root "firmware\spiffs" }

$PARTITION = 0x9E0000     # storage partition from partitions.csv (10,354,688 B)

# --- loudness ---------------------------------------------------------------
# Clips are matched to a common loudness (EBU R128) rather than a common peak.
# Peak-matching doesn't work: a dense clip and a transient one can share a peak
# and still be several dB apart to the ear. Measured across the prompts before
# this existed, the spread was 7.6 LUFS - "try again" at -13.7 against "wifi
# setup" at -21.2 - which is plainly audible when the device speaks.
#
# It matters most in cd\, where clips are played back-to-back to build one
# sentence. Recordings from four different sessions have to sound like one
# voice, and no amount of care with the mic gets you there for free.
#
# TP=-3 also solves clipping. A lossy round trip overshoots the original peaks
# by 2-3 dB on transient speech, so a source at 0 dBFS decodes past full scale.
# Measured on Program\try-again.wav, a clean recording (37 near-full-scale
# samples, longest flat run 2):
#     0 dB in -> 728 clipped samples out
#    -1 dB    -> 450
#    -3 dB    ->   1
#    -6 dB    ->   0
# -1, the usual advice, is nowhere near enough.
#
# Two-pass with linear=true applies one constant gain per file, so nothing is
# compressed or pumped - the performance is untouched, only its level moves.
$LOUD_I   = -16.0    # integrated target, LUFS
$LOUD_TP  = -3.0     # true-peak ceiling, dBTP
$LOUD_LRA = 11.0     # loudness range target

# --- encoding profiles -----------------------------------------------------
# Keyed by the top-level folder under the source root ("" = partition root).
#
#   cd\       the countdown clip bank. Speech, and the ONLY files that get
#             played back-to-back to compose a sentence.
#   Program\  spoken setup/program-mode prompts. Speech, always standalone.
#   ""        the assignable reward sounds - music and effects, not voice.
#
# cd\ was WAV until the decoder learned to trim MP3's encoder delay and frame
# padding, which was adding 74-102 ms of silence per clip - about a third of a
# second of stutter across a four-part phrase. audio.c now reads the LAME/Xing
# tag and skips exactly that much; verified sample-exact against the masters
# (13450 in, 13450 out) so the join between words is as tight as the WAV was.
#
# Rate = 0 means "keep whatever the source is". Only cd\ forces a rate, because
# only cd\ gets concatenated: retuning the I2S clock mid-sentence is audible as
# a tick, so those clips must all agree. Everything else plays standalone, and
# forcing a rate on them only throws away what was recorded - Program\ holds a
# mix of 16 kHz and 22050 Hz takes, and downsampling the 22050 ones cost them
# their top octave for no benefit.
$profiles = @{
  # 22050 because that is what every cd\ master already is - the point of
  # pinning it is that the clips AGREE (no I2S retune mid-sentence), not that
  # they are small. Forcing 16000 would throw away half the bandwidth of the
  # whole countdown bank for nothing.
  "cd"      = @{ Mode = "mp3"; Rate = 22050; Bitrate = "64k"; What = "countdown bank (pinned rate)" }
  "Program" = @{ Mode = "mp3"; Rate = 0;     Bitrate = "64k"; What = "spoken prompts (source rate)" }
  ""        = @{ Mode = "mp3"; Rate = 0;     Bitrate = "64k"; What = "reward sounds (source rate)" }
}

# --- locate ffmpeg ---------------------------------------------------------
if (-not $FFmpeg) {
  $c = Get-Command ffmpeg -ErrorAction SilentlyContinue
  if ($c) { $FFmpeg = $c.Source }
  else {
    # winget installs it outside the PATH this shell inherited.
    $g = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($g) { $FFmpeg = $g.FullName }
  }
}
if (-not $FFmpeg -or -not (Test-Path $FFmpeg)) {
  throw "ffmpeg not found. Install it (winget install Gyan.FFmpeg) or pass -FFmpeg <path>."
}
if (-not (Test-Path $Source)) { throw "source folder not found: $Source" }

$FFprobe = Join-Path (Split-Path $FFmpeg -Parent) "ffprobe.exe"

function ProfileFor($relDir) {
  $top = if ($relDir) { ($relDir -split '[\\/]')[0] } else { "" }
  if ($profiles.ContainsKey($top)) { return $profiles[$top] }
  return $profiles[""]          # anything new defaults to the standalone profile
}

function SourceRate($path) {
  if (Test-Path $FFprobe) {
    $r = & $FFprobe -v error -select_streams a -show_entries stream=sample_rate -of csv=p=0 $path 2>$null
    if ($r -match '^\d+$') { return [int]$r }
  }
  return 22050
}

# Pass 1: measure. loudnorm reports what it found so pass 2 can apply a single
# constant gain instead of riding the level dynamically.
function MeasureLoudness($path) {
  $filt = "loudnorm=I=$LOUD_I`:TP=$LOUD_TP`:LRA=$LOUD_LRA`:print_format=json"
  $out  = & $FFmpeg -hide_banner -i $path -af $filt -f null - 2>&1 | Out-String
  $m = [regex]::Match($out, '\{[^{}]*"input_i"[\s\S]*?\}')
  if (-not $m.Success) { return $null }
  try { return $m.Value | ConvertFrom-Json } catch { return $null }
}

# Pass 2 filter string. linear=true means one gain for the whole file - no
# compression, no pumping. ffmpeg falls back to dynamic on its own if the
# needed gain would breach the true-peak ceiling, which is the right call.
function LoudnormFilter($meas) {
  "loudnorm=I=$LOUD_I`:TP=$LOUD_TP`:LRA=$LOUD_LRA" +
  ":measured_I=$($meas.input_i):measured_TP=$($meas.input_tp)" +
  ":measured_LRA=$($meas.input_lra):measured_thresh=$($meas.input_thresh)" +
  ":offset=$($meas.target_offset):linear=true"
}

# --- walk the source -------------------------------------------------------
$srcFiles = Get-ChildItem $Source -Recurse -File -Include *.wav,*.mp3
if (-not $srcFiles) { throw "no audio found under $Source" }

$enc = 0; $copied = 0; $skipped = 0; $srcBytes = 0; $outBytes = 0
$expected = New-Object System.Collections.Generic.HashSet[string]

foreach ($f in $srcFiles) {
  $rel    = $f.FullName.Substring($Source.Length).TrimStart('\','/')
  $relDir = Split-Path $rel -Parent
  $p      = ProfileFor $relDir

  $outRel = if ($p.Mode -eq "mp3") { [IO.Path]::ChangeExtension($rel, ".mp3") } else { $rel }
  $outPath = Join-Path $Dest $outRel
  [void]$expected.Add($outRel)
  $srcBytes += $f.Length

  $outDir = Split-Path $outPath -Parent
  if (-not (Test-Path $outDir)) {
    if ($WhatIf) { Write-Host "  mkdir $outDir" } else { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
  }

  # Up to date? Compare write times; -Force overrides.
  $fresh = (Test-Path $outPath) -and -not $Force -and
           ((Get-Item $outPath).LastWriteTimeUtc -ge $f.LastWriteTimeUtc)
  if ($fresh) {
    $skipped++; $outBytes += (Get-Item $outPath).Length; continue
  }

  # Rate: cd\ is pinned so composed phrases never retune I2S mid-sentence;
  # everything else keeps whatever it was recorded at.
  $rate    = if ($p.Rate) { [int]$p.Rate } else { SourceRate $f.FullName }
  $rateTxt = if ($p.Rate) { "$($p.Rate) Hz" } else { "source rate" }
  $verb    = if ($p.Mode -eq "mp3") { "encode" } else { "level " }

  if ($WhatIf) {
    $how = if ($p.Mode -eq "mp3") { "$rateTxt $($p.Bitrate)" } else { "$rateTxt wav" }
    Write-Host ("  {0} {1}  ->  {2}  ({3})" -f $verb, $rel, $outRel, $how)
    if ($p.Mode -eq "mp3") { $enc++ } else { $copied++ }
    continue
  }

  # --- pass 1: measure -----------------------------------------------------
  $filter = $null; $lufsBefore = $null
  if (-not $NoNormalize) {
    $meas = MeasureLoudness $f.FullName
    if ($meas) { $filter = LoudnormFilter $meas; $lufsBefore = $meas.input_i }
    else { Write-Warning "$rel : loudness measurement failed; level left as recorded" }
  }

  # --- pass 2: apply, and encode or write straight back out ----------------
  # NB: not $args - that's a PowerShell automatic variable, and assigning to it
  # silently fails to splat. It quietly dropped the filter once already.
  $ffArgs = @("-hide_banner","-loglevel","error","-y","-i",$f.FullName,"-ac","1")
  if ($filter) { $ffArgs += @("-af",$filter) }
  # loudnorm resamples to 192 kHz internally, so state the output rate rather
  # than inheriting whatever the filter graph leaves behind.
  $ffArgs += @("-ar","$rate")
  if ($p.Mode -eq "mp3") {
    # -map_metadata -1 drops tags: the decoder skips ID3 anyway, and on a
    # 9.9 MB partition a few hundred wasted bytes per clip is real.
    # -write_xing 1 keeps the gapless header for when the decoder learns to
    # read it (see docs/audio-roadmap.md).
    $ffArgs += @("-b:a",$p.Bitrate,"-map_metadata","-1","-write_xing","1")
  } else {
    $ffArgs += @("-c:a","pcm_s16le")
  }
  $ffArgs += $outPath
  & $FFmpeg @ffArgs 2>&1 | Where-Object { $_ } | ForEach-Object { Write-Warning "$rel : $_" }
  if (-not (Test-Path $outPath)) { throw "ffmpeg produced nothing for $rel" }

  if ($lufsBefore -ne $null) {
    $shift = [math]::Round($LOUD_I - [double]$lufsBefore, 1)
    if ([math]::Abs($shift) -ge 1.0) {
      Write-Host ("  {0}: {1,6} LUFS -> {2} ({3:+0.0;-0.0} dB)" -f $rel, $lufsBefore, $LOUD_I, $shift) -ForegroundColor DarkGray
    }
  }
  $outBytes += (Get-Item $outPath).Length
  if ($p.Mode -eq "mp3") { $enc++ } else { $copied++ }
}

# --- prune orphans ---------------------------------------------------------
# Audio in the destination with no matching source is a retired clip. Only
# audio extensions, and only inside folders the source actually maps to, so
# www\ and anything hand-placed elsewhere is safe.
$srcTops = @($srcFiles | ForEach-Object {
  $r = $_.FullName.Substring($Source.Length).TrimStart('\','/')
  $d = Split-Path $r -Parent
  if ($d) { ($d -split '[\\/]')[0] } else { "" }
} | Select-Object -Unique)

$stale = @()
if (Test-Path $Dest) {
  foreach ($d in Get-ChildItem $Dest -Recurse -File -Include *.wav,*.mp3) {
    $rel = $d.FullName.Substring($Dest.Length).TrimStart('\','/')
    $top = if (Split-Path $rel -Parent) { ((Split-Path $rel -Parent) -split '[\\/]')[0] } else { "" }
    if ($srcTops -notcontains $top) { continue }        # not ours to manage
    if (-not $expected.Contains($rel)) { $stale += $d }
  }
}
foreach ($s in $stale) {
  $rel = $s.FullName.Substring($Dest.Length).TrimStart('\','/')
  if ($WhatIf) { Write-Host "  remove $rel  (no longer in source)" }
  else { Remove-Item $s.FullName -Force; Write-Host "  removed $rel" -ForegroundColor DarkYellow }
}

# --- report ----------------------------------------------------------------
Write-Host ""
Write-Host "profiles:" -ForegroundColor Cyan
foreach ($k in $profiles.Keys | Sort-Object) {
  $p = $profiles[$k]
  $name = if ($k) { "$k\" } else { "(root)" }
  $how  = if ($p.Mode -ne "mp3")  { "wav $($p.Rate) Hz" }
          elseif ($p.Rate)        { "mp3 $($p.Rate) Hz $($p.Bitrate)" }
          else                    { "mp3 source rate $($p.Bitrate)" }
  Write-Host ("{0,-10} {1,-24} {2}" -f $name, $how, $p.What)
}
Write-Host ""
$norm = if ($NoNormalize) { "levels left as recorded" } else { "matched to $LOUD_I LUFS / $LOUD_TP dBTP" }
Write-Host "encoded $enc, levelled $copied, up-to-date $skipped, removed $($stale.Count)  ($norm)"

if (-not $WhatIf) {
  $total = (Get-ChildItem $Dest -Recurse -File | Measure-Object -Property Length -Sum).Sum
  $pct   = [math]::Round(100 * $total / $PARTITION, 1)
  Write-Host ""
  Write-Host ("source  : {0,8:N0} KB" -f ($srcBytes/1KB))
  Write-Host ("audio   : {0,8:N0} KB  ({1:N1}x smaller)" -f ($outBytes/1KB), $(if ($outBytes) { $srcBytes/$outBytes } else { 0 }))
  Write-Host ("on disk : {0,8:N0} KB of {1:N0} KB partition  ({2}% used)" -f ($total/1KB), ($PARTITION/1KB), $pct)
  if ($pct -gt 75) {
    Write-Warning "Over 75% full. The asset OTA writes <name>.new before renaming, so it needs room for a second copy of the largest file it replaces."
  }
  Write-Host ""
  Write-Host "next: idf.py -C firmware build   (the LittleFS image is built from firmware\spiffs)" -ForegroundColor Cyan
}
